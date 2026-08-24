#include "cuda_test_common.cuh"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr int kPoolCapacity = 4;
constexpr int kSustainedLaunches = kPoolCapacity * 12;

__global__ void
peak_cuda_recycling_sustained_kernel(unsigned int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1U);
    }
}

__global__ void
peak_cuda_recycling_late_marker_kernel(unsigned int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1000U);
    }
}
}

int
main()
{
    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }

    cudaStream_t stream = nullptr;
    unsigned int* device_value = nullptr;
    unsigned int value = 0;
    if (!peak_cuda_test_check(cudaSetDevice(0), "cudaSetDevice") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &stream, cudaStreamNonBlocking),
                              "cudaStreamCreateWithFlags") ||
        !peak_cuda_test_check(cudaMalloc(&device_value, sizeof(*device_value)),
                              "cudaMalloc") ||
        !peak_cuda_test_check(cudaMemsetAsync(device_value, 0,
                                              sizeof(*device_value), stream),
                              "cudaMemsetAsync")) {
        return 1;
    }

    for (int launch = 0; launch < kSustainedLaunches; ++launch) {
        peak_cuda_recycling_sustained_kernel<<<1, 1, 0, stream>>>(device_value);
        if (!peak_cuda_test_check(cudaPeekAtLastError(),
                                  "sustained kernel launch")) {
            return 1;
        }
        if ((launch + 1) % 2 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    peak_cuda_recycling_late_marker_kernel<<<1, 1, 0, stream>>>(device_value);
    if (!peak_cuda_test_check(cudaPeekAtLastError(), "late marker launch") ||
        !peak_cuda_test_check(cudaStreamSynchronize(stream),
                              "final cudaStreamSynchronize") ||
        !peak_cuda_test_check(cudaMemcpy(&value, device_value, sizeof(value),
                                         cudaMemcpyDeviceToHost),
                              "cudaMemcpy")) {
        return 1;
    }

    cudaFree(device_value);
    cudaStreamDestroy(stream);
    if (value != static_cast<unsigned int>(kSustainedLaunches + 1000)) {
        std::fprintf(stderr,
                     "cuda_test_error: unexpected recycling result: %u\n",
                     value);
        return 1;
    }

    std::printf("cuda_event_recycling_ok capacity=%d launches=%d "
                "late_marker=1 per_launch_synchronizations=0 result=%u\n",
                kPoolCapacity, kSustainedLaunches + 1, value);
    return 0;
}
