#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_int workers_ready;
static atomic_int workers_release;

static void*
wait_for_load(void* unused)
{
    (void)unused;
    atomic_fetch_add(&workers_ready, 1);
    while (!atomic_load(&workers_release)) {
        sched_yield();
    }
    return NULL;
}

static int
check_load(const char* library)
{
    void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "failed to load installed PEAK DSO: %s\n", dlerror());
        return 1;
    }
    const char* test_hooks[] = {
        "peak_test_cpu_target_storage_allocated",
        "peak_general_listener_test_call_count",
        "peak_detach_controller_test_gate_waiter_count",
        "dlopen_interceptor_test_drain_dynamic_attach_queue",
        "pthread_listener_test_current_thread_has_slot",
        "peak_memlog_test_open",
        "peak_cuda_test_attach_call_count",
        "peak_cuda_test_kernel_mapping_lifetime",
        "mpi_interceptor_test_original_finalize_call_count",
    };
    for (size_t index = 0;
         index < sizeof(test_hooks) / sizeof(test_hooks[0]);
         ++index) {
        if (dlsym(handle, test_hooks[index]) != NULL) {
            fprintf(stderr, "installed PEAK DSO exports test hook %s\n",
                    test_hooks[index]);
            dlclose(handle);
            return 1;
        }
    }
    if (dlclose(handle) != 0) {
        fprintf(stderr, "failed to close installed PEAK DSO: %s\n", dlerror());
        return 1;
    }
    puts("peak_package_consumer_ok");
    return 0;
}

int
main(int argc, char** argv)
{
    if (argc != 2 &&
        !(argc == 3 && strcmp(argv[2], "--existing-threads") == 0)) {
        fprintf(stderr, "usage: %s /path/to/libpeak [--existing-threads]\n", argv[0]);
        return 2;
    }
    pthread_t workers[2];
    unsigned count = 0;
    if (argc == 3) {
        for (; count < 2; ++count) {
            if (pthread_create(&workers[count], NULL, wait_for_load, NULL) != 0) {
                atomic_store(&workers_release, 1);
                for (unsigned index = 0; index < count; ++index)
                    pthread_join(workers[index], NULL);
                return 1;
            }
        }
        while (atomic_load(&workers_ready) != 2) {
            sched_yield();
        }
    }
    int result = check_load(argv[1]);
    atomic_store(&workers_release, 1);
    for (unsigned index = 0; index < count; ++index) {
        if (pthread_join(workers[index], NULL) != 0)
            result = 1;
    }
    return result;
}
