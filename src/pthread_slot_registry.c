#include "internal/pthread_slot_registry.h"

#include <sched.h>
#include <string.h>

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
            old_entry->occupied = false;
        }
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }

    if (old_entry != NULL) {
        old_entry->occupied = false;
    }
    PeakPthreadSlotRegistryEntry* entry = &registry->entries[slot];
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
    if (entry != NULL) {
        *token = entry->token;
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
    if (entry == NULL || entry->token.generation != generation) {
        (void)pthread_mutex_unlock(&registry->mutex);
        return false;
    }
    if (reusable) {
        size_t reusable_tail =
            (registry->reusable_head + registry->reusable_count) % registry->capacity;
        registry->reusable[reusable_tail] = entry->token.slot;
        registry->reusable_count++;
    }
    entry->occupied = false;
    (void)pthread_mutex_unlock(&registry->mutex);
    return true;
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
        entry->occupied = false;
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

void
peak_pthread_slot_registry_publish_ready(_Atomic int* ready)
{
    atomic_store_explicit(ready, 1, memory_order_release);
}

void
peak_pthread_slot_registry_wait_ready(const _Atomic int* ready)
{
    while (atomic_load_explicit(ready, memory_order_acquire) == 0) {
        sched_yield();
    }
}
