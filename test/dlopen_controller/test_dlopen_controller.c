#include "dlopen_interceptor.h"
#include "detach_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static PeakDlopenDynamicAttachDiagnostics
get_diagnostics(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    memset(&diagnostics, 0, sizeof(diagnostics));
    dlopen_interceptor_get_dynamic_attach_diagnostics(&diagnostics);
    return diagnostics;
}

static void
check_true(const char* label, int condition)
{
    if (!condition) {
        fprintf(stderr, "not ok - %s\n", label);
        failures++;
    }
}

static void
check_ull(const char* label,
          unsigned long long actual,
          unsigned long long expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "not ok - %s: expected %llu, got %llu\n",
                label,
                expected,
                actual);
        failures++;
    }
}

static void
check_size(const char* label, size_t actual, size_t expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "not ok - %s: expected %zu, got %zu\n",
                label,
                expected,
                actual);
        failures++;
    }
}

static void
reset_dynamic_attach_manual(gboolean open)
{
    dlopen_interceptor_test_reset_dynamic_attach(open);
    dlopen_interceptor_test_set_manual_drain(TRUE);
}

static void
restore_dynamic_attach_automatic(void)
{
    dlopen_interceptor_test_set_manual_drain(FALSE);
    dlopen_interceptor_test_reset_dynamic_attach(FALSE);
}

static void
test_queue_is_unbounded(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;
    const unsigned int request_count = 512;

    reset_dynamic_attach_manual(TRUE);
    diagnostics = get_diagnostics();
    check_size("zero capacity denotes unbounded queue", diagnostics.capacity, 0);
    check_true("drain budget is fixed and nonzero", diagnostics.drain_budget > 0);

    for (unsigned int i = 0; i < request_count; i++) {
        char filename[64];

        snprintf(filename, sizeof(filename), "capacity-%u", i);
        check_true("unbounded queue accepts request",
                   dlopen_interceptor_test_enqueue_dummy_dynamic_attach(filename));
    }

    diagnostics = get_diagnostics();
    check_ull("unbounded enqueued count", diagnostics.enqueued, request_count);
    check_size("unbounded queue length", diagnostics.queue_length, request_count);
    check_size("unbounded max depth", diagnostics.max_depth, request_count);
    check_ull("unbounded queue has no full drops", diagnostics.dropped_full, 0);
}

static void
test_closed_queue_drop(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    reset_dynamic_attach_manual(FALSE);
    check_true("closed queue rejects enqueue",
               !dlopen_interceptor_test_enqueue_dummy_dynamic_attach("closed"));

    diagnostics = get_diagnostics();
    check_ull("closed queue enqueued count", diagnostics.enqueued, 0);
    check_size("closed queue length", diagnostics.queue_length, 0);
    check_ull("closed queue drop count", diagnostics.dropped_closed, 1);
}

static void
test_bounded_drain_budget(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;
    size_t request_count;

    reset_dynamic_attach_manual(TRUE);
    diagnostics = get_diagnostics();
    request_count = (size_t)diagnostics.drain_budget + 2;

    for (size_t i = 0; i < request_count; i++) {
        char filename[64];

        snprintf(filename, sizeof(filename), "budget-%zu", i);
        check_true("budget test enqueue",
                   dlopen_interceptor_test_enqueue_dummy_dynamic_attach(filename));
    }

    dlopen_interceptor_test_drain_dynamic_attach_queue();

    diagnostics = get_diagnostics();
    check_ull("bounded drain count",
              diagnostics.drained,
              diagnostics.drain_budget);
    check_size("bounded drain leaves remaining queued",
               diagnostics.queue_length,
               request_count - diagnostics.drain_budget);
}

static void
test_retry_requeues_once_per_drain_cycle(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    reset_dynamic_attach_manual(TRUE);
    check_true("retry test enqueue",
               dlopen_interceptor_test_enqueue_retry_dynamic_attach("retry-once"));

    dlopen_interceptor_test_drain_dynamic_attach_queue();
    diagnostics = get_diagnostics();
    check_ull("retry first drain count", diagnostics.drained, 1);
    check_ull("retry first requeue count", diagnostics.requeued, 1);
    check_size("retry remains queued after first drain",
               diagnostics.queue_length,
               1);

    dlopen_interceptor_test_drain_dynamic_attach_queue();
    diagnostics = get_diagnostics();
    check_ull("retry second drain count", diagnostics.drained, 2);
    check_ull("retry second requeue count", diagnostics.requeued, 2);
    check_size("retry remains queued after second drain",
               diagnostics.queue_length,
               1);
}

static void
test_retry_does_not_block_later_same_cycle_work(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    reset_dynamic_attach_manual(TRUE);
    check_true("retry-first enqueue retry",
               dlopen_interceptor_test_enqueue_retry_dynamic_attach(
                   "retry-first"));
    check_true("retry-first enqueue later dummy",
               dlopen_interceptor_test_enqueue_dummy_dynamic_attach(
                   "later-dummy"));

    dlopen_interceptor_test_drain_dynamic_attach_queue();
    diagnostics = get_diagnostics();
    check_ull("retry-first drained retry and later dummy",
              diagnostics.drained,
              2);
    check_ull("retry-first requeued once", diagnostics.requeued, 1);
    check_size("retry-first leaves only retry queued",
               diagnostics.queue_length,
               1);
}

static void
test_manual_drain_restore_allows_normal_drain(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    reset_dynamic_attach_manual(TRUE);
    check_true("manual restore enqueue",
               dlopen_interceptor_test_enqueue_dummy_dynamic_attach(
                   "manual-restore"));
    dlopen_interceptor_test_normal_drain_dynamic_attach_queue();
    diagnostics = get_diagnostics();
    check_size("manual mode skips normal drain", diagnostics.queue_length, 1);
    check_ull("manual mode skipped drain count", diagnostics.drained, 0);

    dlopen_interceptor_test_set_manual_drain(FALSE);
    dlopen_interceptor_test_normal_drain_dynamic_attach_queue();
    diagnostics = get_diagnostics();
    check_size("manual mode restore permits normal drain",
               diagnostics.queue_length,
               0);
    check_ull("manual mode restore drain count", diagnostics.drained, 1);
}

static void
test_extended_drop_and_retained_handle_diagnostics(void)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    reset_dynamic_attach_manual(TRUE);
    check_size("retained handle slots start empty",
               dlopen_interceptor_test_retained_handle_slots(),
               0);

    dlopen_interceptor_test_record_noload_drop();
    dlopen_interceptor_test_record_requeue_drop();
    dlopen_interceptor_test_record_partial_success_with_retained_handle();

    diagnostics = get_diagnostics();
    check_ull("noload drop count", diagnostics.dropped_noload, 1);
    check_ull("requeue drop count", diagnostics.dropped_requeue, 1);
    check_ull("partial success count", diagnostics.partial_success, 1);
    check_ull("retained handle count is cumulative",
              diagnostics.retained_handles,
              1);
    check_size("retained handle slot exists before release",
               dlopen_interceptor_test_retained_handle_slots(),
               1);

    dlopen_interceptor_test_release_retained_dynamic_handles();
    check_size("retained handle slots released",
               dlopen_interceptor_test_retained_handle_slots(),
               0);
    diagnostics = get_diagnostics();
    check_ull("retained handle count remains cumulative",
              diagnostics.retained_handles,
              1);
}

static void
test_trace_contains_queue_shape(void)
{
    const char* trace_path = getenv("PEAK_DLOPEN_TRACE_PATH");
    char line[512];
    char event[64];
    unsigned long long enqueued;
    unsigned long long drained;
    unsigned long long requeued;
    unsigned long long dropped_full;
    unsigned long long dropped_closed;
    unsigned long long dropped_noload;
    unsigned long long dropped_requeue;
    unsigned long long partial_success;
    unsigned long long retained_handles;
    unsigned long max_depth;
    unsigned long queue_length;
    unsigned int capacity;
    unsigned int drain_budget;
    int matched;
    FILE* fp;
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    check_true("trace path is configured",
               trace_path != NULL && trace_path[0] != '\0');
    if (trace_path == NULL || trace_path[0] == '\0') {
        return;
    }

    remove(trace_path);
    reset_dynamic_attach_manual(TRUE);
    check_true("trace enqueue 0",
               dlopen_interceptor_test_enqueue_dummy_dynamic_attach("trace-0"));
    check_true("trace enqueue 1",
               dlopen_interceptor_test_enqueue_dummy_dynamic_attach("trace-1"));
    check_true("trace enqueue 2",
               dlopen_interceptor_test_enqueue_dummy_dynamic_attach("trace-2"));
    check_true("trace retry enqueue",
               dlopen_interceptor_test_enqueue_retry_dynamic_attach(
                   "trace-retry"));
    dlopen_interceptor_test_drain_dynamic_attach_queue();
    dlopen_interceptor_test_record_noload_drop();
    dlopen_interceptor_test_record_requeue_drop();
    dlopen_interceptor_test_record_partial_success_with_retained_handle();

    diagnostics = get_diagnostics();
    dlopen_interceptor_test_trace_counters("trace-test");

    fp = fopen(trace_path, "r");
    check_true("trace file opened", fp != NULL);
    if (fp == NULL) {
        return;
    }

    check_true("trace row read", fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);

    matched = sscanf(line,
                     "%63[^,],%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%u,%u",
                     event,
                     &enqueued,
                     &drained,
                     &requeued,
                     &dropped_full,
                     &dropped_closed,
                     &dropped_noload,
                     &dropped_requeue,
                     &partial_success,
                     &retained_handles,
                     &max_depth,
                     &queue_length,
                     &capacity,
                     &drain_budget);
    check_true("trace row has expanded diagnostics", matched == 14);
    check_true("trace event", strcmp(event, "trace-test") == 0);
    check_ull("trace enqueued", enqueued, diagnostics.enqueued);
    check_ull("trace drained", drained, diagnostics.drained);
    check_ull("trace requeued", requeued, diagnostics.requeued);
    check_ull("trace dropped full", dropped_full, diagnostics.dropped_full);
    check_ull("trace dropped closed", dropped_closed, diagnostics.dropped_closed);
    check_ull("trace dropped noload",
              dropped_noload,
              diagnostics.dropped_noload);
    check_ull("trace dropped requeue",
              dropped_requeue,
              diagnostics.dropped_requeue);
    check_ull("trace partial success",
              partial_success,
              diagnostics.partial_success);
    check_ull("trace retained handles",
              retained_handles,
              diagnostics.retained_handles);
    check_size("trace max depth", (size_t)max_depth, diagnostics.max_depth);
    check_size("trace queue length",
               (size_t)queue_length,
               diagnostics.queue_length);
    check_true("trace capacity", capacity == diagnostics.capacity);
    check_true("trace drain budget", drain_budget == diagnostics.drain_budget);
    check_true("trace requeued/retried is nonzero", requeued > 0);
    check_true("trace dropped noload is nonzero", dropped_noload > 0);
    check_true("trace dropped requeue is nonzero", dropped_requeue > 0);
    check_true("trace partial success is nonzero", partial_success > 0);
    check_true("trace retained handles is nonzero", retained_handles > 0);
}

static void
test_retryable_prepare_statuses(void)
{
    check_true("timeout is retryable",
               dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_TIMEOUT));
    check_true("classify-failed is retryable",
               dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_CLASSIFY_FAILED));
    check_true("error is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_ERROR));
    check_true("permission-denied is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_PERMISSION_DENIED));

    check_true("safe is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_SAFE));
    check_true("compatibility-allowed is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED));
    check_true("disabled is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_DISABLED));
    check_true("unsupported is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_UNSUPPORTED));
    check_true("missing-gum-api is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(
                   PEAK_DETACH_STATUS_MISSING_GUM_API));
    check_true("unknown status is terminal",
               !dlopen_interceptor_test_retryable_prepare_status(999));
}

int
main(void)
{
    gum_init_embedded();

    test_queue_is_unbounded();
    test_closed_queue_drop();
    test_bounded_drain_budget();
    test_retry_requeues_once_per_drain_cycle();
    test_retry_does_not_block_later_same_cycle_work();
    test_manual_drain_restore_allows_normal_drain();
    test_extended_drop_and_retained_handle_diagnostics();
    test_trace_contains_queue_shape();
    test_retryable_prepare_statuses();
    restore_dynamic_attach_automatic();

    int result = failures == 0 ? 0 : 1;
    gum_deinit_embedded();

    if (failures != 0) {
        return result;
    }

    printf("dlopen_controller_diagnostics_ok\n");
    return result;
}
