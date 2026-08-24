#ifndef PEAK_CUDA_TEST_COMMON_CUH
#define PEAK_CUDA_TEST_COMMON_CUH

#include <cuda_runtime_api.h>

#include <cstdio>

constexpr int PEAK_CUDA_TEST_SKIP = 77;

inline int
peak_cuda_test_require_devices(int required, int* available)
{
    int count = 0;
    cudaError_t result = cudaGetDeviceCount(&count);

    if (result != cudaSuccess) {
        std::printf("cuda_test_skip: cudaGetDeviceCount failed: %s\n",
                    cudaGetErrorString(result));
        return PEAK_CUDA_TEST_SKIP;
    }
    if (available != nullptr) {
        *available = count;
    }
    if (count < required) {
        std::printf("cuda_test_skip: requires %d CUDA device(s), found %d\n",
                    required, count);
        return PEAK_CUDA_TEST_SKIP;
    }
    return 0;
}

inline bool
peak_cuda_test_check(cudaError_t result, const char* operation)
{
    if (result == cudaSuccess) {
        return true;
    }
    std::fprintf(stderr, "cuda_test_error: %s failed: %s\n",
                 operation, cudaGetErrorString(result));
    return false;
}

#endif /* PEAK_CUDA_TEST_COMMON_CUH */
