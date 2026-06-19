#define _GNU_SOURCE
#include "frida-gum.h"
#include "general_listener.h"

#include <dlfcn.h>
#include <errno.h>
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

static double
monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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
typedef gboolean (*PeakRequestDetachFn)(size_t hook_id);
typedef gboolean (*PeakControllerDrainFn)(unsigned int timeout_ms);
typedef void (*PeakControllerStopFn)(void);
typedef PeakHookState (*PeakHookStateFn)(size_t hook_id);

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
find_retry_batch_id(const char* trace_path, char* batch_id, size_t batch_id_size)
{
    FILE* fp = fopen(trace_path, "r");
    char line[1024];

    if (fp == NULL) {
        perror("fopen trace");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* fields[12];
        int count = parse_csv_line(line, fields, 12);

        if (count >= 12 &&
            strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "prepare-failed") == 0 &&
            strcmp(fields[6], "classify-failed") == 0 &&
            atoi(fields[7]) > 0 &&
            atoi(fields[9]) >= 2 &&
            strcmp(fields[11], "0") != 0) {
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
        char* fields[12];
        int count = parse_csv_line(line, fields, 12);

        if (count < 12) {
            continue;
        }

        if (strcmp(fields[2], "peak_detach_hot_target_two") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            atoi(fields[9]) >= 2 &&
            strcmp(fields[11], retry_batch_id) == 0) {
            peer_succeeded_in_retry_batch = 1;
        }

        if (strcmp(fields[2], "peak_detach_hot_target") == 0 &&
            strcmp(fields[3], "detach") == 0 &&
            strcmp(fields[4], "success") == 0 &&
            strcmp(fields[5], "1") == 0 &&
            strcmp(fields[6], "safe") == 0 &&
            strcmp(fields[11], retry_batch_id) != 0 &&
            atof(fields[8]) > 0.0) {
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
#else
static int
run_controller_batch_retry_check(void)
{
    fprintf(stderr, "controller batch retry check requires PEAK_HAVE_GUM_PEAK_PC_API\n");
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

    long threads = parse_long_arg(argc, argv, "--threads", 4);
    long seconds = parse_long_arg(argc, argv, "--seconds", 3);
    int paired_targets = has_flag_arg(argc, argv, "--paired-targets");
    pthread_t* tids = calloc((size_t)threads, sizeof(*tids));
    WorkerState* states = calloc((size_t)threads, sizeof(*states));
    StartGate gate;
    long created_threads = 0;
    if (tids == NULL || states == NULL) {
        perror("calloc");
        return 2;
    }
    if (start_gate_init(&gate, threads + 1) != 0) {
        perror("start_gate_init");
        free(tids);
        free(states);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
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
            return 2;
        }
        created_threads++;
    }

    start_gate_wait(&gate);
    double start = monotonic_seconds();
    usleep((useconds_t)seconds * 1000000u);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);

    uint64_t calls = 0;
    for (long i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
        calls += states[i].calls;
    }
    double elapsed = monotonic_seconds() - start;

    printf("threads=%ld seconds=%ld calls=%lu elapsed=%.6f calls_per_sec=%.3f side_effect=%lu\n",
           threads,
           seconds,
           (unsigned long)calls,
           elapsed,
           (double)calls / elapsed,
           (unsigned long)atomic_load_explicit(&side_effect, memory_order_relaxed));

    start_gate_destroy(&gate);
    free(tids);
    free(states);
    return 0;
}
