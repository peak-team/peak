#include "internal/pthread_slot_registry.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define WORKER_COUNT 8
#define STRESS_ROUNDS 200

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
        peak_pthread_slot_registry_wait_ready(&release_worker[worker_id]);
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

    atomic_store_explicit(&holder->ready, 1, memory_order_release);
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

            peak_pthread_slot_registry_wait_ready(&ready[worker]);
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
        peak_pthread_slot_registry_wait_ready(&holders[holder].ready);
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

int
main(void)
{
    pthread_t worker_threads[WORKER_COUNT];

    CHECK(peak_pthread_slot_registry_init(&registry, WORKER_COUNT + 4));
    CHECK(run_concurrent_stress(worker_threads) == 0);
    peak_pthread_slot_registry_destroy(&registry);
    CHECK(peak_pthread_slot_registry_init(&registry, 4));
    CHECK(run_lifecycle_checks() == 0);
    peak_pthread_slot_registry_destroy(&registry);
    puts("pthread_slot_registry_ok");
    return 0;
}
