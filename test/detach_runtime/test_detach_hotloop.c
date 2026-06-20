#define _GNU_SOURCE
#include "frida-gum.h"
#include "general_listener.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int stop_requested;
static atomic_uint_fast64_t side_effect;
static atomic_uint_fast64_t spawned_worker_count;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long expected;
    long arrived;
    int open;
} StartGate;

__attribute__((noinline))
void peak_detach_hot_target(uint64_t value)
{
    atomic_fetch_add_explicit(&side_effect, value + 1, memory_order_relaxed);
    asm volatile("" ::: "memory");
}

__attribute__((noinline))
void peak_detach_hot_target_two(uint64_t value)
{
    atomic_fetch_xor_explicit(&side_effect,
                              value ^ UINT64_C(0x9e3779b97f4a7c15),
                              memory_order_relaxed);
    asm volatile("" ::: "memory");
}

typedef struct {
    uint64_t calls;
    unsigned int seed;
    StartGate* start_gate;
    int paired_targets;
} WorkerState;

typedef struct {
    unsigned int seed;
} SpawnedWorkerState;

typedef struct {
    unsigned int seed;
    uint64_t created;
} SpawnerState;

static double
monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
sleep_for_seconds(long seconds)
{
    double deadline = monotonic_seconds() + (double)seconds;

    for (;;) {
        double remaining = deadline - monotonic_seconds();
        if (remaining <= 0.0) {
            return;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)remaining;
        ts.tv_nsec = (long)((remaining - (double)ts.tv_sec) * 1000000000.0);
        if (ts.tv_nsec < 0) {
            ts.tv_nsec = 0;
        }
        nanosleep(&ts, NULL);
    }
}

static void*
worker_main(void* arg)
{
    WorkerState* state = (WorkerState*)arg;
    uint64_t local_calls = 0;
    unsigned int seed = state->seed;

    pthread_mutex_lock(&state->start_gate->mutex);
    state->start_gate->arrived++;
    if (state->start_gate->arrived == state->start_gate->expected) {
        state->start_gate->open = 1;
        pthread_cond_broadcast(&state->start_gate->cond);
    } else {
        while (!state->start_gate->open) {
            pthread_cond_wait(&state->start_gate->cond,
                              &state->start_gate->mutex);
        }
    }
    pthread_mutex_unlock(&state->start_gate->mutex);

    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        peak_detach_hot_target((uint64_t)(seed + local_calls));
        if (state->paired_targets) {
            peak_detach_hot_target_two((uint64_t)(seed ^ local_calls));
        }
        local_calls++;
    }

    state->calls = local_calls;
    return NULL;
}

static void*
spawned_worker_main(void* arg)
{
    SpawnedWorkerState* state = (SpawnedWorkerState*)arg;
    unsigned int seed = state->seed;

    for (uint64_t i = 0; i < 32; i++) {
        peak_detach_hot_target((uint64_t)(seed + i));
    }
    free(state);
    return NULL;
}

static void*
spawner_main(void* arg)
{
    SpawnerState* state = (SpawnerState*)arg;

    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        SpawnedWorkerState* child_state = malloc(sizeof(*child_state));
        pthread_t child;

        if (child_state == NULL) {
            break;
        }
        child_state->seed = state->seed + (unsigned int)state->created;
        if (pthread_create(&child, NULL, spawned_worker_main, child_state) != 0) {
            free(child_state);
            break;
        }
        pthread_join(child, NULL);
        state->created++;
    }

    atomic_fetch_add_explicit(&spawned_worker_count,
                              state->created,
                              memory_order_relaxed);
    return NULL;
}

static int
has_flag_arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static long
parse_long_arg(int argc, char** argv, const char* name, long fallback)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            char* end = NULL;
            errno = 0;
            long value = strtol(argv[i + 1], &end, 10);
            if (errno == 0 && end != argv[i + 1] && *end == '\0' && value > 0) {
                return value;
            }
        }
    }
    return fallback;
}

static void*
required_symbol(const char* name)
{
    void* symbol = dlsym(RTLD_DEFAULT, name);

    if (symbol == NULL) {
        fprintf(stderr, "missing required symbol %s: %s\n", name, dlerror());
    }
    return symbol;
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static const char*
parse_string_arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

typedef gboolean (*PeakRequestDetachFn)(size_t hook_id);
typedef gboolean (*PeakRequestReattachFn)(size_t hook_id);
typedef gboolean (*PeakControllerDrainFn)(unsigned int timeout_ms);
typedef void (*PeakControllerStopFn)(void);
typedef PeakHookState (*PeakHookStateFn)(size_t hook_id);
typedef gboolean (*PeakGumGetPcDiagnosticsFn)(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    GumPeakPcDiagnostics* diagnostics);

static int
set_pointer_env(const char* name, gpointer pointer)
{
    char value[2 + sizeof(uintptr_t) * 2 + 1];

    snprintf(value, sizeof(value), "0x%" PRIxPTR, (uintptr_t)pointer);
    return setenv(name, value, 1);
}

static int
write_pointer_file(const char* path, gpointer pointer)
{
    FILE* fp = fopen(path, "w");

    if (fp == NULL) {
        return -1;
    }
    if (fprintf(fp, "0x%" PRIxPTR "\n", (uintptr_t)pointer) < 0) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static void
unlink_if_path(const char* path)
{
    if (path != NULL && path[0] != '\0') {
        unlink(path);
    }
}

static gpointer
choose_unsupported_gum_pc(const GumPeakPcDiagnostics* diagnostics)
{
    if (diagnostics->on_leave_trampoline != NULL) {
        return diagnostics->on_leave_trampoline;
    }
    if (diagnostics->on_invoke_trampoline != NULL) {
        return diagnostics->on_invoke_trampoline;
    }
    if (diagnostics->enter_thunk_start != NULL &&
        diagnostics->enter_thunk_size > 0) {
        return diagnostics->enter_thunk_start;
    }
    if (diagnostics->on_enter_trampoline != NULL &&
        diagnostics->on_leave_trampoline != NULL &&
        (uintptr_t)diagnostics->on_enter_trampoline + 1 <
            (uintptr_t)diagnostics->on_leave_trampoline) {
        return (gpointer)((guint8*)diagnostics->on_enter_trampoline + 1);
    }
    return NULL;
}

static int
resolve_unsupported_gum_pc_case(const char* pc_case,
                                const GumPeakPcDiagnostics* diagnostics,
                                gpointer* pc_out)
{
    if (pc_case == NULL || pc_case[0] == '\0' ||
        strcmp(pc_case, "first-available") == 0) {
        *pc_out = choose_unsupported_gum_pc(diagnostics);
        if (*pc_out == NULL) {
            fprintf(stderr, "no unsupported Gum diagnostic PC available\n");
            return 0;
        }
        return 1;
    }

    if (strcmp(pc_case, "on-enter-plus-one") == 0) {
        if (diagnostics->on_enter_trampoline == NULL ||
            diagnostics->on_leave_trampoline == NULL ||
            (uintptr_t)diagnostics->on_enter_trampoline + 1 >=
                (uintptr_t)diagnostics->on_leave_trampoline) {
            fprintf(stderr, "on-enter-plus-one diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = (gpointer)((guint8*)diagnostics->on_enter_trampoline + 1);
        return 1;
    }

    if (strcmp(pc_case, "on-leave") == 0) {
        if (diagnostics->on_leave_trampoline == NULL) {
            fprintf(stderr, "on-leave diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->on_leave_trampoline;
        return 1;
    }

    if (strcmp(pc_case, "on-invoke") == 0) {
        if (diagnostics->on_invoke_trampoline == NULL) {
            fprintf(stderr, "on-invoke diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->on_invoke_trampoline;
        return 1;
    }

    if (strcmp(pc_case, "enter-thunk") == 0) {
        if (diagnostics->enter_thunk_start == NULL ||
            diagnostics->enter_thunk_size == 0) {
            fprintf(stderr, "enter-thunk diagnostic PC unavailable\n");
            return 0;
        }
        *pc_out = diagnostics->enter_thunk_start;
        return 1;
    }

    fprintf(stderr, "unknown unsupported Gum PC case: %s\n", pc_case);
    return -1;
}

static int
parse_csv_line(char* line, char** fields, size_t field_count)
{
    size_t count = 0;
    char* saveptr = NULL;
    char* token = strtok_r(line, ",\r\n", &saveptr);

    while (token != NULL && count < field_count) {
        fields[count++] = token;
        token = strtok_r(NULL, ",\r\n", &saveptr);
    }
    return (int)count;
}

static int
count_helper_log_entry(const char* path, const char* entry)
{
    FILE* fp = fopen(path, "r");
    char line[128];
    int count = 0;

    if (fp == NULL) {
        fprintf(stderr, "open helper log %s: %s\n", path, strerror(errno));
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

static int
helper_log_count_is(const char* path, const char* entry, int expected)
{
    int actual = count_helper_log_entry(path, entry);

    if (actual != expected) {
        fprintf(stderr,
                "helper log %s count: expected %d, got %d\n",
                entry,
                expected,
                actual);
        return 0;
    }
    return 1;
}

static int
find_retry_batch_id(const char* trace_path, char* batch_id, size_t batch_id_size)
{
    FILE* fp = fopen(trace_path, "r");
    char line[1024];

    if (fp == NULL) {
        perror("fopen trace");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* fields[13];
        int count = parse_csv_line(line, fields, 13);

        if (count >= 13 &&
            strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "prepare-failed") == 0 &&
            strcmp(fields[6], "classify-failed") == 0 &&
            atoi(fields[7]) > 0 &&
            atoi(fields[9]) >= 2 &&
            atof(fields[10]) > 0.0 &&
            strcmp(fields[11], "0") != 0 &&
            strcmp(fields[12], "classify-failed") == 0) {
            snprintf(batch_id, batch_id_size, "%s", fields[11]);
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int
trace_proves_retry_preservation(const char* trace_path, const char* retry_batch_id)
{
    FILE* fp = fopen(trace_path, "r");
    char line[1024];
    int peer_succeeded_in_retry_batch = 0;
    int target_succeeded_later = 0;

    if (fp == NULL) {
        perror("fopen trace");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* fields[13];
        int count = parse_csv_line(line, fields, 13);

        if (count < 12) {
            continue;
        }

        if (strcmp(fields[2], "peak_detach_hot_target_two") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            atoi(fields[9]) >= 2 &&
            atof(fields[10]) > 0.0 &&
            strcmp(fields[11], retry_batch_id) == 0 &&
            (count < 13 || strcmp(fields[12], "safe") == 0)) {
            peer_succeeded_in_retry_batch = 1;
        }

        if (strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            atoi(fields[7]) > 0 &&
            strcmp(fields[11], retry_batch_id) != 0 &&
            atof(fields[8]) > 0.0 &&
            atof(fields[10]) > 0.0 &&
            count >= 13 &&
            strcmp(fields[12], "classify-failed") == 0) {
            target_succeeded_later = 1;
        }
    }

    fclose(fp);
    if (!peer_succeeded_in_retry_batch) {
        fprintf(stderr, "missing peer success in retry batch %s\n", retry_batch_id);
    }
    if (!target_succeeded_later) {
        fprintf(stderr, "missing later retried success for peak_detach_hot_target\n");
    }
    return peer_succeeded_in_retry_batch && target_succeeded_later;
}

static int
trace_has_retryable_classify_failure_without_success(const char* trace_path,
                                                     const char* symbol,
                                                     const char* operation,
                                                     char* batch_id,
                                                     size_t batch_id_size)
{
    FILE* fp = fopen(trace_path, "r");
    char line[1024];
    int saw_failure = 0;
    int saw_success = 0;

    if (fp == NULL) {
        perror("fopen trace");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* fields[13];
        int count = parse_csv_line(line, fields, 13);

        if (count < 12 ||
            strcmp(fields[2], symbol) != 0 ||
            strcmp(fields[3], operation) != 0) {
            continue;
        }

        if (strcmp(fields[4], "prepare-failed") == 0 &&
            strcmp(fields[6], "classify-failed") == 0 &&
            atoi(fields[7]) > 0 &&
            atof(fields[10]) > 0.0 &&
            strcmp(fields[11], "0") != 0 &&
            count >= 13 &&
            strcmp(fields[12], "classify-failed") == 0) {
            saw_failure = 1;
            snprintf(batch_id, batch_id_size, "%s", fields[11]);
        }

        if (strcmp(fields[4], "success") == 0) {
            saw_success = 1;
        }
    }

    fclose(fp);
    if (!saw_failure) {
        fprintf(stderr,
                "missing retryable classify-failed %s trace for %s\n",
                operation,
                symbol);
    }
    if (saw_success) {
        fprintf(stderr,
                "unexpected %s success trace for pending %s\n",
                operation,
                symbol);
    }
    return saw_failure && !saw_success;
}

static int
trace_has_physical_detach_success(const char* trace_path)
{
    FILE* fp;
    char line[1024];

    if (trace_path == NULL || trace_path[0] == '\0') {
        return 0;
    }

    fp = fopen(trace_path, "r");
    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* fields[13];
        int count = parse_csv_line(line, fields, 13);

        if (count >= 12 &&
            strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            atof(fields[10]) > 0.0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int
trace_has_success_after_retry(const char* trace_path,
                              const char* symbol,
                              const char* operation)
{
    FILE* fp = fopen(trace_path, "r");
    char line[1024];
    int saw_success = 0;

    if (fp == NULL) {
        perror("fopen trace");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* fields[13];
        int count = parse_csv_line(line, fields, 13);

        if (count >= 13 &&
            strcmp(fields[2], symbol) == 0 &&
            strcmp(fields[3], operation) == 0 &&
            strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            atoi(fields[7]) > 0 &&
            atof(fields[8]) > 0.0 &&
            atof(fields[10]) > 0.0 &&
            strcmp(fields[11], "0") != 0 &&
            strcmp(fields[12], "classify-failed") == 0) {
            saw_success = 1;
            break;
        }
    }

    fclose(fp);
    if (!saw_success) {
        fprintf(stderr,
                "missing later %s success trace for %s\n",
                operation,
                symbol);
    }
    return saw_success;
}

static int
run_controller_batch_retry_check(void)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    GumInterceptor** interceptor_slot =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners_slot =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** hook_addresses_slot = (gpointer**)required_symbol("hook_address");
    size_t* hook_count = (size_t*)required_symbol("peak_hook_address_count");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    GumInvocationListener* original_listener = NULL;
    char retry_batch_id[32];

    if (request_detach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        interceptor_slot == NULL ||
        listeners_slot == NULL ||
        hook_addresses_slot == NULL ||
        hook_count == NULL) {
        return 2;
    }

    if (*hook_count < 2 ||
        *interceptor_slot == NULL ||
        *listeners_slot == NULL ||
        *hook_addresses_slot == NULL ||
        (*listeners_slot)[0] == NULL ||
        (*listeners_slot)[1] == NULL ||
        (*hook_addresses_slot)[0] == NULL ||
        (*hook_addresses_slot)[1] == NULL) {
        fprintf(stderr, "preloaded PEAK hooks are not initialized for two targets\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED ||
        hook_state(1) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected both hooks to start attached\n");
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }
    unlink(trace_path);

    controller_stop();
    if (!request_detach(0) || !request_detach(1)) {
        fprintf(stderr, "failed to queue both detach requests\n");
        return 2;
    }

    original_listener = (*listeners_slot)[0];
    (*listeners_slot)[0] = (*listeners_slot)[1];
    if (controller_drain(0)) {
        (*listeners_slot)[0] = original_listener;
        fprintf(stderr, "first poisoned drain unexpectedly cleared all pending work\n");
        return 2;
    }
    (*listeners_slot)[0] = original_listener;
    if (hook_state(0) != PEAK_HOOK_DETACHED ||
        hook_state(1) != PEAK_HOOK_DETACHED) {
        if (hook_state(0) != PEAK_HOOK_DETACH_REQUESTED ||
            hook_state(1) != PEAK_HOOK_DETACHED) {
            fprintf(stderr,
                    "expected poisoned hook pending and peer detached after first batch\n");
            return 2;
        }
    }
    if (!controller_drain(1000)) {
        fprintf(stderr, "controller did not drain restored retry batch\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED ||
        hook_state(1) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "expected both hooks detached after restored retry drain\n");
        return 2;
    }
    if (!find_retry_batch_id(trace_path, retry_batch_id, sizeof(retry_batch_id))) {
        fprintf(stderr, "missing retryable prepare-failed batch trace\n");
        return 2;
    }
    if (!trace_proves_retry_preservation(trace_path, retry_batch_id)) {
        return 2;
    }

    printf("controller_batch_retry_ok batch_id=%s\n", retry_batch_id);
    return 0;
}

static int
run_controller_unsupported_gum_pc_retry_check(int argc, char** argv)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    GumInterceptor** interceptor_slot =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners_slot =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** hook_addresses_slot = (gpointer**)required_symbol("hook_address");
    size_t* hook_count = (size_t*)required_symbol("peak_hook_address_count");
    PeakGumGetPcDiagnosticsFn get_pc_diagnostics =
        (PeakGumGetPcDiagnosticsFn)required_symbol(
            "gum_interceptor_peak_get_pc_diagnostics");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    const char* stop_pc_file = getenv("FAKE_DETACH_HELPER_STOP_PC_FILE");
    const char* helper_log_path = getenv("FAKE_DETACH_HELPER_LOG");
    const char* pc_case = parse_string_arg(argc, argv, "--unsupported-gum-pc-case");
    char helper_log_template[] = "/tmp/peak_hotloop_unsupported_gum_pc_log_XXXXXX";
    int helper_log_fd;
    GumPeakPcDiagnostics diagnostics;
    gpointer unsupported_pc = NULL;
    char retry_batch_id[32] = "";
    int resolved;

    if (pc_case == NULL || pc_case[0] == '\0') {
        pc_case = getenv("PEAK_UNSUPPORTED_GUM_PC_CASE");
    }

    if (request_detach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        interceptor_slot == NULL ||
        listeners_slot == NULL ||
        hook_addresses_slot == NULL ||
        hook_count == NULL ||
        get_pc_diagnostics == NULL) {
        return 2;
    }

    if (*hook_count < 1 ||
        *interceptor_slot == NULL ||
        *listeners_slot == NULL ||
        *hook_addresses_slot == NULL ||
        (*listeners_slot)[0] == NULL ||
        (*hook_addresses_slot)[0] == NULL) {
        fprintf(stderr, "preloaded PEAK hook is not initialized\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook 0 to start attached\n");
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }

    if (!get_pc_diagnostics(*interceptor_slot,
                            (*hook_addresses_slot)[0],
                            (*listeners_slot)[0],
                            &diagnostics)) {
        fprintf(stderr, "Gum PC diagnostics unavailable for hotloop hook\n");
        return 77;
    }
    resolved =
        resolve_unsupported_gum_pc_case(pc_case, &diagnostics, &unsupported_pc);
    if (resolved < 0) {
        return 2;
    }
    if (resolved == 0) {
        return 77;
    }

    if (helper_log_path == NULL || helper_log_path[0] == '\0') {
        helper_log_fd = mkstemp(helper_log_template);
        if (helper_log_fd < 0) {
            perror("mkstemp helper log");
            return 2;
        }
        close(helper_log_fd);
        helper_log_path = helper_log_template;
        if (setenv("FAKE_DETACH_HELPER_LOG", helper_log_path, 1) != 0) {
            perror("setenv helper log");
            unlink(helper_log_path);
            return 2;
        }
    } else {
        unlink(helper_log_path);
    }

    unlink(trace_path);
    if (stop_pc_file != NULL && stop_pc_file[0] != '\0') {
        unlink_if_path(stop_pc_file);
        if (write_pointer_file(stop_pc_file, unsupported_pc) != 0) {
            perror("write stop PC file");
            unlink_if_path(helper_log_path);
            unlink_if_path(stop_pc_file);
            return 2;
        }
    } else {
        if (setenv("FAKE_DETACH_HELPER_SCENARIO", "synthetic-stop-once", 1) != 0 ||
            set_pointer_env("FAKE_DETACH_HELPER_STOP_PC", unsupported_pc) != 0) {
            perror("setenv");
            unlink_if_path(helper_log_path);
            return 2;
        }
    }

    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue detach request\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    if (controller_drain(0)) {
        fprintf(stderr, "unsupported Gum PC drain unexpectedly completed\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_DETACH_REQUESTED) {
        fprintf(stderr, "expected unsupported Gum PC hook to remain pending\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!trace_has_retryable_classify_failure_without_success(
            trace_path,
            "peak_detach_hot_target",
            "detach",
            retry_batch_id,
            sizeof(retry_batch_id))) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!helper_log_count_is(helper_log_path, "STOP", 1) ||
        !helper_log_count_is(helper_log_path, "RESUME", 1) ||
        !helper_log_count_is(helper_log_path, "EVACUATE", 0)) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    if (!controller_drain(1000)) {
        fprintf(stderr, "controller did not drain restored Gum PC retry\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "expected hook detached after restored retry drain\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!trace_has_success_after_retry(trace_path,
                                       "peak_detach_hot_target",
                                       "detach")) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!helper_log_count_is(helper_log_path, "STOP", 2) ||
        !helper_log_count_is(helper_log_path, "RESUME", 2) ||
        !helper_log_count_is(helper_log_path, "EVACUATE", 1)) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    printf("controller_unsupported_gum_pc_retry_ok case=%s batch_id=%s\n",
           pc_case != NULL && pc_case[0] != '\0' ? pc_case : "first-available",
           retry_batch_id);
    unlink_if_path(helper_log_path);
    unlink_if_path(stop_pc_file);
    return 0;
}

static int
run_controller_reattach_retry_check(int argc, char** argv)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakRequestReattachFn request_reattach =
        (PeakRequestReattachFn)required_symbol("peak_general_listener_request_reattach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    GumInterceptor** interceptor_slot =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners_slot =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** hook_addresses_slot = (gpointer**)required_symbol("hook_address");
    size_t* hook_count = (size_t*)required_symbol("peak_hook_address_count");
    PeakGumGetPcDiagnosticsFn get_pc_diagnostics =
        (PeakGumGetPcDiagnosticsFn)required_symbol(
            "gum_interceptor_peak_get_pc_diagnostics");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    const char* stop_pc_file = getenv("FAKE_DETACH_HELPER_STOP_PC_FILE");
    const char* helper_log_path = getenv("FAKE_DETACH_HELPER_LOG");
    const char* pc_case = parse_string_arg(argc, argv, "--unsupported-gum-pc-case");
    char helper_log_template[] = "/tmp/peak_hotloop_reattach_retry_log_XXXXXX";
    char stop_pc_template[] = "/tmp/peak_hotloop_reattach_retry_pc_XXXXXX";
    int helper_log_fd;
    int stop_pc_fd = -1;
    GumPeakPcDiagnostics diagnostics;
    gpointer unsupported_pc = NULL;
    char retry_batch_id[32] = "";
    int resolved;

    if (pc_case == NULL || pc_case[0] == '\0') {
        pc_case = getenv("PEAK_UNSUPPORTED_GUM_PC_CASE");
    }

    if (request_detach == NULL ||
        request_reattach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        interceptor_slot == NULL ||
        listeners_slot == NULL ||
        hook_addresses_slot == NULL ||
        hook_count == NULL ||
        get_pc_diagnostics == NULL) {
        return 2;
    }

    if (*hook_count < 1 ||
        *interceptor_slot == NULL ||
        *listeners_slot == NULL ||
        *hook_addresses_slot == NULL ||
        (*listeners_slot)[0] == NULL ||
        (*hook_addresses_slot)[0] == NULL) {
        fprintf(stderr, "preloaded PEAK hook is not initialized\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook 0 to start attached\n");
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }

    if (!get_pc_diagnostics(*interceptor_slot,
                            (*hook_addresses_slot)[0],
                            (*listeners_slot)[0],
                            &diagnostics)) {
        fprintf(stderr, "Gum PC diagnostics unavailable for hotloop hook\n");
        return 77;
    }
    resolved =
        resolve_unsupported_gum_pc_case(pc_case, &diagnostics, &unsupported_pc);
    if (resolved < 0) {
        return 2;
    }
    if (resolved == 0) {
        return 77;
    }

    if (helper_log_path == NULL || helper_log_path[0] == '\0') {
        helper_log_fd = mkstemp(helper_log_template);
        if (helper_log_fd < 0) {
            perror("mkstemp helper log");
            return 2;
        }
        close(helper_log_fd);
        helper_log_path = helper_log_template;
        if (setenv("FAKE_DETACH_HELPER_LOG", helper_log_path, 1) != 0) {
            perror("setenv helper log");
            unlink(helper_log_path);
            return 2;
        }
    } else {
        unlink(helper_log_path);
    }

    if (stop_pc_file == NULL || stop_pc_file[0] == '\0') {
        stop_pc_fd = mkstemp(stop_pc_template);
        if (stop_pc_fd < 0) {
            perror("mkstemp stop PC");
            unlink_if_path(helper_log_path);
            return 2;
        }
        close(stop_pc_fd);
        stop_pc_file = stop_pc_template;
        if (setenv("FAKE_DETACH_HELPER_STOP_PC_FILE", stop_pc_file, 1) != 0) {
            perror("setenv stop PC file");
            unlink_if_path(helper_log_path);
            unlink_if_path(stop_pc_file);
            return 2;
        }
    }

    if (setenv("FAKE_DETACH_HELPER_SCENARIO",
               "synthetic-stop-file-once",
               1) != 0) {
        perror("setenv scenario");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    unlink(trace_path);
    unlink_if_path(stop_pc_file);

    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue initial detach request\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!controller_drain(1000)) {
        fprintf(stderr, "controller did not drain initial detach\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "expected hook detached before reattach retry\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    if (write_pointer_file(stop_pc_file, unsupported_pc) != 0) {
        perror("write stop PC file");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!request_reattach(0)) {
        fprintf(stderr, "failed to queue reattach request\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (controller_drain(0)) {
        fprintf(stderr, "unsupported Gum PC reattach unexpectedly completed\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_REATTACH_REQUESTED) {
        fprintf(stderr, "expected reattach hook to remain pending\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!trace_has_retryable_classify_failure_without_success(
            trace_path,
            "peak_detach_hot_target",
            "reattach",
            retry_batch_id,
            sizeof(retry_batch_id))) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    if (!controller_drain(1000)) {
        fprintf(stderr, "controller did not drain restored reattach retry\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook attached after restored reattach retry\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!trace_has_success_after_retry(trace_path,
                                       "peak_detach_hot_target",
                                       "reattach")) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!helper_log_count_is(helper_log_path, "STOP", 3) ||
        !helper_log_count_is(helper_log_path, "RESUME", 3) ||
        !helper_log_count_is(helper_log_path, "EVACUATE", 2)) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    printf("controller_reattach_retry_ok case=%s batch_id=%s\n",
           pc_case != NULL && pc_case[0] != '\0' ? pc_case : "first-available",
           retry_batch_id);
    unlink_if_path(helper_log_path);
    unlink_if_path(stop_pc_file);
    (void)stop_pc_fd;
    return 0;
}
#else
static int
run_controller_batch_retry_check(void)
{
    fprintf(stderr, "controller batch retry check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
}

static int
run_controller_unsupported_gum_pc_retry_check(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "controller unsupported Gum PC check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
}

static int
run_controller_reattach_retry_check(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "controller reattach retry check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
}
#endif

static int
start_gate_init(StartGate* gate, long expected)
{
    memset(gate, 0, sizeof(*gate));
    gate->expected = expected;
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&gate->cond, NULL) != 0) {
        pthread_mutex_destroy(&gate->mutex);
        return -1;
    }
    return 0;
}

static void
start_gate_wait(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->arrived++;
    if (gate->arrived == gate->expected) {
        gate->open = 1;
        pthread_cond_broadcast(&gate->cond);
    } else {
        while (!gate->open) {
            pthread_cond_wait(&gate->cond, &gate->mutex);
        }
    }
    pthread_mutex_unlock(&gate->mutex);
}

static void
start_gate_abort(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->open = 1;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

static void
start_gate_destroy(StartGate* gate)
{
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
}

int
main(int argc, char** argv)
{
    if (has_flag_arg(argc, argv, "--controller-batch-retry-check")) {
        return run_controller_batch_retry_check();
    }
    if (has_flag_arg(argc, argv, "--controller-unsupported-gum-pc-retry-check")) {
        return run_controller_unsupported_gum_pc_retry_check(argc, argv);
    }
    if (has_flag_arg(argc, argv, "--controller-reattach-retry-check")) {
        return run_controller_reattach_retry_check(argc, argv);
    }

    long threads = parse_long_arg(argc, argv, "--threads", 4);
    long seconds = parse_long_arg(argc, argv, "--seconds", 3);
    long spawner_threads = parse_long_arg(argc, argv, "--spawner-threads", 2);
    int paired_targets = has_flag_arg(argc, argv, "--paired-targets");
    int spawn_transient_threads = has_flag_arg(argc, argv, "--spawn-transient-threads");
    pthread_t* tids = calloc((size_t)threads, sizeof(*tids));
    WorkerState* states = calloc((size_t)threads, sizeof(*states));
    pthread_t* spawner_tids = NULL;
    SpawnerState* spawner_states = NULL;
    StartGate gate;
    long created_threads = 0;
    long created_spawners = 0;
    if (spawn_transient_threads) {
        spawner_tids = calloc((size_t)spawner_threads, sizeof(*spawner_tids));
        spawner_states = calloc((size_t)spawner_threads, sizeof(*spawner_states));
    }
    if (tids == NULL || states == NULL ||
        (spawn_transient_threads &&
         (spawner_tids == NULL || spawner_states == NULL))) {
        perror("calloc");
        free(tids);
        free(states);
        free(spawner_tids);
        free(spawner_states);
        return 2;
    }
    if (start_gate_init(&gate, threads + 1) != 0) {
        perror("start_gate_init");
        free(tids);
        free(states);
        free(spawner_tids);
        free(spawner_states);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&spawned_worker_count, 0, memory_order_relaxed);
    for (long i = 0; i < threads; i++) {
        states[i].seed = (unsigned int)(0x9e3779b9u + (unsigned int)i);
        states[i].start_gate = &gate;
        states[i].paired_targets = paired_targets;
        if (pthread_create(&tids[i], NULL, worker_main, &states[i]) != 0) {
            perror("pthread_create");
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            start_gate_abort(&gate);
            for (long j = 0; j < created_threads; j++) {
                pthread_join(tids[j], NULL);
            }
            start_gate_destroy(&gate);
            free(tids);
            free(states);
            free(spawner_tids);
            free(spawner_states);
            return 2;
        }
        created_threads++;
    }

    start_gate_wait(&gate);
    if (spawn_transient_threads) {
        for (long i = 0; i < spawner_threads; i++) {
            spawner_states[i].seed = 0x7f4a7c15u + (unsigned int)i;
            if (pthread_create(&spawner_tids[i],
                               NULL,
                               spawner_main,
                               &spawner_states[i]) != 0) {
                perror("pthread_create spawner");
                atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
                for (long j = 0; j < created_spawners; j++) {
                    pthread_join(spawner_tids[j], NULL);
                }
                for (long j = 0; j < created_threads; j++) {
                    pthread_join(tids[j], NULL);
                }
                start_gate_destroy(&gate);
                free(tids);
                free(states);
                free(spawner_tids);
                free(spawner_states);
                return 2;
            }
            created_spawners++;
        }
    }

    double start = monotonic_seconds();
    sleep_for_seconds(seconds);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);

    for (long i = 0; i < created_spawners; i++) {
        pthread_join(spawner_tids[i], NULL);
    }

    uint64_t calls = 0;
    for (long i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
        calls += states[i].calls;
    }
    double elapsed = monotonic_seconds() - start;
    int trace_detach_success = 0;
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    trace_detach_success =
        trace_has_physical_detach_success(getenv("PEAK_DETACH_TRACE_PATH"));
#endif

    printf("threads=%ld seconds=%ld calls=%lu elapsed=%.6f calls_per_sec=%.3f side_effect=%lu spawned_threads=%lu trace_detach_success=%d\n",
           threads,
           seconds,
           (unsigned long)calls,
           elapsed,
           (double)calls / elapsed,
           (unsigned long)atomic_load_explicit(&side_effect, memory_order_relaxed),
           (unsigned long)atomic_load_explicit(&spawned_worker_count, memory_order_relaxed),
           trace_detach_success);

    start_gate_destroy(&gate);
    free(tids);
    free(states);
    free(spawner_tids);
    free(spawner_states);
    return 0;
}
