#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned long long (*PeakCudaAttachCallCountFn)(void);
typedef uint32_t (*PeakCapabilityMaskFn)(void);
typedef int (*PeakRuntimeStateFn)(void);

__attribute__((noinline)) int
peak_optional_backend_cpu_target(void)
{
    volatile int value = 0;

    for (int iteration = 0; iteration < 32; iteration++) {
        value += iteration;
    }
    return value;
}

int
main(void)
{
    PeakCudaAttachCallCountFn attach_call_count =
        (PeakCudaAttachCallCountFn)dlsym(
            RTLD_DEFAULT, "peak_cuda_test_attach_call_count");
    PeakCapabilityMaskFn failed_mask =
        (PeakCapabilityMaskFn)dlsym(
            RTLD_DEFAULT, "peak_test_capability_failed_mask");
    PeakRuntimeStateFn runtime_active =
        (PeakRuntimeStateFn)dlsym(
            RTLD_DEFAULT, "peak_test_runtime_active");
    PeakRuntimeStateFn heartbeat_started =
        (PeakRuntimeStateFn)dlsym(
            RTLD_DEFAULT, "peak_test_heartbeat_started");

    if (peak_optional_backend_cpu_target() != 496) {
        return 3;
    }
    if (attach_call_count == NULL || failed_mask == NULL ||
        runtime_active == NULL || heartbeat_started == NULL) {
        fprintf(stderr, "PEAK optional-backend test hooks are unavailable\n");
        return 2;
    }
    unsigned long long calls = attach_call_count();
    printf("cuda_attach_calls=%llu failed_mask=0x%x runtime_active=%d "
           "heartbeat_started=%d\n",
           calls,
           failed_mask(),
           runtime_active(),
           heartbeat_started());
    return calls == 0 ? 0 : 1;
}
