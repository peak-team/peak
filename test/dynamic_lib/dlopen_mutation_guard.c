#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    TEST_HOOK_ATTACHED = 1,
    TEST_HOOK_DETACH_REQUESTED = 2,
    TEST_HOOK_DETACHED = 4
};

typedef int (*PeakRequestFunction)(size_t hook_id);
typedef int (*PeakStateFunction)(size_t hook_id);
typedef int (*PeakPausedFunction)(void);
typedef void (*PeakReleaseFunction)(void);
typedef unsigned long long (*PeakDeferralsFunction)(void);
typedef void (*DynamicTargetFunction)(void);

typedef struct {
    int status;
} WorkerResult;

static volatile unsigned long mutation_guard_sink;

__attribute__((noinline, visibility("default")))
void
peak_dlopen_mutation_guard_target(void)
{
    mutation_guard_sink++;
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
load_worker(void* data)
{
    WorkerResult* result = data;
    DynamicTargetFunction target = NULL;
    void* handle;
    void* symbol;

    handle = dlopen("./libB.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        result->status = 1;
        return NULL;
    }

    dlerror();
    symbol = dlsym(handle, "b_dynamic");
    if (symbol == NULL || dlerror() != NULL) {
        result->status = 2;
        dlclose(handle);
        return NULL;
    }
    copy_function_pointer(&target, sizeof(target), symbol);
    target();
    if (dlclose(handle) != 0) {
        result->status = 3;
    }
    return NULL;
}

int
main(void)
{
    PeakRequestFunction request_detach = NULL;
    PeakRequestFunction request_reattach = NULL;
    PeakStateFunction hook_state = NULL;
    PeakPausedFunction replacement_paused = NULL;
    PeakReleaseFunction release_replacement = NULL;
    PeakDeferralsFunction mutation_deferrals = NULL;
    WorkerResult worker_result = {0};
    pthread_t worker;
    unsigned long long deferrals_before_request;
    double deadline;
    void* symbol;

#define LOAD_FUNCTION(variable, name)                                      \
    do {                                                                   \
        symbol = required_symbol(name);                                    \
        copy_function_pointer(&(variable), sizeof(variable), symbol);      \
    } while (0)

    LOAD_FUNCTION(request_detach, "peak_general_listener_request_detach");
    LOAD_FUNCTION(request_reattach, "peak_general_listener_request_reattach");
    LOAD_FUNCTION(hook_state, "peak_general_listener_hook_state");
    LOAD_FUNCTION(replacement_paused,
                  "dlopen_interceptor_test_replacement_body_is_paused");
    LOAD_FUNCTION(release_replacement,
                  "dlopen_interceptor_test_release_replacement_body");
    LOAD_FUNCTION(mutation_deferrals,
                  "dlopen_interceptor_test_controller_mutation_deferrals");

#undef LOAD_FUNCTION

    if (hook_state(0) != TEST_HOOK_ATTACHED) {
        fprintf(stderr, "startup target is not attached\n");
        return EXIT_FAILURE;
    }
    peak_dlopen_mutation_guard_target();

    if (pthread_create(&worker, NULL, load_worker, &worker_result) != 0) {
        return EXIT_FAILURE;
    }

    deadline = monotonic_seconds() + 5.0;
    while (!replacement_paused() && monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (!replacement_paused()) {
        fprintf(stderr, "dlopen replacement body did not pause\n");
        return EXIT_FAILURE;
    }

    deferrals_before_request = mutation_deferrals();
    if (!request_detach(0)) {
        fprintf(stderr, "detach request was rejected\n");
        return EXIT_FAILURE;
    }

    deadline = monotonic_seconds() + 5.0;
    while (mutation_deferrals() == deferrals_before_request &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (mutation_deferrals() == deferrals_before_request ||
        hook_state(0) != TEST_HOOK_DETACH_REQUESTED) {
        fprintf(stderr,
                "controller mutated while dlopen replacement body was active\n");
        return EXIT_FAILURE;
    }

    release_replacement();
    if (pthread_join(worker, NULL) != 0 || worker_result.status != 0) {
        fprintf(stderr, "dlopen worker failed with status %d\n", worker_result.status);
        return EXIT_FAILURE;
    }

    deadline = monotonic_seconds() + 5.0;
    while (hook_state(0) != TEST_HOOK_DETACHED &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (hook_state(0) != TEST_HOOK_DETACHED) {
        fprintf(stderr, "deferred detach did not complete\n");
        return EXIT_FAILURE;
    }

    deadline = monotonic_seconds() + 5.0;
    while (!request_reattach(0) && monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (hook_state(0) == TEST_HOOK_DETACHED &&
        monotonic_seconds() >= deadline) {
        fprintf(stderr, "reattach request was not accepted\n");
        return EXIT_FAILURE;
    }

    deadline = monotonic_seconds() + 5.0;
    while (hook_state(0) != TEST_HOOK_ATTACHED &&
           monotonic_seconds() < deadline) {
        usleep(1000);
    }
    if (hook_state(0) != TEST_HOOK_ATTACHED) {
        fprintf(stderr, "reattach did not complete\n");
        return EXIT_FAILURE;
    }

    peak_dlopen_mutation_guard_target();
    puts("dlopen_mutation_guard_ok");
    fflush(stdout);
    return EXIT_SUCCESS;
}
