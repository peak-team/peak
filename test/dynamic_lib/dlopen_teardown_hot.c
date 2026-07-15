#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INFLIGHT_WORKER_COUNT 8

typedef int (*PeakBoolFunction)(void);
typedef void (*PeakVoidFunction)(void);

typedef struct {
    const char* path;
    int status;
    _Atomic int finished;
} LoadWorker;

typedef struct {
    PeakBoolFunction dettach;
    int status;
} TeardownWorker;

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
    LoadWorker* worker = data;
    void* handle = dlopen(worker->path, RTLD_NOW | RTLD_LOCAL);

    if (handle == NULL) {
        worker->status = 1;
    } else if (dlclose(handle) != 0) {
        worker->status = 2;
    }
    atomic_store_explicit(&worker->finished, 1, memory_order_release);
    return NULL;
}

static void*
teardown_worker(void* data)
{
    TeardownWorker* worker = data;

    worker->status = worker->dettach() ? 0 : 1;
    return NULL;
}

static int
wait_until(PeakBoolFunction predicate, double timeout_seconds)
{
    double deadline = monotonic_seconds() + timeout_seconds;

    while (!predicate() && monotonic_seconds() < deadline) {
        usleep(1000);
    }
    return predicate() != 0;
}

int
main(void)
{
    PeakBoolFunction replacement_paused = NULL;
    PeakBoolFunction entry_physically_restored = NULL;
    PeakBoolFunction dettach = NULL;
    PeakVoidFunction release_replacement = NULL;
    LoadWorker workers[INFLIGHT_WORKER_COUNT] = {0};
    pthread_t worker_threads[INFLIGHT_WORKER_COUNT];
    LoadWorker late_worker = {
        .path = "./libB.so",
        .status = 0,
        .finished = 0,
    };
    pthread_t late_thread;
    TeardownWorker teardown = {0};
    pthread_t teardown_thread;
    size_t workers_started = 0;
    int teardown_started = 0;
    int late_started = 0;
    int restored_before_release = 0;
    int late_finished_before_release = 0;
    int result = EXIT_FAILURE;
    void* symbol;

#define LOAD_FUNCTION(variable, name)                                      \
    do {                                                                   \
        symbol = required_symbol(name);                                    \
        copy_function_pointer(&(variable), sizeof(variable), symbol);      \
    } while (0)

    LOAD_FUNCTION(replacement_paused,
                  "dlopen_interceptor_test_replacement_body_is_paused");
    LOAD_FUNCTION(release_replacement,
                  "dlopen_interceptor_test_release_replacement_body");
    LOAD_FUNCTION(entry_physically_restored,
                  "dlopen_interceptor_test_entry_physically_restored");
    LOAD_FUNCTION(dettach, "dlopen_interceptor_test_dettach");

#undef LOAD_FUNCTION

    for (size_t i = 0; i < INFLIGHT_WORKER_COUNT; i++) {
        workers[i].path = "./libB.so";
        if (pthread_create(&worker_threads[i], NULL, load_worker, &workers[i]) !=
            0) {
            fprintf(stderr, "failed to start inflight dlopen worker %zu\n", i);
            goto cleanup;
        }
        workers_started++;
    }

    if (!wait_until(replacement_paused, 5.0)) {
        fprintf(stderr, "no dlopen replacement body reached the test pause\n");
        goto cleanup;
    }

    teardown.dettach = dettach;
    if (pthread_create(&teardown_thread, NULL, teardown_worker, &teardown) != 0) {
        fprintf(stderr, "failed to start dlopen teardown worker\n");
        goto cleanup;
    }
    teardown_started = 1;

    restored_before_release = wait_until(entry_physically_restored, 5.0);
    if (!restored_before_release) {
        fprintf(stderr,
                "real dlopen entry was not physically restored before waiting for the paused replacement body\n");
        goto cleanup;
    }

    /*
     * Once the real entry is restored, a later caller must bypass the paused
     * replacement and its serialized load transaction.  It should therefore
     * complete before the deliberately paused body is released.
     */
    if (pthread_create(&late_thread, NULL, load_worker, &late_worker) != 0) {
        fprintf(stderr, "failed to start late dlopen worker\n");
        goto cleanup;
    }
    late_started = 1;
    {
        double deadline = monotonic_seconds() + 5.0;
        while (!atomic_load_explicit(&late_worker.finished,
                                     memory_order_acquire) &&
               monotonic_seconds() < deadline) {
            usleep(1000);
        }
    }
    late_finished_before_release = atomic_load_explicit(
        &late_worker.finished, memory_order_acquire);
    if (!late_finished_before_release) {
        fprintf(stderr,
                "late dlopen still entered the paused replacement after physical entry restore\n");
        goto cleanup;
    }

    result = EXIT_SUCCESS;

cleanup:
    release_replacement();

    if (late_started) {
        if (pthread_join(late_thread, NULL) != 0 || late_worker.status != 0) {
            result = EXIT_FAILURE;
        }
    }
    for (size_t i = 0; i < workers_started; i++) {
        if (pthread_join(worker_threads[i], NULL) != 0 ||
            workers[i].status != 0) {
            result = EXIT_FAILURE;
        }
    }
    if (teardown_started) {
        if (pthread_join(teardown_thread, NULL) != 0 || teardown.status != 0) {
            result = EXIT_FAILURE;
        }
    }

    if (result == EXIT_SUCCESS && restored_before_release &&
        late_finished_before_release) {
        puts("dlopen_teardown_entry_first_ok");
        fflush(stdout);
    }
    return result;
}
