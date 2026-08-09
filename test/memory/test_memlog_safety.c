#define PEAK_ENABLE_TEST_HOOKS 1
#include "malloc_interceptor.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STRESS_THREADS 8
#define STRESS_EVENTS_PER_THREAD 128

static int expect(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "memlog safety test: %s\n", message);
        return 0;
    }
    return 1;
}

static void* write_one_event(void* unused)
{
    (void) unused;
    peak_memlog_test_log_event(2, 1, 1, 1);
    return NULL;
}

static void* finalize_log(void* unused)
{
    (void) unused;
    peak_memlog_test_finalize();
    return NULL;
}

typedef struct {
    unsigned int id;
} StressWriter;

static void* write_stress_events(void* data)
{
    StressWriter* writer = data;
    for (unsigned int i = 0; i < STRESS_EVENTS_PER_THREAD; ++i) {
        int64_t value = (int64_t) writer->id * STRESS_EVENTS_PER_THREAD + i + 1;
        peak_memlog_test_log_event((uint64_t) value, value, (uint64_t) value, 1);
    }
    return NULL;
}

static int run_failure(PeakMemLogTestFailure failure)
{
    PeakMemLogTestSnapshot snapshot;

    peak_memlog_test_set_failure(failure);
    if (!expect(!peak_memlog_test_open(4), "faulted creation unexpectedly succeeded")) return 1;
    peak_memlog_test_snapshot(&snapshot);
    if (!expect(snapshot.state == PEAK_MEMLOG_DISABLED && !snapshot.mapping_live,
                "faulted creation left a live mapping")) return 1;
    peak_memlog_test_finalize();
    peak_memlog_test_snapshot(&snapshot);
    return !expect(snapshot.state == PEAK_MEMLOG_FINALIZED,
                   "disabled log did not finalize");
}

static int run_capacity(void)
{
    PeakMemLogTestSnapshot snapshot;

    if (!expect(peak_memlog_test_open(2), "fixed-capacity log did not open")) return 1;
    for (unsigned int i = 0; i < 10; ++i) {
        peak_memlog_test_log_event(10 + i, 1, i + 1, 1);
    }
    peak_memlog_test_snapshot(&snapshot);
    if (!expect(snapshot.capacity == 2 && snapshot.reserved == 2 &&
                snapshot.committed == 2 && snapshot.dropped == 9,
                "capacity exhaustion did not drop without over-reserving")) return 1;
    peak_memlog_test_finalize();
    return 0;
}

static int run_overflow(void)
{
    PeakMemLogTestSnapshot snapshot;

    if (!expect(!peak_memlog_test_open(SIZE_MAX),
                "overflow-sized fixed storage unexpectedly opened")) return 1;
    peak_memlog_test_snapshot(&snapshot);
    return !expect(snapshot.state == PEAK_MEMLOG_DISABLED && !snapshot.mapping_live,
                   "overflow-sized storage left a live mapping");
}

static int run_stalled_writer(void)
{
    PeakMemLogTestSnapshot snapshot;
    pthread_t writer;
    pthread_t finalizer;

    if (!expect(peak_memlog_test_open(4), "stalled-writer log did not open")) return 1;
    peak_memlog_test_pause_before_commit(1);
    if (pthread_create(&writer, NULL, write_one_event, NULL) != 0) return 1;
    for (unsigned int i = 0; i < 10000 && !peak_memlog_test_writer_is_paused(); ++i) usleep(100);
    if (!expect(peak_memlog_test_writer_is_paused(), "writer did not stall before publish")) return 1;

    peak_memlog_test_snapshot(&snapshot);
    if (!expect(snapshot.reserved == 2 && snapshot.committed == 1 &&
                peak_memlog_test_ready_records() == 1,
                "ready scan accepted an uncommitted reservation")) return 1;

    if (pthread_create(&finalizer, NULL, finalize_log, NULL) != 0) return 1;
    for (unsigned int i = 0; i < 10000; ++i) {
        peak_memlog_test_snapshot(&snapshot);
        if (snapshot.state == PEAK_MEMLOG_DISABLED) break;
        usleep(100);
    }
    peak_memlog_test_snapshot(&snapshot);
    if (!expect(snapshot.state == PEAK_MEMLOG_DISABLED && snapshot.mapping_live,
                "finalizer unmapped storage while a writer still held it")) return 1;

    peak_memlog_test_pause_before_commit(0);
    pthread_join(writer, NULL);
    pthread_join(finalizer, NULL);
    peak_memlog_test_snapshot(&snapshot);
    return !expect(snapshot.state == PEAK_MEMLOG_FINALIZED && !snapshot.mapping_live,
                   "finalizer did not complete after writer publication");
}

static int run_multithread_stress(void)
{
    const size_t event_count = STRESS_THREADS * STRESS_EVENTS_PER_THREAD;
    PeakMemLogTestSnapshot snapshot;
    pthread_t threads[STRESS_THREADS];
    StressWriter writers[STRESS_THREADS];
    unsigned char seen[STRESS_THREADS * STRESS_EVENTS_PER_THREAD] = {0};

    if (!expect(peak_memlog_test_open(event_count + 1), "stress log did not open")) return 1;
    for (unsigned int i = 0; i < STRESS_THREADS; ++i) {
        writers[i].id = i;
        if (pthread_create(&threads[i], NULL, write_stress_events, &writers[i]) != 0) return 1;
    }
    for (unsigned int i = 0; i < STRESS_THREADS; ++i) pthread_join(threads[i], NULL);

    peak_memlog_test_snapshot(&snapshot);
    if (!expect(snapshot.reserved == event_count + 1 &&
                snapshot.committed == event_count + 1 && snapshot.dropped == 0 &&
                peak_memlog_test_ready_records() == event_count + 1,
                "multi-producer records were not fully committed")) return 1;

    for (size_t i = 1; i <= event_count; ++i) {
        PeakMemEvent event;
        if (!expect(peak_memlog_test_read_event(i, &event), "committed record was unreadable")) return 1;
        if (!expect(event.delta > 0 && (uint64_t) event.delta <= event_count &&
                    !seen[event.delta - 1], "duplicate or corrupt concurrent record")) return 1;
        seen[event.delta - 1] = 1;
    }
    for (size_t i = 0; i < event_count; ++i) {
        if (!expect(seen[i], "a concurrent producer record was lost")) return 1;
    }

    peak_memlog_test_log_event(UINT64_MAX, 1, 1, 1);
    peak_memlog_test_log_event(UINT64_MAX, 1, 1, 1);
    peak_memlog_test_snapshot(&snapshot);
    if (!expect(snapshot.dropped == 2 && snapshot.reserved == event_count + 1,
                "fixed capacity did not count dropped concurrent events")) return 1;
    peak_memlog_test_finalize();
    peak_memlog_test_snapshot(&snapshot);
    return !expect(snapshot.state == PEAK_MEMLOG_FINALIZED && !snapshot.mapping_live &&
                   snapshot.exported == event_count + 1,
                   "export did not use only the fully committed records");
}

static int run_output_no_clobber(void)
{
    char path[256];
    FILE* file;
    char contents[32] = {0};

    if (snprintf(path, sizeof(path), "/tmp/peak-memlog-no-clobber-%ld.csv",
                 (long)getpid()) >= (int)sizeof(path)) return 1;
    file = fopen(path, "w");
    if (!expect(file != NULL, "could not create preserved CSV")) return 1;
    if (!expect(fputs("preserved\n", file) >= 0, "could not seed preserved CSV") ||
        !expect(fclose(file) == 0, "could not close preserved CSV")) return 1;
    if (!expect(setenv("PEAK_MEMLOG_TEMPLATE", path, 1) == 0,
                "could not set memlog template")) return 1;
    if (!expect(peak_memlog_test_open(4), "memlog did not open")) return 1;
    peak_memlog_test_log_event(2, 1, 1, 1);
    peak_memlog_test_finalize();
    file = fopen(path, "r");
    if (!expect(file != NULL, "preserved CSV disappeared")) return 1;
    if (!expect(fgets(contents, sizeof(contents), file) != NULL &&
                strcmp(contents, "preserved\n") == 0,
                "memlog replaced an existing CSV")) {
        fclose(file);
        return 1;
    }
    fclose(file);
    unlink(path);
    unsetenv("PEAK_MEMLOG_TEMPLATE");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "create") == 0) return run_failure(PEAK_MEMLOG_TEST_FAIL_CREATE);
    if (strcmp(argv[1], "sizing") == 0) return run_failure(PEAK_MEMLOG_TEST_FAIL_SIZING);
    if (strcmp(argv[1], "mapping") == 0) return run_failure(PEAK_MEMLOG_TEST_FAIL_MAPPING);
    if (strcmp(argv[1], "overflow") == 0) return run_overflow();
    if (strcmp(argv[1], "capacity") == 0) return run_capacity();
    if (strcmp(argv[1], "stalled") == 0) return run_stalled_writer();
    if (strcmp(argv[1], "stress") == 0) return run_multithread_stress();
    if (strcmp(argv[1], "no-clobber") == 0) return run_output_no_clobber();
    if (strcmp(argv[1], "realloc") == 0) {
        gum_init_embedded();
        int result = !peak_malloc_test_failed_realloc_preserves_accounting();
        gum_deinit_embedded();
        return result;
    }
    if (strcmp(argv[1], "allocation") == 0) {
        gum_init_embedded();
        int result = !peak_malloc_test_tracking_allocation_failure();
        gum_deinit_embedded();
        return result;
    }
    return 2;
}
