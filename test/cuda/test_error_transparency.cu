#include "cuda_test_common.cuh"

#include <cuda_runtime.h>
#if defined(PEAK_TEST_CUDA_DRIVER)
#include <cuda.h>
#endif

#include <cstdio>
#include <cstring>

__global__ void
peak_cuda_error_transparency_kernel(unsigned int* value)
{
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        *value += 1;
    }
}

__global__ void
peak_cuda_async_error_kernel()
{
    *static_cast<volatile unsigned int*>(nullptr) = 1;
}

static void
clear_runtime_error()
{
    (void)cudaGetLastError();
}

static void
print_result(const char* label, cudaError_t result)
{
    const cudaError_t peek = cudaPeekAtLastError();
    const cudaError_t get = cudaGetLastError();
    const cudaError_t after_get = cudaPeekAtLastError();
    std::printf("case=%s result=%d peek=%d get=%d after_get=%d\n", label,
                static_cast<int>(result), static_cast<int>(peek),
                static_cast<int>(get), static_cast<int>(after_get));
}

#if defined(PEAK_TEST_CUDA_DRIVER)
static void
print_driver_result(const char* label, CUresult result)
{
    const cudaError_t peek = cudaPeekAtLastError();
    const cudaError_t get = cudaGetLastError();
    const cudaError_t after_get = cudaPeekAtLastError();
    std::printf("case=%s result=%d peek=%d get=%d after_get=%d\n", label,
                static_cast<int>(result), static_cast<int>(peek),
                static_cast<int>(get), static_cast<int>(after_get));
}
#endif

int
main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    if (std::strcmp(argv[1], "list") == 0) {
        std::printf("null_function zero_grid preserved_prior async_error");
#if defined(CUDART_VERSION) && CUDART_VERSION < 13000
        std::printf(" runtime_multi_null");
#endif
#if defined(CUDART_VERSION) && CUDART_VERSION >= 11080
        std::printf(" null_ex_config null_ex_function");
#endif
#if defined(PEAK_TEST_CUDA_DRIVER)
        std::printf(" driver_multi_null");
#if defined(CUDA_VERSION) && CUDA_VERSION >= 11080
        std::printf(" driver_null_ex_config driver_null_ex_function");
#endif
#endif
        std::puts("");
        return 0;
    }
    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }

    cudaStream_t stream = nullptr;
    unsigned int* value = nullptr;
    if (!peak_cuda_test_check(cudaStreamCreate(&stream), "cudaStreamCreate") ||
        !peak_cuda_test_check(cudaMalloc(&value, sizeof(*value)), "cudaMalloc") ||
        !peak_cuda_test_check(cudaMemsetAsync(value, 0, sizeof(*value), stream),
                              "cudaMemsetAsync")) {
        return 1;
    }
    void* args[] = {&value};

    if (std::strcmp(argv[1], "null_function") == 0) {
        clear_runtime_error();
        print_result("null_function",
                     cudaLaunchKernel(nullptr, dim3(1), dim3(1), args, 0,
                                      stream));
    } else if (std::strcmp(argv[1], "zero_grid") == 0) {
        clear_runtime_error();
        print_result("zero_grid",
                     cudaLaunchKernel(
                         reinterpret_cast<const void*>(
                             peak_cuda_error_transparency_kernel),
                         dim3(0), dim3(1), args, 0, stream));
#if defined(CUDART_VERSION) && CUDART_VERSION < 13000
    } else if (std::strcmp(argv[1], "runtime_multi_null") == 0) {
        clear_runtime_error();
        print_result("runtime_multi_null",
                     cudaLaunchCooperativeKernelMultiDevice(
                         nullptr, 0, 0));
#endif
    } else if (std::strcmp(argv[1], "preserved_prior") == 0) {
        clear_runtime_error();
        const cudaError_t sentinel = cudaLaunchKernel(
            reinterpret_cast<const void*>(
                peak_cuda_error_transparency_kernel),
            dim3(0), dim3(1), args, 0, stream);
        const cudaError_t before = cudaPeekAtLastError();
        const cudaError_t valid = cudaLaunchKernel(
            reinterpret_cast<const void*>(
                peak_cuda_error_transparency_kernel),
            dim3(1), dim3(1), args, 0, stream);
        const cudaError_t after = cudaPeekAtLastError();
        const cudaError_t get = cudaGetLastError();
        const cudaError_t after_get = cudaPeekAtLastError();
        std::printf(
            "case=preserved_prior sentinel=%d before=%d result=%d after=%d"
            " get=%d after_get=%d\n",
            static_cast<int>(sentinel), static_cast<int>(before),
            static_cast<int>(valid), static_cast<int>(after),
            static_cast<int>(get), static_cast<int>(after_get));
    } else if (std::strcmp(argv[1], "async_error") == 0) {
        clear_runtime_error();
        const cudaError_t launch = cudaLaunchKernel(
            reinterpret_cast<const void*>(peak_cuda_async_error_kernel),
            dim3(1), dim3(1), nullptr, 0, stream);
        const cudaError_t after_launch = cudaPeekAtLastError();
        const cudaError_t synchronize = cudaStreamSynchronize(stream);
        const cudaError_t after_synchronize = cudaPeekAtLastError();
        const cudaError_t get = cudaGetLastError();
        const cudaError_t after_get = cudaPeekAtLastError();
        std::printf(
            "case=async_error launch=%d after_launch=%d synchronize=%d"
            " after_synchronize=%d get=%d after_get=%d\n",
            static_cast<int>(launch), static_cast<int>(after_launch),
            static_cast<int>(synchronize),
            static_cast<int>(after_synchronize), static_cast<int>(get),
            static_cast<int>(after_get));
        std::puts("cuda_error_transparency_ok case=async_error");
        return 0;

#if defined(CUDART_VERSION) && CUDART_VERSION >= 11080
    } else if (std::strcmp(argv[1], "null_ex_config") == 0) {
        clear_runtime_error();
        print_result("null_ex_config",
                     cudaLaunchKernelExC(
                         nullptr,
                         reinterpret_cast<const void*>(
                             peak_cuda_error_transparency_kernel),
                         args));
    } else if (std::strcmp(argv[1], "null_ex_function") == 0) {
        cudaLaunchConfig_t config = {};
        config.gridDim = dim3(1);
        config.blockDim = dim3(1);
        config.dynamicSmemBytes = 0;
        config.stream = stream;
        config.attrs = nullptr;
        config.numAttrs = 0;
        clear_runtime_error();
        print_result("null_ex_function",
                     cudaLaunchKernelExC(&config, nullptr, args));
#endif
#if defined(PEAK_TEST_CUDA_DRIVER)
    } else if (std::strcmp(argv[1], "driver_multi_null") == 0) {
        clear_runtime_error();
        print_driver_result("driver_multi_null",
                            cuLaunchCooperativeKernelMultiDevice(
                                nullptr, 0, 0));
#if defined(CUDA_VERSION) && CUDA_VERSION >= 11080
    } else if (std::strcmp(argv[1], "driver_null_ex_config") == 0) {
        clear_runtime_error();
        print_driver_result("driver_null_ex_config",
                            cuLaunchKernelEx(nullptr, nullptr,
                                             nullptr, nullptr));
    } else if (std::strcmp(argv[1], "driver_null_ex_function") == 0) {
        CUlaunchConfig config = {};
        config.gridDimX = 1;
        config.gridDimY = 1;
        config.gridDimZ = 1;
        config.blockDimX = 1;
        config.blockDimY = 1;
        config.blockDimZ = 1;
        config.hStream = reinterpret_cast<CUstream>(stream);
        clear_runtime_error();
        print_driver_result("driver_null_ex_function",
                            cuLaunchKernelEx(&config, nullptr,
                                             nullptr, nullptr));
#endif
#endif
    } else {
        return 2;
    }

    if (std::strcmp(argv[1], "preserved_prior") == 0 &&
        !peak_cuda_test_check(cudaStreamSynchronize(stream),
                              "cudaStreamSynchronize")) {
        return 1;
    }
    if (std::strcmp(argv[1], "preserved_prior") == 0) {
        unsigned int host_value = 0;
        if (!peak_cuda_test_check(
                cudaMemcpy(&host_value, value, sizeof(host_value),
                           cudaMemcpyDeviceToHost),
                "cudaMemcpy") || host_value != 1) {
            return 1;
        }
    }
    if (!peak_cuda_test_check(cudaFree(value), "cudaFree") ||
        !peak_cuda_test_check(cudaStreamDestroy(stream),
                              "cudaStreamDestroy")) {
        return 1;
    }
    std::printf("cuda_error_transparency_ok case=%s\n", argv[1]);
    return 0;
}
