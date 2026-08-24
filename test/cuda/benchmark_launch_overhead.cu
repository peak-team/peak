#include "cuda_test_common.cuh"

#include <cuda.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

extern "C" __global__ void
peak_cuda_benchmark_target_kernel(unsigned int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1U);
    }
}

extern "C" __global__ void
peak_cuda_benchmark_non_target_kernel(unsigned int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1U);
    }
}

namespace {
using PeakCudaHarvesterReadyFn = int (*)(void);

struct WorkerResult {
    unsigned long long submission_ns = 0;
    int error = 0;
};

class SpinBarrier {
public:
    explicit SpinBarrier(int participants)
        : participants_(participants), arrived_(0), generation_(0)
    {
    }

    void wait()
    {
        int generation = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) ==
            participants_ - 1) {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

private:
    int participants_;
    std::atomic<int> arrived_;
    std::atomic<int> generation_;
};

bool
parse_positive(const char* text, int* result)
{
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 1000000) {
        return false;
    }
    *result = static_cast<int>(value);
    return true;
}

bool
allowed_cpus(std::vector<int>* cpus)
{
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return false;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            cpus->push_back(cpu);
        }
    }
    return !cpus->empty();
#else
    (void)cpus;
    return false;
#endif
}

bool
pin_current_worker(int cpu)
{
#if defined(__linux__)
    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(cpu, &requested);
    if (pthread_setaffinity_np(pthread_self(), sizeof(requested),
                               &requested) != 0) {
        return false;
    }
    cpu_set_t actual;
    CPU_ZERO(&actual);
    return pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) ==
               0 &&
           CPU_COUNT(&actual) == 1 && CPU_ISSET(cpu, &actual);
#else
    (void)cpu;
    return false;
#endif
}
}

int
main(int argc, char** argv)
{
    const char* mode = "target";
    const char* api = "runtime";
    int thread_count = 1;
    int iterations = 64;
    int batch_size = 4;
    bool native_events = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            mode = argv[++index];
        } else if (std::strcmp(argv[index], "--api") == 0 &&
                   index + 1 < argc) {
            api = argv[++index];
        } else if (std::strcmp(argv[index], "--threads") == 0 &&
                   index + 1 < argc) {
            if (!parse_positive(argv[++index], &thread_count)) {
                return 2;
            }
        } else if (std::strcmp(argv[index], "--iterations") == 0 &&
                   index + 1 < argc) {
            if (!parse_positive(argv[++index], &iterations)) {
                return 2;
            }
        } else if (std::strcmp(argv[index], "--batch") == 0 &&
                   index + 1 < argc) {
            if (!parse_positive(argv[++index], &batch_size)) {
                return 2;
            }
        } else if (std::strcmp(argv[index], "--native-events") == 0) {
            native_events = true;
        } else {
            return 2;
        }
    }
    if (std::strcmp(mode, "target") != 0 &&
        std::strcmp(mode, "non-target") != 0) {
        return 2;
    }
    if (std::strcmp(api, "runtime") != 0 &&
        std::strcmp(api, "driver") != 0) {
        return 2;
    }

    std::vector<int> worker_cpus;
    if (!allowed_cpus(&worker_cpus)) {
        std::fprintf(stderr,
                     "cuda_test_error: unable to read Linux CPU affinity\n");
        return 1;
    }
    if (worker_cpus.size() < static_cast<size_t>(thread_count)) {
        std::fprintf(stderr,
                     "cuda_test_error: benchmark needs %d distinct CPUs, "
                     "affinity allows %zu\n",
                     thread_count, worker_cpus.size());
        return 1;
    }

    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }
    if (!peak_cuda_test_check(cudaSetDevice(0), "cudaSetDevice")) {
        return 1;
    }
    cudaFunction_t runtime_function = nullptr;
    const void* kernel_symbol = std::strcmp(mode, "target") == 0
        ? reinterpret_cast<const void*>(peak_cuda_benchmark_target_kernel)
        : reinterpret_cast<const void*>(
              peak_cuda_benchmark_non_target_kernel);
    if (std::strcmp(api, "driver") == 0 &&
        !peak_cuda_test_check(
            cudaGetFuncBySymbol(&runtime_function, kernel_symbol),
            "benchmark cudaGetFuncBySymbol")) {
        return 1;
    }
    CUfunction driver_function =
        reinterpret_cast<CUfunction>(runtime_function);
    PeakCudaHarvesterReadyFn harvester_ready =
        reinterpret_cast<PeakCudaHarvesterReadyFn>(
            dlsym(RTLD_DEFAULT, "peak_cuda_test_harvester_ready"));
    int harvester_ready_before_measurement = -1;

    std::atomic<int> ready{0};
    std::atomic<bool> start_warmup{false};
    std::atomic<int> warmup_done{0};
    std::atomic<bool> start_measurement{false};
    std::atomic<bool> abort{false};
    std::atomic<int> pinned_workers{0};
    std::atomic<bool> overlap_failure{false};
    SpinBarrier submission_barrier(thread_count);
    std::vector<unsigned long long> batch_start_ns(
        static_cast<size_t>(thread_count));
    std::vector<unsigned long long> batch_end_ns(
        static_cast<size_t>(thread_count));
    unsigned long long aggregate_submission_ns = 0;
    int overlapping_batches = 0;
    const int measured_batches =
        (iterations + batch_size - 1) / batch_size;
    std::vector<WorkerResult> results(static_cast<size_t>(thread_count));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(thread_count));

    for (int worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&, worker]() {
            if (!pin_current_worker(
                    worker_cpus[static_cast<size_t>(worker)])) {
                results[static_cast<size_t>(worker)].error = 1;
                abort.store(true, std::memory_order_release);
                ready.fetch_add(1, std::memory_order_release);
                return;
            }
            pinned_workers.fetch_add(1, std::memory_order_release);
            cudaStream_t stream = nullptr;
            cudaEvent_t start_event = nullptr;
            cudaEvent_t end_event = nullptr;
            unsigned int* device_value = nullptr;
            if (cudaSetDevice(0) != cudaSuccess ||
                cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) !=
                    cudaSuccess ||
                cudaMalloc(&device_value, sizeof(*device_value)) != cudaSuccess ||
                cudaMemsetAsync(device_value, 0, sizeof(*device_value), stream) !=
                    cudaSuccess) {
                results[static_cast<size_t>(worker)].error = 1;
                abort.store(true, std::memory_order_release);
                ready.fetch_add(1, std::memory_order_release);
                return;
            }
            if (native_events &&
                (cudaEventCreate(&start_event) != cudaSuccess ||
                 cudaEventCreate(&end_event) != cudaSuccess)) {
                results[static_cast<size_t>(worker)].error = 1;
                abort.store(true, std::memory_order_release);
                ready.fetch_add(1, std::memory_order_release);
                return;
            }

            CUdeviceptr driver_value =
                reinterpret_cast<CUdeviceptr>(device_value);
            auto launch_kernel = [&]() {
                if (native_events &&
                    cuEventRecord(reinterpret_cast<CUevent>(start_event),
                                  reinterpret_cast<CUstream>(stream)) !=
                        CUDA_SUCCESS) {
                    return false;
                }
                bool launched = true;
                if (std::strcmp(api, "driver") == 0) {
                    void* arguments[] = {&driver_value};
                    launched = cuLaunchKernel(
                               driver_function, 1, 1, 1, 1, 1, 1, 0,
                               reinterpret_cast<CUstream>(stream), arguments,
                               nullptr) == CUDA_SUCCESS;
                } else if (std::strcmp(mode, "target") == 0) {
                    peak_cuda_benchmark_target_kernel<<<1, 1, 0, stream>>>(
                        device_value);
                } else {
                    peak_cuda_benchmark_non_target_kernel<<<1, 1, 0, stream>>>(
                        device_value);
                }
                return launched &&
                    (!native_events ||
                     cuEventRecord(reinterpret_cast<CUevent>(end_event),
                                   reinterpret_cast<CUstream>(stream)) ==
                         CUDA_SUCCESS);
            };

            if (!launch_kernel() || cudaPeekAtLastError() != cudaSuccess ||
                cudaStreamSynchronize(stream) != cudaSuccess) {
                results[static_cast<size_t>(worker)].error = 1;
                ready.fetch_add(1, std::memory_order_release);
                return;
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!start_warmup.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (abort.load(std::memory_order_acquire)) {
                cudaFree(device_value);
                cudaStreamDestroy(stream);
                return;
            }

            for (int launch = 0; launch < batch_size; ++launch) {
                if (!launch_kernel()) {
                    results[static_cast<size_t>(worker)].error = 1;
                    abort.store(true, std::memory_order_release);
                    break;
                }
            }
            if (cudaPeekAtLastError() != cudaSuccess ||
                cudaStreamSynchronize(stream) != cudaSuccess) {
                results[static_cast<size_t>(worker)].error = 1;
                abort.store(true, std::memory_order_release);
            }
            warmup_done.fetch_add(1, std::memory_order_release);
            while (!start_measurement.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (abort.load(std::memory_order_acquire)) {
                cudaFree(device_value);
                cudaStreamDestroy(stream);
                return;
            }

            for (int begin = 0; begin < iterations; begin += batch_size) {
                int count = std::min(batch_size, iterations - begin);
                submission_barrier.wait();
                auto batch_start = std::chrono::steady_clock::now();
                for (int launch = 0; launch < count; ++launch) {
                    if (!launch_kernel()) {
                        results[static_cast<size_t>(worker)].error = 1;
                        abort.store(true, std::memory_order_release);
                        break;
                    }
                }
                auto batch_end = std::chrono::steady_clock::now();
                batch_start_ns[static_cast<size_t>(worker)] =
                    static_cast<unsigned long long>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            batch_start.time_since_epoch())
                            .count());
                batch_end_ns[static_cast<size_t>(worker)] =
                    static_cast<unsigned long long>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            batch_end.time_since_epoch())
                            .count());
                results[static_cast<size_t>(worker)].submission_ns +=
                    static_cast<unsigned long long>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            batch_end - batch_start)
                            .count());
                submission_barrier.wait();
                if (worker == 0) {
                    auto first = std::min_element(batch_start_ns.begin(),
                                                  batch_start_ns.end());
                    auto last = std::max_element(batch_end_ns.begin(),
                                                 batch_end_ns.end());
                    aggregate_submission_ns += *last - *first;
                    auto latest_start = std::max_element(
                        batch_start_ns.begin(), batch_start_ns.end());
                    auto earliest_end = std::min_element(
                        batch_end_ns.begin(), batch_end_ns.end());
                    if (*latest_start < *earliest_end) {
                        ++overlapping_batches;
                    } else {
                        overlap_failure.store(true,
                                              std::memory_order_release);
                        abort.store(true, std::memory_order_release);
                    }
                }
                submission_barrier.wait();
                if (cudaPeekAtLastError() != cudaSuccess ||
                    cudaStreamSynchronize(stream) != cudaSuccess) {
                    results[static_cast<size_t>(worker)].error = 1;
                    abort.store(true, std::memory_order_release);
                }
                submission_barrier.wait();
                if (abort.load(std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            cudaFree(device_value);
            if (start_event != nullptr) {
                cudaEventDestroy(start_event);
            }
            if (end_event != nullptr) {
                cudaEventDestroy(end_event);
            }
            cudaStreamDestroy(stream);
        });
    }

    auto wait_deadline = std::chrono::steady_clock::now() +
                         std::chrono::seconds(10);
    while (ready.load(std::memory_order_acquire) != thread_count &&
           std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (ready.load(std::memory_order_acquire) != thread_count) {
        std::fprintf(stderr, "cuda_test_error: benchmark worker setup timed out\n");
        start_warmup.store(true, std::memory_order_release);
        start_measurement.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        return 1;
    }
    if (abort.load(std::memory_order_acquire)) {
        start_warmup.store(true, std::memory_order_release);
        start_measurement.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        std::fprintf(stderr, "cuda_test_error: benchmark worker setup failed\n");
        return 1;
    }
    if (pinned_workers.load(std::memory_order_acquire) != thread_count) {
        start_warmup.store(true, std::memory_order_release);
        start_measurement.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        std::fprintf(stderr,
                     "cuda_test_error: not every benchmark worker was pinned\n");
        return 1;
    }
    start_warmup.store(true, std::memory_order_release);
    auto warmup_deadline = std::chrono::steady_clock::now() +
                           std::chrono::seconds(10);
    while (warmup_done.load(std::memory_order_acquire) != thread_count &&
           std::chrono::steady_clock::now() < warmup_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (warmup_done.load(std::memory_order_acquire) != thread_count) {
        std::fprintf(stderr, "cuda_test_error: benchmark warmup timed out\n");
        start_measurement.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        return 1;
    }
    if (abort.load(std::memory_order_acquire)) {
        start_measurement.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        std::fprintf(stderr, "cuda_test_error: benchmark warmup failed\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (harvester_ready != nullptr) {
        harvester_ready_before_measurement = harvester_ready();
        if (std::strcmp(mode, "target") == 0 &&
            harvester_ready_before_measurement == 0) {
            abort.store(true, std::memory_order_release);
            start_measurement.store(true, std::memory_order_release);
            for (std::thread& worker : workers) {
                worker.join();
            }
            std::fprintf(
                stderr,
                "cuda_test_error: CUDA harvester was not ready before "
                "the measured target phase\n");
            return 1;
        }
    }
    start_measurement.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }

    unsigned long long total_submission_ns = 0;
    for (const WorkerResult& result : results) {
        if (result.error != 0) {
            std::fprintf(stderr, "cuda_test_error: benchmark worker failed\n");
            return 1;
        }
        total_submission_ns += result.submission_ns;
    }
    if (overlap_failure.load(std::memory_order_acquire) ||
        overlapping_batches != measured_batches) {
        std::fprintf(stderr,
                     "cuda_test_error: only %d/%d measured batches had "
                     "overlapping worker submissions\n",
                     overlapping_batches, measured_batches);
        return 1;
    }
    const unsigned long long calls =
        static_cast<unsigned long long>(thread_count) *
        static_cast<unsigned long long>(iterations);
    const double host_ns_per_launch =
        static_cast<double>(total_submission_ns) / static_cast<double>(calls);
    if (aggregate_submission_ns == 0) {
        std::fprintf(stderr,
                     "cuda_test_error: benchmark aggregate duration is zero\n");
        return 1;
    }
    const double aggregate_launches_per_second =
        static_cast<double>(calls) * 1000000000.0 /
        static_cast<double>(aggregate_submission_ns);
    std::printf("cuda_launch_benchmark_ok api=%s mode=%s threads=%d calls=%llu "
                "warmup_calls=%llu host_ns_per_launch=%.3f "
                "aggregate_launches_per_second=%.3f "
                "per_launch_synchronizations=0 available_cpus=%zu "
                "pinned_workers=%d overlap_batches=%d/%d "
                "harvester_ready_before_measurement=%d native_events=%d\n",
                api, mode, thread_count, calls,
                static_cast<unsigned long long>(thread_count) *
                    static_cast<unsigned long long>(batch_size),
                host_ns_per_launch, aggregate_launches_per_second,
                worker_cpus.size(),
                pinned_workers.load(std::memory_order_acquire),
                overlapping_batches, measured_batches,
                harvester_ready_before_measurement,
                native_events ? 1 : 0);
    return 0;
}
