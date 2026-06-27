#include "peak_detach_controller.h"
#include "peak_signal_policy.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int peak_signal_policy_reserved_signal(void);

static int failures = 0;

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
        .listener = (operation == PEAK_DETACH_OPERATION_ATTACH ||
                     operation == PEAK_DETACH_OPERATION_REPLACE ||
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

    check_prepare("trace-disabled detach prepare",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("trace-disabled detach physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_double_zero("trace-disabled detach prepare stop window",
                      peak_detach_controller_last_stop_window_us());
    check_finish("trace-disabled detach finish", &request, PEAK_DETACH_STATUS_SAFE);
    check_double_zero("trace-disabled detach finish stop window",
                      peak_detach_controller_last_stop_window_us());

    request.operation = PEAK_DETACH_OPERATION_REATTACH;
    check_prepare("trace-disabled reattach prepare",
                  &request,
                  TRUE,
                  PEAK_DETACH_STATUS_SAFE);
    check_true("trace-disabled reattach physical",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_double_zero("trace-disabled reattach prepare stop window",
                      peak_detach_controller_last_stop_window_us());
    check_finish("trace-disabled reattach finish", &request, PEAK_DETACH_STATUS_SAFE);
    check_double_zero("trace-disabled reattach finish stop window",
                      peak_detach_controller_last_stop_window_us());

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
    check_helper_log_count(log_template, "STOP", 1);
    check_helper_log_count(log_template, "EVACUATE", 1);
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

    PeakDetachRequest unsupported_requests[4] = {
        {
            .hook_id = 71,
            .symbol_name = "unsupported_attach",
            .function_address = (gpointer)(uintptr_t)0x7100,
            .interceptor = (GumInterceptor*)(uintptr_t)0x7110,
            .listener = NULL,
            .operation = PEAK_DETACH_OPERATION_ATTACH
        },
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

    memset(results, 0xff, sizeof(results));
    check_true("unsupported batch rejected",
               peak_detach_controller_prepare_hook_mutation_batch(
                   unsupported_requests,
                   4,
                   results,
                   &prepared_count,
                   &status) == FALSE);
    check_int("unsupported batch prepared count", (int)prepared_count, 0);
    check_status("unsupported batch aggregate status",
                 status,
                 PEAK_DETACH_STATUS_UNSUPPORTED);
    for (size_t i = 0; i < 4; i++) {
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
    } else if (strcmp(scenario, "bad-snapshot") == 0 ||
               strcmp(scenario, "duplicate-snapshot") == 0) {
        expected = PEAK_DETACH_STATUS_ERROR;
    } else if (strcmp(scenario, "blocked-pc") == 0) {
        expected = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
    } else if (strcmp(scenario, "evacuate-error") == 0 ||
               strcmp(scenario, "evacuate-release-failed") == 0) {
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
    check_prepare(scenario, &request, FALSE, expected);
    check_true("fake helper failure leaves no held mutation",
               peak_detach_controller_threads_are_held() == FALSE);

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
run_fake_helper_auto_fallback(void)
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

    check_prepare("auto helper fallback prepare", &request, TRUE, PEAK_DETACH_STATUS_SAFE);
    check_true("auto helper fallback uses physical mutation",
               peak_detach_controller_current_mutation_uses_physical_patch() == TRUE);
    check_finish("auto helper fallback finish", &request, PEAK_DETACH_STATUS_SAFE);
    check_true("auto helper fallback releases held mutation",
               peak_detach_controller_threads_are_held() == FALSE);

    PeakDetachStatus shutdown_status = PEAK_DETACH_STATUS_ERROR;
    check_true("auto fallback helper shutdown",
               peak_detach_controller_shutdown_helper(&shutdown_status) == TRUE);
    check_status("auto fallback helper shutdown status",
                 shutdown_status,
                 PEAK_DETACH_STATUS_SAFE);
    if (log_path != NULL && log_path[0] != '\0') {
        check_helper_log_count(log_path, "START", 1);
        check_helper_log_count(log_path, "STOP", 1);
        check_helper_log_count(log_path, "EVACUATE", 0);
        check_helper_log_count(log_path, "RESUME", 0);
        check_helper_log_count(log_path, "SHUTDOWN", 0);
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
run_signal_reserve_early_never(void)
{
    check_int("PEAK_SIGNAL_RESERVE_EARLY=never leaves signal unreserved",
              peak_signal_policy_reserved_signal(),
              0);
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

int
main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "usage: %s strict|strict-helper-empty|strict-helper-stale-caller|fake-helper-trace-disabled-stop-window|fake-helper-shutdown-sequence|fake-helper-batch-detach|fake-helper-batch-abort-rollback|fake-helper-batch-mixed|fake-helper-batch-missing-gum-snapshot|fake-helper-listener-canonical-address|fake-helper-listener-ambiguous-address|fake-helper-batch-canonical-duplicate|fake-helper-batch-reattach|batch-guards|invalid|fake-helper-gum-pc-corridor|fake-helper-reattach-patch-entry|fake-helper-fail-closed|fake-helper-auto-fallback|signal-backend-blocked-thread|signal-backend-missing-thread-gate|helper-backend-missing-thread-gate|signal-reserve-early-never|signal-reserve-helper-auto\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "strict") != 0) {
        peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    }

    if (strcmp(argv[1], "strict") == 0) {
        return run_strict();
    }
    if (strcmp(argv[1], "strict-helper-empty") == 0) {
        return run_strict_helper_empty();
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
    fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return EXIT_FAILURE;
}
