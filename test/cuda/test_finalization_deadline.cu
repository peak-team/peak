#include "cuda_test_common.cuh"

#include <dlfcn.h>

#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
constexpr unsigned int kKernelDurationMs = 2500;
constexpr long long kMaximumReportDurationMs = 1000;

__global__ void
peak_cuda_ready_finalization_kernel()
{
}

__global__ void
peak_cuda_warmup_finalization_kernel()
{
}

__global__ void
peak_cuda_long_finalization_kernel(unsigned long long ticks,
                                   unsigned int* completed)
{
    unsigned long long start = clock64();
    while (clock64() - start < ticks) {
    }
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        *completed = 1;
    }
}

using FinalizeFunction = void (*)(void);
using ForceIncompleteFunction = void (*)(int);
using HarvesterQueryCountFunction = unsigned long long (*)(void);
using HarvesterReadyFunction = int (*)(void);
using ActiveSlotCountFunction = unsigned long long (*)(void);

struct FinalizationSeams {
    FinalizeFunction finalize = nullptr;
    ForceIncompleteFunction force_incomplete = nullptr;
    HarvesterQueryCountFunction query_count = nullptr;
    HarvesterReadyFunction ready = nullptr;
    ActiveSlotCountFunction active_slots = nullptr;
};

FinalizationSeams
load_test_seams()
{
    FinalizationSeams seams;
    seams.finalize = reinterpret_cast<FinalizeFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_finalize"));
    seams.force_incomplete = reinterpret_cast<ForceIncompleteFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_force_incomplete_events"));
    seams.query_count = reinterpret_cast<HarvesterQueryCountFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_harvester_query_count"));
    seams.ready = reinterpret_cast<HarvesterReadyFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_harvester_ready"));
    seams.active_slots = reinterpret_cast<ActiveSlotCountFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_active_slot_count"));
    return seams;
}

bool
wait_for_query_after(const FinalizationSeams& seams,
                     unsigned long long previous)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.query_count() > previous) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
wait_for_no_active_slots(const FinalizationSeams& seams)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.active_slots() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
initialize_harvester(const FinalizationSeams& seams,
                     cudaStream_t stream,
                     int* warmup_attempts)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (seams.ready() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        peak_cuda_warmup_finalization_kernel<<<1, 1, 0, stream>>>();
        if (!peak_cuda_test_check(cudaPeekAtLastError(),
                                  "harvester warmup launch") ||
            !peak_cuda_test_check(cudaStreamSynchronize(stream),
                                  "harvester warmup synchronize")) {
            return false;
        }
        ++*warmup_attempts;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (seams.ready() == 0) {
        return false;
    }

    unsigned long long previous = seams.query_count();
    peak_cuda_ready_finalization_kernel<<<1, 1, 0, stream>>>();
    return peak_cuda_test_check(cudaPeekAtLastError(),
                                "ready marker launch") &&
           peak_cuda_test_check(cudaStreamSynchronize(stream),
                                "ready marker synchronize") &&
           wait_for_query_after(seams, previous) &&
           wait_for_no_active_slots(seams);
}
}

int
main(int argc, char** argv)
{
    bool forced_incomplete = argc == 2 &&
        std::strcmp(argv[1], "--forced-incomplete") == 0;
    if (argc > 2 || (argc == 2 && !forced_incomplete)) {
        return 2;
    }
    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }

    int clock_rate_khz = 0;
    cudaStream_t stream = nullptr;
    unsigned int* device_completed = nullptr;
    unsigned int completed = 0;
    if (!peak_cuda_test_check(cudaSetDevice(0), "cudaSetDevice") ||
        !peak_cuda_test_check(cudaDeviceGetAttribute(
                                  &clock_rate_khz, cudaDevAttrClockRate, 0),
                              "cudaDeviceGetAttribute(clock rate)") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &stream, cudaStreamNonBlocking),
                              "cudaStreamCreateWithFlags") ||
        !peak_cuda_test_check(cudaMalloc(&device_completed,
                                         sizeof(*device_completed)),
                              "cudaMalloc") ||
        !peak_cuda_test_check(cudaMemsetAsync(device_completed, 0,
                                              sizeof(*device_completed), stream),
                              "cudaMemsetAsync")) {
        return 1;
    }
    if (clock_rate_khz <= 0) {
        std::fprintf(stderr,
                     "cuda_test_error: device clock rate is not positive\n");
        return 1;
    }

    FinalizationSeams seams = load_test_seams();
    int seam_count = (seams.finalize != nullptr ? 1 : 0) +
                     (seams.force_incomplete != nullptr ? 1 : 0) +
                     (seams.query_count != nullptr ? 1 : 0) +
                     (seams.ready != nullptr ? 1 : 0) +
                     (seams.active_slots != nullptr ? 1 : 0);
    const bool seams_available = seam_count == 5;
    if ((forced_incomplete && !seams_available) ||
        (seam_count != 0 && !seams_available)) {
        std::fprintf(stderr,
                     "cuda_test_error: incomplete PEAK CUDA finalization "
                     "seam set\n");
        return 1;
    }

    int initialization_warmups = 0;
    if (seams_available &&
        !initialize_harvester(seams, stream, &initialization_warmups)) {
        std::fprintf(stderr,
                     "cuda_test_error: CUDA harvester did not become ready\n");
        return 1;
    }

    if (forced_incomplete) {
        seams.force_incomplete(1);
        peak_cuda_ready_finalization_kernel<<<1, 1, 0, stream>>>();
        if (!peak_cuda_test_check(cudaStreamSynchronize(stream),
                                  "forced-incomplete cudaStreamSynchronize")) {
            return 1;
        }
        auto report_start = std::chrono::steady_clock::now();
        seams.finalize();
        auto report_end = std::chrono::steady_clock::now();
        long long report_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                report_end - report_start)
                .count();
        if (report_ms > kMaximumReportDurationMs) {
            std::fprintf(stderr,
                         "cuda_test_error: forced-incomplete report took "
                         "%lld ms, limit is %lld ms\n",
                         report_ms, kMaximumReportDurationMs);
            return 1;
        }
        cudaFree(device_completed);
        cudaStreamDestroy(stream);
        std::printf("cuda_forced_incomplete_finalization_ok "
                    "configured_ms=100 report_ms=%lld "
                    "initialization_warmups=%d\n",
                    report_ms, initialization_warmups);
        return 0;
    }

    const unsigned long long ticks =
        static_cast<unsigned long long>(clock_rate_khz) *
        static_cast<unsigned long long>(kKernelDurationMs);
    if (!seams_available) {
        peak_cuda_ready_finalization_kernel<<<1, 1, 0, stream>>>();
        if (!peak_cuda_test_check(cudaStreamSynchronize(stream),
                                  "ready kernel cudaStreamSynchronize")) {
            return 1;
        }
    }
    peak_cuda_long_finalization_kernel<<<1, 1, 0, stream>>>(ticks,
                                                            device_completed);
    if (!peak_cuda_test_check(cudaPeekAtLastError(),
                              "ready and long kernel launches")) {
        return 1;
    }

    long long report_ms = -1;
    if (seams_available) {
        auto report_start = std::chrono::steady_clock::now();
        seams.finalize();
        auto report_end = std::chrono::steady_clock::now();
        report_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        report_end - report_start)
                        .count();
        if (report_ms > kMaximumReportDurationMs) {
            std::fprintf(
                stderr,
                "cuda_test_error: CUDA report took %lld ms, limit is %lld ms\n",
                report_ms, kMaximumReportDurationMs);
            return 1;
        }
    }

    if (!peak_cuda_test_check(cudaStreamSynchronize(stream),
                              "application cudaStreamSynchronize") ||
        !peak_cuda_test_check(cudaMemcpy(&completed, device_completed,
                                         sizeof(completed),
                                         cudaMemcpyDeviceToHost),
                              "cudaMemcpy")) {
        return 1;
    }
    cudaFree(device_completed);
    cudaStreamDestroy(stream);
    if (completed != 1) {
        std::fprintf(stderr,
                     "cuda_test_error: long kernel did not complete after "
                     "application synchronization\n");
        return 1;
    }

    if (seams_available) {
        std::printf("cuda_finalization_deadline_ok configured_ms=100 "
                    "report_ms=%lld kernel_ms=%u completed=%u "
                    "initialization_warmups=%d\n",
                    report_ms, kKernelDurationMs, completed,
                    initialization_warmups);
    } else {
        std::printf("cuda_finalization_baseline_ok kernel_ms=%u "
                    "completed=%u\n",
                    kKernelDurationMs, completed);
    }
    return 0;
}
