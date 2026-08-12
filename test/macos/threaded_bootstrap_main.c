#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

unsigned long peak_macos_smoke_target(unsigned long value);

static _Atomic int peak_macos_worker_ready = 0;
static _Atomic int peak_macos_worker_stop = 0;

static void*
peak_macos_bootstrap_worker(void* opaque)
{
    (void)opaque;
    atomic_store_explicit(&peak_macos_worker_ready, 1, memory_order_release);
    while (atomic_load_explicit(&peak_macos_worker_stop,
                                memory_order_acquire) == 0) {
        usleep(1000);
    }
    return NULL;
}

int
main(int argc, char** argv)
{
    pthread_t worker;
    void* peak_handle;
    unsigned long result;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/libpeak.dylib\n", argv[0]);
        return 2;
    }
    if (pthread_create(&worker, NULL, peak_macos_bootstrap_worker, NULL) != 0) {
        return 3;
    }
    while (atomic_load_explicit(&peak_macos_worker_ready,
                                memory_order_acquire) == 0) {
        sched_yield();
    }

    peak_handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (peak_handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        atomic_store_explicit(&peak_macos_worker_stop,
                              1,
                              memory_order_release);
        pthread_join(worker, NULL);
        return 4;
    }

    result = peak_macos_smoke_target(7);
    atomic_store_explicit(&peak_macos_worker_stop, 1, memory_order_release);
    if (pthread_join(worker, NULL) != 0) {
        return 5;
    }

    printf("macos_threaded_bootstrap_result=%lu\n", result);
    return result == 22 ? 0 : 1;
}
