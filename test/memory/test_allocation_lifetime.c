#define PEAK_ENABLE_TEST_HOOKS 1
#include "malloc_interceptor.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OLD_SIZE = 64,
    REUSED_SIZE = 80,
    NEW_SIZE = 128,
    STRESS_THREADS = 8,
    STRESS_ITERATIONS = 256,
};

typedef enum {
    ALLOCATOR_LIBC = 0,
    ALLOCATOR_FREE_REUSE,
    ALLOCATOR_REALLOC_MOVE_REUSE,
    ALLOCATOR_REALLOC_IN_PLACE,
    ALLOCATOR_REALLOC_FAILURE,
    ALLOCATOR_REALLOC_ZERO_FREE,
    ALLOCATOR_REALLOC_ZERO_ALLOCATE,
    ALLOCATOR_REALLOC_ZERO_FAILURE,
    ALLOCATOR_REALLOC_UNTRACKED,
    ALLOCATOR_REALLOC_NULL,
} TestAllocatorMode;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    TestAllocatorMode mode;
    void* old_pointer;
    void* result_pointer;
    int address_reusable;
    int reused_lifetime_tracked;
    unsigned int realloc_calls;
    unsigned int realloc_null_calls;
} TestAllocatorState;

typedef struct {
    _Atomic int failures;
    unsigned int id;
} StressWorker;

static TestAllocatorState allocator_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

static int
expect(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "allocation lifetime test: %s\n", message);
        return 0;
    }
    return 1;
}

static void
reset_allocator(TestAllocatorMode mode, void* old_pointer, void* result_pointer)
{
    pthread_mutex_lock(&allocator_state.mutex);
    allocator_state.mode = mode;
    allocator_state.old_pointer = old_pointer;
    allocator_state.result_pointer = result_pointer;
    allocator_state.address_reusable = 0;
    allocator_state.reused_lifetime_tracked = 0;
    allocator_state.realloc_calls = 0;
    allocator_state.realloc_null_calls = 0;
    pthread_mutex_unlock(&allocator_state.mutex);
}

static void*
test_malloc(size_t size)
{
    TestAllocatorMode mode;
    void* old_pointer;

    pthread_mutex_lock(&allocator_state.mutex);
    mode = allocator_state.mode;
    old_pointer = allocator_state.old_pointer;
    pthread_mutex_unlock(&allocator_state.mutex);
    if ((mode == ALLOCATOR_FREE_REUSE ||
         mode == ALLOCATOR_REALLOC_MOVE_REUSE) &&
        size == REUSED_SIZE) {
        return old_pointer;
    }
    return malloc(size);
}

static void
wait_for_reused_lifetime(void)
{
    pthread_mutex_lock(&allocator_state.mutex);
    allocator_state.address_reusable = 1;
    pthread_cond_broadcast(&allocator_state.condition);
    while (!allocator_state.reused_lifetime_tracked) {
        pthread_cond_wait(&allocator_state.condition, &allocator_state.mutex);
    }
    pthread_mutex_unlock(&allocator_state.mutex);
}

static void
test_free(void* ptr)
{
    TestAllocatorMode mode;
    void* old_pointer;

    pthread_mutex_lock(&allocator_state.mutex);
    mode = allocator_state.mode;
    old_pointer = allocator_state.old_pointer;
    pthread_mutex_unlock(&allocator_state.mutex);
    if (mode == ALLOCATOR_FREE_REUSE && ptr == old_pointer) {
        wait_for_reused_lifetime();
        return;
    }
    free(ptr);
}

static void*
test_realloc(void* ptr, size_t size)
{
    TestAllocatorMode mode;
    void* old_pointer;
    void* result_pointer;

    pthread_mutex_lock(&allocator_state.mutex);
    ++allocator_state.realloc_calls;
    if (ptr == NULL) ++allocator_state.realloc_null_calls;
    mode = allocator_state.mode;
    old_pointer = allocator_state.old_pointer;
    result_pointer = allocator_state.result_pointer;
    pthread_mutex_unlock(&allocator_state.mutex);

    if (mode == ALLOCATOR_REALLOC_MOVE_REUSE && ptr == old_pointer) {
        wait_for_reused_lifetime();
        return result_pointer;
    }
    if (mode == ALLOCATOR_REALLOC_IN_PLACE) return ptr;
    if (mode == ALLOCATOR_REALLOC_FAILURE ||
        mode == ALLOCATOR_REALLOC_ZERO_FAILURE) {
        errno = ENOMEM;
        return NULL;
    }
    if (mode == ALLOCATOR_REALLOC_ZERO_FREE) {
        errno = 0;
        return NULL;
    }
    if (mode == ALLOCATOR_REALLOC_ZERO_ALLOCATE ||
        mode == ALLOCATOR_REALLOC_UNTRACKED ||
        mode == ALLOCATOR_REALLOC_NULL) {
        return result_pointer;
    }
    return realloc(ptr, size);
}

static int
begin_tracking(TestAllocatorMode mode, void* old_pointer, void* result_pointer)
{
    const PeakMallocTestAllocator allocator = {
        .malloc_fn = test_malloc,
        .free_fn = test_free,
        .realloc_fn = test_realloc,
    };

    reset_allocator(mode, old_pointer, result_pointer);
    return expect(peak_malloc_test_begin(&allocator),
                  "could not initialize tracking fixture");
}

static void*
reuse_old_address(void* unused)
{
    void* result;

    (void) unused;
    pthread_mutex_lock(&allocator_state.mutex);
    while (!allocator_state.address_reusable) {
        pthread_cond_wait(&allocator_state.condition, &allocator_state.mutex);
    }
    pthread_mutex_unlock(&allocator_state.mutex);

    result = peak_malloc_test_malloc(REUSED_SIZE);
    pthread_mutex_lock(&allocator_state.mutex);
    allocator_state.reused_lifetime_tracked = result == allocator_state.old_pointer;
    pthread_cond_broadcast(&allocator_state.condition);
    pthread_mutex_unlock(&allocator_state.mutex);
    return result == allocator_state.old_pointer ? NULL : (void*) 1;
}

static int
run_free_reuse(void)
{
    PeakMallocTestTrackingSnapshot snapshot;
    pthread_t reuse_thread;
    void* thread_result = NULL;
    void* old_pointer = malloc(NEW_SIZE);
    int result = 1;

    if (!expect(old_pointer != NULL, "could not allocate free fixture")) return 1;
    if (!begin_tracking(ALLOCATOR_FREE_REUSE, old_pointer, NULL)) goto out;
    if (!expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                "could not seed freed lifetime")) goto tracking_out;
    if (!expect(pthread_create(&reuse_thread, NULL, reuse_old_address, NULL) == 0,
                "could not start reuse thread")) goto tracking_out;
    peak_malloc_test_free(old_pointer);
    pthread_join(reuse_thread, &thread_result);
    peak_malloc_test_tracking_snapshot(old_pointer, &snapshot);
    result = !(expect(thread_result == NULL, "allocator did not reuse the old address") &&
               expect(snapshot.entry_count == 1 && snapshot.pointer_tracked &&
                      snapshot.pointer_size == REUSED_SIZE,
                      "free bookkeeping removed the reused lifetime") &&
               expect(snapshot.current_bytes == REUSED_SIZE &&
                      snapshot.table_bytes == REUSED_SIZE,
                      "free reuse accounting diverged"));

tracking_out:
    peak_malloc_test_end();
out:
    free(old_pointer);
    return result;
}

static int
run_realloc_reuse(void)
{
    PeakMallocTestTrackingSnapshot old_snapshot;
    PeakMallocTestTrackingSnapshot new_snapshot;
    pthread_t reuse_thread;
    void* thread_result = NULL;
    void* old_pointer = malloc(OLD_SIZE);
    void* new_pointer = malloc(NEW_SIZE);
    void* realloc_result;
    int result = 1;

    if (!expect(old_pointer != NULL && new_pointer != NULL,
                "could not allocate moving realloc fixture")) goto out;
    if (!begin_tracking(ALLOCATOR_REALLOC_MOVE_REUSE,
                        old_pointer, new_pointer)) goto out;
    if (!expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                "could not seed realloc lifetime")) goto tracking_out;
    if (!expect(pthread_create(&reuse_thread, NULL, reuse_old_address, NULL) == 0,
                "could not start realloc reuse thread")) goto tracking_out;
    realloc_result = peak_malloc_test_realloc(old_pointer, NEW_SIZE);
    pthread_join(reuse_thread, &thread_result);
    peak_malloc_test_tracking_snapshot(old_pointer, &old_snapshot);
    peak_malloc_test_tracking_snapshot(new_pointer, &new_snapshot);
    result = !(expect(realloc_result == new_pointer && thread_result == NULL,
                      "moving realloc did not use deterministic addresses") &&
               expect(old_snapshot.entry_count == 2 &&
                      old_snapshot.pointer_tracked &&
                      old_snapshot.pointer_size == REUSED_SIZE,
                      "moving realloc mutated the reused lifetime") &&
               expect(new_snapshot.pointer_tracked &&
                      new_snapshot.pointer_size == NEW_SIZE,
                      "moving realloc lost its new lifetime") &&
               expect(new_snapshot.current_bytes == REUSED_SIZE + NEW_SIZE &&
                      new_snapshot.table_bytes == REUSED_SIZE + NEW_SIZE,
                      "moving realloc accounting diverged"));

tracking_out:
    peak_malloc_test_end();
out:
    free(old_pointer);
    free(new_pointer);
    return result;
}

static int
check_single_entry(void* pointer, size_t size, const char* message)
{
    PeakMallocTestTrackingSnapshot snapshot;

    peak_malloc_test_tracking_snapshot(pointer, &snapshot);
    return expect(snapshot.entry_count == 1 && snapshot.pointer_tracked &&
                  snapshot.pointer_size == size &&
                  snapshot.current_bytes == size && snapshot.table_bytes == size,
                  message);
}

static int
run_realloc_matrix(void)
{
    PeakMallocTestTrackingSnapshot snapshot;
    void* old_pointer = malloc(NEW_SIZE);
    void* new_pointer = malloc(NEW_SIZE);
    void* result;
    int passed = 1;

    if (!expect(old_pointer != NULL && new_pointer != NULL,
                "could not allocate realloc matrix fixture")) goto out;

    passed &= begin_tracking(ALLOCATOR_REALLOC_FAILURE, old_pointer, NULL);
    passed &= expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                     "could not seed failed realloc");
    result = peak_malloc_test_realloc(old_pointer, NEW_SIZE);
    passed &= expect(result == NULL, "failed realloc unexpectedly succeeded");
    passed &= check_single_entry(old_pointer, OLD_SIZE,
                                 "failed realloc did not restore accounting");
    peak_malloc_test_end();

    passed &= begin_tracking(ALLOCATOR_REALLOC_IN_PLACE, old_pointer, NULL);
    passed &= expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                     "could not seed in-place realloc");
    result = peak_malloc_test_realloc(old_pointer, NEW_SIZE);
    passed &= expect(result == old_pointer, "in-place realloc moved");
    passed &= check_single_entry(old_pointer, NEW_SIZE,
                                 "in-place realloc accounting is wrong");
    peak_malloc_test_end();

    passed &= begin_tracking(ALLOCATOR_REALLOC_ZERO_FREE, old_pointer, NULL);
    passed &= expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                     "could not seed zero-size realloc");
    errno = E2BIG;
    result = peak_malloc_test_realloc(old_pointer, 0);
    peak_malloc_test_tracking_snapshot(old_pointer, &snapshot);
    passed &= expect(result == NULL && errno == E2BIG,
                     "zero-size realloc did not preserve allocator result or errno");
    passed &= expect(snapshot.entry_count == 0 && snapshot.current_bytes == 0 &&
                     snapshot.table_bytes == 0,
                     "zero-size freeing realloc remained tracked");
    passed &= expect(allocator_state.realloc_calls == 1,
                     "zero-size realloc bypassed the real realloc");
    peak_malloc_test_end();

    passed &= begin_tracking(ALLOCATOR_REALLOC_ZERO_ALLOCATE,
                             old_pointer, new_pointer);
    passed &= expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                     "could not seed allocating zero-size realloc");
    result = peak_malloc_test_realloc(old_pointer, 0);
    passed &= expect(result == new_pointer,
                     "zero-size allocating realloc result changed");
    passed &= check_single_entry(new_pointer, 0,
                                 "zero-size allocation was not tracked");
    peak_malloc_test_end();

    passed &= begin_tracking(ALLOCATOR_REALLOC_ZERO_FAILURE, old_pointer, NULL);
    passed &= expect(peak_malloc_test_seed(old_pointer, OLD_SIZE),
                     "could not seed failed zero-size realloc");
    result = peak_malloc_test_realloc(old_pointer, 0);
    passed &= expect(result == NULL && errno == ENOMEM,
                     "zero-size realloc failure changed errno");
    passed &= check_single_entry(old_pointer, OLD_SIZE,
                                 "zero-size realloc failure lost the old lifetime");
    peak_malloc_test_end();

    passed &= begin_tracking(ALLOCATOR_REALLOC_UNTRACKED,
                             old_pointer, new_pointer);
    result = peak_malloc_test_realloc(old_pointer, NEW_SIZE);
    passed &= expect(result == new_pointer,
                     "untracked-pointer realloc result changed");
    passed &= check_single_entry(new_pointer, NEW_SIZE,
                                 "untracked-pointer realloc was not added");
    peak_malloc_test_end();

    passed &= begin_tracking(ALLOCATOR_REALLOC_NULL, NULL, new_pointer);
    result = peak_malloc_test_realloc(NULL, NEW_SIZE);
    passed &= expect(result == new_pointer && allocator_state.realloc_null_calls == 1,
                     "realloc(NULL, size) did not call the real realloc");
    passed &= check_single_entry(new_pointer, NEW_SIZE,
                                 "realloc(NULL, size) was not tracked");
    peak_malloc_test_end();

out:
    free(old_pointer);
    free(new_pointer);
    return !passed;
}

static void*
run_stress_worker(void* data)
{
    StressWorker* worker = data;

    for (unsigned int i = 0; i < STRESS_ITERATIONS; ++i) {
        size_t first_size = 32 + worker->id + (i % 17);
        size_t second_size = first_size + 31;
        void* pointer = peak_malloc_test_malloc(first_size);
        void* resized;

        if (pointer == NULL) {
            atomic_fetch_add_explicit(&worker->failures, 1, memory_order_relaxed);
            continue;
        }
        resized = peak_malloc_test_realloc(pointer, second_size);
        if (resized == NULL) {
            atomic_fetch_add_explicit(&worker->failures, 1, memory_order_relaxed);
            peak_malloc_test_free(pointer);
            continue;
        }
        peak_malloc_test_free(resized);
    }
    return NULL;
}

static int
run_tracking_stress(void)
{
    const size_t expected_events =
        1 + STRESS_THREADS * STRESS_ITERATIONS * 4;
    PeakMallocTestTrackingSnapshot tracking;
    PeakMemLogTestSnapshot memlog;
    StressWorker workers[STRESS_THREADS];
    pthread_t threads[STRESS_THREADS];
    int64_t event_delta = 0;
    int passed = 1;

    if (!begin_tracking(ALLOCATOR_LIBC, NULL, NULL)) return 1;
    if (!expect(peak_memlog_test_open(expected_events),
                "could not open stress event log")) {
        peak_malloc_test_end();
        return 1;
    }
    for (unsigned int i = 0; i < STRESS_THREADS; ++i) {
        atomic_init(&workers[i].failures, 0);
        workers[i].id = i;
        if (pthread_create(&threads[i], NULL, run_stress_worker, &workers[i]) != 0) {
            return 1;
        }
    }
    for (unsigned int i = 0; i < STRESS_THREADS; ++i) {
        pthread_join(threads[i], NULL);
        passed &= expect(atomic_load_explicit(&workers[i].failures,
                                             memory_order_relaxed) == 0,
                         "stress allocator operation failed");
    }
    peak_malloc_test_tracking_snapshot(NULL, &tracking);
    peak_memlog_test_snapshot(&memlog);
    passed &= expect(tracking.entry_count == 0 && tracking.current_bytes == 0 &&
                     tracking.table_bytes == 0,
                     "stress tracking table did not return to zero");
    passed &= expect(memlog.committed == expected_events && memlog.dropped == 0,
                     "stress event log lost committed transitions");
    for (size_t i = 0; i < memlog.committed; ++i) {
        PeakMemEvent event;
        if (!peak_memlog_test_read_event(i, &event)) {
            passed = 0;
            break;
        }
        event_delta += event.delta;
    }
    passed &= expect(event_delta == 0,
                     "stress event deltas disagree with aggregate accounting");
    peak_memlog_test_finalize();
    peak_malloc_test_end();
    return !passed;
}

int
main(int argc, char** argv)
{
    int result;

    if (argc != 2) return 2;
    gum_init_embedded();
    if (strcmp(argv[1], "free-reuse") == 0) {
        result = run_free_reuse();
    } else if (strcmp(argv[1], "realloc-reuse") == 0) {
        result = run_realloc_reuse();
    } else if (strcmp(argv[1], "realloc-matrix") == 0) {
        result = run_realloc_matrix();
    } else if (strcmp(argv[1], "stress") == 0) {
        result = run_tracking_stress();
    } else {
        result = 2;
    }
    gum_deinit_embedded();
    return result;
}
