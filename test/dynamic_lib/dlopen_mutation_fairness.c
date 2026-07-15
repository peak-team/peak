#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKER_COUNT 8
#define READY_ITERATIONS 10

enum {
    TEST_HOOK_ATTACHED = 1,
    TEST_HOOK_DETACHED = 4
};

typedef struct {
    unsigned long long enqueued;
    unsigned long long drained;
    unsigned long long requeued;
    unsigned long long dropped_full;
    unsigned long long dropped_closed;
    unsigned long long dropped_noload;
    unsigned long long dropped_requeue;
    unsigned long long partial_success;
    unsigned long long retained_handles;
    size_t max_depth;
    size_t queue_length;
    unsigned int capacity;
    unsigned int drain_budget;
} DlopenDiagnostics;

typedef int (*PeakRequestFunction)(size_t hook_id);
typedef int (*PeakStateFunction)(size_t hook_id);
typedef void (*PeakDiagnosticsFunction)(DlopenDiagnostics* diagnostics);

static _Atomic int keep_running = 1;
static _Atomic int worker_failed;
static _Atomic unsigned int ready_workers;
static _Atomic unsigned long long load_iterations;
static volatile unsigned long fairness_target_sink;

__attribute__((noinline, visibility("default")))
void
peak_dlopen_fairness_target(void)
{
    fairness_target_sink++;
}

static double
monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void*
required_symbol(const char* name)
{
    void* symbol;
    const char* error;

    dlerror();
    symbol = dlsym(RTLD_DEFAULT, name);
    error = dlerror();
    if (symbol == NULL || error != NULL) {
        fprintf(stderr,
                "missing required symbol %s: %s\n",
                name,
                error != NULL ? error : "not found");
        exit(EXIT_FAILURE);
    }
    return symbol;
}

static void
copy_function_pointer(void* destination, size_t destination_size, void* symbol)
{
    if (destination_size != sizeof(symbol)) {
        fprintf(stderr, "function pointer size mismatch\n");
        exit(EXIT_FAILURE);
    }
    memcpy(destination, &symbol, destination_size);
}

static void*
load_worker(void* argument)
{
    unsigned int local_iterations = 0;

    (void)argument;
    while (atomic_load_explicit(&keep_running, memory_order_acquire)) {
        void* handle = dlopen("./libB.so", RTLD_LAZY | RTLD_LOCAL);

        if (handle == NULL || dlclose(handle) != 0) {
            atomic_store_explicit(&worker_failed, 1, memory_order_release);
            return NULL;
        }
        local_iterations++;
        atomic_fetch_add_explicit(&load_iterations, 1, memory_order_relaxed);
        if (local_iterations == READY_ITERATIONS) {
            atomic_fetch_add_explicit(&ready_workers, 1, memory_order_release);
        }
    }
    return NULL;
}

int
main(void)
{
    PeakRequestFunction request_detach = NULL;
    PeakRequestFunction request_reattach = NULL;
    PeakStateFunction hook_state = NULL;
    PeakDiagnosticsFunction get_diagnostics = NULL;
    DlopenDiagnostics before = {0};
    DlopenDiagnostics after = {0};
    pthread_t workers[WORKER_COUNT];
    void* symbol;
    double deadline;

#define LOAD_FUNCTION(variable, name)                                      \
    do {                                                                   \
        symbol = required_symbol(name);                                    \
        copy_function_pointer(&(variable), sizeof(variable), symbol);      \
    } while (0)

    LOAD_FUNCTION(request_detach, "peak_general_listener_request_detach");
    LOAD_FUNCTION(request_reattach, "peak_general_listener_request_reattach");
    LOAD_FUNCTION(hook_state, "peak_general_listener_hook_state");
    LOAD_FUNCTION(get_diagnostics,
                  "dlopen_interceptor_get_dynamic_attach_diagnostics");

#undef LOAD_FUNCTION

    if (hook_state(0) != TEST_HOOK_ATTACHED) {
        fprintf(stderr, "startup target is not attached\n");
        return EXIT_FAILURE;
    }
    peak_dlopen_fairness_target();
    get_diagnostics(&before);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        if (pthread_create(&workers[i], NULL, load_worker, NULL) != 0) {
            atomic_store(&keep_running, 0);
            return EXIT_FAILURE;
        }
    }

    deadline = monotonic_seconds() + 5.0;
    while (atomic_load_explicit(&ready_workers, memory_order_acquire) !=
               WORKER_COUNT &&
           !atomic_load_explicit(&worker_failed, memory_order_acquire) &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (atomic_load(&ready_workers) != WORKER_COUNT ||
        atomic_load(&worker_failed)) {
        fprintf(stderr, "continuous dlopen workers did not become ready\n");
        atomic_store(&keep_running, 0);
        return EXIT_FAILURE;
    }

    if (!request_detach(0)) {
        fprintf(stderr, "detach request was rejected\n");
        atomic_store(&keep_running, 0);
        return EXIT_FAILURE;
    }
    deadline = monotonic_seconds() + 5.0;
    while (hook_state(0) != TEST_HOOK_DETACHED &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (hook_state(0) != TEST_HOOK_DETACHED) {
        fprintf(stderr, "controller was starved by continuous dlopen traffic\n");
        atomic_store(&keep_running, 0);
        return EXIT_FAILURE;
    }

    atomic_store_explicit(&keep_running, 0, memory_order_release);
    for (size_t i = 0; i < WORKER_COUNT; i++) {
        if (pthread_join(workers[i], NULL) != 0) {
            return EXIT_FAILURE;
        }
    }
    if (atomic_load(&worker_failed) ||
        atomic_load(&load_iterations) < WORKER_COUNT * READY_ITERATIONS) {
        fprintf(stderr, "continuous dlopen workload failed\n");
        return EXIT_FAILURE;
    }

    get_diagnostics(&after);
    if (after.enqueued != before.enqueued) {
        fprintf(stderr,
                "unrelated dlopen traffic entered the synchronous attach queue: before=%llu after=%llu\n",
                before.enqueued,
                after.enqueued);
        return EXIT_FAILURE;
    }

    deadline = monotonic_seconds() + 5.0;
    while (!request_reattach(0) && monotonic_seconds() < deadline) {
        usleep(1000);
    }
    while (hook_state(0) != TEST_HOOK_ATTACHED &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (hook_state(0) != TEST_HOOK_ATTACHED) {
        fprintf(stderr, "reattach did not complete\n");
        return EXIT_FAILURE;
    }

    peak_dlopen_fairness_target();
    puts("dlopen_mutation_fairness_ok");
    fflush(stdout);
    return EXIT_SUCCESS;
}
