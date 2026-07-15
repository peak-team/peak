#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define THREAD_COUNT 16

typedef void (*target_fn)(void);

static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static unsigned int ready_count;
static int start_released;
static _Atomic int worker_failed;

static int
wait_for_start_release(void)
{
    int status = pthread_mutex_lock(&start_mutex);
    if (status != 0) {
        return status;
    }

    ready_count++;
    if (ready_count == THREAD_COUNT) {
        pthread_cond_broadcast(&start_cond);
    }
    while (!start_released) {
        status = pthread_cond_wait(&start_cond, &start_mutex);
        if (status != 0) {
            pthread_mutex_unlock(&start_mutex);
            return status;
        }
    }
    return pthread_mutex_unlock(&start_mutex);
}

static void*
load_and_call(void* argument)
{
    (void)argument;

    if (wait_for_start_release() != 0) {
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

    for (size_t i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, load_and_call, NULL) != 0) {
            return 2;
        }
    }

    if (pthread_mutex_lock(&start_mutex) != 0) {
        return 1;
    }
    while (ready_count != THREAD_COUNT) {
        if (pthread_cond_wait(&start_cond, &start_mutex) != 0) {
            pthread_mutex_unlock(&start_mutex);
            return 1;
        }
    }
    start_released = 1;
    pthread_cond_broadcast(&start_cond);
    if (pthread_mutex_unlock(&start_mutex) != 0) {
        return 1;
    }

    for (size_t i = 0; i < THREAD_COUNT; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            return 3;
        }
    }
    pthread_cond_destroy(&start_cond);
    pthread_mutex_destroy(&start_mutex);

    if (atomic_load(&worker_failed) != 0) {
        return 4;
    }
    puts("concurrent_dlopen_ok");
    fflush(stdout);
    return 0;
}
