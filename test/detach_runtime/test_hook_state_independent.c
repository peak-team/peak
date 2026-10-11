#define _GNU_SOURCE
#include "general_listener.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

__attribute__((noinline, used, visibility("default")))
void peak_hook_state_probe_target(void)
{
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
}

typedef PeakHookState (*StateFn)(size_t);
typedef void (*LockFn)(void);
typedef void (*IntFn)(int);
typedef int (*NoArgIntFn)(void);
typedef int (*ReserveFn)(size_t);
typedef size_t (*CapacityFn)(void);
typedef void (*CountFn)(size_t);
typedef void (*StoreFn)(size_t, PeakHookState);

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    StateFn state;
    size_t hook_id;
    int ready;
    int go;
    int done;
    PeakHookState result;
} Probe;

typedef struct {
    StateFn state;
    atomic_int stop;
    atomic_int invalid;
    atomic_uint reads;
} LoopProbe;

static void* looping_reader(void* argument)
{
    LoopProbe* probe = argument;
    while (!atomic_load(&probe->stop)) {
        PeakHookState value = probe->state(0);
        atomic_fetch_add(&probe->reads, 1);
        if (value < PEAK_HOOK_UNRESOLVED || value > PEAK_HOOK_SHUTDOWN) {
            atomic_store(&probe->invalid, 1);
        }
    }
    return NULL;
}

static void* query_worker(void* argument)
{
    Probe* probe = argument;
    pthread_mutex_lock(&probe->mutex);
    probe->ready = 1;
    pthread_cond_signal(&probe->cond);
    while (!probe->go) {
        pthread_cond_wait(&probe->cond, &probe->mutex);
    }
    pthread_mutex_unlock(&probe->mutex);

    PeakHookState result = probe->state(probe->hook_id);
    pthread_mutex_lock(&probe->mutex);
    probe->result = result;
    probe->done = 1;
    pthread_cond_signal(&probe->cond);
    pthread_mutex_unlock(&probe->mutex);
    return NULL;
}

static int wait_paused(NoArgIntFn paused, int stage)
{
    for (int i = 0; i < 2000; i++) {
        if (paused() == stage) {
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

static int start_paused_query(Probe* probe, pthread_t* worker, IntFn pause,
                              NoArgIntFn paused, int stage, size_t hook_id)
{
    pause(stage);
    pthread_mutex_lock(&probe->mutex);
    probe->hook_id = hook_id;
    probe->ready = probe->go = probe->done = 0;
    pthread_mutex_unlock(&probe->mutex);
    if (pthread_create(worker, NULL, query_worker, probe) != 0) {
        return 0;
    }
    pthread_mutex_lock(&probe->mutex);
    while (!probe->ready) {
        pthread_cond_wait(&probe->cond, &probe->mutex);
    }
    probe->go = 1;
    pthread_cond_signal(&probe->cond);
    pthread_mutex_unlock(&probe->mutex);
    return wait_paused(paused, stage);
}

int main(void)
{
    StateFn state = (StateFn)dlsym(RTLD_DEFAULT, "peak_general_listener_hook_state");
    LockFn lock = (LockFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_controller_lock");
    LockFn unlock = (LockFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_controller_unlock");
#ifndef HOOK_STATE_BASELINE
    IntFn fail_after = (IntFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_fail_after");
    ReserveFn reserve = (ReserveFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_reserve");
    CapacityFn capacity = (CapacityFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_capacity");
    CapacityFn allocations = (CapacityFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_allocations");
    CapacityFn live_chunks = (CapacityFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_live_chunks");
    CapacityFn chunk_bytes = (CapacityFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_chunk_bytes");
    CountFn publish_count = (CountFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_publish_count");
    CountFn reset_prefix = (CountFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_reset_prefix");
    StoreFn store = (StoreFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_store");
    IntFn pause = (IntFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_pause");
    NoArgIntFn paused = (NoArgIntFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_paused");
    LockFn resume = (LockFn)dlsym(RTLD_DEFAULT, "peak_general_listener_test_status_resume");
#endif
    Probe probe = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                    .cond = PTHREAD_COND_INITIALIZER,
                    .state = state };
    pthread_t worker;
    int completed_while_locked = 0;

    peak_hook_state_probe_target();
    if (state == NULL || lock == NULL || unlock == NULL ||
#ifndef HOOK_STATE_BASELINE
        fail_after == NULL || reserve == NULL || capacity == NULL ||
        allocations == NULL || live_chunks == NULL || chunk_bytes == NULL ||
        publish_count == NULL || reset_prefix == NULL || store == NULL ||
        pause == NULL || paused == NULL || resume == NULL ||
#endif
        state(0) != PEAK_HOOK_ATTACHED ||
        state(SIZE_MAX) != PEAK_HOOK_UNRESOLVED) {
        fprintf(stderr, "hook state setup or invalid-ID check failed\n");
        return 2;
    }

    lock();
    if (pthread_create(&worker, NULL, query_worker, &probe) != 0) {
        unlock();
        return 2;
    }
    pthread_mutex_lock(&probe.mutex);
    while (!probe.ready) {
        pthread_cond_wait(&probe.cond, &probe.mutex);
    }
    probe.go = 1;
    pthread_cond_signal(&probe.cond);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    while (!probe.done) {
        if (pthread_cond_timedwait(&probe.cond, &probe.mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    completed_while_locked = probe.done;
    pthread_mutex_unlock(&probe.mutex);
    unlock();
    pthread_join(worker, NULL);
    if (!completed_while_locked || probe.result != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "hook state query waited for controller lock\n");
        return 1;
    }

#ifndef HOOK_STATE_BASELINE
    /* The captured count remains valid while the root grows. */
    int reached = start_paused_query(&probe, &worker, pause, paused, 1, 0);
    lock();
    int grew = reserve(65);
    unlock();
    resume();
    pthread_join(worker, NULL);
    if (!reached || !grew || probe.result != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "count-before-root growth query failed\n");
        return 1;
    }

    /* A captured old root remains valid after count invalidation and reuse. */
    lock();
    store(64, PEAK_HOOK_DETACHED);
    publish_count(65);
    unlock();
    reached = start_paused_query(&probe, &worker, pause, paused, 2, 64);
    lock();
    publish_count(0);
    reset_prefix(1);
    publish_count(1);
    unlock();
    resume();
    pthread_join(worker, NULL);
    if (!reached || probe.result != PEAK_HOOK_DETACHED ||
        state(64) != PEAK_HOOK_UNRESOLVED) {
        fprintf(stderr, "root-before-cleanup query failed\n");
        return 1;
    }

    reached = start_paused_query(&probe, &worker, pause, paused, 2, 0);
    lock();
    publish_count(0);
    reset_prefix(1);
    publish_count(1);
    store(0, PEAK_HOOK_ATTACHED);
    unlock();
    resume();
    pthread_join(worker, NULL);
    if (!reached || probe.result != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "same-index reuse query failed\n");
        return 1;
    }

    lock();
    for (int value = PEAK_HOOK_UNRESOLVED; value <= PEAK_HOOK_SHUTDOWN; value++) {
        store(0, (PeakHookState)value);
        if (state(0) != (PeakHookState)value) {
            unlock();
            fprintf(stderr, "enum state publication failed at %d\n", value);
            return 1;
        }
    }
    store(0, PEAK_HOOK_ATTACHED);
    unlock();

    /* Failed multi-chunk reserve rolls back unpublished allocations. */
    lock();
    size_t before = capacity();
    size_t allocated_before = allocations();
    size_t live_before = live_chunks();
    fail_after(1);
    int failed = !reserve(before + 128);
    int unchanged = capacity() == before &&
                    live_chunks() == live_before &&
                    allocations() == allocated_before + 1 &&
                    state(0) == PEAK_HOOK_ATTACHED &&
                    state(before) == PEAK_HOOK_UNRESOLVED;
    fail_after(-1);
    int recovered = reserve(before + 128) &&
                    capacity() == before + 128 &&
                    live_chunks() == live_before + 2;
    size_t high_water = capacity();
    size_t steady_allocations = allocations();
    unlock();
    LoopProbe loop_probe = { .state = state };
    pthread_t loop_thread;
    if (pthread_create(&loop_thread, NULL, looping_reader, &loop_probe) != 0) {
        return 2;
    }
    for (int attempt = 0; attempt < 2000 && !atomic_load(&loop_probe.reads); attempt++) {
        usleep(1000);
    }
    unsigned int reads_before = atomic_load(&loop_probe.reads);
    lock();
    for (int i = 0; i < 2000; i++) {
        publish_count(0);
        reset_prefix(1);
        publish_count(1);
        store(0, (PeakHookState)(i % (PEAK_HOOK_SHUTDOWN + 1)));
        if (!reserve(1) || capacity() != high_water) {
            recovered = 0;
            break;
        }
        if (i % 32 == 0) {
            sched_yield();
        }
    }
    unsigned int reads_during = atomic_load(&loop_probe.reads);
    store(0, PEAK_HOOK_ATTACHED);
    atomic_store(&loop_probe.stop, 1);
    unlock();
    pthread_join(loop_thread, NULL);
    recovered = recovered && reads_before > 0 &&
                reads_during > reads_before &&
                !atomic_load(&loop_probe.invalid) &&
                allocations() == steady_allocations &&
                live_chunks() == high_water / 64;
    if (!failed || !unchanged || !recovered) {
        fprintf(stderr, "reserve rollback or lifecycle reuse failed\n");
        return 1;
    }
    printf("hook_status_storage chunk_bytes=%zu allocations=%zu live_chunks=%zu capacity=%zu\n",
           chunk_bytes(), allocations(), live_chunks(), capacity());
#endif
    puts("hook_state_independent_ok");
    return 0;
}
