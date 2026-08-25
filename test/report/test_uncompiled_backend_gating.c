#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t (*PeakCapabilityMaskFn)(void);
typedef int (*PeakRuntimeStateFn)(void);

int
main(void)
{
    PeakCapabilityMaskFn failed_mask =
        (PeakCapabilityMaskFn)dlsym(
            RTLD_DEFAULT, "peak_test_capability_failed_mask");
    PeakRuntimeStateFn runtime_active =
        (PeakRuntimeStateFn)dlsym(
            RTLD_DEFAULT, "peak_test_runtime_active");
    PeakRuntimeStateFn heartbeat_started =
        (PeakRuntimeStateFn)dlsym(
            RTLD_DEFAULT, "peak_test_heartbeat_started");

    if (failed_mask == NULL || runtime_active == NULL ||
        heartbeat_started == NULL) {
        fputs("PEAK backend-gating test hooks are unavailable\n", stderr);
        return 2;
    }
    printf("failed_mask=0x%x runtime_active=%d heartbeat_started=%d\n",
           failed_mask(), runtime_active(), heartbeat_started());
    return 0;
}
