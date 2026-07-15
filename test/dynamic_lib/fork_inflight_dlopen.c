#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

typedef void (*get_diagnostics_fn)(DlopenDiagnostics* diagnostics);
typedef void (*target_fn)(void);

static int worker_failed;

static void*
load_target(void* argument)
{
    void* handle;
    void* address;
    target_fn target;

    (void)argument;
    handle = dlopen("./libB.so", RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        worker_failed = 1;
        return NULL;
    }
    dlerror();
    address = dlsym(handle, "b_dynamic");
    if (dlerror() != NULL || address == NULL) {
        worker_failed = 1;
        return NULL;
    }
    if (sizeof(target) != sizeof(address)) {
        worker_failed = 1;
        return NULL;
    }
    memcpy(&target, &address, sizeof(target));
    target();
    return NULL;
}

static int
wait_for_child(pid_t child)
{
    const struct timespec poll_interval = { .tv_sec = 0, .tv_nsec = 1000000L };

    for (unsigned int attempt = 0; attempt < 3000; attempt++) {
        int status = 0;
        pid_t waited = waitpid(child, &status, WNOHANG);

        if (waited == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        }
        if (waited < 0) {
            return -1;
        }
        nanosleep(&poll_interval, NULL);
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return -1;
}

int
main(void)
{
    const struct timespec poll_interval = { .tv_sec = 0, .tv_nsec = 1000000L };
    get_diagnostics_fn get_diagnostics;
    pthread_t worker;
    void* diagnostics_address;
    int observed = 0;

    dlerror();
    diagnostics_address = dlsym(RTLD_DEFAULT,
                                 "dlopen_interceptor_get_dynamic_attach_diagnostics");
    if (dlerror() != NULL || diagnostics_address == NULL) {
        return 2;
    }
    if (sizeof(get_diagnostics) != sizeof(diagnostics_address)) {
        return 2;
    }
    memcpy(&get_diagnostics,
           &diagnostics_address,
           sizeof(get_diagnostics));

    if (pthread_create(&worker, NULL, load_target, NULL) != 0) {
        return 3;
    }
    for (unsigned int attempt = 0; attempt < 5000; attempt++) {
        DlopenDiagnostics diagnostics = {0};

        get_diagnostics(&diagnostics);
        if (diagnostics.enqueued > diagnostics.drained) {
            observed = 1;
            break;
        }
        nanosleep(&poll_interval, NULL);
    }
    if (!observed) {
        pthread_join(worker, NULL);
        return 4;
    }

    pid_t child = fork();
    if (child == 0) {
        void* handle = dlopen("./libA_staB.so", RTLD_LAZY | RTLD_LOCAL);
        _exit(handle != NULL ? 0 : 1);
    }
    if (child < 0 || wait_for_child(child) != 0) {
        pthread_join(worker, NULL);
        return 5;
    }
    if (pthread_join(worker, NULL) != 0 || worker_failed) {
        return 6;
    }

    puts("fork_inflight_dlopen_ok");
    return 0;
}
