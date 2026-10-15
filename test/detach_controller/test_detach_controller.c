#define _GNU_SOURCE
#include "detach_controller.h"
#include "internal/filesystem_stat_guard.h"
#include <sys/vfs.h>
#include "detach_helper_protocol.h"
#include "internal/signal_policy_internal.h"
#include "internal/unsafe_gum_prologue.h"
#include "signal_policy.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

static int failures = 0;

/* An application query must be admitted after recoverable helper cleanup.
 * Reopen only on test failure so the worker can always be joined. */
static atomic_int cleanup_query_returned;
static int cleanup_query_result;
static void* cleanup_query_worker(void* unused)
{
    struct statfs info;
    (void)unused;
    cleanup_query_result = statfs("/tmp", &info);
    atomic_store(&cleanup_query_returned, 1);
    return NULL;
}
static void check_cleanup_query(void)
{
    pthread_t worker;
    atomic_store(&cleanup_query_returned, 0);
    if (pthread_create(&worker, NULL, cleanup_query_worker, NULL) != 0) {
        fprintf(stderr, "cleanup query thread creation failed\n");
        failures++;
        return;
    }
    for (unsigned i = 0; i < 200 && !atomic_load(&cleanup_query_returned); i++)
        usleep(1000);
    if (!atomic_load(&cleanup_query_returned)) {
        fprintf(stderr, "recoverable EVACUATE left filesystem query blocked\n");
        failures++;
        peak_filesystem_stat_guard_resume();
    }
    pthread_join(worker, NULL);
    if (cleanup_query_result != 0) {
        fprintf(stderr, "cleanup filesystem query failed: %d\n",
                cleanup_query_result);
        failures++;
    }
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
typedef struct {
    int expected_sequence;
    atomic_int started;
    atomic_int returned;
} SignalEventWaitContext;

static void*
signal_event_wait_worker(void* argument)
{
    SignalEventWaitContext* context = argument;

    atomic_store_explicit(&context->started, 1, memory_order_release);
    peak_detach_controller_test_wait_for_signal_event(
        context->expected_sequence);
    atomic_store_explicit(&context->returned, 1, memory_order_release);
    return NULL;
}
#endif

static void
check_true(const char* label, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static void
check_int(const char* label, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "FAIL: %s: expected %d, got %d\n",
                label,
                expected,
                actual);
        failures++;
    }
}

static void
check_uint64(const char* label, uint64_t actual, uint64_t expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "FAIL: %s: expected %" PRIu64 ", got %" PRIu64 "\n",
                label,
                expected,
                actual);
        failures++;
    }
}

static void
check_double_zero(const char* label, double actual)
{
    if (actual != 0.0) {
        fprintf(stderr,
                "FAIL: %s: expected 0.0, got %.9f\n",
                label,
                actual);
        failures++;
    }
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static gboolean
check_accounting_snapshot(const char* label,
                          PeakDetachAccountingSnapshot* snapshot)
{
    gboolean ok = peak_detach_controller_accounting_snapshot(snapshot);

    if (!ok) {
        fprintf(stderr, "FAIL: %s snapshot returned FALSE\n", label);
        failures++;
    }
    return ok;
}

static void
check_stop_window_finish(const char* label,
                         PeakDetachAccountingSnapshot* before)
{
    PeakDetachAccountingSnapshot after;
    uint64_t wall_ns = 0;
    double expected_us = 0.0;
    double actual_us = peak_detach_controller_last_stop_window_us();

    if (!check_accounting_snapshot(label, &after)) {
        return;
    }
    if (after.stop_window_wall_ns > before->stop_window_wall_ns) {
        wall_ns = after.stop_window_wall_ns - before->stop_window_wall_ns;
        expected_us = (double)wall_ns / 1000.0;
    }

    if (after.completed_stop_window_count !=
            before->completed_stop_window_count + 1 ||
        after.failed_stop_window_count != before->failed_stop_window_count ||
        wall_ns == 0 || actual_us <= 0.0 || actual_us != expected_us) {
        fprintf(stderr,
                "FAIL: %s: before=(completed=%" PRIu64
                ", failed=%" PRIu64 ", %" PRIu64
                " ns), after=(completed=%" PRIu64
                ", failed=%" PRIu64 ", %" PRIu64
                " ns), last=%.9f us, expected=%.9f us\n",
                label,
                before->completed_stop_window_count,
                before->failed_stop_window_count,
                before->stop_window_wall_ns,
                after.completed_stop_window_count,
                after.failed_stop_window_count,
                after.stop_window_wall_ns,
                actual_us,
                expected_us);
        failures++;
    }

    *before = after;
}

static void
check_stop_window_failure(const char* label,
                          PeakDetachAccountingSnapshot* before,
                          uint64_t min_wall_delta_ns)
{
    PeakDetachAccountingSnapshot after;
    uint64_t wall_ns = 0;

    if (!check_accounting_snapshot(label, &after)) {
        return;
    }
    if (after.stop_window_wall_ns >= before->stop_window_wall_ns) {
        wall_ns = after.stop_window_wall_ns - before->stop_window_wall_ns;
    }

    if (after.completed_stop_window_count !=
            before->completed_stop_window_count ||
        after.failed_stop_window_count != before->failed_stop_window_count + 1 ||
        wall_ns < min_wall_delta_ns) {
        fprintf(stderr,
                "FAIL: %s: before=(completed=%" PRIu64
                ", failed=%" PRIu64 ", %" PRIu64
                " ns), after=(completed=%" PRIu64
                ", failed=%" PRIu64 ", %" PRIu64
                " ns), wall_delta=%" PRIu64
                " ns, min=%" PRIu64 " ns\n",
                label,
                before->completed_stop_window_count,
                before->failed_stop_window_count,
                before->stop_window_wall_ns,
                after.completed_stop_window_count,
                after.failed_stop_window_count,
                after.stop_window_wall_ns,
                wall_ns,
                min_wall_delta_ns);
        failures++;
    }

    *before = after;
}
#endif

static void
check_status(const char* label,
             PeakDetachStatus actual,
             PeakDetachStatus expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "FAIL: %s: expected %s, got %s\n",
                label,
                peak_detach_controller_status_string(expected),
                peak_detach_controller_status_string(actual));
        failures++;
    }
}

static void
check_string(const char* label, const char* actual, const char* expected)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr,
                "FAIL: %s: expected '%s', got '%s'\n",
                label,
                expected,
                actual);
        failures++;
    }
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static int
count_proc_threads(void)
{
    DIR* dir = opendir("/proc/self/task");
    int count = 0;

    if (dir == NULL) {
        return -1;
    }

    for (;;) {
        struct dirent* entry;
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            count++;
        }
    }

    if (errno != 0) {
        count = -1;
    }
    closedir(dir);
    return count;
}
#endif

static PeakDetachRequest
valid_request(PeakDetachOperation operation)
{
    PeakDetachRequest request = {
        .hook_id = 7,
        .symbol_name = "test_symbol",
        .function_address = (gpointer)0x1,
        .interceptor = (GumInterceptor*)0x2,
        .listener = (operation == PEAK_DETACH_OPERATION_REPLACE ||
                     operation == PEAK_DETACH_OPERATION_REVERT)
                        ? NULL
                        : (GumInvocationListener*)0x3,
        .operation = operation
    };
    return request;
}

static PeakDetachStatus
expected_unavailable_strict_status(PeakDetachOperation operation)
{
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    (void)operation;
    return PEAK_DETACH_STATUS_UNSUPPORTED;
#else
    (void)operation;
    return PEAK_DETACH_STATUS_MISSING_GUM_API;
#endif
}

static void
check_prepare(const char* label,
              const PeakDetachRequest* request,
              gboolean expected_prepared,
              PeakDetachStatus expected_status)
{
    PeakDetachStatus status = PEAK_DETACH_STATUS_SAFE;
    gboolean prepared =
        peak_detach_controller_prepare_hook_mutation(request, &status);

    check_true(label, prepared == expected_prepared);
    if (status != expected_status) {
        const PeakDetachFailureDetail* detail =
            peak_detach_controller_last_failure_detail();
        fprintf(stderr,
                "FAIL: %s status: expected %s, got %s (reason=%s tid=%ld pc=0x%llx aux=0x%llx)\n",
                label,
                peak_detach_controller_status_string(expected_status),
                peak_detach_controller_status_string(status),
                detail != NULL && detail->reason != NULL ? detail->reason : "<none>",
                detail != NULL ? (long)detail->tid : 0L,
                (unsigned long long)(detail != NULL ? detail->pc : 0),
                (unsigned long long)(detail != NULL ? detail->aux : 0));
        failures++;
    }
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static void
check_finish(const char* label,
             const PeakDetachRequest* request,
             PeakDetachStatus expected_status)
{
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    gboolean finished =
        peak_detach_controller_finish_hook_mutation(request, &status);

    check_true(label, finished == TRUE);
    if (status != expected_status) {
        fprintf(stderr,
                "FAIL: %s finish status: expected %s, got %s\n",
                label,
                peak_detach_controller_status_string(expected_status),
                peak_detach_controller_status_string(status));
        failures++;
    }
}
#endif

static void
check_string_tables(void)
{
    check_string("operation attach",
                 peak_detach_controller_operation_string(PEAK_DETACH_OPERATION_ATTACH),
                 "attach");
    check_string("operation detach",
                 peak_detach_controller_operation_string(PEAK_DETACH_OPERATION_DETACH),
                 "detach");
    check_string("operation reattach",
                 peak_detach_controller_operation_string(PEAK_DETACH_OPERATION_REATTACH),
                 "reattach");
    check_string("operation shutdown",
                 peak_detach_controller_operation_string(PEAK_DETACH_OPERATION_SHUTDOWN),
                 "shutdown");
    check_string("operation replace",
                 peak_detach_controller_operation_string(PEAK_DETACH_OPERATION_REPLACE),
                 "replace");
    check_string("operation revert",
                 peak_detach_controller_operation_string(PEAK_DETACH_OPERATION_REVERT),
                 "revert");
    check_string("operation unknown",
                 peak_detach_controller_operation_string((PeakDetachOperation)999),
                 "unknown");

    check_string("status safe",
                 peak_detach_controller_status_string(PEAK_DETACH_STATUS_SAFE),
                 "safe");
    check_string("status compatibility",
                 peak_detach_controller_status_string(
                     PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED),
                 "compatibility-allowed");
    check_string("status missing gum api",
                 peak_detach_controller_status_string(
                     PEAK_DETACH_STATUS_MISSING_GUM_API),
                 "missing-gum-api");
    check_string("status unsupported",
                 peak_detach_controller_status_string(PEAK_DETACH_STATUS_UNSUPPORTED),
                 "unsupported");
    check_string("status unknown",
                 peak_detach_controller_status_string((PeakDetachStatus)999),
                 "unknown");
}

static int
run_strict(void)
{
    PeakDetachOperation operations[] = {
        PEAK_DETACH_OPERATION_ATTACH,
        PEAK_DETACH_OPERATION_DETACH,
        PEAK_DETACH_OPERATION_REATTACH,
        PEAK_DETACH_OPERATION_SHUTDOWN,
        PEAK_DETACH_OPERATION_REPLACE,
        PEAK_DETACH_OPERATION_REVERT
    };
    int result;

    gum_init_embedded();
    check_string_tables();
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        PeakDetachRequest request = valid_request(operations[i]);
        PeakDetachStatus expected_status =
            expected_unavailable_strict_status(operations[i]);

        check_prepare(peak_detach_controller_operation_string(operations[i]),
                      &request,
                      FALSE,
                      expected_status);
    }

    result = failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    gum_deinit_embedded();
    return result;
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static volatile int worker_running = 0;
static volatile int signal_blocked_worker_ready = 0;
static pthread_mutex_t stale_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stale_worker_cond = PTHREAD_COND_INITIALIZER;
static int stale_worker_parked = 0;
static int stale_worker_release = 0;
static int stale_worker_calls = 0;
static int stale_worker_warmup_calls = 1024;

__attribute__((noinline, noclone))
static void
strict_helper_target(void)
{
#if defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__)
    asm volatile(
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

__attribute__((noinline, noclone))
static void
strict_helper_target_two(void)
{
#if defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__)
    asm volatile(
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

static void*
strict_helper_worker(void* arg)
{
    (void)arg;
    worker_running = 1;
    while (worker_running) {
        usleep(1000);
    }
    return NULL;
}

static void*
strict_helper_stale_worker(void* arg)
{
    (void)arg;

    for (int i = 0; i < stale_worker_warmup_calls; i++) {
        strict_helper_target();
        stale_worker_calls++;
    }

    pthread_mutex_lock(&stale_worker_mutex);
    stale_worker_parked = 1;
    pthread_cond_broadcast(&stale_worker_cond);
    while (!stale_worker_release) {
        pthread_cond_wait(&stale_worker_cond, &stale_worker_mutex);
    }
    pthread_mutex_unlock(&stale_worker_mutex);
    return NULL;
}

static void*
strict_helper_signal_blocked_worker(void* arg)
{
    (void)arg;
    signal_blocked_worker_ready =
        peak_signal_policy_test_block_reserved_for_current_thread() == 0;

    worker_running = 1;
    while (worker_running) {
        strict_helper_target();
    }
    return NULL;
}

static void
strict_helper_on_enter(GumInvocationContext* context, gpointer user_data)
{
    (void)context;
    (void)user_data;
}

static void
check_pc_state(const char* label,
               GumInterceptor* interceptor,
               GumInvocationListener* listener,
               gpointer pc,
               GumPeakPcState expected_state,
               gpointer expected_safe_pc)
{
    GumPeakFunctionContext* ctx = NULL;
    GumPeakPcState state = GUM_PEAK_PC_UNKNOWN;
    gboolean classified =
        gum_interceptor_peak_classify_pc(interceptor,
                                         (gpointer)strict_helper_target,
                                         listener,
                                         pc,
                                         &ctx,
                                         &state);
    char state_label[128];

    snprintf(state_label, sizeof(state_label), "%s classify", label);
    check_true(state_label, classified == TRUE);
    snprintf(state_label, sizeof(state_label), "%s state", label);
    check_true(state_label, state == expected_state);

    gpointer safe_pc = gum_interceptor_peak_safe_pc(ctx, pc, state);
    snprintf(state_label, sizeof(state_label), "%s safe pc", label);
    check_true(state_label, safe_pc == expected_safe_pc);
}

static void
check_strict_helper_pc_classification(GumInterceptor* interceptor,
                                      GumInvocationListener* listener)
{
    GumPeakPcDiagnostics diagnostics;
    gboolean got_diagnostics =
        gum_interceptor_peak_get_pc_diagnostics(interceptor,
                                                (gpointer)strict_helper_target,
                                                listener,
                                                &diagnostics);

    check_true("diagnostics available", got_diagnostics == TRUE);
    if (!got_diagnostics) {
        return;
    }

    check_true("diagnostics function address",
               diagnostics.function_address == (gpointer)strict_helper_target);
    check_true("diagnostics prologue length",
               diagnostics.overwritten_prologue_len > 0 &&
               diagnostics.overwritten_prologue_len <= GUM_PEAK_MAX_PROLOGUE_SIZE);
    check_true("diagnostics trampoline slice",
               diagnostics.trampoline_slice_start != NULL &&
               diagnostics.trampoline_slice_size > 0);
    check_true("diagnostics enter trampoline",
               diagnostics.on_enter_trampoline != NULL);

    check_pc_state("function entry",
                   interceptor,
                   listener,
                   (gpointer)strict_helper_target,
                   GUM_PEAK_PC_AT_PATCH_ENTRY,
                   NULL);

    check_pc_state("enter trampoline start",
                   interceptor,
                   listener,
                   diagnostics.on_enter_trampoline,
                   GUM_PEAK_PC_IN_ENTER_TRAMPOLINE,
                   (gpointer)strict_helper_target);

    if (diagnostics.on_leave_trampoline != NULL &&
        (uintptr_t)diagnostics.on_leave_trampoline >
            (uintptr_t)diagnostics.on_enter_trampoline) {
        check_pc_state("enter trampoline interior",
                       interceptor,
                       listener,
                       (gpointer)((guint8*)diagnostics.on_enter_trampoline + 1),
                       GUM_PEAK_PC_IN_ENTER_TRAMPOLINE,
                       NULL);

        check_pc_state("leave trampoline start",
                       interceptor,
                       listener,
                       diagnostics.on_leave_trampoline,
                       GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE,
                       NULL);
    }

    if (diagnostics.on_invoke_trampoline != NULL) {
        check_pc_state("invoke trampoline start",
                       interceptor,
                       listener,
                       diagnostics.on_invoke_trampoline,
                       GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE,
                       NULL);
    }

    if (diagnostics.enter_thunk_start != NULL &&
        diagnostics.enter_thunk_size > 0) {
        check_pc_state("shared enter thunk",
                       interceptor,
                       listener,
                       diagnostics.enter_thunk_start,
                       GUM_PEAK_PC_IN_DISPATCH,
                       NULL);
    }
}
#endif

static int
count_helper_log_entry(const char* path, const char* entry)
{
    FILE* fp = fopen(path, "r");
    char line[128];
    int count = 0;

    if (fp == NULL) {
        fprintf(stderr, "FAIL: open helper log %s: %s\n", path, strerror(errno));
        failures++;
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, entry) == 0) {
            count++;
        }
    }

    fclose(fp);
    return count;
}

static void
check_helper_log_count(const char* path,
                       const char* entry,
                       int expected_count)
{
    char label[160];
    int actual_count = count_helper_log_entry(path, entry);

    snprintf(label, sizeof(label), "helper log %s", entry);
    check_int(label, actual_count, expected_count);
}

static int
set_fake_helper_env_default(const char* default_scenario, const char* log_path)
{
    const char* scenario = getenv("FAKE_DETACH_HELPER_SCENARIO");

    if ((scenario == NULL || scenario[0] == '\0') &&
        setenv("FAKE_DETACH_HELPER_SCENARIO", default_scenario, 1) != 0) {
        return -1;
    }
    if (setenv("FAKE_DETACH_HELPER_LOG", log_path, 1) != 0) {
        return -1;
    }
    return 0;
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static int
set_fake_helper_pointer_env(const char* name, gpointer pointer)
{
    char value[2 + sizeof(uintptr_t) * 2 + 1];

    snprintf(value, sizeof(value), "0x%" PRIxPTR, (uintptr_t)pointer);
    return setenv(name, value, 1);
}

static int
set_fake_helper_uint_env(const char* name, guint value)
{
    char text[32];

    snprintf(text, sizeof(text), "%u", value);
    return setenv(name, text, 1);
}

static int
set_fake_helper_hex_bytes_env(const char* name,
                              const guint8* bytes,
                              guint byte_count)
{
    char text[GUM_PEAK_MAX_PROLOGUE_SIZE * 2 + 1];

    if (byte_count > GUM_PEAK_MAX_PROLOGUE_SIZE) {
        return -1;
    }
    for (guint i = 0; i < byte_count; i++) {
        snprintf(&text[i * 2], sizeof(text) - i * 2, "%02x", bytes[i]);
    }
    text[byte_count * 2] = '\0';
    return setenv(name, text, 1);
}

static int
resolve_fake_helper_gum_pc_case(const char* pc_case,
                                const GumPeakPcDiagnostics* diagnostics,
                                gpointer* pc_out,
                                gboolean* expect_prepared_out)
{
    if (pc_case == NULL || pc_case[0] == '\0') {
        pc_case = "on-enter";
    }

    if (strcmp(pc_case, "patch-entry") == 0) {
        if (diagnostics->function_address == NULL) {
            fprintf(stderr, "patch-entry diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->function_address;
        *expect_prepared_out = TRUE;
        return 1;
    }

    if (strcmp(pc_case, "patch-entry-plus-one") == 0) {
        if (diagnostics->function_address == NULL ||
            diagnostics->overwritten_prologue_len <= 1) {
            fprintf(stderr, "patch-entry-plus-one diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = (gpointer)((guint8*)diagnostics->function_address + 1);
        *expect_prepared_out = FALSE;
        return 1;
    }

    if (strcmp(pc_case, "on-enter") == 0) {
        if (diagnostics->on_enter_trampoline == NULL) {
            fprintf(stderr, "on-enter diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->on_enter_trampoline;
        *expect_prepared_out = TRUE;
        return 1;
    }

    if (strcmp(pc_case, "on-enter-plus-one") == 0) {
        gpointer pc;

        if (diagnostics->on_enter_trampoline == NULL ||
            diagnostics->on_leave_trampoline == NULL ||
            (uintptr_t)diagnostics->on_enter_trampoline + 1 >=
                (uintptr_t)diagnostics->on_leave_trampoline) {
            fprintf(stderr, "on-enter-plus-one diagnostic PC unavailable\n");
            return 0;
        }
        pc = (gpointer)((guint8*)diagnostics->on_enter_trampoline + 1);
        *pc_out = pc;
        *expect_prepared_out = TRUE;
        return 1;
    }

    if (strcmp(pc_case, "on-leave") == 0) {
        if (diagnostics->on_leave_trampoline == NULL) {
            fprintf(stderr, "on-leave diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->on_leave_trampoline;
        *expect_prepared_out = TRUE;
        return 1;
    }

    if (strcmp(pc_case, "on-invoke") == 0) {
        if (diagnostics->on_invoke_trampoline == NULL) {
            fprintf(stderr, "on-invoke diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->on_invoke_trampoline;
        *expect_prepared_out = TRUE;
        return 1;
    }

    if (strcmp(pc_case, "enter-thunk") == 0) {
        if (diagnostics->enter_thunk_start == NULL ||
            diagnostics->enter_thunk_size == 0) {
            fprintf(stderr, "enter-thunk diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->enter_thunk_start;
        *expect_prepared_out = TRUE;
        return 1;
    }

    fprintf(stderr, "unknown Gum PC corridor case: %s\n", pc_case);
    return -1;
}
#endif

static int
run_strict_helper_empty(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "strict-helper-empty requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    pthread_t worker;
    int thread_count;
    gboolean signal_backend =
        g_getenv("PEAK_DETACH_BACKEND") != NULL &&
        g_ascii_strcasecmp(g_getenv("PEAK_DETACH_BACKEND"), "signal") == 0;

    if (pthread_create(&worker, NULL, strict_helper_worker, NULL) != 0) {
        perror("pthread_create");
        return EXIT_FAILURE;
    }
    while (!worker_running) {
        usleep(1000);
    }

    thread_count = count_proc_threads();
    if (thread_count < 2) {
        worker_running = 0;
        pthread_join(worker, NULL);
        fprintf(stderr,
                "strict-helper-empty requires a visible worker thread, got %d\n",
                thread_count);
        return 77;
    }

    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener =
        gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    GumAttachReturn attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        worker_running = 0;
        pthread_join(worker, NULL);
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    check_strict_helper_pc_classification(interceptor, listener);
    if (signal_backend) {
        peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    }

    PeakDetachRequest request = {
        .hook_id = 17,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
    gboolean prepared =
        peak_detach_controller_prepare_hook_mutation(&request, &prepare_status);
    if (!prepared && prepare_status == PEAK_DETACH_STATUS_PERMISSION_DENIED) {
        fprintf(stderr,
                "strict-helper-empty skipped: ptrace permission denied\n");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        worker_running = 0;
        pthread_join(worker, NULL);
        return 77;
    }
    check_true("strict helper threaded physical detach", prepared == TRUE);
    check_status("strict helper threaded physical detach status",
                 prepare_status,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper holds threaded stop",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("strict helper applies physical patch",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("strict helper threaded resume", &request, PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper released threaded stop",
               peak_detach_controller_threads_are_held() == FALSE);

    request.operation = PEAK_DETACH_OPERATION_REATTACH;
    check_prepare("strict helper threaded physical reattach",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper holds threaded reattach stop",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("strict helper reapplies physical patch",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("strict helper threaded reattach resume",
                 &request,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper released threaded reattach stop",
               peak_detach_controller_threads_are_held() == FALSE);
    strict_helper_target();

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    worker_running = 0;
    pthread_join(worker, NULL);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_cached_helper_configuration(void)
{
    check_true("change helper path after configuration",
               g_setenv("PEAK_DETACH_HELPER",
                        "/no/such/peak_detach_helper",
                        TRUE));
    return run_strict_helper_empty();
}

static int
run_fake_helper_trace_disabled_stop_window(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr,
            "fake-helper-trace-disabled-stop-window requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_trace_disabled_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    PeakDetachAccountingSnapshot accounting;
    double last_success_us;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0 ||
        unsetenv("PEAK_DETACH_TRACE_PATH") != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    check_true("trace-disabled Gum attach", attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();

    PeakDetachRequest request = {
        .hook_id = 29,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_accounting_snapshot("trace-disabled accounting baseline", &accounting);
    check_prepare("trace-disabled detach prepare",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("trace-disabled detach physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_double_zero("trace-disabled detach prepare stop window",
                      peak_detach_controller_last_stop_window_us());
    check_finish("trace-disabled detach finish", &request, PEAK_DETACH_STATUS_SAFE);
    check_stop_window_finish("trace-disabled detach finish stop window",
                             &accounting);
    last_success_us = peak_detach_controller_last_stop_window_us();

    request.operation = PEAK_DETACH_OPERATION_REATTACH;
    check_prepare("trace-disabled reattach prepare",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("trace-disabled reattach physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_true("trace-disabled reattach prepare preserves last stop window",
               peak_detach_controller_last_stop_window_us() == last_success_us);
    check_finish("trace-disabled reattach finish", &request, PEAK_DETACH_STATUS_SAFE);
    check_stop_window_finish("trace-disabled reattach finish stop window",
                             &accounting);
    last_success_us = peak_detach_controller_last_stop_window_us();

    check_true("trace-disabled helper shutdown before failure",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_status("trace-disabled helper shutdown before failure status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);
    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "stop-permission", 1) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    request.operation = PEAK_DETACH_OPERATION_DETACH;
    check_prepare("trace-disabled fast stop failure",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_PERMISSION_DENIED);
    check_stop_window_failure("trace-disabled fast stop failure accounting",
                              &accounting,
                              1);
    check_true("trace-disabled fast stop failure leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("trace-disabled fast stop failure preserves last stop window",
               peak_detach_controller_last_stop_window_us() == last_success_us);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    check_true("trace-disabled helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_status("trace-disabled helper shutdown status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);

    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_strict_helper_stale_caller(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "strict-helper-stale-caller requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    pthread_t worker;
    gboolean signal_backend =
        g_getenv("PEAK_DETACH_BACKEND") != NULL &&
        g_ascii_strcasecmp(g_getenv("PEAK_DETACH_BACKEND"), "signal") == 0;

    stale_worker_parked = 0;
    stale_worker_release = 0;
    stale_worker_calls = 0;

    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener =
        gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    GumAttachReturn attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);

    if (pthread_create(&worker, NULL, strict_helper_stale_worker, NULL) != 0) {
        perror("pthread_create");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }

    pthread_mutex_lock(&stale_worker_mutex);
    while (!stale_worker_parked) {
        pthread_cond_wait(&stale_worker_cond, &stale_worker_mutex);
    }
    pthread_mutex_unlock(&stale_worker_mutex);

    check_int("stale worker warmup calls",
              stale_worker_calls,
              stale_worker_warmup_calls);
    check_true("stale worker parked", stale_worker_parked == 1);
    if (signal_backend) {
        peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    }

    PeakDetachRequest request = {
        .hook_id = 23,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
    gboolean prepared =
        peak_detach_controller_prepare_hook_mutation(&request, &prepare_status);
    if (!prepared && prepare_status == PEAK_DETACH_STATUS_PERMISSION_DENIED) {
        fprintf(stderr,
                "strict-helper-stale-caller skipped: ptrace permission denied\n");
        pthread_mutex_lock(&stale_worker_mutex);
        stale_worker_release = 1;
        pthread_cond_broadcast(&stale_worker_cond);
        pthread_mutex_unlock(&stale_worker_mutex);
        pthread_join(worker, NULL);
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return 77;
    }
    check_true("strict helper stale-caller physical detach", prepared == TRUE);
    check_status("strict helper stale-caller physical detach status",
                 prepare_status,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper holds stale-caller stop",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("strict helper applies stale-caller physical patch",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("strict helper stale-caller detach resume",
                 &request,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper released stale-caller detach stop",
               peak_detach_controller_threads_are_held() == FALSE);

    request.operation = PEAK_DETACH_OPERATION_REATTACH;
    check_prepare("strict helper stale-caller physical reattach",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper holds stale-caller reattach stop",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("strict helper reapplies stale-caller physical patch",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("strict helper stale-caller reattach resume",
                 &request,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("strict helper released stale-caller reattach stop",
               peak_detach_controller_threads_are_held() == FALSE);
    strict_helper_target();

    pthread_mutex_lock(&stale_worker_mutex);
    stale_worker_release = 1;
    pthread_cond_broadcast(&stale_worker_cond);
    pthread_mutex_unlock(&stale_worker_mutex);
    pthread_join(worker, NULL);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_shutdown_sequence(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-shutdown-sequence requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_status;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "success-zero", 1) != 0 ||
        setenv("FAKE_DETACH_HELPER_LOG", log_template, 1) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener_one,
                               NULL);
    check_true("first Gum attach for shutdown sequence",
               attach_status == GUM_ATTACH_OK);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target_two,
                               listener_two,
                               NULL);
    check_true("second Gum attach for shutdown sequence",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();

    PeakDetachRequest request_one = {
        .hook_id = 31,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener_one,
        .operation = PEAK_DETACH_OPERATION_SHUTDOWN
    };
    PeakDetachRequest request_two = {
        .hook_id = 32,
        .symbol_name = "strict_helper_target_two",
        .function_address = (gpointer)strict_helper_target_two,
        .interceptor = interceptor,
        .listener = listener_two,
        .operation = PEAK_DETACH_OPERATION_SHUTDOWN
    };
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    check_prepare("first shutdown prepare",
                  &request_one,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("first shutdown holds helper threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_finish("first shutdown finish resumes helper",
                 &request_one,
                 PEAK_DETACH_STATUS_SAFE);
    gum_interceptor_detach(interceptor, listener_one);

    check_prepare("second shutdown prepare",
                  &request_two,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("second shutdown holds helper threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_finish("second shutdown finish resumes helper",
                 &request_two,
                 PEAK_DETACH_STATUS_SAFE);
    gum_interceptor_detach(interceptor, listener_two);
    gum_interceptor_flush(interceptor);

    check_true("final helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_true("final helper shutdown status",
               status == PEAK_DETACH_STATUS_SAFE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 2);
    check_helper_log_count(log_template, "EVACUATE", 2);
    check_helper_log_count(log_template, "RESUME", 2);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_attach(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-attach requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_attach_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_one;
    GumAttachReturn attach_two;
    PeakDetachBatchResult results[2];
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 39,
            .symbol_name = "strict_helper_target",
            .function_address = (gpointer)strict_helper_target,
            .interceptor = interceptor,
            .listener = listener_one,
            .operation = PEAK_DETACH_OPERATION_ATTACH
        },
        {
            .hook_id = 40,
            .symbol_name = "strict_helper_target_two",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener_two,
            .operation = PEAK_DETACH_OPERATION_ATTACH
        }
    };

    check_true("batch attach capacity covers focused test",
               peak_detach_controller_max_batch_requests() >= 2);
    check_true("batch attach prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == TRUE);
    check_status("batch attach status", status, PEAK_DETACH_STATUS_SAFE);
    check_int("batch attach prepared count", (int)prepared_count, 2);
    check_true("first batch attach prepared", results[0].prepared == TRUE);
    check_true("second batch attach prepared", results[1].prepared == TRUE);
    check_true("first batch attach is not physical",
               results[0].uses_physical_patch == FALSE);
    check_true("second batch attach is not physical",
               results[1].uses_physical_patch == FALSE);
    check_true("batch attach holds helper threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("batch attach does not report a physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    gum_interceptor_begin_transaction(interceptor);
    attach_one = gum_interceptor_attach(interceptor,
                                        (gpointer)strict_helper_target,
                                        listener_one,
                                        NULL);
    attach_two = gum_interceptor_attach(interceptor,
                                        (gpointer)strict_helper_target_two,
                                        listener_two,
                                        NULL);
    gum_interceptor_end_transaction(interceptor);
    check_true("first Gum batch attach", attach_one == GUM_ATTACH_OK);
    check_true("second Gum batch attach", attach_two == GUM_ATTACH_OK);

    check_true("batch attach finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);
    check_status("batch attach finish status", status, PEAK_DETACH_STATUS_SAFE);
    check_true("batch attach released helper threads",
               peak_detach_controller_threads_are_held() == FALSE);

    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();
    if (attach_one == GUM_ATTACH_OK) {
        gum_interceptor_detach(interceptor, listener_one);
    }
    if (attach_two == GUM_ATTACH_OK) {
        gum_interceptor_detach(interceptor, listener_two);
    }
    gum_interceptor_flush(interceptor);
    check_true("batch attach helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 1);
    check_helper_log_count(log_template, "EVACUATE", 0);
    check_helper_log_count(log_template, "RESUME", 1);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_arm64_attach_plan(void)
{
#if !defined(PEAK_HAVE_GUM_PEAK_PC_API) || !defined(__aarch64__)
    fprintf(stderr,
            "fake-helper-arm64-attach-plan requires AArch64 and PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_arm64_attach_plan_XXXXXX";
    int log_fd = mkstemp(log_template);
    guint32 branch_to_canonical_plt[20] = {
        [0] = UINT32_C(0x14000010),  /* b +64 */
        [16] = UINT32_C(0x90000010), /* adrp x16, ... */
        [17] = UINT32_C(0xf9400211), /* ldr x17, [x16, #0] */
        [18] = UINT32_C(0x91000210), /* add x16, x16, #0 */
        [19] = UINT32_C(0xd61f0220)  /* br x17 */
    };
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    PeakGumTargetAttachPlan plan;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("synthetic-stop", log_template) != 0 ||
        setenv("FAKE_DETACH_HELPER_STOP_TID", "target-pid", 1) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    peak_gum_target_attach_plan((gpointer)branch_to_canonical_plt, &plan);
    check_true("Arm64 attach plan selects forced relocation",
               plan.options.instrumentation.relocation_policy ==
                   GUM_RELOCATION_FORCED);
    check_true("Arm64 attach plan points at PLT entry",
               plan.mutation_address ==
                   (gpointer)&branch_to_canonical_plt[16]);
    check_true("Arm64 attach plan has a mutation guard",
               plan.mutation_guard_size == GUM_PEAK_MAX_PROLOGUE_SIZE);
    if (set_fake_helper_pointer_env("FAKE_DETACH_HELPER_STOP_PC",
                                    plan.mutation_address) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    PeakDetachRequest request = {
        .hook_id = 41,
        .symbol_name = "arm64_branch_to_plt",
        .function_address = (gpointer)branch_to_canonical_plt,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_ATTACH,
        .blocked_pc_start = plan.mutation_address,
        .blocked_pc_size = plan.mutation_guard_size
    };

    check_prepare("Arm64 PLT mutation live PC",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    const PeakDetachFailureDetail* detail =
        peak_detach_controller_last_failure_detail();
    check_true("Arm64 attach rejects the planned PLT range",
               detail != NULL && detail->reason != NULL &&
               strcmp(detail->reason, "blocked-pc") == 0);
    check_true("Arm64 rejected attach leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("Arm64 attach-plan helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_status("Arm64 attach-plan helper shutdown status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);
    check_helper_log_count(log_template, "STOP", 1);
    check_helper_log_count(log_template, "EVACUATE", 0);
    check_helper_log_count(log_template, "RESUME", 1);

    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_detach(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-detach requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_status;
    PeakDetachBatchResult results[2];
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    const char* scenario;
    const char* helper_path;
    int helper_expected_to_start;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }
    scenario = getenv("FAKE_DETACH_HELPER_SCENARIO");
    if (scenario == NULL || scenario[0] == '\0') {
        scenario = "success-zero";
    }
    helper_path = getenv("PEAK_DETACH_HELPER");
    helper_expected_to_start =
        helper_path == NULL || helper_path[0] == '\0' ||
        access(helper_path, X_OK) == 0;

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener_one,
                               NULL);
    check_true("first Gum attach for batch detach",
               attach_status == GUM_ATTACH_OK);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target_two,
                               listener_two,
                               NULL);
    check_true("second Gum attach for batch detach",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 41,
            .symbol_name = "strict_helper_target",
            .function_address = (gpointer)strict_helper_target,
            .interceptor = interceptor,
            .listener = listener_one,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 42,
            .symbol_name = "strict_helper_target_two",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener_two,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    check_true("batch detach prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == TRUE);
    check_true("batch detach status safe", status == PEAK_DETACH_STATUS_SAFE);
    check_int("batch detach prepared count", (int)prepared_count, 2);
    check_true("first batch detach prepared", results[0].prepared == TRUE);
    check_true("second batch detach prepared", results[1].prepared == TRUE);
    check_true("first batch detach physical", results[0].uses_physical_patch == TRUE);
    check_true("second batch detach physical", results[1].uses_physical_patch == TRUE);
    check_true("batch detach holds helper threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("batch detach current mutation physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_true("batch detach finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);
    check_true("batch detach finish status", status == PEAK_DETACH_STATUS_SAFE);
    check_true("batch detach released helper threads",
               peak_detach_controller_threads_are_held() == FALSE);

    gum_interceptor_detach(interceptor, listener_one);
    gum_interceptor_detach(interceptor, listener_two);
    gum_interceptor_flush(interceptor);
    check_true("batch helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START",
                           helper_expected_to_start ? 1 : 0);
    check_helper_log_count(log_template, "STOP",
                           helper_expected_to_start ? 1 : 0);
    if (!helper_expected_to_start ||
        strcmp(scenario, "stop-permission") == 0 ||
        strcmp(scenario, "stop-timeout") == 0 ||
        strcmp(scenario, "stop-unsupported") == 0) {
        check_helper_log_count(log_template, "EVACUATE", 0);
        check_helper_log_count(log_template, "RESUME", 0);
        check_helper_log_count(log_template, "SHUTDOWN", 0);
    } else {
        check_helper_log_count(log_template, "EVACUATE", 1);
        check_helper_log_count(log_template, "RESUME", 1);
        check_helper_log_count(log_template, "SHUTDOWN", 1);
    }

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_abort_rolls_back_records(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-abort-rollback requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_abort_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_status;
    PeakDetachBatchResult results[2];
    PeakDetachBatchResult reattach_result[1];
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("evacuate-error", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener_one,
                               NULL);
    check_true("first Gum attach for rollback batch",
               attach_status == GUM_ATTACH_OK);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target_two,
                               listener_two,
                               NULL);
    check_true("second Gum attach for rollback batch",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 43,
            .symbol_name = "strict_helper_target",
            .function_address = (gpointer)strict_helper_target,
            .interceptor = interceptor,
            .listener = listener_one,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 44,
            .symbol_name = "strict_helper_target_two",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener_two,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    check_true("rollback batch prepare aborts",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_status("rollback batch aggregate status",
                 status,
                 PEAK_DETACH_STATUS_ERROR);
    check_int("rollback batch prepared count", (int)prepared_count, 0);
    check_true("rollback first not prepared", results[0].prepared == FALSE);
    check_true("rollback second not prepared", results[1].prepared == FALSE);
    check_status("rollback first status",
                 results[0].status,
                 PEAK_DETACH_STATUS_ERROR);
    check_status("rollback second status",
                 results[1].status,
                 PEAK_DETACH_STATUS_ERROR);
    check_true("rollback batch leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);

    check_cleanup_query();
    prepared_count = 99;
    check_true("subsequent batch STOP and recoverable EVACUATE",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests, 2, results, &prepared_count, &status) == FALSE);
    check_int("subsequent batch prepared count", (int)prepared_count, 0);
    check_cleanup_query();

    requests[0].operation = PEAK_DETACH_OPERATION_REATTACH;
    prepared_count = 99;
    memset(reattach_result, 0xff, sizeof(reattach_result));
    status = PEAK_DETACH_STATUS_SAFE;
    check_true("rollback reattach missing rolled-back record",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   1,
                   reattach_result,
                   &prepared_count,
                   &status) == FALSE);
    check_int("rollback reattach prepared count", (int)prepared_count, 0);
    check_status("rollback reattach aggregate status",
                 status,
                 PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    check_true("rollback reattach not prepared",
               reattach_result[0].prepared == FALSE);
    check_status("rollback reattach result status",
                 reattach_result[0].status,
                 PEAK_DETACH_STATUS_CLASSIFY_FAILED);

    gum_interceptor_detach(interceptor, listener_one);
    gum_interceptor_detach(interceptor, listener_two);
    gum_interceptor_flush(interceptor);
    check_true("rollback batch helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 2);
    check_helper_log_count(log_template, "EVACUATE", 2);
    check_helper_log_count(log_template, "RESUME", 0);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_mixed(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-mixed requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_mixed_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_status;
    PeakDetachBatchResult results[2];
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "blocked-pc", 1) != 0 ||
        setenv("FAKE_DETACH_HELPER_LOG", log_template, 1) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener_one,
                               NULL);
    check_true("first Gum attach for mixed batch",
               attach_status == GUM_ATTACH_OK);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target_two,
                               listener_two,
                               NULL);
    check_true("second Gum attach for mixed batch",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 51,
            .symbol_name = "strict_helper_target",
            .function_address = (gpointer)strict_helper_target,
            .interceptor = interceptor,
            .listener = listener_one,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 52,
            .symbol_name = "strict_helper_target_two",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener_two,
            .operation = PEAK_DETACH_OPERATION_DETACH,
            .blocked_pc_start = (gpointer)(uintptr_t)0x1200,
            .blocked_pc_size = 0x100
        }
    };

    check_true("mixed batch prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == TRUE);
    check_true("mixed batch status safe", status == PEAK_DETACH_STATUS_SAFE);
    check_int("mixed batch prepared count", (int)prepared_count, 1);
    check_true("mixed first prepared", results[0].prepared == TRUE);
    check_true("mixed second not prepared", results[1].prepared == FALSE);
    check_true("mixed second classify failed",
               results[1].status == PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    check_true("mixed batch holds helper threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("mixed batch finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);
    check_true("mixed batch finish status", status == PEAK_DETACH_STATUS_SAFE);
    check_true("mixed batch released helper threads",
               peak_detach_controller_threads_are_held() == FALSE);

    gum_interceptor_detach(interceptor, listener_one);
    gum_interceptor_detach(interceptor, listener_two);
    gum_interceptor_flush(interceptor);
    check_true("mixed batch helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 1);
    check_helper_log_count(log_template, "EVACUATE", 1);
    check_helper_log_count(log_template, "RESUME", 1);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_missing_gum_snapshot(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-missing-gum-snapshot requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_gum_snapshot_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_status;
    PeakDetachBatchResult results[2];
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener_one,
                               NULL);
    check_true("first Gum attach for missing snapshot batch",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 53,
            .symbol_name = "strict_helper_target",
            .function_address = (gpointer)strict_helper_target,
            .interceptor = interceptor,
            .listener = listener_one,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 54,
            .symbol_name = "strict_helper_target_two_unattached_listener",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener_two,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    check_true("missing Gum snapshot batch prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == TRUE);
    check_status("missing Gum snapshot aggregate status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);
    check_int("missing Gum snapshot prepared count", (int)prepared_count, 1);
    check_true("missing Gum snapshot first prepared",
               results[0].prepared == TRUE);
    check_true("missing Gum snapshot first physical",
               results[0].uses_physical_patch == TRUE);
    check_status("missing Gum snapshot first status",
                 results[0].status,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("missing Gum snapshot second not prepared",
               results[1].prepared == FALSE);
    check_true("missing Gum snapshot second not physical",
               results[1].uses_physical_patch == FALSE);
    check_status("missing Gum snapshot second status",
                 results[1].status,
                 PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    check_true("missing Gum snapshot batch holds helper threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("missing Gum snapshot batch finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);
    check_status("missing Gum snapshot finish status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("missing Gum snapshot released helper threads",
               peak_detach_controller_threads_are_held() == FALSE);

    gum_interceptor_detach(interceptor, listener_one);
    gum_interceptor_flush(interceptor);
    check_true("missing Gum snapshot helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 1);
    check_helper_log_count(log_template, "EVACUATE", 1);
    check_helper_log_count(log_template, "RESUME", 1);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_listener_canonical_address(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-listener-canonical-address requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_listener_canonical_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    GumPeakPcDiagnostics diagnostics;
    guint8 active_patch[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original_prologue[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint prologue_len = 0;
    PeakDetachBatchResult batch_result;
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    check_true("canonical listener Gum attach",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();

    check_true("canonical listener diagnostics via mismatched address",
               gum_interceptor_peak_get_pc_diagnostics(
                   interceptor,
                   (gpointer)strict_helper_target_two,
                   listener,
                   &diagnostics) == TRUE);
    check_true("canonical listener recovered function address",
               diagnostics.function_address == (gpointer)strict_helper_target);
    check_true("canonical listener function patch via mismatched address",
               gum_interceptor_peak_get_function_patch(
                   interceptor,
                   (gpointer)strict_helper_target_two,
                   listener,
                   active_patch,
                   original_prologue,
                   &prologue_len) == TRUE);
    check_true("canonical listener prologue length",
               prologue_len > 0 && prologue_len <= GUM_PEAK_MAX_PROLOGUE_SIZE);
    if (failures != 0) {
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    if (setenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS",
               "WRITE_MEMORY",
               1) != 0 ||
        set_fake_helper_pointer_env("FAKE_DETACH_HELPER_EXPECT_WRITE_ADDRESS",
                                    diagnostics.function_address) != 0 ||
        set_fake_helper_uint_env("FAKE_DETACH_HELPER_EXPECT_WRITE_SIZE",
                                 prologue_len) != 0 ||
        set_fake_helper_hex_bytes_env(
            "FAKE_DETACH_HELPER_EXPECT_WRITE_BYTES_HEX",
            original_prologue,
            prologue_len) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    PeakDetachRequest request = {
        .hook_id = 59,
        .symbol_name = "strict_helper_target_wrong_address",
        .function_address = (gpointer)strict_helper_target_two,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_prepare("canonical listener detach prepare",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("canonical listener helper holds threads",
               peak_detach_controller_threads_are_held() == TRUE);
    check_true("canonical listener uses physical patch",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("canonical listener detach finish",
                 &request,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("canonical listener released helper threads",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("canonical listener helper shutdown after detach",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    if (setenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS",
               "WRITE_MEMORY",
               1) != 0 ||
        set_fake_helper_pointer_env("FAKE_DETACH_HELPER_EXPECT_WRITE_ADDRESS",
                                    diagnostics.function_address) != 0 ||
        set_fake_helper_uint_env("FAKE_DETACH_HELPER_EXPECT_WRITE_SIZE",
                                 prologue_len) != 0 ||
        set_fake_helper_hex_bytes_env(
            "FAKE_DETACH_HELPER_EXPECT_WRITE_BYTES_HEX",
            active_patch,
            prologue_len) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    request.operation = PEAK_DETACH_OPERATION_REATTACH;
    memset(&batch_result, 0, sizeof(batch_result));
    prepared_count = 0;
    check_true("canonical listener batch reattach prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   &request,
                   1,
                   &batch_result,
                   &prepared_count,
                   &status) == TRUE);
    check_status("canonical listener batch reattach status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);
    check_int("canonical listener batch reattach prepared count",
              (int)prepared_count,
              1);
    check_true("canonical listener batch reattach prepared",
               batch_result.prepared == TRUE);
    check_true("canonical listener batch reattach physical",
               batch_result.uses_physical_patch == TRUE);
    check_true("canonical listener batch reattach finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);
    check_status("canonical listener batch reattach finish status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("canonical listener batch reattach released helper threads",
               peak_detach_controller_threads_are_held() == FALSE);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    check_true("canonical listener helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 2);
    check_helper_log_count(log_template, "STOP", 2);
    check_helper_log_count(log_template, "EVACUATE", 2);
    check_helper_log_count(log_template, "RESUME", 2);
    check_helper_log_count(log_template, "SHUTDOWN", 2);

    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_listener_ambiguous_address(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-listener-ambiguous-address requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    GumPeakPcDiagnostics diagnostics;
    guint8 active_patch[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original_prologue[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint prologue_len = 0;

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    check_true("ambiguous listener first Gum attach",
               attach_status == GUM_ATTACH_OK);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target_two,
                               listener,
                               NULL);
    check_true("ambiguous listener second Gum attach",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();

    check_true("ambiguous listener direct first diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(
                   interceptor,
                   (gpointer)strict_helper_target,
                   listener,
                   &diagnostics) == TRUE);
    check_true("ambiguous listener direct first canonical address",
               diagnostics.function_address == (gpointer)strict_helper_target);
    check_true("ambiguous listener direct second diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(
                   interceptor,
                   (gpointer)strict_helper_target_two,
                   listener,
                   &diagnostics) == TRUE);
    check_true("ambiguous listener direct second canonical address",
               diagnostics.function_address == (gpointer)strict_helper_target_two);
    check_true("ambiguous listener stale diagnostics fail closed",
               gum_interceptor_peak_get_pc_diagnostics(
                   interceptor,
                   (gpointer)strict_helper_worker,
                   listener,
                   &diagnostics) == FALSE);
    check_true("ambiguous listener stale patch fail closed",
               gum_interceptor_peak_get_function_patch(
                   interceptor,
                   (gpointer)strict_helper_worker,
                   listener,
                   active_patch,
                   original_prologue,
                   &prologue_len) == FALSE);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_canonical_duplicate(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-canonical-duplicate requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_canonical_dup_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    PeakDetachBatchResult results[2];
    size_t prepared_count = 99;
    PeakDetachStatus status = PEAK_DETACH_STATUS_SAFE;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    check_true("canonical duplicate Gum attach",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 63,
            .symbol_name = "strict_helper_target_stale_one",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 64,
            .symbol_name = "strict_helper_target_stale_two",
            .function_address = (gpointer)strict_helper_worker,
            .interceptor = interceptor,
            .listener = listener,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    memset(results, 0xff, sizeof(results));
    check_true("canonical duplicate batch prepare fails",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_status("canonical duplicate batch status",
                 status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_int("canonical duplicate prepared count", (int)prepared_count, 0);
    check_true("canonical duplicate first not prepared",
               results[0].prepared == FALSE);
    check_true("canonical duplicate second not prepared",
               results[1].prepared == FALSE);
    check_status("canonical duplicate first status",
                 results[0].status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_status("canonical duplicate second status",
                 results[1].status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("canonical duplicate no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);

    const PeakDetachFailureDetail* detail =
        peak_detach_controller_last_failure_detail();
    check_true("canonical duplicate failure detail",
               detail != NULL &&
               detail->reason != NULL &&
               strcmp(detail->reason, "batch-canonical-duplicate") == 0);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    check_true("canonical duplicate helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 0);
    check_helper_log_count(log_template, "STOP", 0);
    check_helper_log_count(log_template, "EVACUATE", 0);
    check_helper_log_count(log_template, "RESUME", 0);
    check_helper_log_count(log_template, "SHUTDOWN", 0);

    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_batch_reattach(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-batch-reattach requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    char log_template[] = "/tmp/peak_fake_helper_batch_reattach_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener_one;
    GumInvocationListener* listener_two;
    GumAttachReturn attach_status;
    PeakDetachBatchResult results[2];
    size_t prepared_count = 0;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);
    if (set_fake_helper_env_default("success-zero", log_template) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener_one = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    listener_two = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);

    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener_one,
                               NULL);
    check_true("first Gum attach for batch reattach",
               attach_status == GUM_ATTACH_OK);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target_two,
                               listener_two,
                               NULL);
    check_true("second Gum attach for batch reattach",
               attach_status == GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    strict_helper_target_two();

    PeakDetachRequest requests[2] = {
        {
            .hook_id = 61,
            .symbol_name = "strict_helper_target",
            .function_address = (gpointer)strict_helper_target,
            .interceptor = interceptor,
            .listener = listener_one,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 62,
            .symbol_name = "strict_helper_target_two",
            .function_address = (gpointer)strict_helper_target_two,
            .interceptor = interceptor,
            .listener = listener_two,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    check_true("batch reattach initial detach prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == TRUE);
    check_int("batch reattach detach prepared count", (int)prepared_count, 2);
    check_true("batch reattach initial detach finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);

    requests[0].operation = PEAK_DETACH_OPERATION_REATTACH;
    requests[1].operation = PEAK_DETACH_OPERATION_REATTACH;
    prepared_count = 0;
    memset(results, 0, sizeof(results));

    check_true("batch reattach prepare",
               peak_detach_controller_prepare_hook_mutation_batch(
                   requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == TRUE);
    check_true("batch reattach status safe", status == PEAK_DETACH_STATUS_SAFE);
    check_int("batch reattach prepared count", (int)prepared_count, 2);
    check_true("first batch reattach prepared", results[0].prepared == TRUE);
    check_true("second batch reattach prepared", results[1].prepared == TRUE);
    check_true("first batch reattach physical", results[0].uses_physical_patch == TRUE);
    check_true("second batch reattach physical", results[1].uses_physical_patch == TRUE);
    check_true("batch reattach finish",
               peak_detach_controller_finish_hook_mutation_batch(&status) == TRUE);
    check_true("batch reattach finish status", status == PEAK_DETACH_STATUS_SAFE);

    gum_interceptor_detach(interceptor, listener_one);
    gum_interceptor_detach(interceptor, listener_two);
    gum_interceptor_flush(interceptor);
    check_true("batch reattach helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 2);
    check_helper_log_count(log_template, "EVACUATE", 2);
    check_helper_log_count(log_template, "RESUME", 2);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    g_object_unref(listener_one);
    g_object_unref(listener_two);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_batch_guards(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "batch-guards requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    PeakDetachStatus status = PEAK_DETACH_STATUS_SAFE;
    PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
    size_t prepared_count = 99;
    PeakDetachBatchResult results[4];
    int result;

    gum_init_embedded();

    check_true("batch capacity matches helper protocol",
               peak_detach_controller_max_batch_requests() ==
                   PEAK_DETACH_HELPER_MAX_BATCH_WRITES);

    PeakDetachRequest invalid_attach_request = {
        .hook_id = 71,
        .symbol_name = "invalid_attach_listener",
        .function_address = (gpointer)(uintptr_t)0x7100,
        .interceptor = (GumInterceptor*)(uintptr_t)0x7110,
        .listener = NULL,
        .operation = PEAK_DETACH_OPERATION_ATTACH
    };

    memset(results, 0xff, sizeof(results));
    check_true("attach batch without listener rejected",
               peak_detach_controller_prepare_hook_mutation_batch(
                   &invalid_attach_request,
                   1,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_int("invalid attach prepared count", (int)prepared_count, 0);
    check_status("invalid attach aggregate status",
                 status,
                 PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    check_true("invalid attach not prepared", results[0].prepared == FALSE);
    check_true("invalid attach not physical",
               results[0].uses_physical_patch == FALSE);
    check_status("invalid attach status",
                 results[0].status,
                 PEAK_DETACH_STATUS_ERROR);

    PeakDetachRequest unsupported_requests[3] = {
        {
            .hook_id = 72,
            .symbol_name = "unsupported_shutdown",
            .function_address = (gpointer)(uintptr_t)0x7200,
            .interceptor = (GumInterceptor*)(uintptr_t)0x7210,
            .listener = (GumInvocationListener*)(uintptr_t)0x7220,
            .operation = PEAK_DETACH_OPERATION_SHUTDOWN
        },
        {
            .hook_id = 73,
            .symbol_name = "unsupported_replace",
            .function_address = (gpointer)(uintptr_t)0x7300,
            .interceptor = (GumInterceptor*)(uintptr_t)0x7310,
            .listener = NULL,
            .operation = PEAK_DETACH_OPERATION_REPLACE
        },
        {
            .hook_id = 74,
            .symbol_name = "unsupported_revert",
            .function_address = (gpointer)(uintptr_t)0x7400,
            .interceptor = (GumInterceptor*)(uintptr_t)0x7410,
            .listener = NULL,
            .operation = PEAK_DETACH_OPERATION_REVERT
        }
    };

    status = PEAK_DETACH_STATUS_SAFE;
    prepared_count = 99;
    memset(results, 0xff, sizeof(results));
    check_true("unsupported batch rejected",
               peak_detach_controller_prepare_hook_mutation_batch(
                   unsupported_requests,
                   3,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_int("unsupported batch prepared count", (int)prepared_count, 0);
    check_status("unsupported batch aggregate status",
                 status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    for (size_t i = 0; i < 3; i++) {
        char label[96];
        snprintf(label, sizeof(label), "unsupported result %zu prepared", i);
        check_true(label, results[i].prepared == FALSE);
        snprintf(label, sizeof(label), "unsupported result %zu physical", i);
        check_true(label, results[i].uses_physical_patch == FALSE);
        snprintf(label, sizeof(label), "unsupported result %zu status", i);
        check_status(label, results[i].status, PEAK_DETACH_STATUS_UNSUPPORTED);
    }
    check_true("unsupported batch leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("unsupported batch leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);
    check_true("unsupported batch idle finish",
               peak_detach_controller_finish_hook_mutation_batch(&finish_status) == TRUE);
    check_status("unsupported batch idle finish status",
                 finish_status,
                 PEAK_DETACH_STATUS_SAFE);

    PeakDetachRequest duplicate_hook_requests[2] = {
        {
            .hook_id = 81,
            .symbol_name = "duplicate_hook_one",
            .function_address = (gpointer)(uintptr_t)0x8100,
            .interceptor = (GumInterceptor*)(uintptr_t)0x8110,
            .listener = (GumInvocationListener*)(uintptr_t)0x8120,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 81,
            .symbol_name = "duplicate_hook_two",
            .function_address = (gpointer)(uintptr_t)0x8200,
            .interceptor = (GumInterceptor*)(uintptr_t)0x8210,
            .listener = (GumInvocationListener*)(uintptr_t)0x8220,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    status = PEAK_DETACH_STATUS_SAFE;
    prepared_count = 99;
    memset(results, 0xff, sizeof(results));
    check_true("duplicate hook batch rejected",
               peak_detach_controller_prepare_hook_mutation_batch(
                   duplicate_hook_requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_int("duplicate hook prepared count", (int)prepared_count, 0);
    check_status("duplicate hook aggregate status",
                 status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("duplicate hook first not prepared",
               results[0].prepared == FALSE);
    check_true("duplicate hook second not prepared",
               results[1].prepared == FALSE);
    check_status("duplicate hook first status",
                 results[0].status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_status("duplicate hook second status",
                 results[1].status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("duplicate hook leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("duplicate hook leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    PeakDetachRequest duplicate_function_requests[2] = {
        {
            .hook_id = 91,
            .symbol_name = "duplicate_function_one",
            .function_address = (gpointer)(uintptr_t)0x9100,
            .interceptor = (GumInterceptor*)(uintptr_t)0x9110,
            .listener = (GumInvocationListener*)(uintptr_t)0x9120,
            .operation = PEAK_DETACH_OPERATION_DETACH
        },
        {
            .hook_id = 92,
            .symbol_name = "duplicate_function_two",
            .function_address = (gpointer)(uintptr_t)0x9100,
            .interceptor = (GumInterceptor*)(uintptr_t)0x9210,
            .listener = (GumInvocationListener*)(uintptr_t)0x9220,
            .operation = PEAK_DETACH_OPERATION_DETACH
        }
    };

    status = PEAK_DETACH_STATUS_SAFE;
    prepared_count = 99;
    memset(results, 0xff, sizeof(results));
    check_true("duplicate function batch rejected",
               peak_detach_controller_prepare_hook_mutation_batch(
                   duplicate_function_requests,
                   2,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_int("duplicate function prepared count", (int)prepared_count, 0);
    check_status("duplicate function aggregate status",
                 status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("duplicate function first not prepared",
               results[0].prepared == FALSE);
    check_true("duplicate function second not prepared",
               results[1].prepared == FALSE);
    check_status("duplicate function first status",
                 results[0].status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_status("duplicate function second status",
                 results[1].status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("duplicate function leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("duplicate function leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    PeakDetachRequest missing_record_request = {
        .hook_id = 101,
        .symbol_name = "missing_reattach_record",
        .function_address = (gpointer)(uintptr_t)0xa100,
        .interceptor = (GumInterceptor*)(uintptr_t)0xa110,
        .listener = (GumInvocationListener*)(uintptr_t)0xa120,
        .operation = PEAK_DETACH_OPERATION_REATTACH
    };

    status = PEAK_DETACH_STATUS_SAFE;
    prepared_count = 99;
    memset(results, 0xff, sizeof(results));
    check_true("missing reattach record batch rejected",
               peak_detach_controller_prepare_hook_mutation_batch(
                   &missing_record_request,
                   1,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_int("missing reattach record prepared count", (int)prepared_count, 0);
    check_status("missing reattach record aggregate status",
                 status,
                 PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    check_true("missing reattach record not prepared",
               results[0].prepared == FALSE);
    check_true("missing reattach record not physical",
               results[0].uses_physical_patch == FALSE);
    check_status("missing reattach record status",
                 results[0].status,
                 PEAK_DETACH_STATUS_CLASSIFY_FAILED);
    check_true("missing reattach record leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("missing reattach record leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    result = failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    gum_deinit_embedded();
    return result;
#endif
}

static int
run_invalid(void)
{
    PeakDetachRequest request = valid_request(PEAK_DETACH_OPERATION_DETACH);
    check_prepare("null request", NULL, FALSE, PEAK_DETACH_STATUS_ERROR);

    request = valid_request(PEAK_DETACH_OPERATION_DETACH);
    request.interceptor = NULL;
    check_prepare("missing interceptor", &request, FALSE, PEAK_DETACH_STATUS_ERROR);

    request = valid_request(PEAK_DETACH_OPERATION_DETACH);
    request.function_address = NULL;
    check_prepare("missing function address",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_ERROR);

    request = valid_request(PEAK_DETACH_OPERATION_DETACH);
    request.listener = NULL;
    check_prepare("missing detach listener",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_ERROR);

    request = valid_request(PEAK_DETACH_OPERATION_ATTACH);
    request.listener = NULL;
    check_prepare("missing attach listener",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_ERROR);

    request = valid_request((PeakDetachOperation)999);
    check_prepare("invalid operation",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_ERROR);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int
run_fake_helper_gum_pc_corridor(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-gum-pc-corridor requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    const char* pc_case = getenv("FAKE_DETACH_CONTROLLER_GUM_PC_CASE");
    const char* effective_pc_case =
        pc_case != NULL && pc_case[0] != '\0' ? pc_case : "on-enter";
    char log_template[] = "/tmp/peak_fake_helper_gum_pc_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    GumPeakPcDiagnostics diagnostics;
    guint8 active_patch[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original_prologue[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint prologue_len = 0;
    gpointer stop_pc = NULL;
    gboolean expect_prepared = FALSE;
    const char* operation_case =
        getenv("FAKE_DETACH_CONTROLLER_GUM_PC_OPERATION");
    gboolean expect_metadata_fail =
        operation_case != NULL &&
        g_ascii_strcasecmp(operation_case, "shutdown") == 0;
    int resolved;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);

    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "synthetic-stop", 1) != 0 ||
        setenv("FAKE_DETACH_HELPER_LOG", log_template, 1) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();

    check_true("Gum corridor diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(
                   interceptor,
                   (gpointer)strict_helper_target,
                   listener,
                   &diagnostics) == TRUE);
    if (failures != 0) {
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    resolved = resolve_fake_helper_gum_pc_case(effective_pc_case,
                                               &diagnostics,
                                               &stop_pc,
                                               &expect_prepared);
    if (resolved < 0) {
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }
    if (expect_metadata_fail) {
        expect_prepared = FALSE;
    }
    if (resolved == 0) {
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return 77;
    }
    if (set_fake_helper_pointer_env("FAKE_DETACH_HELPER_STOP_PC",
                                    stop_pc) != 0 ||
        setenv("FAKE_DETACH_HELPER_STOP_TID", "target-pid", 1) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    if (expect_prepared) {
        check_true("Gum corridor function patch",
                   gum_interceptor_peak_get_function_patch(
                       interceptor,
                       (gpointer)strict_helper_target,
                       listener,
                       active_patch,
                       original_prologue,
                       &prologue_len) == TRUE);
        check_true("Gum corridor function patch length",
                   prologue_len > 0 &&
                       prologue_len <= GUM_PEAK_MAX_PROLOGUE_SIZE);
        if (failures != 0) {
            gum_interceptor_detach(interceptor, listener);
            gum_interceptor_flush(interceptor);
            g_object_unref(listener);
            g_object_unref(interceptor);
            gum_deinit_embedded();
            unlink(log_template);
            return EXIT_FAILURE;
        }
        if (setenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS",
                   "WRITE_MEMORY",
                   1) != 0 ||
            set_fake_helper_pointer_env("FAKE_DETACH_HELPER_EXPECT_WRITE_ADDRESS",
                                        (gpointer)strict_helper_target) != 0 ||
            set_fake_helper_uint_env("FAKE_DETACH_HELPER_EXPECT_WRITE_SIZE",
                                     prologue_len) != 0 ||
            set_fake_helper_hex_bytes_env(
                "FAKE_DETACH_HELPER_EXPECT_WRITE_BYTES_HEX",
                original_prologue,
                prologue_len) != 0) {
            perror("setenv");
            gum_interceptor_detach(interceptor, listener);
            gum_interceptor_flush(interceptor);
            g_object_unref(listener);
            g_object_unref(interceptor);
            gum_deinit_embedded();
            unlink(log_template);
            return EXIT_FAILURE;
        }
    } else {
        unsetenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS");
        unsetenv("FAKE_DETACH_HELPER_EXPECT_SET_PC");
        unsetenv("FAKE_DETACH_HELPER_EXPECT_SET_PC_TID");
        unsetenv("FAKE_DETACH_HELPER_EXPECT_WRITE_ADDRESS");
        unsetenv("FAKE_DETACH_HELPER_EXPECT_WRITE_SIZE");
        unsetenv("FAKE_DETACH_HELPER_EXPECT_WRITE_BYTES_HEX");
    }

    PeakDetachRequest request = {
        .hook_id = 111,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = expect_metadata_fail
            ? PEAK_DETACH_OPERATION_SHUTDOWN
            : PEAK_DETACH_OPERATION_DETACH
    };

    if (expect_prepared) {
        check_prepare("Gum entry-byte corridor",
                      &request,
                      TRUE,
                      PEAK_DETACH_STATUS_SAFE);
        check_true("Gum entry-byte corridor holds helper threads",
                   peak_detach_controller_threads_are_held() == TRUE);
        check_true("Gum entry-byte corridor uses physical patch",
                   peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
        check_finish("Gum entry-byte corridor finish",
                     &request,
                     PEAK_DETACH_STATUS_SAFE);
        check_true("Gum entry-byte corridor released helper threads",
                   peak_detach_controller_threads_are_held() == FALSE);
    } else {
        check_prepare("Gum entry-byte corridor prepares",
                      &request,
                      FALSE,
                      PEAK_DETACH_STATUS_CLASSIFY_FAILED);
        if (strcmp(effective_pc_case, "patch-entry-plus-one") == 0) {
            const PeakDetachFailureDetail* detail =
                peak_detach_controller_last_failure_detail();
            check_string("patch-entry failure reason",
                         detail->reason,
                         expect_metadata_fail ? "shutdown-patch-interior-pc"
                                              : "detach-patch-interior-pc");
            check_true("patch-entry failure pc",
                       detail->pc == (uintptr_t)stop_pc);
        }
        check_true("Gum entry-byte corridor holds helper threads",
                   peak_detach_controller_threads_are_held() == FALSE);
        check_true("Gum entry-byte corridor uses physical patch",
                   peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);
    }

    check_true("Gum corridor helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_status("Gum corridor helper shutdown status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);

    check_helper_log_count(log_template, "START", 1);
    check_helper_log_count(log_template, "STOP", 1);
    check_helper_log_count(log_template,
                           "EVACUATE",
                           expect_prepared ? 1 : 0);
    check_helper_log_count(log_template, "RESUME", 1);
    check_helper_log_count(log_template, "SHUTDOWN", 1);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}


static int
run_fake_helper_reattach_patch_entry(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-reattach-patch-entry requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    const char* pc_case = getenv("FAKE_DETACH_CONTROLLER_REATTACH_PATCH_PC_CASE");
    char log_template[] = "/tmp/peak_fake_helper_reattach_patch_entry_log_XXXXXX";
    int log_fd = mkstemp(log_template);
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    GumPeakPcDiagnostics diagnostics;
    PeakDetachRequest request;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    gpointer stop_pc = NULL;

    if (log_fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(log_fd);

    if (pc_case == NULL || pc_case[0] == '\0') {
        pc_case = "entry";
    }

    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "success-zero", 1) != 0 ||
        setenv("FAKE_DETACH_HELPER_LOG", log_template, 1) != 0 ||
        setenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS", "WRITE_MEMORY", 1) != 0) {
        perror("setenv");
        unlink(log_template);
        return EXIT_FAILURE;
    }
    unsetenv("FAKE_DETACH_HELPER_STOP_PC");
    unsetenv("FAKE_DETACH_HELPER_STOP_TID");

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();

    check_true("reattach patch-entry diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(
                   interceptor,
                   (gpointer)strict_helper_target,
                   listener,
                   &diagnostics) == TRUE);
    if (failures != 0) {
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    if (strcmp(pc_case, "entry") == 0) {
        stop_pc = (gpointer)strict_helper_target;
    } else if (strcmp(pc_case, "entry-plus-one") == 0) {
        if (diagnostics.overwritten_prologue_len <= 1) {
            gum_interceptor_detach(interceptor, listener);
            gum_interceptor_flush(interceptor);
            g_object_unref(listener);
            g_object_unref(interceptor);
            gum_deinit_embedded();
            unlink(log_template);
            return 77;
        }
        stop_pc = (gpointer)((guint8*)strict_helper_target + 1);
    } else {
        fprintf(stderr, "unknown reattach patch-entry case: %s\n", pc_case);
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    request = (PeakDetachRequest){
        .hook_id = 113,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_prepare("reattach patch-entry initial detach",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("reattach patch-entry initial detach physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("reattach patch-entry initial detach finish",
                 &request,
                 PEAK_DETACH_STATUS_SAFE);
    check_true("reattach patch-entry initial helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_status("reattach patch-entry initial shutdown status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);

    if (setenv("FAKE_DETACH_HELPER_SCENARIO", "synthetic-stop", 1) != 0 ||
        set_fake_helper_pointer_env("FAKE_DETACH_HELPER_STOP_PC", stop_pc) != 0 ||
        setenv("FAKE_DETACH_HELPER_STOP_TID", "target-pid", 1) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    if (setenv("FAKE_DETACH_HELPER_EXPECT_EVACUATE_ACTIONS",
               strcmp(pc_case, "entry-plus-one") == 0
                   ? "SINGLE_STEP_OUT_OF_RANGE,WRITE_MEMORY"
                   : "WRITE_MEMORY",
               1) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        unlink(log_template);
        return EXIT_FAILURE;
    }

    request.operation = PEAK_DETACH_OPERATION_REATTACH;
    check_prepare("reattach patch-entry prepares",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("reattach patch-entry physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("reattach patch-entry finish",
                 &request,
                 PEAK_DETACH_STATUS_SAFE);

    check_true("reattach patch-entry helper shutdown",
               peak_detach_controller_shutdown_helper(&status) == TRUE);
    check_status("reattach patch-entry helper shutdown status",
                 status,
                 PEAK_DETACH_STATUS_SAFE);

    check_helper_log_count(log_template, "START", 2);
    check_helper_log_count(log_template, "STOP", 2);
    check_helper_log_count(log_template, "EVACUATE", 2);
    check_helper_log_count(log_template, "RESUME", 2);
    check_helper_log_count(log_template, "SHUTDOWN", 2);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    unlink(log_template);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}


#if defined(__linux__) && defined(PEAK_HAVE_GUM_PEAK_PC_API)
/* The wrapper forwards every delivery to the real SI_QUEUE sender. Only the
 * controller's sends are counted; the seam calls __real_ directly. */
static void supported_after_preflight(int epoch);
static void supported_observe_send(pid_t, int, unsigned long, int, int);
static atomic_int receipt_consumed_count;
static atomic_int receipt_consumed_epoch, receipt_consumed_tid, receipt_consumed_signum;
static atomic_ulong receipt_consumed_cookie;
static atomic_int receipt_fail_next_send;
static int receipt_failed_tid, receipt_failed_signum;
static unsigned long receipt_failed_cookie;
static uint32_t receipt_test_capacity = PEAK_DETACH_HELPER_MAX_THREADS;
static int receipt_control_replay, receipt_control_replay_rc;
static void receipt_control_after_preflight(int epoch);

uint32_t peak_test_signal_receipt_capacity(void)
{
    return receipt_test_capacity;
}

void peak_test_signal_retired_receipt_consumed(int signum, int epoch,
                                              pid_t tid, unsigned long cookie)
{
    atomic_store_explicit(&receipt_consumed_signum, signum, memory_order_relaxed);
    atomic_store_explicit(&receipt_consumed_epoch, epoch, memory_order_relaxed);
    atomic_store_explicit(&receipt_consumed_tid, (int)tid, memory_order_relaxed);
    atomic_store_explicit(&receipt_consumed_cookie, cookie, memory_order_relaxed);
    atomic_fetch_add_explicit(&receipt_consumed_count, 1, memory_order_release);
}

static atomic_int preflight_normal_sends;
static atomic_int preflight_tracking;
static atomic_int preflight_worker_tid;
static atomic_int preflight_worker_running;
static atomic_int preflight_handler_returned;
static atomic_int preflight_callback_count;
static int preflight_inject;
static int preflight_unexpected_before;
static int preflight_injection_rc;
static int preflight_seen_epoch;

extern int __real_peak_signal_policy_send_thread_signal(pid_t, int, unsigned long);
int
__wrap_peak_signal_policy_send_thread_signal(pid_t tid, int signo,
                                            unsigned long cookie)
{
    if (atomic_load(&preflight_tracking))
        atomic_fetch_add(&preflight_normal_sends, 1);
    if (atomic_exchange(&receipt_fail_next_send, 0)) {
        receipt_failed_tid = (int)tid;
        receipt_failed_signum = signo;
        receipt_failed_cookie = cookie;
        errno = EAGAIN;
        return -1;
    }
    int rc = __real_peak_signal_policy_send_thread_signal(tid, signo, cookie);
    int saved_errno = errno;
    supported_observe_send(tid, signo, cookie, rc, saved_errno);
    errno = saved_errno;
    return rc;
}

static double
preflight_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

void
peak_test_after_signal_mask_preflight(int epoch)
{
    supported_after_preflight(epoch);
    receipt_control_after_preflight(epoch);
    if (!preflight_inject)
        return;
    preflight_seen_epoch = epoch;
    pid_t tid = atomic_load(&preflight_worker_tid);
    int signo = peak_detach_controller_test_signal_backend_signum();
    unsigned long cookie = peak_signal_policy_cookie_for(epoch - 1, tid);
    preflight_injection_rc = __real_peak_signal_policy_send_thread_signal(
        tid, signo, cookie);
    double deadline = preflight_now() + 2.0;
    /* The worker publishes this only after the real handler has returned to
     * application code, so no temporary handler mask survives this seam. */
    while (!atomic_load(&preflight_handler_returned) &&
           preflight_now() < deadline)
        usleep(1000);
    fprintf(stderr,
            "preflight-injection epoch=%d wrong_epoch=%d tid=%d signum=%d "
            "si_code=%d cookie=0x%lx rc=%d unexpected_before=%d "
            "unexpected_after=%d handler_returned=%d\n",
            epoch, epoch - 1, tid, signo, SI_QUEUE, cookie,
            preflight_injection_rc, preflight_unexpected_before,
            peak_signal_policy_unexpected_delivery_count(),
            atomic_load(&preflight_handler_returned));
}

static void
receipt_control_after_preflight(int epoch)
{
    if (!receipt_control_replay)
        return;
    receipt_control_replay_rc = __real_peak_signal_policy_send_thread_signal(
        receipt_failed_tid, receipt_failed_signum, receipt_failed_cookie);
    double deadline = preflight_now() + 2.0;
    while (!atomic_load(&preflight_handler_returned) && preflight_now() < deadline)
        usleep(1000);
    fprintf(stderr, "receipt-failed-send-replay new_epoch=%d tid=%d S=%d cookie=0x%lx "
            "rc=%d handler_returned=%d unexpected=%d\n",
            epoch, receipt_failed_tid, receipt_failed_signum, receipt_failed_cookie,
            receipt_control_replay_rc, atomic_load(&preflight_handler_returned),
            peak_signal_policy_unexpected_delivery_count());
}

static void*
preflight_worker(void* unused)
{
    (void)unused;
    atomic_store(&preflight_worker_tid, (int)syscall(SYS_gettid));
    while (atomic_load(&preflight_worker_running)) {
        if (peak_signal_policy_unexpected_delivery_count() ==
            preflight_unexpected_before + 1)
            atomic_store(&preflight_handler_returned, 1);
        usleep(1000);
    }
    return NULL;
}

static void
preflight_on_enter(GumInvocationContext* context, gpointer unused)
{
    (void)context;
    (void)unused;
    atomic_fetch_add(&preflight_callback_count, 1);
}

static void*
preflight_gate_probe(void* unused)
{
    (void)unused;
    peak_detach_controller_wait_for_mutation_window();
    return NULL;
}

static void
preflight_dump_bytes(const char* stage, const uint8_t* bytes, guint length)
{
    fprintf(stderr, "preflight-bytes stage=%s len=%u hex=", stage, length);
    for (guint i = 0; i < length; i++)
        fprintf(stderr, "%02x", bytes[i]);
    fputc('\n', stderr);
}
#endif

static int
run_signal_preflight_contamination(int batch, int inject)
{
#if !defined(__linux__) || !defined(PEAK_HAVE_GUM_PEAK_PC_API)
    (void)batch;
    (void)inject;
    return 77;
#else
    uint8_t original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    uint8_t active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    uint8_t after[GUM_PEAK_MAX_PROLOGUE_SIZE];
    GumPeakPcDiagnostics before_diag = {0}, after_diag = {0};
    pthread_t worker;
    preflight_inject = 0;
    preflight_seen_epoch = 0;
    preflight_injection_rc = -1;
    atomic_store(&preflight_worker_tid, 0);
    atomic_store(&preflight_handler_returned, 0);
    atomic_store(&preflight_callback_count, 0);
    preflight_unexpected_before = peak_signal_policy_unexpected_delivery_count();
    memcpy(original, (void*)strict_helper_target, sizeof(original));
    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener =
        gum_make_call_listener(preflight_on_enter, NULL, NULL, NULL);
    check_int("preflight fixture Gum attach",
              gum_interceptor_attach(interceptor, (gpointer)strict_helper_target,
                                     listener, NULL), GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    check_true("preflight fixture diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(interceptor,
                   (gpointer)strict_helper_target, listener, &before_diag));
    guint length = before_diag.overwritten_prologue_len;
    if (length == 0 || length > sizeof(active)) {
        fprintf(stderr, "invalid preflight fixture prologue length=%u\n", length);
        failures++;
        goto cleanup_gum;
    }
    memcpy(active, (void*)strict_helper_target, length);
    check_true("preflight fixture active entry differs from original",
               memcmp(active, original, length) != 0);
    strict_helper_target();
    check_int("preflight fixture initial callback",
              atomic_load(&preflight_callback_count), 1);
    peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    /* Initialize reservation before the worker starts and before preflight. */
    int signo = peak_detach_controller_test_signal_backend_signum();
    check_true("preflight fixture reserved signal", signo > 0);
    atomic_store(&preflight_worker_running, 1);
    if (pthread_create(&worker, NULL, preflight_worker, NULL) != 0) {
        failures++;
        goto cleanup_gum;
    }
    double deadline = preflight_now() + 2.0;
    while (atomic_load(&preflight_worker_tid) == 0 && preflight_now() < deadline)
        usleep(1000);
    check_true("preflight worker ready", atomic_load(&preflight_worker_tid) > 0);

    PeakDetachRequest request = {
        .hook_id = 129, .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor, .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };
    preflight_inject = inject;
    for (int operation = 0; operation < (inject ? 1 : 2); operation++) {
        PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
        PeakDetachBatchResult result = {0};
        size_t count = SIZE_MAX;
        atomic_store(&preflight_normal_sends, 0);
        atomic_store(&preflight_tracking, 1);
        gboolean prepared = batch
            ? peak_detach_controller_prepare_hook_mutation_batch(
                &request, 1, &result, &count, &status)
            : peak_detach_controller_prepare_hook_mutation(&request, &status);
        atomic_store(&preflight_tracking, 0);
        preflight_inject = 0;
        gboolean held = peak_detach_controller_threads_are_held();
        gboolean physical = peak_detach_controller_current_mutation_uses_physical_patch();
        const PeakDetachFailureDetail* detail = peak_detach_controller_last_failure_detail();
        const char* reason = detail != NULL && detail->reason != NULL
            ? detail->reason : "<none>";
        memcpy(after, (void*)strict_helper_target, length);
        preflight_dump_bytes("original", original, length);
        preflight_dump_bytes("active-before", active, length);
        preflight_dump_bytes(operation == 0 ? "after-detach-prepare" :
                             "after-reattach-prepare", after, length);
        fprintf(stderr,
                "preflight-result batch=%d inject=%d operation=%d prepared=%d "
                "status=%s reason=%s normal_sends=%d held=%d physical=%d "
                "count=%zu result_prepared=%d result_physical=%d "
                "entry_equal_active=%d\n",
                batch, inject, operation, prepared,
                peak_detach_controller_status_string(status), reason,
                atomic_load(&preflight_normal_sends), held, physical, count,
                result.prepared, result.uses_physical_patch,
                memcmp(after, active, length) == 0);
        if (inject) {
            fprintf(stderr, "preflight-detail tid=%ld pc=%" PRIuPTR " aux=%" PRIuPTR "\n",
                    detail != NULL ? detail->tid : -1,
                    detail != NULL ? detail->pc : 0,
                    detail != NULL ? detail->aux : 0);
            check_true("contaminated preflight precise failure detail",
                       detail != NULL && detail->tid == 0 &&
                       detail->pc == (uintptr_t)signo &&
                       detail->aux == (uintptr_t)(preflight_unexpected_before + 1));
            check_true("preflight injection delivered and handler returned",
                       preflight_seen_epoch > 0 && preflight_injection_rc == 0 &&
                       atomic_load(&preflight_handler_returned) &&
                       peak_signal_policy_unexpected_delivery_count() ==
                           preflight_unexpected_before + 1);
            check_true("contaminated preflight sends no owned stop packets",
                       atomic_load(&preflight_normal_sends) == 0);
            check_true("contaminated preflight refuses all permission",
                       !prepared && status == PEAK_DETACH_STATUS_UNSUPPORTED &&
                       !held && !physical &&
                       strcmp(reason, "signal-unexpected-delivery") == 0);
            if (batch)
                check_true("contaminated batch result refuses all permission",
                           count == 0 && !result.prepared &&
                           !result.uses_physical_patch &&
                           result.status == PEAK_DETACH_STATUS_UNSUPPORTED);
            check_true("contaminated preflight preserves entry bytes",
                       memcmp(after, active, length) == 0);
        } else {
            check_true("clean signal prepare physical permission",
                       prepared && status == PEAK_DETACH_STATUS_SAFE && held && physical &&
                       atomic_load(&preflight_normal_sends) > 0);
            if (batch)
                check_true("clean batch result physical permission",
                           count == 1 && result.prepared && result.uses_physical_patch);
            check_true("clean physical entry bytes",
                       memcmp(after, operation == 0 ? original : active, length) == 0);
        }
        /* Baseline may already have changed entry bytes and stopped workers.
         * Always finish before callbacks, diagnostics or ordinary cleanup. */
        if (prepared)
            check_true("preflight successful prepare finish",
                       batch ? peak_detach_controller_finish_hook_mutation_batch(&status)
                             : peak_detach_controller_finish_hook_mutation(&request, &status));
        memset(&after_diag, 0, sizeof(after_diag));
        check_true("preflight post-prepare Gum diagnostics",
                   gum_interceptor_peak_get_pc_diagnostics(interceptor,
                       (gpointer)strict_helper_target, listener, &after_diag));
        int diag_equal = memcmp(&before_diag, &after_diag, sizeof(before_diag)) == 0;
        check_true("preflight preserves Gum diagnostics", diag_equal);
        int callbacks_before = atomic_load(&preflight_callback_count);
        strict_helper_target();
        int callback_delta = atomic_load(&preflight_callback_count) - callbacks_before;
        fprintf(stderr, "preflight-after-finish batch=%d inject=%d operation=%d "
                "diagnostics_equal=%d callback_delta=%d gate_epoch=%d\n",
                batch, inject, operation, diag_equal, callback_delta,
                peak_detach_controller_test_thread_creation_gate_epoch());
        check_int("preflight callback behavior", callback_delta,
                  inject || operation == 1 ? 1 : 0);
        check_int("preflight cleanup creation gate open",
                  peak_detach_controller_test_thread_creation_gate_epoch(), 0);
        request.operation = PEAK_DETACH_OPERATION_REATTACH;
    }
    /* The original target remains alive through every prepare and assertion. */
    check_int("preflight target still alive",
              (int)syscall(SYS_tgkill, getpid(), atomic_load(&preflight_worker_tid), 0), 0);
    pthread_t probe;
    if (pthread_create(&probe, NULL, preflight_gate_probe, NULL) == 0)
        pthread_join(probe, NULL);
    else
        failures++;
    struct sigaction application_action = {0}, previous_action = {0};
    sigemptyset(&application_action.sa_mask);
    application_action.sa_handler = SIG_DFL;
    errno = 0;
    int action_rc = sigaction(signo, &application_action, &previous_action);
    fprintf(stderr, "preflight-application-sigaction signum=%d rc=%d errno=%d\n",
            signo, action_rc, errno);
    check_int("ordinary application sigaction remains accepted", action_rc, 0);
    atomic_store(&preflight_worker_running, 0);
    pthread_join(worker, NULL);
cleanup_gum:
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    fprintf(stderr, "preflight-complete batch=%d inject=%d failures=%d\n",
            batch, inject, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

#if defined(__linux__) && defined(PEAK_HAVE_GUM_PEAK_PC_API)
static int supported_entered_pipe[2] = {-1, -1};
static int supported_done_pipe[2] = {-1, -1};
static pthread_t supported_thread;
static atomic_int supported_tid;
static atomic_int supported_running;
static atomic_int supported_returned;
static atomic_int supported_callbacks;
static atomic_int supported_release_started;
static atomic_int supported_replay_enabled;
static atomic_int supported_replay_returned;
static int supported_replay_before;
static int supported_replay_rc;
static int supported_signum;
static int supported_enabled;
static int supported_epoch;
static int supported_pthread_kill_rc;
static int supported_entered;
static int supported_handler_mask_s;
static int supported_normal_send_count;
static int supported_owned_rc;
static int supported_owned_pending_s;
static unsigned long supported_owned_cookie;

static double
supported_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int
supported_status_bit(pid_t tid, const char* field, int signo)
{
    char path[96], line[256];
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    FILE* file = fopen(path, "r");
    if (file == NULL)
        return -1;
    int bit = -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long mask;
        if (strncmp(line, field, strlen(field)) == 0 &&
            sscanf(line + strlen(field), "%llx", &mask) == 1) {
            bit = (int)((mask >> (signo - 1)) & 1);
            break;
        }
    }
    fclose(file);
    return bit;
}

static void
supported_application_handler(int signo)
{
    int saved_errno = errno;
    char byte = 'E';
    ssize_t result;
    (void)signo;
    do { result = write(supported_entered_pipe[1], &byte, 1); }
    while (result < 0 && errno == EINTR);
    do { result = read(supported_done_pipe[0], &byte, 1); }
    while (result < 0 && errno == EINTR);
    errno = saved_errno;
}

static int
supported_read_ack(int fd)
{
    double deadline = supported_now() + 2.0;
    for (;;) {
        double remaining = deadline - supported_now();
        if (remaining <= 0)
            return -1;
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        int rc = poll(&poll_fd, 1, (int)(remaining * 1000) + 1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0 || !(poll_fd.revents & POLLIN))
            return -1;
        char byte;
        return read(fd, &byte, 1) == 1 ? 0 : -1;
    }
}

static void
supported_release_handler(void)
{
    char byte = 'D';
    ssize_t rc;
    atomic_store(&supported_release_started, 1);
    do { rc = write(supported_done_pipe[1], &byte, 1); }
    while (rc < 0 && errno == EINTR);
}

static void
supported_after_preflight(int epoch)
{
    if (atomic_load(&supported_replay_enabled)) {
        pid_t tid = atomic_load(&supported_tid);
        supported_replay_rc = __real_peak_signal_policy_send_thread_signal(
            tid, supported_signum, supported_owned_cookie);
        double deadline = supported_now() + 2.0;
        while (!atomic_load(&supported_replay_returned) && supported_now() < deadline)
            usleep(1000);
        fprintf(stderr, "supported-consumed-cookie-replay old_epoch=%d new_epoch=%d "
                "tid=%d S=%d old_cookie=0x%lx rc=%d handler_returned=%d "
                "unexpected_before=%d unexpected_after=%d\n",
                supported_epoch, epoch, tid, supported_signum,
                supported_owned_cookie, supported_replay_rc,
                atomic_load(&supported_replay_returned), supported_replay_before,
                peak_signal_policy_unexpected_delivery_count());
        return;
    }
    if (!supported_enabled)
        return;
    supported_epoch = epoch;
    supported_pthread_kill_rc = pthread_kill(supported_thread, SIGUSR1);
    supported_entered = supported_pthread_kill_rc == 0 &&
        supported_read_ack(supported_entered_pipe[0]) == 0;
    supported_handler_mask_s = supported_status_bit(
        atomic_load(&supported_tid), "SigBlk:", supported_signum);
    fprintf(stderr, "supported-preflight epoch=%d tid=%d P=%d S=%d "
            "pthread_kill_rc=%d entered=%d handler_mask_S=%d\n",
            epoch, atomic_load(&supported_tid), SIGUSR1, supported_signum,
            supported_pthread_kill_rc, supported_entered,
            supported_handler_mask_s);
}

static void
supported_observe_send(pid_t tid, int signo, unsigned long cookie,
                       int rc, int saved_errno)
{
    if (supported_enabled) {
        supported_normal_send_count++;
        supported_owned_rc = rc;
        supported_owned_cookie = cookie;
        supported_owned_pending_s = supported_status_bit(tid, "SigPnd:", signo);
        fprintf(stderr, "supported-owned-send epoch=%d tid=%d S=%d "
                "cookie=0x%lx expected_cookie=0x%lx rc=%d errno=%d "
                "thread_SigPnd_S=%d thread_SigBlk_S=%d\n",
                supported_epoch, tid, signo, cookie,
                peak_signal_policy_cookie_for(supported_epoch, tid), rc,
                saved_errno, supported_owned_pending_s,
                supported_status_bit(tid, "SigBlk:", signo));
    }
}

static void*
supported_worker(void* unused)
{
    (void)unused;
    atomic_store(&supported_tid, (int)syscall(SYS_gettid));
    while (atomic_load(&supported_running)) {
        /* This is application code reached only after H_P and the subsequently
         * delivered real PEAK handler have returned. */
        if (atomic_load(&supported_release_started))
            atomic_store(&supported_returned, 1);
        if (atomic_load(&supported_replay_enabled) &&
            peak_signal_policy_unexpected_delivery_count() == supported_replay_before + 1)
            atomic_store(&supported_replay_returned, 1);
        usleep(1000);
    }
    return NULL;
}

static void
supported_on_enter(GumInvocationContext* context, gpointer unused)
{
    (void)context;
    (void)unused;
    atomic_fetch_add(&supported_callbacks, 1);
}
#endif

static int
run_supported_handler_recovery(void)
{
#if !defined(__linux__) || !defined(PEAK_HAVE_GUM_PEAK_PC_API)
    return 77;
#else
    GumPeakPcDiagnostics before_diag = {0}, after_diag = {0};
    uint8_t before_bytes[GUM_PEAK_MAX_PROLOGUE_SIZE];
    uint8_t original_bytes[GUM_PEAK_MAX_PROLOGUE_SIZE];
    uint8_t after_bytes[GUM_PEAK_MAX_PROLOGUE_SIZE];
    int worker_started = 0;
    struct sigaction action = {0}, queried = {0}, previous = {0};
    supported_signum = peak_detach_controller_test_signal_backend_signum();
    check_true("supported reserved signal differs from P", supported_signum > 0 &&
               supported_signum <= 64 && supported_signum != SIGUSR1);
    check_int("supported initial unexpected count",
              peak_signal_policy_unexpected_delivery_count(), 0);
    if (pipe(supported_entered_pipe) != 0 || pipe(supported_done_pipe) != 0) {
        perror("supported pipe");
        return EXIT_FAILURE;
    }
    action.sa_handler = supported_application_handler;
    sigfillset(&action.sa_mask);
    action.sa_flags = 0;
    errno = 0;
    int action_rc = sigaction(SIGUSR1, &action, &previous);
    int query_rc = sigaction(SIGUSR1, NULL, &queried);
    fprintf(stderr, "supported-application-sigaction P=%d S=%d rc=%d query_rc=%d "
            "handler_equal=%d requested_flags=0 queried_flags=0x%x mask_P=%d mask_S=%d\n",
            SIGUSR1, supported_signum, action_rc, query_rc,
            queried.sa_handler == supported_application_handler, queried.sa_flags,
            sigismember(&queried.sa_mask, SIGUSR1),
            sigismember(&queried.sa_mask, supported_signum));
    check_true("ordinary sigaction with full handler mask accepted",
               action_rc == 0 && query_rc == 0 &&
               queried.sa_handler == supported_application_handler &&
               sigismember(&queried.sa_mask, supported_signum) == 1 &&
               (queried.sa_flags & (SA_NODEFER | SA_RESETHAND | SA_SIGINFO)) == 0);
    memcpy(original_bytes, (void*)strict_helper_target, sizeof(original_bytes));
    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener =
        gum_make_call_listener(supported_on_enter, NULL, NULL, NULL);
    check_int("supported Gum attach",
              gum_interceptor_attach(interceptor, (gpointer)strict_helper_target,
                                     listener, NULL), GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    check_true("supported initial Gum diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(interceptor,
                   (gpointer)strict_helper_target, listener, &before_diag));
    guint length = before_diag.overwritten_prologue_len;
    if (length == 0 || length > sizeof(before_bytes)) {
        failures++;
        goto cleanup;
    }
    memcpy(before_bytes, (void*)strict_helper_target, length);
    strict_helper_target();
    check_int("supported initial callback", atomic_load(&supported_callbacks), 1);
    peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    atomic_store(&supported_running, 1);
    if (pthread_create(&supported_thread, NULL, supported_worker, NULL) != 0) {
        failures++;
        goto cleanup;
    }
    worker_started = 1;
    double deadline = supported_now() + 2.0;
    while (atomic_load(&supported_tid) == 0 && supported_now() < deadline)
        usleep(1000);
    int initial_mask_p = supported_status_bit(atomic_load(&supported_tid), "SigBlk:", SIGUSR1);
    int initial_mask_s = supported_status_bit(atomic_load(&supported_tid), "SigBlk:", supported_signum);
    fprintf(stderr, "supported-initial-mask tid=%d P=%d S=%d mask_P=%d mask_S=%d\n",
            atomic_load(&supported_tid), SIGUSR1, supported_signum,
            initial_mask_p, initial_mask_s);
    check_true("supported worker initially unblocks P and S",
               atomic_load(&supported_tid) > 0 && initial_mask_p == 0 && initial_mask_s == 0);
    if (failures != 0)
        goto cleanup;
    PeakDetachRequest request = {
        .hook_id = 130, .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor, .listener = listener,
        .operation = PEAK_DETACH_OPERATION_REATTACH
    };
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    supported_enabled = 1;
    double started = supported_now();
    gboolean prepared = peak_detach_controller_prepare_hook_mutation(&request, &status);
    double elapsed = supported_now() - started;
    supported_enabled = 0;
    gboolean held = peak_detach_controller_threads_are_held();
    gboolean physical = peak_detach_controller_current_mutation_uses_physical_patch();
    /* Even an unexpected successful prepare must release before any target call. */
    if (prepared)
        check_true("supported unexpected prepare finish",
                   peak_detach_controller_finish_hook_mutation(&request, &status));
    int unexpected_at_return = peak_signal_policy_unexpected_delivery_count();
    memcpy(after_bytes, (void*)strict_helper_target, length);
    fprintf(stderr, "supported-entry-before len=%u hex=", length);
    for (guint i = 0; i < length; i++) fprintf(stderr, "%02x", before_bytes[i]);
    fprintf(stderr, "\nsupported-entry-after len=%u hex=", length);
    for (guint i = 0; i < length; i++) fprintf(stderr, "%02x", after_bytes[i]);
    fputc('\n', stderr);
    check_true("supported post-timeout Gum diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(interceptor,
                   (gpointer)strict_helper_target, listener, &after_diag));
    int callback_before = atomic_load(&supported_callbacks);
    strict_helper_target();
    int callback_delta = atomic_load(&supported_callbacks) - callback_before;
    fprintf(stderr, "supported-first-prepare prepared=%d status=%s elapsed=%.9f "
            "unexpected=%d held=%d physical=%d bytes_equal=%d diagnostics_equal=%d "
            "callback_delta=%d gate_epoch=%d\n",
            prepared, peak_detach_controller_status_string(status), elapsed,
            unexpected_at_return, held, physical,
            memcmp(before_bytes, after_bytes, length) == 0,
            memcmp(&before_diag, &after_diag, sizeof(before_diag)) == 0,
            callback_delta, peak_detach_controller_test_thread_creation_gate_epoch());
    check_true("ordinary handler mask creates a real pending owned stop",
               supported_epoch > 0 && supported_pthread_kill_rc == 0 &&
               supported_entered && supported_handler_mask_s == 1 &&
               supported_normal_send_count == 1 && supported_owned_rc == 0 &&
               supported_owned_pending_s == 1 && supported_owned_cookie ==
                   peak_signal_policy_cookie_for(supported_epoch, atomic_load(&supported_tid)));
    check_true("main ordinary handler mask reproduces timeout before mutation",
               !prepared && status == PEAK_DETACH_STATUS_TIMEOUT && elapsed >= 9.0 &&
               elapsed < 13.0 && unexpected_at_return == 0 && !held && !physical &&
               memcmp(before_bytes, after_bytes, length) == 0 &&
               memcmp(&before_diag, &after_diag, sizeof(before_diag)) == 0 &&
               callback_delta == 1 &&
               peak_detach_controller_test_thread_creation_gate_epoch() == 0);
    check_int("supported target alive before releasing H_P",
              (int)syscall(SYS_tgkill, getpid(), atomic_load(&supported_tid), 0), 0);
    supported_release_handler();
    deadline = supported_now() + 2.0;
    while (!atomic_load(&supported_returned) && supported_now() < deadline)
        usleep(1000);
    int unexpected_after = peak_signal_policy_unexpected_delivery_count();
    fprintf(stderr, "supported-real-late-delivery handler_returned=%d unexpected=%d\n",
            atomic_load(&supported_returned), unexpected_after);
    fprintf(stderr, "supported-genuine-consume count=%d epoch=%d tid=%d S=%d cookie=0x%lx\n",
            atomic_load_explicit(&receipt_consumed_count, memory_order_acquire),
            atomic_load(&receipt_consumed_epoch), atomic_load(&receipt_consumed_tid),
            atomic_load(&receipt_consumed_signum), atomic_load(&receipt_consumed_cookie));
    check_true("ordinary H_P return drains original owned packet without contamination",
               atomic_load(&supported_returned) && unexpected_after == 0 &&
               atomic_load_explicit(&receipt_consumed_count, memory_order_acquire) == 1 &&
               atomic_load(&receipt_consumed_epoch) == supported_epoch &&
               atomic_load(&receipt_consumed_tid) == atomic_load(&supported_tid) &&
               atomic_load(&receipt_consumed_signum) == supported_signum &&
               atomic_load(&receipt_consumed_cookie) == supported_owned_cookie);
    int batch = g_getenv("PEAK_TEST_SUPPORTED_BATCH") != NULL;
    for (int operation = 0; operation < 2; operation++) {
        request.operation = operation == 0 ? PEAK_DETACH_OPERATION_DETACH
                                         : PEAK_DETACH_OPERATION_REATTACH;
        status = PEAK_DETACH_STATUS_ERROR;
        PeakDetachBatchResult result = {0};
        size_t count = SIZE_MAX;
        gboolean next_prepared = batch
            ? peak_detach_controller_prepare_hook_mutation_batch(
                &request, 1, &result, &count, &status)
            : peak_detach_controller_prepare_hook_mutation(&request, &status);
        gboolean next_physical = peak_detach_controller_current_mutation_uses_physical_patch();
        const PeakDetachFailureDetail* detail = peak_detach_controller_last_failure_detail();
        fprintf(stderr, "supported-recovery-prepare batch=%d operation=%d prepared=%d "
                "status=%s reason=%s count=%zu physical=%d\n",
                batch, operation, next_prepared,
                peak_detach_controller_status_string(status),
                detail != NULL && detail->reason != NULL ? detail->reason : "<none>",
                count, next_physical);
        check_true("drained own packet permits next physical mutation",
                   next_prepared && status == PEAK_DETACH_STATUS_SAFE && next_physical &&
                   (!batch || (count == 1 && result.prepared && result.uses_physical_patch)));
        /* Unexpected success and ordinary success both finish before callbacks. */
        if (next_prepared)
            check_true("supported recovery physical finish",
                       batch ? peak_detach_controller_finish_hook_mutation_batch(&status)
                             : peak_detach_controller_finish_hook_mutation(&request, &status));
        memcpy(after_bytes, (void*)strict_helper_target, length);
        int calls_before = atomic_load(&supported_callbacks);
        strict_helper_target();
        int calls_delta = atomic_load(&supported_callbacks) - calls_before;
        fprintf(stderr, "supported-recovery-entry batch=%d operation=%d "
                "equal_active=%d callback_delta=%d gate_epoch=%d len=%u hex=",
                batch, operation, memcmp(before_bytes, after_bytes, length) == 0,
                calls_delta, peak_detach_controller_test_thread_creation_gate_epoch(), length);
        for (guint i = 0; i < length; i++) fprintf(stderr, "%02x", after_bytes[i]);
        fputc('\n', stderr);
        check_true("supported recovery physical entry transition",
                   memcmp(operation == 0 ? original_bytes : before_bytes,
                          after_bytes, length) == 0);
        memset(&after_diag, 0, sizeof(after_diag));
        check_true("supported recovery keeps Gum metadata",
                   gum_interceptor_peak_get_pc_diagnostics(interceptor,
                       (gpointer)strict_helper_target, listener, &after_diag) &&
                   memcmp(&before_diag, &after_diag, sizeof(before_diag)) == 0);
        check_int("supported recovery callback transition", calls_delta, operation);
        check_int("supported recovery gate open",
                  peak_detach_controller_test_thread_creation_gate_epoch(), 0);
    }
    fprintf(stderr, "supported-after-physical-cycle unexpected=%d consume_count=%d pending_S=%d\n",
            peak_signal_policy_unexpected_delivery_count(), atomic_load(&receipt_consumed_count),
            supported_status_bit(atomic_load(&supported_tid), "SigPnd:", supported_signum));
    check_int("complete physical recovery leaves unexpected zero",
              peak_signal_policy_unexpected_delivery_count(), 0);
    check_int("the genuine canceled packet is consumed once",
              atomic_load(&receipt_consumed_count), 1);
    if (g_getenv("PEAK_TEST_SUPPORTED_REPLAY") != NULL) {
        supported_replay_before = peak_signal_policy_unexpected_delivery_count();
        atomic_store(&supported_replay_returned, 0);
        atomic_store(&supported_replay_enabled, 1);
        atomic_store(&preflight_normal_sends, 0);
        atomic_store(&preflight_tracking, 1);
        request.operation = PEAK_DETACH_OPERATION_DETACH;
        status = PEAK_DETACH_STATUS_ERROR;
        gboolean replay_prepared = peak_detach_controller_prepare_hook_mutation(&request, &status);
        atomic_store(&preflight_tracking, 0);
        atomic_store(&supported_replay_enabled, 0);
        const PeakDetachFailureDetail* replay_detail = peak_detach_controller_last_failure_detail();
        int replay_held = peak_detach_controller_threads_are_held();
        int replay_physical = peak_detach_controller_current_mutation_uses_physical_patch();
        memcpy(after_bytes, (void*)strict_helper_target, length);
        if (replay_prepared)
            check_true("consumed-cookie unexpected prepare finish",
                       peak_detach_controller_finish_hook_mutation(&request, &status));
        int calls_before = atomic_load(&supported_callbacks);
        strict_helper_target();
        int calls_delta = atomic_load(&supported_callbacks) - calls_before;
        fprintf(stderr, "supported-consumed-cookie-guard prepared=%d status=%s "
                "reason=%s normal_sends=%d held=%d physical=%d entry_equal=%d "
                "callback_delta=%d gate_epoch=%d\n",
                replay_prepared, peak_detach_controller_status_string(status),
                replay_detail != NULL && replay_detail->reason != NULL ? replay_detail->reason : "<none>",
                atomic_load(&preflight_normal_sends), replay_held, replay_physical,
                memcmp(before_bytes, after_bytes, length) == 0, calls_delta,
                peak_detach_controller_test_thread_creation_gate_epoch());
        check_true("consumed canceled identity is not accepted twice",
                   supported_replay_before == 0 && supported_replay_rc == 0 &&
                   atomic_load(&supported_replay_returned) &&
                   peak_signal_policy_unexpected_delivery_count() == 1 &&
                   !replay_prepared && status == PEAK_DETACH_STATUS_UNSUPPORTED &&
                   replay_detail != NULL && replay_detail->reason != NULL &&
                   strcmp(replay_detail->reason, "signal-unexpected-delivery") == 0 &&
                   replay_detail->tid == 0 && replay_detail->pc == (uintptr_t)supported_signum &&
                   replay_detail->aux == 1 && atomic_load(&preflight_normal_sends) == 0 &&
                   !replay_held && !replay_physical &&
                   memcmp(before_bytes, after_bytes, length) == 0 && calls_delta == 1 &&
                   peak_detach_controller_test_thread_creation_gate_epoch() == 0);
    }
    struct sigaction no_debt_action = {0};
    sigemptyset(&no_debt_action.sa_mask);
    no_debt_action.sa_handler = SIG_DFL;
    errno = 0;
    int no_debt_rc = sigaction(supported_signum, &no_debt_action, NULL);
    fprintf(stderr, "supported-no-current-debt-application-sigaction S=%d rc=%d errno=%d\n",
            supported_signum, no_debt_rc, errno);
    check_int("no-current-debt application sigaction stays accepted", no_debt_rc, 0);
cleanup:
    supported_enabled = 0;
    /* Release first on every path, including assertion failures. */
    supported_release_handler();
    if (worker_started) {
        atomic_store(&supported_running, 0);
        pthread_join(supported_thread, NULL);
    }
    sigaction(SIGUSR1, &previous, NULL);
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    close(supported_entered_pipe[0]); close(supported_entered_pipe[1]);
    close(supported_done_pipe[0]); close(supported_done_pipe[1]);
    fprintf(stderr, "supported-handler-owned-recovery failures=%d\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_signal_receipt_control(int failed_send)
{
#if !defined(__linux__) || !defined(PEAK_HAVE_GUM_PEAK_PC_API)
    (void)failed_send;
    return 77;
#else
    pthread_t worker;
    int worker_started = 0;
    uint8_t before[GUM_PEAK_MAX_PROLOGUE_SIZE], after[GUM_PEAK_MAX_PROLOGUE_SIZE];
    GumPeakPcDiagnostics diagnostics = {0};
    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener = gum_make_call_listener(preflight_on_enter, NULL, NULL, NULL);
    check_int("receipt control attach", gum_interceptor_attach(interceptor,
              (gpointer)strict_helper_target, listener, NULL), GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    check_true("receipt control diagnostics",
               gum_interceptor_peak_get_pc_diagnostics(interceptor,
                   (gpointer)strict_helper_target, listener, &diagnostics));
    guint length = diagnostics.overwritten_prologue_len;
    if (length == 0 || length > sizeof(before)) {
        failures++;
        goto cleanup;
    }
    memcpy(before, (void*)strict_helper_target, length);
    peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    int signo = peak_detach_controller_test_signal_backend_signum();
    preflight_unexpected_before = peak_signal_policy_unexpected_delivery_count();
    atomic_store(&preflight_worker_tid, 0);
    atomic_store(&preflight_handler_returned, 0);
    atomic_store(&preflight_worker_running, 1);
    if (pthread_create(&worker, NULL, preflight_worker, NULL) != 0) {
        failures++;
        goto cleanup;
    }
    worker_started = 1;
    double deadline = preflight_now() + 2.0;
    while (atomic_load(&preflight_worker_tid) == 0 && preflight_now() < deadline)
        usleep(1000);
    check_true("receipt control worker ready", atomic_load(&preflight_worker_tid) > 0);
    receipt_test_capacity = failed_send ? PEAK_DETACH_HELPER_MAX_THREADS : 0;
    atomic_store(&receipt_fail_next_send, failed_send);
    atomic_store(&preflight_normal_sends, 0);
    atomic_store(&preflight_tracking, 1);
    PeakDetachRequest request = {
        .hook_id = 131, .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor, .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    gboolean prepared = peak_detach_controller_prepare_hook_mutation(&request, &status);
    atomic_store(&preflight_tracking, 0);
    receipt_test_capacity = PEAK_DETACH_HELPER_MAX_THREADS;
    const PeakDetachFailureDetail* detail = peak_detach_controller_last_failure_detail();
    int held = peak_detach_controller_threads_are_held();
    int physical = peak_detach_controller_current_mutation_uses_physical_patch();
    memcpy(after, (void*)strict_helper_target, length);
    if (prepared)
        check_true("receipt control unexpected prepare finish",
                   peak_detach_controller_finish_hook_mutation(&request, &status));
    int callbacks_before = atomic_load(&preflight_callback_count);
    strict_helper_target();
    int callbacks = atomic_load(&preflight_callback_count) - callbacks_before;
    fprintf(stderr, "receipt-control failed_send=%d prepared=%d status=%s reason=%s "
            "normal_send_attempts=%d forced_failure_tid=%d forced_failure_S=%d "
            "forced_failure_cookie=0x%lx real_sends=0 held=%d physical=%d "
            "entry_equal=%d callback_delta=%d unexpected=%d consume_count=%d gate=%d\n",
            failed_send, prepared, peak_detach_controller_status_string(status),
            detail != NULL && detail->reason != NULL ? detail->reason : "<none>",
            atomic_load(&preflight_normal_sends), receipt_failed_tid, receipt_failed_signum,
            receipt_failed_cookie, held, physical, memcmp(before, after, length) == 0,
            callbacks, peak_signal_policy_unexpected_delivery_count(),
            atomic_load(&receipt_consumed_count),
            peak_detach_controller_test_thread_creation_gate_epoch());
    check_true("receipt bound or send failure grants no permission",
               !prepared && !held && !physical && memcmp(before, after, length) == 0 &&
               callbacks == 1 && peak_signal_policy_unexpected_delivery_count() == 0 &&
               atomic_load(&receipt_consumed_count) == 0 &&
               peak_detach_controller_test_thread_creation_gate_epoch() == 0);
    if (!failed_send) {
        check_true("receipt capacity refuses before first send",
                   status == PEAK_DETACH_STATUS_UNSUPPORTED &&
                   atomic_load(&preflight_normal_sends) == 0 && detail != NULL &&
                   detail->reason != NULL && strcmp(detail->reason, "signal-receipt-capacity") == 0);
    } else {
        check_true("receipt synthetic send failed without actual delivery",
                   status == PEAK_DETACH_STATUS_TIMEOUT &&
                   atomic_load(&preflight_normal_sends) == 1 &&
                   receipt_failed_tid == atomic_load(&preflight_worker_tid) && receipt_failed_signum == signo);
        receipt_control_replay = 1;
        atomic_store(&preflight_normal_sends, 0);
        atomic_store(&preflight_tracking, 1);
        status = PEAK_DETACH_STATUS_ERROR;
        prepared = peak_detach_controller_prepare_hook_mutation(&request, &status);
        atomic_store(&preflight_tracking, 0);
        receipt_control_replay = 0;
        detail = peak_detach_controller_last_failure_detail();
        memcpy(after, (void*)strict_helper_target, length);
        if (prepared)
            check_true("failed-send identity unexpected prepare finish",
                       peak_detach_controller_finish_hook_mutation(&request, &status));
        fprintf(stderr, "receipt-failed-send-credit replay_rc=%d handler_returned=%d "
                "prepared=%d status=%s reason=%s normal_sends=%d unexpected=%d consume_count=%d\n",
                receipt_control_replay_rc, atomic_load(&preflight_handler_returned),
                prepared, peak_detach_controller_status_string(status),
                detail != NULL && detail->reason != NULL ? detail->reason : "<none>",
                atomic_load(&preflight_normal_sends), peak_signal_policy_unexpected_delivery_count(),
                atomic_load(&receipt_consumed_count));
        check_true("failed send never grants a canceled owned credit",
                   receipt_control_replay_rc == 0 && atomic_load(&preflight_handler_returned) &&
                   !prepared && status == PEAK_DETACH_STATUS_UNSUPPORTED &&
                   detail != NULL && detail->reason != NULL &&
                   strcmp(detail->reason, "signal-unexpected-delivery") == 0 &&
                   detail->tid == 0 && detail->pc == (uintptr_t)signo && detail->aux == 1 &&
                   atomic_load(&preflight_normal_sends) == 0 &&
                   peak_signal_policy_unexpected_delivery_count() == 1 &&
                   atomic_load(&receipt_consumed_count) == 0 && memcmp(before, after, length) == 0 &&
                   !peak_detach_controller_threads_are_held() &&
                   !peak_detach_controller_current_mutation_uses_physical_patch() &&
                   peak_detach_controller_test_thread_creation_gate_epoch() == 0);
    }
cleanup:
    receipt_test_capacity = PEAK_DETACH_HELPER_MAX_THREADS;
    receipt_control_replay = 0;
    atomic_store(&receipt_fail_next_send, 0);
    if (worker_started) {
        atomic_store(&preflight_worker_running, 0);
        pthread_join(worker, NULL);
    }
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener); g_object_unref(interceptor); gum_deinit_embedded();
    fprintf(stderr, "receipt-control-complete failed_send=%d failures=%d\n", failed_send, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_signal_backend_blocked_thread(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "signal-backend-blocked-thread requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    pthread_t worker;
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;

    worker_running = 0;
    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    peak_detach_controller_note_thread_creation_gate_installed(TRUE);

    signal_blocked_worker_ready = 0;
    if (pthread_create(&worker, NULL, strict_helper_signal_blocked_worker, NULL) != 0) {
        perror("pthread_create");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    while (!worker_running) {
        usleep(1000);
    }
    check_true("signal backend blocked worker actually blocked reserved signal",
               signal_blocked_worker_ready == 1);

    PeakDetachRequest request = {
        .hook_id = 127,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_prepare("signal backend blocked thread fast fail",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_UNSUPPORTED);
    const PeakDetachFailureDetail* detail =
        peak_detach_controller_last_failure_detail();
    check_true("signal backend blocked thread reports reserved-signal reason",
               detail != NULL &&
               detail->reason != NULL &&
               strcmp(detail->reason, "signal-reserved-blocked") == 0);
    check_true("signal backend blocked thread leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("signal backend blocked thread leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    worker_running = 0;
    pthread_join(worker, NULL);
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_signal_event_futex(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "signal-event-futex requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    SignalEventWaitContext context = {
        .expected_sequence =
            peak_detach_controller_test_signal_wait_sequence(),
        .started = ATOMIC_VAR_INIT(0),
        .returned = ATOMIC_VAR_INIT(0),
    };
    pthread_t waiter;
    int initial_wait_count =
        peak_detach_controller_test_signal_wait_count();
    int create_status =
        pthread_create(&waiter, NULL, signal_event_wait_worker, &context);

    check_int("signal event waiter create",
              create_status,
              0);
    if (create_status != 0) {
        return EXIT_FAILURE;
    }
    while (atomic_load_explicit(&context.started, memory_order_acquire) == 0) {
        sched_yield();
    }
    while (peak_detach_controller_test_signal_wait_count() ==
           initial_wait_count) {
        sched_yield();
    }
    usleep(20000);
    check_int("signal event waiter remains parked without polling",
              atomic_load_explicit(&context.returned, memory_order_acquire),
              0);
    check_int("signal event waiter performs one event-driven wait",
              peak_detach_controller_test_signal_wait_count(),
              initial_wait_count + 1);
    peak_detach_controller_test_wake_signal_waiters();
    check_int("signal event waiter join", pthread_join(waiter, NULL), 0);
    check_int("signal event waiter observes wake",
              atomic_load_explicit(&context.returned, memory_order_acquire),
              1);

    int stale_sequence = peak_detach_controller_test_signal_wait_sequence();
    peak_detach_controller_test_wake_signal_waiters();
    peak_detach_controller_test_wait_for_signal_event(stale_sequence);

    if (failures == 0) {
        puts("signal_event_futex_ok");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_signal_backend_missing_thread_gate(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "signal-backend-missing-thread-gate requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    peak_detach_controller_note_thread_creation_gate_installed(FALSE);

    PeakDetachRequest request = {
        .hook_id = 128,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_prepare("signal backend missing pthread gate",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("signal backend missing pthread gate leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("signal backend missing pthread gate leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_helper_backend_missing_thread_gate(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "helper-backend-missing-thread-gate requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    peak_detach_controller_note_thread_creation_gate_installed(FALSE);

    PeakDetachRequest request = {
        .hook_id = 128,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_prepare("helper backend missing pthread gate",
                  &request,
                  FALSE,
                  PEAK_DETACH_STATUS_UNSUPPORTED);
    check_true("helper backend missing pthread gate leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);
    check_true("helper backend missing pthread gate leaves no physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == FALSE);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_fail_closed(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-fail-closed requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    const char* scenario = getenv("FAKE_DETACH_HELPER_SCENARIO");
    PeakDetachStatus expected = PEAK_DETACH_STATUS_ERROR;
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    PeakDetachAccountingSnapshot accounting;
    double last_success_us;
    uint64_t min_failure_wall_ns = 1;

    if (scenario == NULL) {
        scenario = "stop-permission";
    }

    if (strcmp(scenario, "stop-permission") == 0) {
        expected = PEAK_DETACH_STATUS_PERMISSION_DENIED;
    } else if (strcmp(scenario, "stop-missing-response") == 0) {
        expected = PEAK_DETACH_STATUS_ERROR;
    } else if (strcmp(scenario, "stop-release-failed") == 0) {
        expected = PEAK_DETACH_STATUS_ERROR;
    } else if (strcmp(scenario, "stop-timeout") == 0 ||
               strcmp(scenario, "stop-timeout-delayed") == 0) {
        expected = PEAK_DETACH_STATUS_TIMEOUT;
        if (strcmp(scenario, "stop-timeout-delayed") == 0) {
            min_failure_wall_ns = 5ULL * 1000ULL * 1000ULL * 1000ULL;
        }
    } else if (strcmp(scenario, "bad-snapshot") == 0 ||
               strcmp(scenario, "duplicate-snapshot") == 0) {
        expected = PEAK_DETACH_STATUS_ERROR;
    } else if (strcmp(scenario, "blocked-pc") == 0) {
        expected = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
    } else if (strcmp(scenario, "evacuate-error") == 0 ||
               strcmp(scenario, "evacuate-release-failed") == 0 ||
               strcmp(scenario, "evacuate-protocol-error") == 0 ||
               strcmp(scenario, "evacuate-unknown-status") == 0) {
        expected = PEAK_DETACH_STATUS_ERROR;
    } else {
        fprintf(stderr, "unknown fake helper scenario: %s\n", scenario);
        return EXIT_FAILURE;
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    if (strcmp(scenario, "duplicate-snapshot") == 0 &&
        set_fake_helper_pointer_env("FAKE_DETACH_HELPER_STOP_PC",
                                    (gpointer)((guint8*)strict_helper_target +
                                               GUM_PEAK_MAX_PROLOGUE_SIZE)) != 0) {
        perror("setenv");
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_flush(interceptor);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }

    PeakDetachRequest request = {
        .hook_id = 23,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };
    if (strcmp(scenario, "blocked-pc") == 0) {
        request.blocked_pc_start = (gpointer)(uintptr_t)0x1200;
        request.blocked_pc_size = 0x100;
    }
    check_accounting_snapshot("fake helper failure accounting before",
                              &accounting);
    last_success_us = peak_detach_controller_last_stop_window_us();
    check_prepare(scenario, &request, FALSE, expected);
    check_stop_window_failure(scenario, &accounting, min_failure_wall_ns);
    check_true("fake helper failure preserves last stop window",
               peak_detach_controller_last_stop_window_us() == last_success_us);
    check_true("fake helper failure leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);

    if (strcmp(scenario, "evacuate-error") == 0) {
        check_cleanup_query();
        check_prepare("subsequent STOP and recoverable EVACUATE", &request,
                      FALSE, expected);
        check_cleanup_query();
    }

    PeakDetachStatus shutdown_status = PEAK_DETACH_STATUS_ERROR;
    check_true("fake helper shutdown",
               peak_detach_controller_shutdown_helper(&shutdown_status) == TRUE);
    check_true("fake helper shutdown status",
               shutdown_status == PEAK_DETACH_STATUS_SAFE ||
               shutdown_status == PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED);

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_auto_fallback_with_mpi(gboolean mpi_process,
                                       gboolean mutate_after_configuration)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    fprintf(stderr, "fake-helper-auto-fallback requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    const char* scenario = getenv("FAKE_DETACH_HELPER_SCENARIO");
    const char* log_path = getenv("FAKE_DETACH_HELPER_LOG");
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn attach_status;
    PeakDetachAccountingSnapshot accounting;

    if (scenario == NULL) {
        scenario = "stop-permission";
    }
    if (strcmp(scenario, "stop-permission") != 0 &&
        strcmp(scenario, "stop-timeout") != 0 &&
        strcmp(scenario, "stop-unsupported") != 0) {
        fprintf(stderr, "unsupported auto fallback fake helper scenario: %s\n", scenario);
        return EXIT_FAILURE;
    }
    if (log_path != NULL && log_path[0] != '\0') {
        unlink(log_path);
    }
    peak_detach_controller_configure_mpi_process(mpi_process);
    if (mutate_after_configuration) {
        check_true("change backend environment after configuration",
                   g_setenv("PEAK_DETACH_BACKEND", "helper", TRUE));
        check_true("change ptrace scope after configuration",
                   g_setenv("PEAK_TEST_PTRACE_SCOPE", "0", TRUE));
    }

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(strict_helper_on_enter, NULL, NULL, NULL);
    attach_status =
        gum_interceptor_attach(interceptor,
                               (gpointer)strict_helper_target,
                               listener,
                               NULL);
    if (attach_status != GUM_ATTACH_OK) {
        fprintf(stderr, "gum_interceptor_attach failed: %d\n", attach_status);
        g_object_unref(listener);
        g_object_unref(interceptor);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    gum_interceptor_flush(interceptor);
    strict_helper_target();
    peak_detach_controller_note_thread_creation_gate_installed(TRUE);

    PeakDetachRequest request = {
        .hook_id = 129,
        .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target,
        .interceptor = interceptor,
        .listener = listener,
        .operation = PEAK_DETACH_OPERATION_DETACH
    };

    check_accounting_snapshot("auto helper fallback accounting before",
                              &accounting);
    check_prepare("auto helper fallback prepare", &request, TRUE, PEAK_DETACH_STATUS_SAFE);
    check_true("auto helper fallback uses physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("auto helper fallback finish", &request, PEAK_DETACH_STATUS_SAFE);
    check_stop_window_finish("auto helper fallback composite stop window",
                             &accounting);
    check_true("auto helper fallback releases held mutation",
               peak_detach_controller_threads_are_held() == FALSE);

    PeakDetachStatus shutdown_status = PEAK_DETACH_STATUS_ERROR;
    check_true("auto fallback helper shutdown",
               peak_detach_controller_shutdown_helper(&shutdown_status) == TRUE);
    check_status("auto fallback helper shutdown status",
                 shutdown_status,
                 PEAK_DETACH_STATUS_SAFE);
    if (log_path != NULL && log_path[0] != '\0') {
        if (mpi_process) {
            check_true("MPI auto backend never creates helper log",
                       access(log_path, F_OK) != 0);
        } else if (mutate_after_configuration) {
            check_true("cached auto backend never creates helper log",
                       access(log_path, F_OK) != 0);
        } else {
            check_helper_log_count(log_path, "START", 1);
            check_helper_log_count(log_path, "STOP", 1);
            check_helper_log_count(log_path, "EVACUATE", 0);
            check_helper_log_count(log_path, "RESUME", 0);
            check_helper_log_count(log_path, "SHUTDOWN", 0);
        }
        unlink(log_path);
    }

    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    peak_detach_controller_note_thread_creation_gate_installed(FALSE);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

static int
run_fake_helper_auto_fallback(void)
{
    return run_fake_helper_auto_fallback_with_mpi(FALSE, FALSE);
}

static int
run_fake_helper_auto_mpi(void)
{
    return run_fake_helper_auto_fallback_with_mpi(TRUE, FALSE);
}

static int
run_cached_auto_backend_configuration(void)
{
    return run_fake_helper_auto_fallback_with_mpi(FALSE, TRUE);
}

static int
run_signal_reserve_early_never(void)
{
    check_int("PEAK_SIGNAL_RESERVE_EARLY=never leaves signal unreserved",
              peak_signal_policy_reserved_signal(),
              0);
    check_true("change detach signal after configuration",
               g_setenv("PEAK_DETACH_SIGNAL", "invalid-after-config", TRUE));
    int reserved = peak_signal_policy_choose_reserved_signal();
    check_true("cached auto signal selection ignores later invalid environment",
               reserved >= SIGRTMIN && reserved <= SIGRTMAX);
    peak_signal_policy_clear_reserved_signal();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int
run_signal_reserve_helper_auto(void)
{
    check_int("helper backend with PEAK_DETACH_SIGNAL=auto leaves signal unreserved",
              peak_signal_policy_reserved_signal(),
              0);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int
run_accounting_snapshot_contention(void)
{
#ifndef PEAK_ENABLE_TEST_HOOKS
    fprintf(stderr,
            "accounting-snapshot-contention requires PEAK_ENABLE_TEST_HOOKS\n");
    return 77;
#else
    PeakDetachAccountingSnapshot snapshot = {
        .completed_stop_window_count = 11,
        .failed_stop_window_count = 22,
        .stop_window_wall_ns = 44
    };

    peak_detach_controller_test_accounting_begin_publish();
    check_true("accounting snapshot returns false while publish is odd",
               peak_detach_controller_accounting_snapshot(&snapshot) == FALSE);
    peak_detach_controller_test_accounting_end_publish();

    check_uint64("accounting contention preserves completed",
                 snapshot.completed_stop_window_count,
                 11);
    check_uint64("accounting contention preserves failed",
                 snapshot.failed_stop_window_count,
                 22);
    check_uint64("accounting contention preserves wall",
                 snapshot.stop_window_wall_ns,
                 44);

    check_true("accounting snapshot returns true after publish release",
               peak_detach_controller_accounting_snapshot(&snapshot) == TRUE);
    check_true("accounting snapshot completed never sentinel",
               snapshot.completed_stop_window_count != UINT64_MAX);
    check_true("accounting snapshot failed never sentinel",
               snapshot.failed_stop_window_count != UINT64_MAX);
    check_true("accounting snapshot wall never sentinel",
               snapshot.stop_window_wall_ns != UINT64_MAX);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

#if defined(PEAK_ENABLE_TEST_HOOKS) && defined(PEAK_HAVE_GUM_PEAK_PC_API)
typedef struct {
    PeakDetachAccountingSnapshot baseline;
    PeakDetachAccountingSnapshot accepted;
    _Atomic int abort_requested;
    _Atomic int reader_ready;
    _Atomic int publication_open;
    _Atomic int odd_rejected;
    _Atomic int publication_closed;
    _Atomic int post_close_accepted;
} AccountingSnapshotConcurrentContext;

static void*
run_accounting_snapshot_writer(void* user_data)
{
    AccountingSnapshotConcurrentContext* context = user_data;

    while (atomic_load_explicit(&context->reader_ready,
                                memory_order_acquire) == 0) {
        if (atomic_load_explicit(&context->abort_requested,
                                 memory_order_acquire) != 0) {
            return NULL;
        }
        sched_yield();
    }

    peak_detach_controller_test_accounting_begin_publish();
    peak_detach_controller_test_accounting_update_tuple(7, TRUE);
    atomic_store_explicit(&context->publication_open,
                          1,
                          memory_order_release);
    while (atomic_load_explicit(&context->odd_rejected,
                                memory_order_acquire) == 0) {
        if (atomic_load_explicit(&context->abort_requested,
                                 memory_order_acquire) != 0) {
            peak_detach_controller_test_accounting_end_publish();
            atomic_store_explicit(&context->publication_closed,
                                  1,
                                  memory_order_release);
            return NULL;
        }
        sched_yield();
    }
    peak_detach_controller_test_accounting_end_publish();
    atomic_store_explicit(&context->publication_closed,
                          1,
                          memory_order_release);
    return NULL;
}

static void*
run_accounting_snapshot_reader(void* user_data)
{
    AccountingSnapshotConcurrentContext* context = user_data;
    PeakDetachAccountingSnapshot snapshot = {
        .completed_stop_window_count = UINT64_MAX,
        .failed_stop_window_count = UINT64_MAX,
        .stop_window_wall_ns = UINT64_MAX
    };

    atomic_store_explicit(&context->reader_ready, 1, memory_order_release);
    while (atomic_load_explicit(&context->publication_open,
                                memory_order_acquire) == 0) {
        if (atomic_load_explicit(&context->abort_requested,
                                 memory_order_acquire) != 0) {
            return NULL;
        }
        sched_yield();
    }

    if (peak_detach_controller_accounting_snapshot(&snapshot)) {
        atomic_store_explicit(&context->abort_requested,
                              1,
                              memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&context->odd_rejected, 1, memory_order_release);

    while (atomic_load_explicit(&context->publication_closed,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
    if (peak_detach_controller_accounting_snapshot(&context->accepted)) {
        atomic_store_explicit(&context->post_close_accepted,
                              1,
                              memory_order_release);
    }
    return NULL;
}
#endif

static int
run_accounting_snapshot_concurrent(void)
{
#if !defined(PEAK_ENABLE_TEST_HOOKS) || !defined(PEAK_HAVE_GUM_PEAK_PC_API)
    fprintf(stderr,
            "accounting-snapshot-concurrent requires PEAK_ENABLE_TEST_HOOKS and PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
#else
    AccountingSnapshotConcurrentContext context = { 0 };
    pthread_t writer;
    pthread_t reader;
    int create_result;

    if (!check_accounting_snapshot("accounting concurrent baseline",
                                  &context.baseline)) {
        return EXIT_FAILURE;
    }
    create_result = pthread_create(&reader,
                                   NULL,
                                   run_accounting_snapshot_reader,
                                   &context);
    check_int("accounting concurrent reader create", create_result, 0);
    if (create_result != 0) {
        return EXIT_FAILURE;
    }
    create_result = pthread_create(&writer,
                                   NULL,
                                   run_accounting_snapshot_writer,
                                   &context);
    check_int("accounting concurrent writer create", create_result, 0);
    if (create_result != 0) {
        atomic_store_explicit(&context.abort_requested,
                              1,
                              memory_order_release);
        check_int("accounting concurrent reader cleanup join",
                  pthread_join(reader, NULL),
                  0);
        return EXIT_FAILURE;
    }
    check_int("accounting concurrent writer join", pthread_join(writer, NULL), 0);
    check_int("accounting concurrent reader join", pthread_join(reader, NULL), 0);
    check_true("accounting concurrent reader rejected odd publication",
               atomic_load_explicit(&context.odd_rejected,
                                    memory_order_acquire) != 0);
    check_true("accounting concurrent reader accepted closed publication",
               atomic_load_explicit(&context.post_close_accepted,
                                    memory_order_acquire) != 0);
    check_uint64("accounting concurrent completed tuple",
                 context.accepted.completed_stop_window_count,
                 context.baseline.completed_stop_window_count + 1);
    check_uint64("accounting concurrent failed tuple",
                 context.accepted.failed_stop_window_count,
                 context.baseline.failed_stop_window_count);
    check_uint64("accounting concurrent wall tuple",
                 context.accepted.stop_window_wall_ns,
                 context.baseline.stop_window_wall_ns + 7);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
}

int
main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "usage: %s strict|strict-helper-empty|cached-helper-configuration|strict-helper-stale-caller|fake-helper-trace-disabled-stop-window|fake-helper-shutdown-sequence|fake-helper-batch-attach|fake-helper-arm64-attach-plan|fake-helper-batch-detach|fake-helper-batch-abort-rollback|fake-helper-batch-mixed|fake-helper-batch-missing-gum-snapshot|fake-helper-listener-canonical-address|fake-helper-listener-ambiguous-address|fake-helper-batch-canonical-duplicate|fake-helper-batch-reattach|batch-guards|invalid|fake-helper-gum-pc-corridor|fake-helper-reattach-patch-entry|fake-helper-fail-closed|fake-helper-auto-fallback|fake-helper-auto-mpi|cached-auto-backend-configuration|signal-event-futex|signal-backend-blocked-thread|signal-backend-missing-thread-gate|helper-backend-missing-thread-gate|signal-reserve-early-never|signal-reserve-helper-auto|accounting-snapshot-contention|accounting-snapshot-concurrent\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    peak_detach_controller_configure_mpi_process(FALSE);

    if (strcmp(argv[1], "strict") != 0) {
        peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    }

    if (strcmp(argv[1], "strict") == 0) {
        return run_strict();
    }
    if (strcmp(argv[1], "strict-helper-empty") == 0) {
        return run_strict_helper_empty();
    }
    if (strcmp(argv[1], "cached-helper-configuration") == 0) {
        return run_cached_helper_configuration();
    }
    if (strcmp(argv[1], "strict-helper-stale-caller") == 0) {
        return run_strict_helper_stale_caller();
    }
    if (strcmp(argv[1], "fake-helper-trace-disabled-stop-window") == 0) {
        return run_fake_helper_trace_disabled_stop_window();
    }
    if (strcmp(argv[1], "fake-helper-shutdown-sequence") == 0) {
        return run_fake_helper_shutdown_sequence();
    }
    if (strcmp(argv[1], "fake-helper-batch-attach") == 0) {
        return run_fake_helper_batch_attach();
    }
    if (strcmp(argv[1], "fake-helper-arm64-attach-plan") == 0) {
        return run_fake_helper_arm64_attach_plan();
    }
    if (strcmp(argv[1], "fake-helper-batch-detach") == 0) {
        return run_fake_helper_batch_detach();
    }
    if (strcmp(argv[1], "fake-helper-batch-abort-rollback") == 0) {
        return run_fake_helper_batch_abort_rolls_back_records();
    }
    if (strcmp(argv[1], "fake-helper-batch-mixed") == 0) {
        return run_fake_helper_batch_mixed();
    }
    if (strcmp(argv[1], "fake-helper-batch-missing-gum-snapshot") == 0) {
        return run_fake_helper_batch_missing_gum_snapshot();
    }
    if (strcmp(argv[1], "fake-helper-listener-canonical-address") == 0) {
        return run_fake_helper_listener_canonical_address();
    }
    if (strcmp(argv[1], "fake-helper-listener-ambiguous-address") == 0) {
        return run_fake_helper_listener_ambiguous_address();
    }
    if (strcmp(argv[1], "fake-helper-batch-canonical-duplicate") == 0) {
        return run_fake_helper_batch_canonical_duplicate();
    }
    if (strcmp(argv[1], "fake-helper-batch-reattach") == 0) {
        return run_fake_helper_batch_reattach();
    }
    if (strcmp(argv[1], "batch-guards") == 0) {
        return run_batch_guards();
    }
    if (strcmp(argv[1], "invalid") == 0) {
        return run_invalid();
    }
    if (strcmp(argv[1], "fake-helper-gum-pc-corridor") == 0) {
        return run_fake_helper_gum_pc_corridor();
    }
    if (strcmp(argv[1], "fake-helper-reattach-patch-entry") == 0) {
        return run_fake_helper_reattach_patch_entry();
    }
    if (strcmp(argv[1], "fake-helper-fail-closed") == 0) {
        return run_fake_helper_fail_closed();
    }
    if (strcmp(argv[1], "fake-helper-auto-fallback") == 0) {
        return run_fake_helper_auto_fallback();
    }
    if (strcmp(argv[1], "fake-helper-auto-mpi") == 0) {
        return run_fake_helper_auto_mpi();
    }
    if (strcmp(argv[1], "cached-auto-backend-configuration") == 0) {
        return run_cached_auto_backend_configuration();
    }
    if (strcmp(argv[1], "signal-event-futex") == 0) {
        return run_signal_event_futex();
    }
    if (strcmp(argv[1], "signal-preflight-contamination") == 0) {
        return run_signal_preflight_contamination(
            g_getenv("PEAK_TEST_PREFLIGHT_BATCH") != NULL,
            g_getenv("PEAK_TEST_PREFLIGHT_CLEAN") == NULL);
    }
    if (strcmp(argv[1], "supported-handler-owned-recovery") == 0) {
        return run_supported_handler_recovery();
    }
    if (strcmp(argv[1], "signal-receipt-control") == 0) {
        return run_signal_receipt_control(g_getenv("PEAK_TEST_RECEIPT_FAILED_SEND") != NULL);
    }
    if (strcmp(argv[1], "signal-backend-blocked-thread") == 0) {
        return run_signal_backend_blocked_thread();
    }
    if (strcmp(argv[1], "signal-backend-missing-thread-gate") == 0) {
        return run_signal_backend_missing_thread_gate();
    }
    if (strcmp(argv[1], "helper-backend-missing-thread-gate") == 0) {
        return run_helper_backend_missing_thread_gate();
    }
    if (strcmp(argv[1], "signal-reserve-early-never") == 0) {
        return run_signal_reserve_early_never();
    }
    if (strcmp(argv[1], "signal-reserve-helper-auto") == 0) {
        return run_signal_reserve_helper_auto();
    }
    if (strcmp(argv[1], "accounting-snapshot-contention") == 0) {
        return run_accounting_snapshot_contention();
    }
    if (strcmp(argv[1], "accounting-snapshot-concurrent") == 0) {
        return run_accounting_snapshot_concurrent();
    }
    fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return EXIT_FAILURE;
}
