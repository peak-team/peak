#include "internal/pthread_slot_registry.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t peak_pthread_start_gate_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peak_pthread_start_gate_cond;
static pthread_once_t peak_pthread_start_gate_once = PTHREAD_ONCE_INIT;
static _Atomic int peak_pthread_start_gate_ready;
#ifdef PEAK_ENABLE_TEST_HOOKS
static unsigned int peak_pthread_start_test_waiters;
#endif

static void
peak_pthread_start_gate_initialize(void)
{
#if defined(__linux__)
    pthread_condattr_t attr;
    int status;

    if (pthread_condattr_init(&attr) != 0) {
        return;
    }
    status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (status == 0) {
        status = pthread_cond_init(&peak_pthread_start_gate_cond, &attr);
    }
    (void)pthread_condattr_destroy(&attr);
    if (status == 0) {
        atomic_store_explicit(&peak_pthread_start_gate_ready, 1,
                              memory_order_release);
    }
#elif defined(__APPLE__)
    if (pthread_cond_init(&peak_pthread_start_gate_cond, NULL) == 0) {
        atomic_store_explicit(&peak_pthread_start_gate_ready, 1,
                              memory_order_release);
    }
#endif
}

static bool
peak_pthread_start_gate_is_ready(void)
{
    return pthread_once(&peak_pthread_start_gate_once,
                        peak_pthread_start_gate_initialize) == 0 &&
           atomic_load_explicit(&peak_pthread_start_gate_ready,
                                memory_order_acquire) != 0;
}

static bool
peak_pthread_start_deadline(struct timespec* deadline,
                            unsigned int timeout_ms)
{
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
        return false;
    }
    deadline->tv_sec += timeout_ms / 1000U;
    deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

static int
peak_pthread_start_timedwait(const struct timespec* deadline)
{
#if defined(__linux__)
    return pthread_cond_timedwait(&peak_pthread_start_gate_cond,
                                  &peak_pthread_start_gate_mutex,
                                  deadline);
#elif defined(__APPLE__)
    struct timespec now;
    struct timespec remaining;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return EINVAL;
    }
    remaining.tv_sec = deadline->tv_sec - now.tv_sec;
    remaining.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (remaining.tv_nsec < 0) {
        remaining.tv_sec--;
        remaining.tv_nsec += 1000000000L;
    }
    if (remaining.tv_sec < 0 ||
        (remaining.tv_sec == 0 && remaining.tv_nsec == 0)) {
        return ETIMEDOUT;
    }
    return pthread_cond_timedwait_relative_np(&peak_pthread_start_gate_cond,
                                              &peak_pthread_start_gate_mutex,
                                              &remaining);
#else
    (void)deadline;
    return ENOTSUP;
#endif
}

typedef struct {
    pthread_mutex_t* mutex;
    _Atomic int* state;
} PeakPthreadStartWaitCleanup;

static void
peak_pthread_start_wait_cancel(void* data)
{
    PeakPthreadStartWaitCleanup* cleanup = data;
    int expected = PEAK_PTHREAD_START_PENDING;

#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_pthread_start_test_waiters--;
#endif
    (void)atomic_compare_exchange_strong_explicit(
        cleanup->state, &expected, PEAK_PTHREAD_START_ABANDONED,
        memory_order_acq_rel, memory_order_acquire);
    (void)pthread_mutex_unlock(cleanup->mutex);
}

static bool
peak_pthread_slot_registry_tid_equal(pthread_t left, pthread_t right)
{
    return pthread_equal(left, right) != 0;
}

static PeakPthreadSlotRegistryEntry*
peak_pthread_slot_registry_find_unlocked(PeakPthreadSlotRegistry* registry,
                                         pthread_t tid)
{
    for (size_t index = 0; index < registry->capacity; index++) {
        PeakPthreadSlotRegistryEntry* entry = &registry->entries[index];

        if (entry->occupied &&
            peak_pthread_slot_registry_tid_equal(entry->tid, tid)) {
            return entry;
        }
    }
    return NULL;
}

static PeakPthreadSlotRegistryEntry*
peak_pthread_slot_registry_find_token_unlocked(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token)
{
    if (token.slot >= registry->capacity) {
        return NULL;
    }

    PeakPthreadSlotRegistryEntry* entry = &registry->entries[token.slot];
    if (!entry->occupied || entry->token.generation != token.generation) {
        return NULL;
    }
    return entry;
}

static void
peak_pthread_slot_registry_discard_unlocked(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotRegistryEntry* entry)
{
    if (entry->exit_pending && registry->exit_pending_count != 0) {
        registry->exit_pending_count--;
    }
    entry->occupied = false;
    entry->detached = false;
    entry->final_destructor_pass = false;
    entry->exit_pending = false;
    entry->retiring = false;
    entry->kernel_tid = 0;
}

static bool
peak_pthread_slot_registry_update_exit_pending_unlocked(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotRegistryEntry* entry)
{
    if (entry->exit_pending || entry->retiring || !entry->detached ||
        !entry->final_destructor_pass || entry->kernel_tid <= 0) {
        return false;
    }

    entry->exit_pending = true;
    registry->exit_pending_count++;
    if (registry->exit_pending_count > registry->max_exit_pending_count) {
        registry->max_exit_pending_count = registry->exit_pending_count;
    }
    return true;
}

static void
peak_pthread_slot_registry_enqueue_reusable_unlocked(
    PeakPthreadSlotRegistry* registry,
    size_t slot)
{
    size_t reusable_tail =
        (registry->reusable_head + registry->reusable_count) %
        registry->capacity;
    registry->reusable[reusable_tail] = slot;
    registry->reusable_count++;
}

bool
peak_pthread_slot_registry_init(PeakPthreadSlotRegistry* registry,
                                size_t capacity)
{
    if (registry == NULL || capacity > PEAK_PTHREAD_SLOT_REGISTRY_MAX_CAPACITY) {
        return false;
    }

    memset(registry, 0, sizeof(*registry));
    if (pthread_mutex_init(&registry->mutex, NULL) != 0) {
        return false;
    }
    registry->capacity = capacity;
    registry->initialized = true;
    return true;
}

void
peak_pthread_slot_registry_destroy(PeakPthreadSlotRegistry* registry)
{
    if (registry == NULL || !registry->initialized) {
        return;
    }

    registry->initialized = false;
    (void)pthread_mutex_destroy(&registry->mutex);
}

bool
peak_pthread_slot_registry_reserve_insert(PeakPthreadSlotRegistry* registry,
                                          pthread_t tid,
                                          PeakPthreadSlotToken* token)
{
    PeakPthreadSlotRegistryEntry* old_entry;
    size_t slot;

    if (registry == NULL || token == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    old_entry = peak_pthread_slot_registry_find_unlocked(registry, tid);
    if (registry->reusable_count != 0) {
        slot = registry->reusable[registry->reusable_head];
        registry->reusable_head =
            (registry->reusable_head + 1) % registry->capacity;
        registry->reusable_count--;
    } else if (registry->next_slot < registry->capacity) {
        slot = registry->next_slot++;
    } else {
        /* A newly created thread can receive a pthread_t held by an unjoined
         * predecessor. Drop that ambiguous mapping, but never recycle it. */
        if (old_entry != NULL) {
            peak_pthread_slot_registry_discard_unlocked(registry, old_entry);
        }
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }

    if (old_entry != NULL) {
        peak_pthread_slot_registry_discard_unlocked(registry, old_entry);
    }
    PeakPthreadSlotRegistryEntry* entry = &registry->entries[slot];
    memset(entry, 0, sizeof(*entry));
    entry->tid = tid;
    entry->token.slot = slot;
    entry->token.generation = ++registry->next_generation;
    if (entry->token.generation == 0) {
        entry->token.generation = ++registry->next_generation;
    }
    entry->occupied = true;
    *token = entry->token;
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
}

bool
peak_pthread_slot_registry_capture(PeakPthreadSlotRegistry* registry,
                                   pthread_t tid,
                                   PeakPthreadSlotToken* token)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || token == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_unlocked(registry, tid);
    if (entry != NULL && !entry->retiring) {
        *token = entry->token;
    } else {
        entry = NULL;
    }
    (void)pthread_mutex_unlock(&registry->mutex);
    return entry != NULL;
}

bool
peak_pthread_slot_registry_compare_remove(PeakPthreadSlotRegistry* registry,
                                          pthread_t tid,
                                          uint64_t generation,
                                          bool reusable)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_unlocked(registry, tid);
    if (entry == NULL || entry->retiring ||
        entry->token.generation != generation) {
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }
    if (reusable) {
        peak_pthread_slot_registry_enqueue_reusable_unlocked(
            registry, entry->token.slot);
    }
    peak_pthread_slot_registry_discard_unlocked(registry, entry);
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
}

bool
peak_pthread_slot_registry_compare_remove_token(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    bool reusable)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_token_unlocked(registry, token);
    if (entry == NULL || entry->retiring) {
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }
    if (reusable) {
        peak_pthread_slot_registry_enqueue_reusable_unlocked(
            registry, entry->token.slot);
    }
    peak_pthread_slot_registry_discard_unlocked(registry, entry);
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
}

bool
peak_pthread_slot_registry_begin_retire(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_token_unlocked(registry, token);
    if (entry == NULL || entry->retiring) {
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }
    entry->retiring = true;
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
}

bool
peak_pthread_slot_registry_complete_retire(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    bool reusable)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_token_unlocked(registry, token);
    if (entry == NULL || !entry->retiring) {
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }
    if (reusable) {
        peak_pthread_slot_registry_enqueue_reusable_unlocked(
            registry, entry->token.slot);
    }
    peak_pthread_slot_registry_discard_unlocked(registry, entry);
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
}

bool
peak_pthread_slot_registry_defer_retire(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_token_unlocked(registry, token);
    if (entry == NULL || !entry->retiring) {
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }
    entry->retiring = false;
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
}

bool
peak_pthread_slot_registry_mark_kernel_tid(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    pid_t kernel_tid)
{
    PeakPthreadSlotRegistryEntry* entry;
    bool became_exit_pending = false;

    if (registry == NULL || !registry->initialized || kernel_tid <= 0) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_token_unlocked(registry, token);
    if (entry != NULL) {
        entry->kernel_tid = kernel_tid;
        became_exit_pending =
            peak_pthread_slot_registry_update_exit_pending_unlocked(
                registry, entry);
    }
    (void)pthread_mutex_unlock(&registry->mutex);
    return entry != NULL || became_exit_pending;
}

bool
peak_pthread_slot_registry_mark_detached(
    PeakPthreadSlotRegistry* registry,
    pthread_t tid,
    uint64_t generation,
    bool* became_exit_pending)
{
    PeakPthreadSlotRegistryEntry* entry;
    bool became_pending = false;

    if (became_exit_pending != NULL) {
        *became_exit_pending = false;
    }
    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_unlocked(registry, tid);
    if (entry != NULL && entry->token.generation == generation) {
        entry->detached = true;
        became_pending =
            peak_pthread_slot_registry_update_exit_pending_unlocked(
                registry, entry);
    } else {
        entry = NULL;
    }
    (void)pthread_mutex_unlock(&registry->mutex);
    if (became_exit_pending != NULL) {
        *became_exit_pending = became_pending;
    }
    return entry != NULL;
}

bool
peak_pthread_slot_registry_mark_final_destructor(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    bool* became_exit_pending)
{
    PeakPthreadSlotRegistryEntry* entry;
    bool became_pending = false;

    if (became_exit_pending != NULL) {
        *became_exit_pending = false;
    }
    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_token_unlocked(registry, token);
    if (entry != NULL) {
        entry->final_destructor_pass = true;
        became_pending =
            peak_pthread_slot_registry_update_exit_pending_unlocked(
                registry, entry);
    }
    (void)pthread_mutex_unlock(&registry->mutex);
    if (became_exit_pending != NULL) {
        *became_exit_pending = became_pending;
    }
    return entry != NULL;
}

size_t
peak_pthread_slot_registry_snapshot_exit_pending(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadDetachedCandidate* candidates,
    size_t budget,
    size_t* entries_examined)
{
    size_t count = 0;
    size_t examined = 0;

    if (entries_examined != NULL) {
        *entries_examined = 0;
    }
    if (registry == NULL || !registry->initialized || candidates == NULL ||
        budget == 0 || registry->capacity == 0) {
        return 0;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    size_t limit = budget < registry->capacity ? budget : registry->capacity;
    while (examined < limit) {
        size_t index = (registry->reclaim_cursor + examined) %
                       registry->capacity;
        PeakPthreadSlotRegistryEntry* entry = &registry->entries[index];

        if (entry->occupied && entry->exit_pending && !entry->retiring) {
            candidates[count].token = entry->token;
            candidates[count].kernel_tid = entry->kernel_tid;
            count++;
        }
        examined++;
    }
    registry->reclaim_cursor =
        (registry->reclaim_cursor + examined) % registry->capacity;
    (void)pthread_mutex_unlock(&registry->mutex);

    if (entries_examined != NULL) {
        *entries_examined = examined;
    }
    return count;
}

size_t
peak_pthread_slot_registry_exit_pending_count(
    PeakPthreadSlotRegistry* registry,
    size_t* max_count)
{
    size_t count = 0;

    if (max_count != NULL) {
        *max_count = 0;
    }
    if (registry == NULL || !registry->initialized) {
        return 0;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    count = registry->exit_pending_count;
    if (max_count != NULL) {
        *max_count = registry->max_exit_pending_count;
    }
    (void)pthread_mutex_unlock(&registry->mutex);
    return count;
}

bool
peak_pthread_slot_registry_quarantine(PeakPthreadSlotRegistry* registry,
                                      pthread_t tid)
{
    PeakPthreadSlotRegistryEntry* entry;

    if (registry == NULL || !registry->initialized) {
        return false;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    entry = peak_pthread_slot_registry_find_unlocked(registry, tid);
    if (entry != NULL) {
        peak_pthread_slot_registry_discard_unlocked(registry, entry);
    }
    (void)pthread_mutex_unlock(&registry->mutex);
    return entry != NULL;
}

bool
peak_pthread_slot_registry_contains(PeakPthreadSlotRegistry* registry,
                                    pthread_t tid)
{
    PeakPthreadSlotToken token;

    return peak_pthread_slot_registry_capture(registry, tid, &token);
}

size_t
peak_pthread_slot_registry_snapshot(PeakPthreadSlotRegistry* registry,
                                    pthread_t* tids,
                                    size_t* slots,
                                    size_t capacity,
                                    bool* complete)
{
    size_t count = 0;
    bool copied_all = true;

    if (complete != NULL) {
        *complete = true;
    }
    if (registry == NULL || !registry->initialized) {
        return 0;
    }

    (void)pthread_mutex_lock(&registry->mutex);
    for (size_t index = 0; index < registry->capacity; index++) {
        PeakPthreadSlotRegistryEntry* entry = &registry->entries[index];

        if (!entry->occupied) {
            continue;
        }
        if (tids == NULL || slots == NULL || count == capacity) {
            copied_all = false;
            continue;
        }
        tids[count] = entry->tid;
        slots[count] = entry->token.slot;
        count++;
    }
    (void)pthread_mutex_unlock(&registry->mutex);

    if (complete != NULL) {
        *complete = copied_all;
    }
    return count;
}

bool
peak_pthread_slot_registry_publish_ready(_Atomic int* state)
{
    int expected = PEAK_PTHREAD_START_PENDING;
    bool published;

    if (!peak_pthread_start_gate_is_ready() ||
        pthread_mutex_lock(&peak_pthread_start_gate_mutex) != 0) {
        return atomic_compare_exchange_strong_explicit(
            state, &expected, PEAK_PTHREAD_START_READY,
            memory_order_release, memory_order_acquire);
    }
    published = atomic_compare_exchange_strong_explicit(
        state, &expected, PEAK_PTHREAD_START_READY,
        memory_order_release, memory_order_acquire);
    if (published) {
        (void)pthread_cond_broadcast(&peak_pthread_start_gate_cond);
    }
    (void)pthread_mutex_unlock(&peak_pthread_start_gate_mutex);
    return published;
}

PeakPthreadStartHandshakeState
peak_pthread_slot_registry_wait_ready(_Atomic int* state,
                                      unsigned int timeout_ms)
{
    struct timespec deadline;
    int current = atomic_load_explicit(state, memory_order_acquire);
    PeakPthreadStartHandshakeState result;
    PeakPthreadStartWaitCleanup cleanup = {
        .mutex = &peak_pthread_start_gate_mutex,
        .state = state,
    };

    if (current != PEAK_PTHREAD_START_PENDING) {
        return (PeakPthreadStartHandshakeState)current;
    }
    if (timeout_ms == 0 || !peak_pthread_start_gate_is_ready() ||
        !peak_pthread_start_deadline(&deadline, timeout_ms) ||
        pthread_mutex_lock(&peak_pthread_start_gate_mutex) != 0) {
        int expected = PEAK_PTHREAD_START_PENDING;

        (void)atomic_compare_exchange_strong_explicit(
            state, &expected, PEAK_PTHREAD_START_ABANDONED,
            memory_order_acq_rel, memory_order_acquire);
        return (PeakPthreadStartHandshakeState)atomic_load_explicit(
            state, memory_order_acquire);
    }
    pthread_cleanup_push(peak_pthread_start_wait_cancel, &cleanup);
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_pthread_start_test_waiters++;
#endif
    while ((current = atomic_load_explicit(state, memory_order_acquire)) ==
           PEAK_PTHREAD_START_PENDING) {
        int wait_status = peak_pthread_start_timedwait(&deadline);

        if (wait_status != 0) {
            int expected = PEAK_PTHREAD_START_PENDING;

            (void)atomic_compare_exchange_strong_explicit(
                state, &expected, PEAK_PTHREAD_START_ABANDONED,
                memory_order_acq_rel, memory_order_acquire);
            break;
        }
    }
    result = (PeakPthreadStartHandshakeState)atomic_load_explicit(
        state, memory_order_acquire);
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_pthread_start_test_waiters--;
#endif
    pthread_cleanup_pop(0);
    (void)pthread_mutex_unlock(&peak_pthread_start_gate_mutex);
    return result;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
unsigned int
peak_pthread_slot_registry_test_wake_waiters(void)
{
    unsigned int waiters = 0;

    if (peak_pthread_start_gate_is_ready() &&
        pthread_mutex_lock(&peak_pthread_start_gate_mutex) == 0) {
        waiters = peak_pthread_start_test_waiters;
        (void)pthread_cond_broadcast(&peak_pthread_start_gate_cond);
        (void)pthread_mutex_unlock(&peak_pthread_start_gate_mutex);
    }
    return waiters;
}
#endif
