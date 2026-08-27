#include <dlfcn.h>
#include <stdio.h>

int
main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/libpeak\n", argv[0]);
        return 2;
    }
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
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
