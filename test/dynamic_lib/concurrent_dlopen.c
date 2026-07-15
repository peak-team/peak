#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define THREAD_COUNT 16

typedef void (*target_fn)(void);

static pthread_barrier_t start_barrier;
static _Atomic int worker_failed;

static void*
load_and_call(void* argument)
{
    (void)argument;

    int barrier_status = pthread_barrier_wait(&start_barrier);
    if (barrier_status != 0 &&
        barrier_status != PTHREAD_BARRIER_SERIAL_THREAD) {
        atomic_store(&worker_failed, 1);
        return NULL;
    }

    void* handle = dlopen("./libB.so", RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        atomic_store(&worker_failed, 1);
        return NULL;
    }

    dlerror();
    target_fn target = (target_fn)dlsym(handle, "b_dynamic");
    const char* error = dlerror();
    if (error != NULL || target == NULL) {
        atomic_store(&worker_failed, 1);
        dlclose(handle);
        return NULL;
    }

    target();
    if (dlclose(handle) != 0) {
        atomic_store(&worker_failed, 1);
    }
    return NULL;
}

int
main(void)
{
    pthread_t threads[THREAD_COUNT];

    if (pthread_barrier_init(&start_barrier, NULL, THREAD_COUNT) != 0) {
        return 1;
    }
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, load_and_call, NULL) != 0) {
            return 2;
        }
    }
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            return 3;
        }
    }
    pthread_barrier_destroy(&start_barrier);

    if (atomic_load(&worker_failed) != 0) {
        return 4;
    }
    puts("concurrent_dlopen_ok");
    fflush(stdout);
    return 0;
}
