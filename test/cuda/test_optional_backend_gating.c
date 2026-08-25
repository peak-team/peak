#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

typedef unsigned long long (*PeakCudaAttachCallCountFn)(void);

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

    if (peak_optional_backend_cpu_target() != 496) {
        return 3;
    }
    if (attach_call_count == NULL) {
        fprintf(stderr, "CUDA attach counter is unavailable\n");
        return 2;
    }
    unsigned long long calls = attach_call_count();
    printf("cuda_attach_calls=%llu\n", calls);
    return calls == 0 ? 0 : 1;
}
