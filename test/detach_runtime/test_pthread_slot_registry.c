#include "internal/pthread_slot_registry.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define WORKER_COUNT 8
#define STRESS_ROUNDS 200
#define HANDSHAKE_TIMEOUT_MS 5000U

static PeakPthreadSlotRegistry registry;
static _Atomic int ready[WORKER_COUNT];
static _Atomic int release_worker[WORKER_COUNT];
static _Atomic int worker_failures;
static uint64_t payload[WORKER_COUNT];

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed: %s\n", #condition); \
            return 1; \
        } \
    } while (0)

typedef struct {
    int worker_id;
} WorkerArgument;

typedef struct {
    _Atomic int ready;
    _Atomic int release;
} KeyHolder;

typedef struct {
    _Atomic int* state;
    PeakPthreadStartHandshakeState result;
} HandshakeWaiter;

static _Atomic int cancellation_waiter_started;

static void*
handshake_waiter(void* data)
{
    HandshakeWaiter* waiter = data;

    waiter->result = peak_pthread_slot_registry_wait_ready(
        waiter->state, HANDSHAKE_TIMEOUT_MS);
    return NULL;
}

static void*
cancellation_waiter(void* data)
{
    _Atomic int* state = data;

    atomic_store_explicit(&cancellation_waiter_started, 1,
                          memory_order_release);
    (void)peak_pthread_slot_registry_wait_ready(state,
                                                HANDSHAKE_TIMEOUT_MS);
    return NULL;
}

static int
run_handshake_checks(void)
{
    _Atomic int state = PEAK_PTHREAD_START_PENDING;
    HandshakeWaiter waiter = { .state = &state };
    pthread_t thread;

    CHECK(peak_pthread_slot_registry_publish_ready(&state));
    CHECK(peak_pthread_slot_registry_wait_ready(
              &state, HANDSHAKE_TIMEOUT_MS) == PEAK_PTHREAD_START_READY);

    atomic_store_explicit(&state, PEAK_PTHREAD_START_PENDING,
                          memory_order_release);
    CHECK(peak_pthread_slot_registry_wait_ready(&state, 1) ==
          PEAK_PTHREAD_START_ABANDONED);
    CHECK(!peak_pthread_slot_registry_publish_ready(&state));

    atomic_store_explicit(&state, PEAK_PTHREAD_START_PENDING,
                          memory_order_release);
    CHECK(pthread_create(&thread, NULL, handshake_waiter, &waiter) == 0);
    CHECK(peak_pthread_slot_registry_publish_ready(&state));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.result == PEAK_PTHREAD_START_READY);

    atomic_store_explicit(&state, PEAK_PTHREAD_START_PENDING,
                          memory_order_release);
    waiter.result = PEAK_PTHREAD_START_ABANDONED;
    CHECK(pthread_create(&thread, NULL, handshake_waiter, &waiter) == 0);
    unsigned int waiter_count = 0;
    for (unsigned int attempt = 0;
         attempt < 10000 && waiter_count == 0; ++attempt) {
        waiter_count = peak_pthread_slot_registry_test_wake_waiters();
    }
    CHECK(waiter_count == 1);
    CHECK(atomic_load_explicit(&state, memory_order_acquire) ==
          PEAK_PTHREAD_START_PENDING);
    CHECK(peak_pthread_slot_registry_publish_ready(&state));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.result == PEAK_PTHREAD_START_READY);

    atomic_store_explicit(&state, PEAK_PTHREAD_START_PENDING,
                          memory_order_release);
    atomic_store_explicit(&cancellation_waiter_started, 0,
                          memory_order_release);
    CHECK(pthread_create(&thread, NULL, cancellation_waiter, &state) == 0);
    while (atomic_load_explicit(&cancellation_waiter_started,
                                memory_order_acquire) == 0) {
    }
    CHECK(pthread_cancel(thread) == 0);
    void* canceled_result = NULL;
    CHECK(pthread_join(thread, &canceled_result) == 0);
    CHECK(canceled_result == PTHREAD_CANCELED);
    CHECK(atomic_load_explicit(&state, memory_order_acquire) ==
          PEAK_PTHREAD_START_ABANDONED);

    /* Cancellation must release the shared wait mutex for later handshakes. */
    atomic_store_explicit(&state, PEAK_PTHREAD_START_PENDING,
                          memory_order_release);
    CHECK(peak_pthread_slot_registry_publish_ready(&state));
    CHECK(peak_pthread_slot_registry_wait_ready(
              &state, HANDSHAKE_TIMEOUT_MS) == PEAK_PTHREAD_START_READY);
    return 0;
}

static void*
registry_worker(void* data)
{
    WorkerArgument* argument = data;
    int worker_id = argument->worker_id;

    for (int round = 0; round < STRESS_ROUNDS; round++) {
        PeakPthreadSlotToken token;

        if (!peak_pthread_slot_registry_reserve_insert(&registry, pthread_self(),
                                                        &token)) {
            atomic_fetch_add_explicit(&worker_failures, 1, memory_order_relaxed);
            return NULL;
        }
        payload[worker_id] = ((uint64_t)(unsigned int)round << 32) |
                             (uint32_t)worker_id;
        peak_pthread_slot_registry_publish_ready(&ready[worker_id]);
        if (peak_pthread_slot_registry_wait_ready(
                &release_worker[worker_id], HANDSHAKE_TIMEOUT_MS) !=
            PEAK_PTHREAD_START_READY) {
            atomic_fetch_add_explicit(&worker_failures, 1,
                                      memory_order_relaxed);
            return NULL;
        }
        if (!peak_pthread_slot_registry_compare_remove(&registry, pthread_self(),
                                                       token.generation, true)) {
            atomic_fetch_add_explicit(&worker_failures, 1, memory_order_relaxed);
            return NULL;
        }
        atomic_store_explicit(&ready[worker_id], 0, memory_order_release);
        while (atomic_load_explicit(&release_worker[worker_id],
                                    memory_order_acquire) != 0) {
        }
    }
    return NULL;
}

static void*
key_holder(void* data)
{
    KeyHolder* holder = data;

    (void)peak_pthread_slot_registry_publish_ready(&holder->ready);
    while (atomic_load_explicit(&holder->release, memory_order_acquire) == 0) {
    }
    return NULL;
}

static int
run_concurrent_stress(pthread_t worker_threads[WORKER_COUNT])
{
    WorkerArgument arguments[WORKER_COUNT];

    for (int worker = 0; worker < WORKER_COUNT; worker++) {
        arguments[worker].worker_id = worker;
        CHECK(pthread_create(&worker_threads[worker], NULL, registry_worker,
                             &arguments[worker]) == 0);
    }

    for (int round = 0; round < STRESS_ROUNDS; round++) {
        for (int worker = 0; worker < WORKER_COUNT; worker++) {
            PeakPthreadSlotToken token;

            CHECK(peak_pthread_slot_registry_wait_ready(
                      &ready[worker], HANDSHAKE_TIMEOUT_MS) ==
                  PEAK_PTHREAD_START_READY);
            CHECK(payload[worker] ==
                  (((uint64_t)(unsigned int)round << 32) | (uint32_t)worker));
            CHECK(peak_pthread_slot_registry_capture(&registry,
                                                     worker_threads[worker],
                                                     &token));
            CHECK(!peak_pthread_slot_registry_compare_remove(
                &registry, worker_threads[worker], token.generation - 1, false));
            CHECK(peak_pthread_slot_registry_contains(&registry,
                                                      worker_threads[worker]));
        }
        for (int worker = 0; worker < WORKER_COUNT; worker++) {
            peak_pthread_slot_registry_publish_ready(&release_worker[worker]);
        }
        for (int worker = 0; worker < WORKER_COUNT; worker++) {
            while (atomic_load_explicit(&ready[worker], memory_order_acquire) != 0) {
            }
            atomic_store_explicit(&release_worker[worker], 0, memory_order_release);
        }
    }

    for (int worker = 0; worker < WORKER_COUNT; worker++) {
        CHECK(pthread_join(worker_threads[worker], NULL) == 0);
    }
    CHECK(atomic_load_explicit(&worker_failures, memory_order_relaxed) == 0);
    return 0;
}

static int
run_lifecycle_checks(void)
{
    PeakPthreadSlotToken old_token;
    PeakPthreadSlotToken current_token;
    PeakPthreadSlotToken reused_token;
    PeakPthreadSlotToken quarantined_token;
    PeakPthreadSlotToken second_active_token;
    pthread_t holder_threads[4];
    KeyHolder holders[4] = {0};
    pthread_t snapshot_tids[4];
    size_t snapshot_slots[4];
    bool complete = false;

    for (int holder = 0; holder < 4; holder++) {
        CHECK(pthread_create(&holder_threads[holder], NULL, key_holder,
                             &holders[holder]) == 0);
        CHECK(peak_pthread_slot_registry_wait_ready(
                  &holders[holder].ready, HANDSHAKE_TIMEOUT_MS) ==
              PEAK_PTHREAD_START_READY);
    }

    CHECK(peak_pthread_slot_registry_reserve_insert(&registry, holder_threads[0],
                                                    &old_token));
    CHECK(peak_pthread_slot_registry_reserve_insert(&registry, holder_threads[0],
                                                    &current_token));
    CHECK(current_token.generation != old_token.generation);
    CHECK(!peak_pthread_slot_registry_compare_remove(&registry, holder_threads[0],
                                                     old_token.generation, false));
    CHECK(peak_pthread_slot_registry_capture(&registry, holder_threads[0],
                                             &reused_token));
    CHECK(reused_token.generation == current_token.generation);
    CHECK(peak_pthread_slot_registry_compare_remove(&registry, holder_threads[0],
                                                    current_token.generation, true));
    CHECK(peak_pthread_slot_registry_reserve_insert(&registry, holder_threads[1],
                                                    &reused_token));
    CHECK(reused_token.slot == current_token.slot);
    CHECK(peak_pthread_slot_registry_quarantine(&registry, holder_threads[1]));
    CHECK(peak_pthread_slot_registry_reserve_insert(&registry, holder_threads[2],
                                                    &quarantined_token));
    CHECK(quarantined_token.slot != reused_token.slot);
    CHECK(peak_pthread_slot_registry_reserve_insert(&registry, holder_threads[3],
                                                    &second_active_token));
    CHECK(peak_pthread_slot_registry_snapshot(&registry, snapshot_tids,
                                              snapshot_slots, 1, &complete) == 1);
    CHECK(!complete);
    CHECK(peak_pthread_slot_registry_snapshot(&registry, snapshot_tids,
                                              snapshot_slots, 4, &complete) == 2);
    CHECK(complete);
    CHECK(peak_pthread_slot_registry_compare_remove(&registry, holder_threads[2],
                                                    quarantined_token.generation,
                                                    false));
    CHECK(peak_pthread_slot_registry_compare_remove(&registry, holder_threads[3],
                                                    second_active_token.generation,
                                                    false));
    for (int holder = 0; holder < 4; holder++) {
        atomic_store_explicit(&holders[holder].release, 1, memory_order_release);
        CHECK(pthread_join(holder_threads[holder], NULL) == 0);
    }
    return 0;
}

static int
run_detached_reclamation_checks(void)
{
    PeakPthreadSlotToken late_detach_token;
    PeakPthreadSlotToken detached_at_create_token;
    PeakPthreadSlotToken reused_token;
    PeakPthreadDetachedCandidate candidate;
    pthread_t holder_threads[3];
    KeyHolder holders[3] = {0};
    bool became_exit_pending = false;
    size_t entries_examined = 0;
    size_t max_pending = 0;

    for (int holder = 0; holder < 3; holder++) {
        CHECK(pthread_create(&holder_threads[holder], NULL, key_holder,
                             &holders[holder]) == 0);
        CHECK(peak_pthread_slot_registry_wait_ready(
                  &holders[holder].ready, HANDSHAKE_TIMEOUT_MS) ==
              PEAK_PTHREAD_START_READY);
    }

    /* A thread that exits before pthread_detach remains quarantined until the
     * successful late detach publishes the missing half of EXIT_PENDING. */
    CHECK(peak_pthread_slot_registry_reserve_insert(
        &registry, holder_threads[0], &late_detach_token));
    CHECK(peak_pthread_slot_registry_mark_kernel_tid(
        &registry, late_detach_token, 1001));
    CHECK(peak_pthread_slot_registry_mark_final_destructor(
        &registry, late_detach_token, &became_exit_pending));
    CHECK(!became_exit_pending);
    CHECK(peak_pthread_slot_registry_exit_pending_count(
              &registry, &max_pending) == 0);
    CHECK(!peak_pthread_slot_registry_mark_detached(
        &registry, holder_threads[0], late_detach_token.generation + 1,
        &became_exit_pending));
    CHECK(!became_exit_pending);
    CHECK(peak_pthread_slot_registry_mark_detached(
        &registry, holder_threads[0], late_detach_token.generation,
        &became_exit_pending));
    CHECK(became_exit_pending);
    CHECK(peak_pthread_slot_registry_exit_pending_count(
              &registry, &max_pending) == 1);
    CHECK(max_pending == 1);
    CHECK(peak_pthread_slot_registry_snapshot_exit_pending(
              &registry, &candidate, 1, &entries_examined) == 1);
    CHECK(entries_examined == 1);
    CHECK(candidate.token.slot == late_detach_token.slot);
    CHECK(candidate.token.generation == late_detach_token.generation);
    CHECK(candidate.kernel_tid == 1001);
    PeakPthreadDetachedCandidate stale_candidate = candidate;

    /* Retirement owns one generation before its physical slot is cleared.
     * Capture/snapshot cannot hand the same generation to a competing join or
     * controller pass, and a failed retire can restore pending eligibility. */
    CHECK(peak_pthread_slot_registry_begin_retire(
        &registry, late_detach_token));
    CHECK(!peak_pthread_slot_registry_capture(
        &registry, holder_threads[0], &reused_token));
    CHECK(!peak_pthread_slot_registry_begin_retire(
        &registry, late_detach_token));
    CHECK(peak_pthread_slot_registry_snapshot_exit_pending(
              &registry, &candidate, 4, &entries_examined) == 0);
    CHECK(entries_examined == 4);
    CHECK(peak_pthread_slot_registry_defer_retire(
        &registry, late_detach_token));
    CHECK(peak_pthread_slot_registry_capture(
        &registry, holder_threads[0], &reused_token));
    CHECK(reused_token.generation == late_detach_token.generation);
    CHECK(peak_pthread_slot_registry_begin_retire(
        &registry, late_detach_token));

    /* A stale /proc candidate must not remove a replacement generation even
     * when the physical slot and pthread_t are reused. */
    CHECK(peak_pthread_slot_registry_complete_retire(
        &registry, late_detach_token, true));
    CHECK(peak_pthread_slot_registry_exit_pending_count(
              &registry, NULL) == 0);
    CHECK(peak_pthread_slot_registry_reserve_insert(
        &registry, holder_threads[1], &reused_token));
    CHECK(reused_token.slot == late_detach_token.slot);
    CHECK(reused_token.generation != late_detach_token.generation);
    CHECK(!peak_pthread_slot_registry_compare_remove_token(
        &registry, stale_candidate.token, true));
    CHECK(peak_pthread_slot_registry_contains(&registry, holder_threads[1]));
    CHECK(peak_pthread_slot_registry_compare_remove_token(
        &registry, reused_token, true));

    /* Create-time detached state is recorded before the child's kernel TID
     * and final destructor pass arrive. Only the complete triple is eligible. */
    CHECK(peak_pthread_slot_registry_reserve_insert(
        &registry, holder_threads[2], &detached_at_create_token));
    CHECK(peak_pthread_slot_registry_mark_detached(
        &registry, holder_threads[2], detached_at_create_token.generation,
        &became_exit_pending));
    CHECK(!became_exit_pending);
    CHECK(peak_pthread_slot_registry_mark_kernel_tid(
        &registry, detached_at_create_token, 1002));
    CHECK(peak_pthread_slot_registry_exit_pending_count(
              &registry, NULL) == 0);
    CHECK(peak_pthread_slot_registry_mark_final_destructor(
        &registry, detached_at_create_token, &became_exit_pending));
    CHECK(became_exit_pending);

    /* Every snapshot is bounded by its caller-supplied scan budget and the
     * round-robin cursor eventually reaches the pending entry. */
    bool found = false;
    for (size_t scan = 0; scan < 4; scan++) {
        size_t count = peak_pthread_slot_registry_snapshot_exit_pending(
            &registry, &candidate, 1, &entries_examined);
        CHECK(entries_examined == 1);
        CHECK(count <= 1);
        if (count == 1) {
            CHECK(candidate.token.slot == detached_at_create_token.slot);
            CHECK(candidate.token.generation ==
                  detached_at_create_token.generation);
            CHECK(candidate.kernel_tid == 1002);
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(peak_pthread_slot_registry_compare_remove_token(
        &registry, detached_at_create_token, false));
    CHECK(peak_pthread_slot_registry_exit_pending_count(
              &registry, NULL) == 0);

    for (int holder = 0; holder < 3; holder++) {
        atomic_store_explicit(&holders[holder].release, 1,
                              memory_order_release);
        CHECK(pthread_join(holder_threads[holder], NULL) == 0);
    }
    return 0;
}

int
main(void)
{
    pthread_t worker_threads[WORKER_COUNT];

    CHECK(run_handshake_checks() == 0);
    CHECK(peak_pthread_slot_registry_init(&registry, WORKER_COUNT + 4));
    CHECK(run_concurrent_stress(worker_threads) == 0);
    peak_pthread_slot_registry_destroy(&registry);
    CHECK(peak_pthread_slot_registry_init(&registry, 4));
    CHECK(run_lifecycle_checks() == 0);
    peak_pthread_slot_registry_destroy(&registry);
    CHECK(peak_pthread_slot_registry_init(&registry, 4));
    CHECK(run_detached_reclamation_checks() == 0);
    peak_pthread_slot_registry_destroy(&registry);
    puts("pthread_slot_registry_ok");
    return 0;
}
