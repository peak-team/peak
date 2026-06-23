#define _GNU_SOURCE
#include "frida-gum.h"
#include "general_listener.h"

#include <aio.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mqueue.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int stop_requested;
static atomic_int unblock_backend_signal_requested;
static atomic_int user_signal_handler_count;
static atomic_uint_fast64_t side_effect;
static atomic_uint_fast64_t spawned_worker_count;
static atomic_uint_fast64_t blocked_signal_thread_count;

#define PEAK_DETACH_HOT_BURST 64
#define PEAK_DETACH_HOT_NOP_SLED \
    "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" \
    "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" \
    "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" \
    "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long expected;
    long arrived;
    int open;
} StartGate;

static void start_gate_wait(StartGate* gate);
static int start_gate_init(StartGate* gate, long expected);
static void start_gate_abort(StartGate* gate);
static void start_gate_destroy(StartGate* gate);

static void
user_collision_signal_handler(int signo)
{
    (void)signo;
    atomic_fetch_add_explicit(&user_signal_handler_count,
                              1,
                              memory_order_relaxed);
}

__attribute__((noinline, noclone, used, externally_visible, visibility("default")))
void peak_detach_hot_target(uint64_t value)
{
    (void)value;
    asm volatile(PEAK_DETACH_HOT_NOP_SLED ::: "memory");
}

__attribute__((noinline, noclone, used, externally_visible, visibility("default")))
void peak_detach_hot_target_two(uint64_t value)
{
    (void)value;
    asm volatile(PEAK_DETACH_HOT_NOP_SLED ::: "memory");
}

typedef struct {
    uint64_t calls;
    unsigned int seed;
    StartGate* start_gate;
    int paired_targets;
    int block_backend_signal;
    int wait_to_unblock_backend_signal;
} WorkerState;

typedef struct {
    unsigned int seed;
} SpawnedWorkerState;

typedef struct {
    unsigned int seed;
    uint64_t created;
} SpawnerState;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int open;
} ReleaseGate;

typedef struct {
    atomic_int* child_started;
    atomic_int* child_started_while_gate;
    int (*gate_epoch)(void);
    unsigned int seed;
} GateRaceChildState;

typedef struct {
    StartGate* ready_gate;
    ReleaseGate* release_gate;
    atomic_int* child_started;
    atomic_int* child_started_while_gate;
    atomic_int* create_attempted_during_gate;
    int (*gate_epoch)(void);
    unsigned int seed;
    int create_status;
    int join_status;
} GateRaceCreatorState;

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

static int
backend_signal_number(void)
{
    typedef int (*PeakSignalBackendSignumFn)(void);
    PeakSignalBackendSignumFn selected =
        (PeakSignalBackendSignumFn)dlsym(
            RTLD_DEFAULT,
            "peak_detach_controller_test_signal_backend_signum");
    int signo = selected != NULL ? selected() : SIGRTMIN + 2;

    if (signo > SIGRTMAX) {
        return 0;
    }
    return signo;
}

static int
set_backend_signal_blocked(int blocked)
{
    int signo = backend_signal_number();
    sigset_t set;

    if (signo == 0) {
        return -1;
    }
    if (blocked) {
        typedef int (*PeakSignalTestBlockFn)(void);
        PeakSignalTestBlockFn block_reserved =
            (PeakSignalTestBlockFn)dlsym(
                RTLD_DEFAULT,
                "peak_signal_policy_test_block_reserved_for_current_thread");
        if (block_reserved != NULL) {
            return block_reserved();
        }
    }
    sigemptyset(&set);
    sigaddset(&set, signo);
    return pthread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

static void*
worker_main(void* arg)
{
    WorkerState* state = (WorkerState*)arg;
    uint64_t local_calls = 0;
    unsigned int seed = state->seed;
    int backend_signal_blocked = 0;

    if (state->block_backend_signal &&
        set_backend_signal_blocked(1) == 0) {
        backend_signal_blocked = 1;
        atomic_fetch_add_explicit(&blocked_signal_thread_count,
                                  1,
                                  memory_order_relaxed);
    }

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
        if (backend_signal_blocked &&
            state->wait_to_unblock_backend_signal &&
            atomic_load_explicit(&unblock_backend_signal_requested,
                                 memory_order_acquire)) {
            if (set_backend_signal_blocked(0) == 0) {
                backend_signal_blocked = 0;
            }
        }
        for (int burst = 0; burst < PEAK_DETACH_HOT_BURST; burst++) {
            peak_detach_hot_target((uint64_t)(seed + local_calls));
            if (state->paired_targets) {
                peak_detach_hot_target_two((uint64_t)(seed ^ local_calls));
            }
            local_calls++;
        }
    }

    atomic_fetch_add_explicit(&side_effect,
                              ((uint64_t)seed << 32) ^ local_calls,
                              memory_order_relaxed);
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
    atomic_fetch_add_explicit(&side_effect,
                              ((uint64_t)seed << 32) ^ UINT64_C(32),
                              memory_order_relaxed);
    free(state);
    return NULL;
}

static int
release_gate_init(ReleaseGate* gate)
{
    memset(gate, 0, sizeof(*gate));
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
release_gate_wait(ReleaseGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    while (!gate->open) {
        pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
}

static void
release_gate_open(ReleaseGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->open = 1;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

static void
release_gate_destroy(ReleaseGate* gate)
{
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
}

static void*
gate_race_child_main(void* arg)
{
    GateRaceChildState* state = (GateRaceChildState*)arg;
    unsigned int seed = state->seed;

    if (state->gate_epoch != NULL && state->gate_epoch() != 0) {
        atomic_fetch_add_explicit(state->child_started_while_gate,
                                  1,
                                  memory_order_release);
    }
    atomic_fetch_add_explicit(state->child_started, 1, memory_order_release);
    for (uint64_t i = 0; i < 32; i++) {
        peak_detach_hot_target((uint64_t)(seed + i));
    }
    free(state);
    return NULL;
}

static void*
gate_race_creator_main(void* arg)
{
    GateRaceCreatorState* state = (GateRaceCreatorState*)arg;
    GateRaceChildState* child_state;
    pthread_t child;

    start_gate_wait(state->ready_gate);
    release_gate_wait(state->release_gate);

    child_state = malloc(sizeof(*child_state));
    if (child_state == NULL) {
        state->create_status = ENOMEM;
        return NULL;
    }
    child_state->child_started = state->child_started;
    child_state->child_started_while_gate = state->child_started_while_gate;
    child_state->gate_epoch = state->gate_epoch;
    child_state->seed = state->seed;

    if (state->gate_epoch != NULL && state->gate_epoch() != 0) {
        atomic_fetch_add_explicit(state->create_attempted_during_gate,
                                  1,
                                  memory_order_release);
    }
    state->create_status =
        pthread_create(&child, NULL, gate_race_child_main, child_state);
    if (state->create_status != 0) {
        free(child_state);
        return NULL;
    }
    state->join_status = pthread_join(child, NULL);
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
typedef int (*PeakControllerGateEpochFn)(void);
typedef size_t (*PeakControllerGateWaiterCountFn)(void);
typedef int (*PeakSignalBackendSignumFn)(void);
typedef int (*PeakSignalConflictCountFn)(void);
typedef int (*PeakSignalMigrationCountFn)(void);
typedef int (*PeakSignalUnexpectedDeliveryCountFn)(void);
typedef int (*PeakSignalBadCookieFn)(void);
typedef int (*PeakSignalTestBlockFn)(void);
typedef int (*PeakSignalfd4Fn)(int fd,
                               const sigset_t* mask,
                               int sizemask,
                               int flags);
typedef gboolean (*PeakGumGetPcDiagnosticsFn)(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    GumPeakPcDiagnostics* diagnostics);

typedef struct {
    PeakControllerDrainFn drain;
    unsigned int timeout_ms;
    gboolean drained;
} ControllerDrainState;

static int
expect_signal_migrated(const char* label,
                       PeakSignalBackendSignumFn selected_signal,
                       int previous_signum,
                       int* migrated_signum)
{
    int current = selected_signal();
    if (current <= 0 || current == previous_signum) {
        fprintf(stderr,
                "%s collision did not migrate PEAK reserved signal: previous=%d current=%d\n",
                label,
                previous_signum,
                current);
        return 0;
    }
    *migrated_signum = current;
    return 1;
}

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

static void*
controller_drain_main(void* arg)
{
    ControllerDrainState* state = (ControllerDrainState*)arg;

    state->drained = state->drain(state->timeout_ms);
    return NULL;
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
trace_has_entry_byte_only_success_without_classify_failure(const char* trace_path,
                                                           const char* symbol,
                                                           const char* operation)
{
    FILE* fp = fopen(trace_path, "r");
    char line[1024];
    int saw_success = 0;
    int saw_classify_failure = 0;

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

        if ((strcmp(fields[4], "prepare-failed") == 0 &&
             strcmp(fields[6], "classify-failed") == 0) ||
            (count >= 13 && strcmp(fields[12], "classify-failed") == 0)) {
            saw_classify_failure = 1;
        }

        if (strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            atof(fields[10]) > 0.0 &&
            (count < 13 || strcmp(fields[12], "safe") == 0)) {
            saw_success = 1;
        }
    }

    fclose(fp);
    if (!saw_success) {
        fprintf(stderr,
                "missing entry-byte-only %s success trace for %s\n",
                operation,
                symbol);
    }
    if (saw_classify_failure) {
        fprintf(stderr,
                "unexpected classify-failed %s trace for %s\n",
                operation,
                symbol);
    }
    return saw_success && !saw_classify_failure;
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
trace_has_detach_prepare_blocked_signal(const char* trace_path)
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
        char* fields[17];
        int count = parse_csv_line(line, fields, 17);

        if (count >= 14 &&
            strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "prepare-failed") == 0 &&
            strcmp(fields[5], "0") == 0 &&
            strcmp(fields[6], "unsupported") == 0 &&
            strcmp(fields[13], "signal-reserved-blocked") == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int
trace_has_detach_prepare_unexpected_signal(const char* trace_path)
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
        char* fields[17];
        int count = parse_csv_line(line, fields, 17);

        if (count >= 14 &&
            strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "prepare-failed") == 0 &&
            strcmp(fields[5], "0") == 0 &&
            strcmp(fields[6], "unsupported") == 0 &&
            strcmp(fields[13], "signal-unexpected-delivery") == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
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

    if (!controller_drain(1000)) {
        fprintf(stderr, "controller did not drain entry-byte-only Gum PC detach\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "expected unsupported Gum PC hook detached\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!trace_has_entry_byte_only_success_without_classify_failure(
            trace_path,
            "peak_detach_hot_target",
            "detach")) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!helper_log_count_is(helper_log_path, "STOP", 1) ||
        !helper_log_count_is(helper_log_path, "RESUME", 1) ||
        !helper_log_count_is(helper_log_path, "EVACUATE", 1)) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    printf("controller_unsupported_gum_pc_entry_bytes_ok case=%s\n",
           pc_case != NULL && pc_case[0] != '\0' ? pc_case : "first-available");
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
    if (!controller_drain(1000)) {
        fprintf(stderr, "controller did not drain entry-byte-only Gum PC reattach\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook attached after entry-byte-only reattach\n");
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!trace_has_entry_byte_only_success_without_classify_failure(
            trace_path,
            "peak_detach_hot_target",
            "reattach")) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }
    if (!helper_log_count_is(helper_log_path, "STOP", 2) ||
        !helper_log_count_is(helper_log_path, "RESUME", 2) ||
        !helper_log_count_is(helper_log_path, "EVACUATE", 2)) {
        unlink_if_path(helper_log_path);
        unlink_if_path(stop_pc_file);
        return 2;
    }

    printf("controller_reattach_entry_bytes_ok case=%s\n",
           pc_case != NULL && pc_case[0] != '\0' ? pc_case : "first-available");
    unlink_if_path(helper_log_path);
    unlink_if_path(stop_pc_file);
    (void)stop_pc_fd;
    return 0;
}

static int
run_signal_blocked_delivery_check(int argc, char** argv)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    long threads = parse_long_arg(argc, argv, "--threads", 4);
    pthread_t* tids = NULL;
    WorkerState* states = NULL;
    StartGate gate;
    int gate_initialized = 0;
    long created_threads = 0;
    int ok = 0;

    if (request_detach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL) {
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook 0 to start attached\n");
        return 2;
    }

    tids = calloc((size_t)threads, sizeof(*tids));
    states = calloc((size_t)threads, sizeof(*states));
    if (tids == NULL || states == NULL) {
        perror("calloc");
        goto cleanup;
    }
    if (start_gate_init(&gate, threads + 1) != 0) {
        perror("start_gate_init");
        goto cleanup;
    }
    gate_initialized = 1;

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&unblock_backend_signal_requested,
                          0,
                          memory_order_release);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&spawned_worker_count, 0, memory_order_relaxed);
    atomic_store_explicit(&blocked_signal_thread_count, 0, memory_order_relaxed);

    for (long i = 0; i < threads; i++) {
        states[i].seed = (unsigned int)(0x6a09e667u + (unsigned int)i);
        states[i].start_gate = &gate;
        states[i].paired_targets = 0;
        states[i].block_backend_signal = 1;
        states[i].wait_to_unblock_backend_signal = 1;
        if (pthread_create(&tids[i], NULL, worker_main, &states[i]) != 0) {
            perror("pthread_create blocked worker");
            goto cleanup;
        }
        created_threads++;
    }
    start_gate_wait(&gate);

    if (atomic_load_explicit(&blocked_signal_thread_count,
                             memory_order_acquire) != (uint64_t)threads) {
        fprintf(stderr,
                "expected all workers to block backend signal: got %lu expected %ld\n",
                (unsigned long)atomic_load_explicit(&blocked_signal_thread_count,
                                                    memory_order_relaxed),
                threads);
        goto cleanup;
    }

    unlink(trace_path);
    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue blocked-signal detach request\n");
        goto cleanup;
    }
    if (!controller_drain(2000)) {
        fprintf(stderr, "blocked-signal detach did not drain after fast failure\n");
        goto cleanup;
    }
    if (hook_state(0) == PEAK_HOOK_DETACHED) {
        fprintf(stderr, "blocked-signal detach physically detached\n");
        goto cleanup;
    }
    if (!trace_has_detach_prepare_blocked_signal(trace_path)) {
        fprintf(stderr, "missing blocked-signal fail-closed trace\n");
        goto cleanup;
    }

    ok = 1;

cleanup:
    atomic_store_explicit(&unblock_backend_signal_requested,
                          1,
                          memory_order_release);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
    if (gate_initialized) {
        start_gate_abort(&gate);
    }
    for (long i = 0; i < created_threads; i++) {
        pthread_join(tids[i], NULL);
    }
    if (ok) {
        (void)controller_drain(2000);
    }
    if (gate_initialized) {
        start_gate_destroy(&gate);
    }
    free(tids);
    free(states);

    if (!ok) {
        return 2;
    }

    printf("signal_blocked_delivery_ok blocked_signal_threads=%lu blocked_fast_fail=1\n",
           (unsigned long)atomic_load_explicit(&blocked_signal_thread_count,
                                               memory_order_relaxed));
    return 0;
}

static int
run_signal_user_collision_check(void)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    PeakSignalBackendSignumFn selected_signal =
        (PeakSignalBackendSignumFn)required_symbol(
            "peak_detach_controller_test_signal_backend_signum");
    PeakSignalConflictCountFn conflict_count =
        (PeakSignalConflictCountFn)required_symbol(
            "peak_signal_policy_conflict_count");
    PeakSignalMigrationCountFn migration_count =
        (PeakSignalMigrationCountFn)required_symbol(
            "peak_signal_policy_migration_count");
    PeakSignalUnexpectedDeliveryCountFn unexpected_count =
        (PeakSignalUnexpectedDeliveryCountFn)required_symbol(
            "peak_signal_policy_unexpected_delivery_count");
    PeakSignalTestBlockFn block_reserved_signal =
        (PeakSignalTestBlockFn)required_symbol(
            "peak_signal_policy_test_block_reserved_for_current_thread");
    PeakSignalfd4Fn signalfd4_fn =
        (PeakSignalfd4Fn)dlsym(RTLD_DEFAULT, "signalfd4");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    int collision_signum = SIGRTMIN + 2;
    int reserved_signum;
    int initial_reserved_signum;
    int migrated_signum;
    int conflicts_before;
    int conflicts_after;
    int migrations_before;
    int migrations_after;
    int unexpected_before;
    sigset_t set;
    sigset_t oldset;
    struct timespec zero_timeout = { 0, 0 };
    pthread_t worker;
    WorkerState worker_state;
    StartGate worker_gate;
    int worker_started = 0;
    int worker_gate_initialized = 0;
    int collision_ok = 0;

    if (request_detach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        selected_signal == NULL ||
        conflict_count == NULL ||
        migration_count == NULL ||
        unexpected_count == NULL) {
        return 2;
    }
    if (block_reserved_signal == NULL) {
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook 0 to start attached\n");
        return 2;
    }
    if (collision_signum > SIGRTMAX) {
        return 77;
    }

    atomic_store_explicit(&user_signal_handler_count, 0, memory_order_relaxed);
    struct sigaction user_action;
    memset(&user_action, 0, sizeof(user_action));
    sigemptyset(&user_action.sa_mask);
    user_action.sa_handler = user_collision_signal_handler;
    if (sigaction(collision_signum, &user_action, NULL) != 0) {
        perror("sigaction user collision signal");
        return 2;
    }
    if (raise(collision_signum) != 0) {
        perror("raise user collision signal");
        return 2;
    }
    if (atomic_load_explicit(&user_signal_handler_count,
                             memory_order_relaxed) != 1) {
        fprintf(stderr, "user collision signal handler was not preserved\n");
        return 2;
    }

    reserved_signum = selected_signal();
    if (reserved_signum <= 0 || reserved_signum > SIGRTMAX) {
        fprintf(stderr, "signal backend did not reserve a real-time signal\n");
        return 2;
    }
    if (reserved_signum == collision_signum) {
        fprintf(stderr, "reserved signal collided with user-owned signal\n");
        return 2;
    }
    initial_reserved_signum = reserved_signum;

    conflicts_before = conflict_count();
    migrations_before = migration_count();
    unexpected_before = unexpected_count();

    struct sigaction query_action;
    memset(&query_action, 0, sizeof(query_action));
    if (sigaction(reserved_signum, NULL, &query_action) != 0) {
        perror("sigaction query reserved signal");
        return 2;
    }
    if (query_action.sa_handler != SIG_DFL) {
        fprintf(stderr,
                "reserved signal query leaked non-default handler to user code\n");
        return 2;
    }
    if (selected_signal() != reserved_signum) {
        fprintf(stderr, "read-only reserved signal query unexpectedly migrated\n");
        return 2;
    }

    struct sigaction previous_action;
    memset(&previous_action, 0, sizeof(previous_action));
    user_action.sa_handler = user_collision_signal_handler;
    if (sigaction(reserved_signum, &user_action, &previous_action) != 0) {
        perror("sigaction reserved signal migrated");
        return 2;
    }
    if (previous_action.sa_handler != SIG_DFL) {
        fprintf(stderr,
                "reserved signal migration exposed PEAK handler as old action\n");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "sigaction collision did not migrate PEAK reserved signal\n");
        return 2;
    }
    if (raise(reserved_signum) != 0) {
        perror("raise migrated user signal");
        return 2;
    }
    if (atomic_load_explicit(&user_signal_handler_count,
                             memory_order_relaxed) != 2) {
        fprintf(stderr, "migrated user signal handler did not run\n");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    if (pthread_sigmask(SIG_BLOCK, &set, &oldset) != 0) {
        perror("pthread_sigmask reserved signal migrated");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "pthread_sigmask collision did not migrate PEAK reserved signal\n");
        return 2;
    }
    sigset_t current_set;
    if (pthread_sigmask(SIG_BLOCK, NULL, &current_set) != 0) {
        perror("pthread_sigmask query after migration");
        return 2;
    }
    if (sigismember(&current_set, reserved_signum) != 1 ||
        sigismember(&current_set, migrated_signum) == 1) {
        fprintf(stderr,
                "mask migration did not preserve user mask while keeping PEAK signal unblocked\n");
        return 2;
    }
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        perror("pthread_sigmask unblock migrated user signal");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
        perror("sigprocmask reserved signal migrated");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "sigprocmask collision did not migrate PEAK reserved signal\n");
        return 2;
    }
    if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0) {
        perror("sigprocmask unblock migrated user signal");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    if (sigtimedwait(&set, NULL, &zero_timeout) != -1 || errno != EAGAIN) {
        fprintf(stderr,
                "sigtimedwait migrated reserved signal returned unexpected result: errno=%d\n",
                errno);
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "sigtimedwait collision did not migrate PEAK reserved signal\n");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    if (block_reserved_signal() != 0) {
        perror("block reserved signal for sigwait");
        return 2;
    }
    if (syscall(SYS_tgkill,
                getpid(),
                syscall(SYS_gettid),
                reserved_signum) != 0) {
        perror("queue reserved signal for sigwait");
        return 2;
    }
    int waited_signal = 0;
    if (sigwait(&set, &waited_signal) != 0 ||
        waited_signal != reserved_signum) {
        fprintf(stderr,
                "sigwait did not consume migrated reserved signal: got=%d expected=%d\n",
                waited_signal,
                reserved_signum);
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "sigwait collision did not migrate PEAK reserved signal\n");
        return 2;
    }
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        perror("unblock migrated sigwait signal");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    if (block_reserved_signal() != 0) {
        perror("block reserved signal for sigwaitinfo");
        return 2;
    }
    if (syscall(SYS_tgkill,
                getpid(),
                syscall(SYS_gettid),
                reserved_signum) != 0) {
        perror("queue reserved signal for sigwaitinfo");
        return 2;
    }
    siginfo_t waited_info;
    memset(&waited_info, 0, sizeof(waited_info));
    if (sigwaitinfo(&set, &waited_info) != reserved_signum) {
        perror("sigwaitinfo migrated reserved signal");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "sigwaitinfo collision did not migrate PEAK reserved signal\n");
        return 2;
    }
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        perror("unblock migrated sigwaitinfo signal");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    int sfd = signalfd(-1, &set, 0);
    if (sfd < 0) {
        perror("signalfd reserved signal migrated");
        return 2;
    }
    close(sfd);
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "signalfd collision did not migrate PEAK reserved signal\n");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    if (signalfd4_fn != NULL) {
        errno = 0;
        sfd = signalfd4_fn(-1, &set, (int)(_NSIG / 8), SFD_CLOEXEC);
        if (sfd < 0) {
            perror("signalfd4 reserved signal migrated");
            return 2;
        }
        close(sfd);
        migrated_signum = selected_signal();
        if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
            fprintf(stderr,
                    "signalfd4 collision did not migrate PEAK reserved signal\n");
            return 2;
        }
    }

    reserved_signum = migrated_signum;
    struct sigevent ev;
    timer_t timerid;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = reserved_signum;
    errno = 0;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timerid) != 0) {
        perror("timer_create reserved SIGEV_SIGNAL migrated");
        return 2;
    }
    timer_delete(timerid);
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "timer_create collision did not migrate PEAK reserved signal\n");
        return 2;
    }

#ifdef SIGEV_THREAD_ID
    reserved_signum = migrated_signum;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = reserved_signum;
    ev._sigev_un._tid = (int)syscall(SYS_gettid);
    errno = 0;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timerid) != 0) {
        perror("timer_create reserved SIGEV_THREAD_ID migrated");
        return 2;
    }
    timer_delete(timerid);
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "SIGEV_THREAD_ID collision did not migrate PEAK reserved signal\n");
        return 2;
    }
#endif

    reserved_signum = migrated_signum;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = reserved_signum;
    errno = 0;
    (void)mq_notify((mqd_t)-1, &ev);
    if (!expect_signal_migrated("mq_notify",
                                selected_signal,
                                reserved_signum,
                                &migrated_signum)) {
        return 2;
    }

#ifdef SYS_rt_sigprocmask
    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    (void)syscall(SYS_rt_sigprocmask,
                  SIG_BLOCK,
                  &set,
                  NULL,
                  sizeof(unsigned long));
    if (!expect_signal_migrated("syscall:rt_sigprocmask",
                                selected_signal,
                                reserved_signum,
                                &migrated_signum)) {
        return 2;
    }
    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        perror("unblock raw rt_sigprocmask user signal");
        return 2;
    }
#endif

#ifdef SYS_rt_sigtimedwait
    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    (void)syscall(SYS_rt_sigtimedwait,
                  &set,
                  NULL,
                  &zero_timeout,
                  sizeof(unsigned long));
    if (!expect_signal_migrated("syscall:rt_sigtimedwait",
                                selected_signal,
                                reserved_signum,
                                &migrated_signum)) {
        return 2;
    }
#endif

#ifdef SYS_signalfd4
    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    (void)syscall(SYS_signalfd4,
                  -1,
                  &set,
                  sizeof(unsigned long),
                  SFD_CLOEXEC);
    if (!expect_signal_migrated("syscall:signalfd4",
                                selected_signal,
                                reserved_signum,
                                &migrated_signum)) {
        return 2;
    }
#endif

#ifdef SYS_timer_create
    reserved_signum = migrated_signum;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = reserved_signum;
    errno = 0;
    if (syscall(SYS_timer_create, CLOCK_MONOTONIC, &ev, &timerid) == 0) {
        timer_delete(timerid);
    }
    if (!expect_signal_migrated("syscall:timer_create",
                                selected_signal,
                                reserved_signum,
                                &migrated_signum)) {
        return 2;
    }
#endif

#ifdef SYS_mq_notify
    reserved_signum = migrated_signum;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = reserved_signum;
    errno = 0;
    (void)syscall(SYS_mq_notify, (mqd_t)-1, &ev);
    if (!expect_signal_migrated("syscall:mq_notify",
                                selected_signal,
                                reserved_signum,
                                &migrated_signum)) {
        return 2;
    }
#endif

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    if (pselect(0, NULL, NULL, NULL, &zero_timeout, &set) != 0) {
        perror("pselect reserved signal migrated");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "pselect collision did not migrate PEAK reserved signal\n");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    errno = 0;
    if (ppoll(NULL, 0, &zero_timeout, &set) != 0) {
        perror("ppoll reserved signal migrated");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "ppoll collision did not migrate PEAK reserved signal\n");
        return 2;
    }

    reserved_signum = migrated_signum;
    sigemptyset(&set);
    sigaddset(&set, reserved_signum);
    sigset_t collision_set;
    sigset_t collision_oldset;
    sigemptyset(&collision_set);
    sigaddset(&collision_set, collision_signum);
    if (pthread_sigmask(SIG_BLOCK, &collision_set, &collision_oldset) != 0) {
        perror("block collision signal for sigsuspend");
        return 2;
    }
    if (raise(collision_signum) != 0) {
        perror("queue collision signal for sigsuspend");
        return 2;
    }
    errno = 0;
    if (sigsuspend(&set) != -1 || errno != EINTR) {
        fprintf(stderr,
                "sigsuspend migrated reserved signal returned unexpected result: errno=%d\n",
                errno);
        return 2;
    }
    if (pthread_sigmask(SIG_SETMASK, &collision_oldset, NULL) != 0) {
        perror("restore collision signal mask");
        return 2;
    }
    migrated_signum = selected_signal();
    if (migrated_signum <= 0 || migrated_signum == reserved_signum) {
        fprintf(stderr,
                "sigsuspend collision did not migrate PEAK reserved signal\n");
        return 2;
    }

    sigemptyset(&oldset);
    if (pthread_sigmask(SIG_BLOCK, NULL, &oldset) != 0) {
        perror("pthread_sigmask query");
        return 2;
    }
    if (sigismember(&oldset, migrated_signum) == 1) {
        fprintf(stderr, "current PEAK signal became blocked after migration calls\n");
        return 2;
    }

    if (unexpected_count() != unexpected_before) {
        fprintf(stderr, "migration collision generated unexpected PEAK delivery\n");
        return 2;
    }
    conflicts_after = conflict_count();
    migrations_after = migration_count();
    if (migrations_after <= migrations_before) {
        fprintf(stderr,
                "expected signal policy migrations: before=%d after=%d\n",
                migrations_before,
                migrations_after);
        return 2;
    }
    if (conflicts_after < conflicts_before + (migrations_after - migrations_before)) {
        fprintf(stderr,
                "expected conflicts to record migrations: before=%d after=%d migrations=%d\n",
                conflicts_before,
                conflicts_after,
                migrations_after - migrations_before);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&blocked_signal_thread_count, 0, memory_order_relaxed);
    if (start_gate_init(&worker_gate, 2) != 0) {
        perror("start_gate_init collision worker");
        return 2;
    }
    worker_gate_initialized = 1;
    memset(&worker_state, 0, sizeof(worker_state));
    worker_state.seed = 0x31415926u;
    worker_state.start_gate = &worker_gate;
    worker_state.paired_targets = 0;
    worker_state.block_backend_signal = 0;
    worker_state.wait_to_unblock_backend_signal = 0;
    if (pthread_create(&worker, NULL, worker_main, &worker_state) != 0) {
        perror("pthread_create collision worker");
        goto collision_cleanup;
    }
    worker_started = 1;
    start_gate_wait(&worker_gate);

    unlink(trace_path);
    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue collision detach request\n");
        goto collision_cleanup;
    }
    if (!controller_drain(2000)) {
        fprintf(stderr, "collision detach did not drain\n");
        goto collision_cleanup;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "collision detach did not physically detach\n");
        goto collision_cleanup;
    }
    if (!trace_has_physical_detach_success(trace_path)) {
        fprintf(stderr, "missing physical detach success trace after collision\n");
        goto collision_cleanup;
    }
    collision_ok = 1;

collision_cleanup:
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
    if (worker_gate_initialized) {
        start_gate_abort(&worker_gate);
    }
    if (worker_started) {
        pthread_join(worker, NULL);
    }
    if (worker_gate_initialized) {
        start_gate_destroy(&worker_gate);
    }

    if (!collision_ok) {
        return 2;
    }

    printf("signal_user_collision_ok user_signal=%d initial_reserved_signal=%d current_reserved_signal=%d detach_success=1 migration_count=%d handler_preserved=1 mask_preserved=1 wait_preserved=1 signalfd_preserved=1 timer_preserved=1 mq_migrated=1 syscall_migrated=1 worker_calls=%lu conflicts=%d\n",
           collision_signum,
           initial_reserved_signum,
           selected_signal(),
           migrations_after - migrations_before,
           (unsigned long)worker_state.calls,
           conflicts_after - conflicts_before);
    return 0;
}

static int
run_signal_forced_collision_check(void)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    PeakSignalBackendSignumFn selected_signal =
        (PeakSignalBackendSignumFn)required_symbol(
            "peak_detach_controller_test_signal_backend_signum");
    PeakSignalConflictCountFn conflict_count =
        (PeakSignalConflictCountFn)required_symbol(
            "peak_signal_policy_conflict_count");
    PeakSignalMigrationCountFn migration_count =
        (PeakSignalMigrationCountFn)required_symbol(
            "peak_signal_policy_migration_count");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    int reserved_signum;
    int conflicts_before;
    int migrations_before;
    pthread_t worker;
    WorkerState worker_state;
    StartGate worker_gate;
    struct timespec zero_timeout = { 0, 0 };
    int worker_started = 0;
    int worker_gate_initialized = 0;
    int ok = 0;

    if (request_detach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        selected_signal == NULL ||
        conflict_count == NULL ||
        migration_count == NULL) {
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook 0 to start attached\n");
        return 2;
    }

    reserved_signum = selected_signal();
    if (reserved_signum <= 0 || reserved_signum > SIGRTMAX) {
        fprintf(stderr, "forced signal backend did not reserve a signal\n");
        return 2;
    }
#ifdef SYS_rt_sigprocmask
    errno = 0;
    if (syscall(SYS_rt_sigprocmask,
                SIG_BLOCK,
                (void*)1,
                NULL,
                sizeof(unsigned long)) != -1) {
        fprintf(stderr, "invalid raw rt_sigprocmask pointer was not rejected\n");
        return 2;
    }
#endif
#ifdef SYS_rt_sigtimedwait
    errno = 0;
    if (syscall(SYS_rt_sigtimedwait,
                (void*)1,
                NULL,
                &zero_timeout,
                sizeof(unsigned long)) != -1) {
        fprintf(stderr, "invalid raw rt_sigtimedwait pointer was not rejected\n");
        return 2;
    }
#endif
#ifdef SYS_timer_create
    timer_t invalid_timerid;
    errno = 0;
    if (syscall(SYS_timer_create,
                CLOCK_MONOTONIC,
                (void*)1,
                &invalid_timerid) != -1) {
        timer_delete(invalid_timerid);
        fprintf(stderr, "invalid raw timer_create sigevent pointer was accepted\n");
        return 2;
    }
#endif
#ifdef SYS_mq_notify
    errno = 0;
    if (syscall(SYS_mq_notify, (mqd_t)-1, (void*)1) != -1) {
        fprintf(stderr, "invalid raw mq_notify sigevent pointer was accepted\n");
        return 2;
    }
#endif
#ifdef SYS_pselect6
    errno = 0;
    if (syscall(SYS_pselect6,
                0,
                NULL,
                NULL,
                NULL,
                &zero_timeout,
                (void*)1) != -1) {
        fprintf(stderr, "invalid raw pselect6 sigmask pointer was accepted\n");
        return 2;
    }
#endif
    if (selected_signal() != reserved_signum) {
        fprintf(stderr, "invalid-pointer raw syscall probes migrated PEAK signal\n");
        return 2;
    }
    conflicts_before = conflict_count();
    migrations_before = migration_count();

    struct sigaction user_action;
    memset(&user_action, 0, sizeof(user_action));
    sigemptyset(&user_action.sa_mask);
    user_action.sa_handler = user_collision_signal_handler;
    errno = 0;
    if (sigaction(reserved_signum, &user_action, NULL) == 0 ||
        errno != EINVAL) {
        fprintf(stderr,
                "forced reserved signal collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
    if (selected_signal() != reserved_signum) {
        fprintf(stderr, "forced signal collision unexpectedly migrated\n");
        return 2;
    }
    errno = 0;
#ifdef SYS_tgkill
    if (syscall(SYS_tgkill,
                getpid(),
                syscall(SYS_gettid),
                reserved_signum) != -1 ||
        errno != EINVAL) {
        fprintf(stderr,
                "forced raw tgkill collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
#endif
#ifdef SYS_rt_tgsigqueueinfo
    siginfo_t raw_info;
    memset(&raw_info, 0, sizeof(raw_info));
    raw_info.si_signo = reserved_signum;
    raw_info.si_code = SI_QUEUE;
    raw_info.si_pid = getpid();
    raw_info.si_uid = getuid();
    errno = 0;
    if (syscall(SYS_rt_tgsigqueueinfo,
                getpid(),
                syscall(SYS_gettid),
                reserved_signum,
                &raw_info) != -1 ||
        errno != EINVAL) {
        fprintf(stderr,
                "forced raw rt_tgsigqueueinfo collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
#endif
    struct sigevent forced_ev;
    memset(&forced_ev, 0, sizeof(forced_ev));
    forced_ev.sigev_notify = SIGEV_SIGNAL;
    forced_ev.sigev_signo = reserved_signum;
    errno = 0;
    if (mq_notify((mqd_t)-1, &forced_ev) != -1 || errno != EINVAL) {
        fprintf(stderr,
                "forced mq_notify collision was not denied: errno=%d\n",
                errno);
        return 2;
    }

    struct aiocb forced_aiocb;
    memset(&forced_aiocb, 0, sizeof(forced_aiocb));
    forced_aiocb.aio_fildes = -1;
    forced_aiocb.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
    forced_aiocb.aio_sigevent.sigev_signo = reserved_signum;
    errno = 0;
    if (aio_read(&forced_aiocb) != -1 || errno != EINVAL) {
        fprintf(stderr,
                "forced aio_read collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
    errno = 0;
    if (aio_write(&forced_aiocb) != -1 || errno != EINVAL) {
        fprintf(stderr,
                "forced aio_write collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
    errno = 0;
    if (aio_fsync(O_SYNC, &forced_aiocb) != -1 || errno != EINVAL) {
        fprintf(stderr,
                "forced aio_fsync collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
    forced_aiocb.aio_lio_opcode = LIO_READ;
    struct aiocb* forced_aiocb_list[1] = { &forced_aiocb };
    errno = 0;
    if (lio_listio(LIO_NOWAIT,
                   forced_aiocb_list,
                   1,
                   &forced_ev) != -1 ||
        errno != EINVAL) {
        fprintf(stderr,
                "forced lio_listio collision was not denied: errno=%d\n",
                errno);
        return 2;
    }
    if (migration_count() != migrations_before ||
        conflict_count() <= conflicts_before) {
        fprintf(stderr, "forced collision counters did not match fail-closed path\n");
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&blocked_signal_thread_count, 0, memory_order_relaxed);
    if (start_gate_init(&worker_gate, 2) != 0) {
        perror("start_gate_init forced collision worker");
        return 2;
    }
    worker_gate_initialized = 1;
    memset(&worker_state, 0, sizeof(worker_state));
    worker_state.seed = 0x27182818u;
    worker_state.start_gate = &worker_gate;
    worker_state.paired_targets = 0;
    worker_state.block_backend_signal = 0;
    worker_state.wait_to_unblock_backend_signal = 0;
    if (pthread_create(&worker, NULL, worker_main, &worker_state) != 0) {
        perror("pthread_create forced collision worker");
        goto forced_cleanup;
    }
    worker_started = 1;
    start_gate_wait(&worker_gate);

    unlink(trace_path);
    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue forced collision detach request\n");
        goto forced_cleanup;
    }
    if (!controller_drain(2000)) {
        fprintf(stderr, "forced collision detach did not drain\n");
        goto forced_cleanup;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "forced collision detach did not physically detach\n");
        goto forced_cleanup;
    }
    if (!trace_has_physical_detach_success(trace_path)) {
        fprintf(stderr,
                "missing physical detach success trace after forced collision\n");
        goto forced_cleanup;
    }
    ok = 1;

forced_cleanup:
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
    if (worker_gate_initialized) {
        start_gate_abort(&worker_gate);
    }
    if (worker_started) {
        pthread_join(worker, NULL);
    }
    if (worker_gate_initialized) {
        start_gate_destroy(&worker_gate);
    }
    if (!ok) {
        return 2;
    }

    printf("signal_forced_collision_ok forced_signal=%d detach_success=1 migration_count=0 denied=1 invalid_pointer_guard=1 mq_denied=1 aio_denied=1 syscall_denied=1 worker_calls=%lu conflicts=%d\n",
           reserved_signum,
           (unsigned long)worker_state.calls,
           conflict_count() - conflicts_before);
    return 0;
}

static int
run_signal_bad_cookie_check(void)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)required_symbol("peak_general_listener_request_detach");
    PeakControllerDrainFn controller_drain =
        (PeakControllerDrainFn)required_symbol("peak_general_listener_controller_drain");
    PeakControllerStopFn controller_stop =
        (PeakControllerStopFn)required_symbol("peak_general_listener_controller_stop");
    PeakHookStateFn hook_state =
        (PeakHookStateFn)required_symbol("peak_general_listener_hook_state");
    PeakSignalBackendSignumFn selected_signal =
        (PeakSignalBackendSignumFn)required_symbol(
            "peak_detach_controller_test_signal_backend_signum");
    PeakSignalUnexpectedDeliveryCountFn unexpected_count =
        (PeakSignalUnexpectedDeliveryCountFn)required_symbol(
            "peak_signal_policy_unexpected_delivery_count");
    PeakSignalBadCookieFn send_bad_cookie =
        (PeakSignalBadCookieFn)required_symbol(
            "peak_signal_policy_test_send_bad_cookie_to_current_thread");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");

    if (request_detach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        selected_signal == NULL ||
        unexpected_count == NULL ||
        send_bad_cookie == NULL) {
        return 2;
    }
    if (trace_path == NULL || trace_path[0] == '\0') {
        fprintf(stderr, "PEAK_DETACH_TRACE_PATH is required\n");
        return 2;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook 0 to start attached\n");
        return 2;
    }
    int reserved_signum = selected_signal();
    if (reserved_signum <= 0 || reserved_signum > SIGRTMAX) {
        fprintf(stderr, "signal backend did not reserve a real-time signal\n");
        return 2;
    }

    int unexpected_before = unexpected_count();
    if (send_bad_cookie() != 0) {
        perror("send bad-cookie reserved signal");
        return 2;
    }
    double deadline = monotonic_seconds() + 2.0;
    while (unexpected_count() == unexpected_before &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (unexpected_count() <= unexpected_before) {
        fprintf(stderr, "bad-cookie reserved signal was not recorded\n");
        return 2;
    }

    unlink(trace_path);
    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue bad-cookie detach request\n");
        return 2;
    }
    if (!controller_drain(2000)) {
        fprintf(stderr, "bad-cookie detach did not drain\n");
        return 2;
    }
    if (hook_state(0) == PEAK_HOOK_DETACHED) {
        fprintf(stderr, "bad-cookie contamination physically detached\n");
        return 2;
    }
    if (!trace_has_detach_prepare_unexpected_signal(trace_path)) {
        fprintf(stderr, "missing bad-cookie contamination fail-closed trace\n");
        return 2;
    }

    printf("signal_bad_cookie_ok reserved_signal=%d contamination_seen=1 detach_blocked=1\n",
           reserved_signum);
    return 0;
}

static int
run_pthread_gate_race_check(int argc, char** argv)
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
    PeakControllerGateEpochFn gate_epoch =
        (PeakControllerGateEpochFn)required_symbol(
            "peak_detach_controller_test_thread_creation_gate_epoch");
    PeakControllerGateWaiterCountFn gate_waiter_count =
        (PeakControllerGateWaiterCountFn)required_symbol(
            "peak_detach_controller_test_gate_waiter_count");
    GumInterceptor** interceptor_slot =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners_slot =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** hook_addresses_slot = (gpointer**)required_symbol("hook_address");
    size_t* hook_count = (size_t*)required_symbol("peak_hook_address_count");
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    long threads = parse_long_arg(argc, argv, "--threads", 4);
    long creator_threads = parse_long_arg(argc, argv, "--creator-threads", 2);
    pthread_t* tids = NULL;
    WorkerState* states = NULL;
    pthread_t* creator_tids = NULL;
    GateRaceCreatorState* creator_states = NULL;
    StartGate worker_gate;
    StartGate creator_ready_gate;
    ReleaseGate creator_release_gate;
    int worker_gate_initialized = 0;
    int creator_ready_gate_initialized = 0;
    int creator_release_gate_initialized = 0;
    atomic_int child_started;
    long created_threads = 0;
    long created_creators = 0;
    pthread_t drain_thread;
    ControllerDrainState drain_state;
    size_t gate_waiters = 0;
    int child_started_before_release = -1;
    atomic_int child_started_while_gate;
    atomic_int create_attempted_during_gate;
    int saw_gate = 0;
    int ok = 0;

    if (request_detach == NULL ||
        request_reattach == NULL ||
        controller_drain == NULL ||
        controller_stop == NULL ||
        hook_state == NULL ||
        gate_epoch == NULL ||
        gate_waiter_count == NULL ||
        interceptor_slot == NULL ||
        listeners_slot == NULL ||
        hook_addresses_slot == NULL ||
        hook_count == NULL) {
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
    unlink(trace_path);

    tids = calloc((size_t)threads, sizeof(*tids));
    states = calloc((size_t)threads, sizeof(*states));
    creator_tids = calloc((size_t)creator_threads, sizeof(*creator_tids));
    creator_states =
        calloc((size_t)creator_threads, sizeof(*creator_states));
    if (tids == NULL ||
        states == NULL ||
        creator_tids == NULL ||
        creator_states == NULL) {
        perror("calloc");
        goto cleanup;
    }
    if (start_gate_init(&worker_gate, threads + 1) != 0) {
        perror("gate init");
        goto cleanup;
    }
    worker_gate_initialized = 1;
    if (start_gate_init(&creator_ready_gate, creator_threads + 1) != 0) {
        perror("gate init");
        goto cleanup;
    }
    creator_ready_gate_initialized = 1;
    if (release_gate_init(&creator_release_gate) != 0) {
        perror("gate init");
        goto cleanup;
    }
    creator_release_gate_initialized = 1;

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&unblock_backend_signal_requested,
                          0,
                          memory_order_release);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&spawned_worker_count, 0, memory_order_relaxed);
    atomic_store_explicit(&blocked_signal_thread_count, 0, memory_order_relaxed);
    atomic_init(&child_started, 0);
    atomic_init(&child_started_while_gate, 0);
    atomic_init(&create_attempted_during_gate, 0);

    for (long i = 0; i < threads; i++) {
        states[i].seed = (unsigned int)(0x9e3779b9u + (unsigned int)i);
        states[i].start_gate = &worker_gate;
        states[i].paired_targets = 0;
        states[i].block_backend_signal = 1;
        states[i].wait_to_unblock_backend_signal = 1;
        if (pthread_create(&tids[i], NULL, worker_main, &states[i]) != 0) {
            perror("pthread_create worker");
            goto cleanup;
        }
        created_threads++;
    }
    start_gate_wait(&worker_gate);

    for (long i = 0; i < creator_threads; i++) {
        creator_states[i].ready_gate = &creator_ready_gate;
        creator_states[i].release_gate = &creator_release_gate;
        creator_states[i].child_started = &child_started;
        creator_states[i].child_started_while_gate = &child_started_while_gate;
        creator_states[i].create_attempted_during_gate =
            &create_attempted_during_gate;
        creator_states[i].gate_epoch = gate_epoch;
        creator_states[i].seed = 0x45d9f3bu + (unsigned int)i;
        if (pthread_create(&creator_tids[i],
                           NULL,
                           gate_race_creator_main,
                           &creator_states[i]) != 0) {
            perror("pthread_create creator");
            goto cleanup;
        }
        created_creators++;
    }
    start_gate_wait(&creator_ready_gate);

    controller_stop();
    if (!request_detach(0)) {
        fprintf(stderr, "failed to queue gate-race detach request\n");
        goto cleanup;
    }

    drain_state.drain = controller_drain;
    drain_state.timeout_ms = 12000;
    drain_state.drained = FALSE;
    if (pthread_create(&drain_thread, NULL, controller_drain_main, &drain_state) != 0) {
        perror("pthread_create drain");
        goto cleanup;
    }

    double gate_deadline = monotonic_seconds() + 2.0;
    while (monotonic_seconds() < gate_deadline) {
        if (gate_epoch() != 0) {
            saw_gate = 1;
            break;
        }
        usleep(1000);
    }
    if (!saw_gate) {
        fprintf(stderr, "thread creation gate did not open during detach\n");
        atomic_store_explicit(&unblock_backend_signal_requested,
                              1,
                              memory_order_release);
        pthread_join(drain_thread, NULL);
        goto cleanup;
    }

    release_gate_open(&creator_release_gate);

    double waiter_deadline = monotonic_seconds() + 2.0;
    while (monotonic_seconds() < waiter_deadline) {
        gate_waiters = gate_waiter_count();
        if (gate_waiters >= (size_t)creator_threads) {
            break;
        }
        usleep(1000);
    }
    child_started_before_release =
        atomic_load_explicit(&child_started, memory_order_acquire);
    atomic_store_explicit(&unblock_backend_signal_requested,
                          1,
                          memory_order_release);
    pthread_join(drain_thread, NULL);

    if (!drain_state.drained) {
        fprintf(stderr, "controller did not drain gate-race detach\n");
        goto cleanup;
    }
    if (hook_state(0) != PEAK_HOOK_DETACHED) {
        fprintf(stderr, "expected hook detached after gate-race drain\n");
        goto cleanup;
    }
    if (!trace_has_entry_byte_only_success_without_classify_failure(
            trace_path,
            "peak_detach_hot_target",
            "detach")) {
        goto cleanup;
    }
    if (gate_waiters < (size_t)creator_threads) {
        fprintf(stderr,
                "not all newborn threads waited on mutation gate: %zu of %ld\n",
                gate_waiters,
                creator_threads);
        goto cleanup;
    }
    if (atomic_load_explicit(&create_attempted_during_gate,
                             memory_order_acquire) != (int)creator_threads) {
        fprintf(stderr,
                "not all creators attempted pthread_create during mutation gate: %d of %ld\n",
                atomic_load_explicit(&create_attempted_during_gate,
                                     memory_order_relaxed),
                creator_threads);
        goto cleanup;
    }
    if (child_started_before_release != 0) {
        fprintf(stderr,
                "child started before mutation gate release: %d\n",
                child_started_before_release);
        goto cleanup;
    }

    for (long i = 0; i < created_creators; i++) {
        pthread_join(creator_tids[i], NULL);
        if (creator_states[i].create_status != 0 ||
            creator_states[i].join_status != 0) {
            fprintf(stderr,
                    "creator %ld failed: create=%d join=%d\n",
                    i,
                    creator_states[i].create_status,
                    creator_states[i].join_status);
            goto cleanup;
        }
    }
    created_creators = 0;
    if (atomic_load_explicit(&child_started_while_gate,
                             memory_order_acquire) != 0) {
        fprintf(stderr,
                "child user code started while mutation gate was active: %d\n",
                atomic_load_explicit(&child_started_while_gate,
                                     memory_order_relaxed));
        goto cleanup;
    }

    if (!request_reattach(0)) {
        fprintf(stderr, "failed to queue gate-race reattach request\n");
        goto cleanup;
    }
    if (!controller_drain(12000)) {
        fprintf(stderr, "controller did not drain gate-race reattach\n");
        goto cleanup;
    }
    if (hook_state(0) != PEAK_HOOK_ATTACHED) {
        fprintf(stderr, "expected hook attached after gate-race reattach\n");
        goto cleanup;
    }
    if (!trace_has_entry_byte_only_success_without_classify_failure(
            trace_path,
            "peak_detach_hot_target",
            "reattach")) {
        goto cleanup;
    }

    ok = 1;

cleanup:
    atomic_store_explicit(&unblock_backend_signal_requested,
                          1,
                          memory_order_release);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
    if (worker_gate_initialized) {
        start_gate_abort(&worker_gate);
    }
    if (creator_ready_gate_initialized) {
        start_gate_abort(&creator_ready_gate);
    }
    if (created_creators > 0) {
        release_gate_open(&creator_release_gate);
    }
    for (long i = 0; i < created_creators; i++) {
        pthread_join(creator_tids[i], NULL);
    }
    for (long i = 0; i < created_threads; i++) {
        pthread_join(tids[i], NULL);
    }
    if (worker_gate_initialized) {
        start_gate_destroy(&worker_gate);
    }
    if (creator_ready_gate_initialized) {
        start_gate_destroy(&creator_ready_gate);
    }
    if (creator_release_gate_initialized) {
        release_gate_destroy(&creator_release_gate);
    }
    free(tids);
    free(states);
    free(creator_tids);
    free(creator_states);

    if (!ok) {
        return 2;
    }

    printf("pthread_gate_race_ok gate_waiters=%zu gate_waiters_ok=1 create_attempted_during_gate=%d child_started_before_release=%d child_started_while_gate=%d blocked_signal_threads=%lu detached=1 reattached=1\n",
           gate_waiters,
           atomic_load_explicit(&create_attempted_during_gate,
                                memory_order_relaxed),
           child_started_before_release,
           atomic_load_explicit(&child_started_while_gate,
                                memory_order_relaxed),
           (unsigned long)atomic_load_explicit(&blocked_signal_thread_count,
                                               memory_order_relaxed));
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

static int
run_pthread_gate_race_check(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "pthread gate race check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
}

static int
run_signal_blocked_delivery_check(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "signal blocked delivery check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
}

static int
run_signal_user_collision_check(void)
{
    fprintf(stderr, "signal user collision check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
    return 77;
}

static int
run_signal_bad_cookie_check(void)
{
    fprintf(stderr, "signal bad-cookie check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
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
    if (has_flag_arg(argc, argv, "--signal-blocked-delivery-check")) {
        return run_signal_blocked_delivery_check(argc, argv);
    }
    if (has_flag_arg(argc, argv, "--signal-user-collision-check")) {
        return run_signal_user_collision_check();
    }
    if (has_flag_arg(argc, argv, "--signal-forced-collision-check")) {
        return run_signal_forced_collision_check();
    }
    if (has_flag_arg(argc, argv, "--signal-bad-cookie-check")) {
        return run_signal_bad_cookie_check();
    }
    if (has_flag_arg(argc, argv, "--pthread-gate-race-check")) {
        return run_pthread_gate_race_check(argc, argv);
    }

    long threads = parse_long_arg(argc, argv, "--threads", 4);
    long seconds = parse_long_arg(argc, argv, "--seconds", 3);
    long spawner_threads = parse_long_arg(argc, argv, "--spawner-threads", 2);
    int paired_targets = has_flag_arg(argc, argv, "--paired-targets");
    int spawn_transient_threads = has_flag_arg(argc, argv, "--spawn-transient-threads");
    int block_backend_signal =
        has_flag_arg(argc, argv, "--block-backend-signal");
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
    atomic_store_explicit(&unblock_backend_signal_requested,
                          0,
                          memory_order_release);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&spawned_worker_count, 0, memory_order_relaxed);
    atomic_store_explicit(&blocked_signal_thread_count, 0, memory_order_relaxed);
    for (long i = 0; i < threads; i++) {
        states[i].seed = (unsigned int)(0x9e3779b9u + (unsigned int)i);
        states[i].start_gate = &gate;
        states[i].paired_targets = paired_targets;
        states[i].block_backend_signal = block_backend_signal;
        states[i].wait_to_unblock_backend_signal = 0;
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

    printf("threads=%ld seconds=%ld calls=%lu elapsed=%.6f calls_per_sec=%.3f side_effect=%lu spawned_threads=%lu blocked_signal_threads=%lu trace_detach_success=%d\n",
           threads,
           seconds,
           (unsigned long)calls,
           elapsed,
           (double)calls / elapsed,
           (unsigned long)atomic_load_explicit(&side_effect, memory_order_relaxed),
           (unsigned long)atomic_load_explicit(&spawned_worker_count, memory_order_relaxed),
           (unsigned long)atomic_load_explicit(&blocked_signal_thread_count,
                                               memory_order_relaxed),
           trace_detach_success);

    start_gate_destroy(&gate);
    free(tids);
    free(states);
    free(spawner_tids);
    free(spawner_states);
    return 0;
}
