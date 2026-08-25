#include "internal/cuda_profiler_state.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

template <typename Operation>
static double
measure_ns(std::size_t iterations, Operation operation)
{
    const Clock::time_point start = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        operation(iteration);
    }
    const Clock::time_point end = Clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() /
           static_cast<double>(iterations);
}

int
main()
{
    constexpr std::size_t kCapacity = 256;
    constexpr std::size_t kIterations = 256;
    const std::vector<std::string> targets = {"wanted"};
    PeakCudaKernelIdentity result;

    PeakCudaProfilerState hit_state(kCapacity);
    hit_state.reset(kCapacity, kCapacity, false, targets);
    (void)hit_state.identify(1, false, "unwanted", "unwanted");
    const double cache_hit_ns = measure_ns(kIterations, [&](std::size_t) {
        if (!hit_state.cached_identity(1, false, &result)) {
            std::abort();
        }
    });

    PeakCudaProfilerState miss_state(kCapacity);
    miss_state.reset(kCapacity, kCapacity, false, targets);
    const double first_miss_ns = measure_ns(kIterations, [&](std::size_t i) {
        (void)miss_state.identify(0x1000 + i, false,
                                  "unwanted", "unwanted");
    });

    PeakCudaProfilerState overflow_state(kCapacity);
    overflow_state.reset(kCapacity, 1, false, targets);
    (void)overflow_state.identify(1, false, "unwanted", "unwanted");
    const double negative_overflow_ns = measure_ns(
        kIterations, [&](std::size_t i) {
            (void)overflow_state.identify(0x2000 + i, false,
                                          "unwanted", "unwanted");
        });

    PeakCudaProfilerState positive_state(kCapacity);
    positive_state.reset(kCapacity, kCapacity, false, targets);
    for (std::size_t i = 0; i < kCapacity; ++i) {
        (void)positive_state.identify(0x3000 + i, false,
                                      "unwanted", "unwanted");
    }
    const double positive_after_saturation_ns = measure_ns(
        kIterations, [&](std::size_t i) {
            PeakCudaKernelIdentity positive = positive_state.identify(
                0x4000 + i, false, "wanted", "wanted");
            if (!positive.target_match) {
                std::abort();
            }
        });

    const PeakCudaProfilerCounters overflow = overflow_state.counters();
    const PeakCudaProfilerCounters positive = positive_state.counters();
    if (cache_hit_ns <= 0.0 || first_miss_ns <= 0.0 ||
        negative_overflow_ns <= 0.0 ||
        positive_after_saturation_ns <= 0.0 ||
        overflow.cached_identities > overflow.identity_capacity ||
        overflow.cached_overflow_identities > 1 ||
        positive.cached_identities > positive.identity_capacity ||
        positive.cached_overflow_identities > kCapacity) {
        return 1;
    }

    std::printf(
        "cuda_identity_cache_benchmark_ok cache_hit_ns=%.3f"
        " first_miss_ns=%.3f negative_overflow_ns=%.3f"
        " positive_after_saturation_ns=%.3f\n",
        cache_hit_ns, first_miss_ns, negative_overflow_ns,
        positive_after_saturation_ns);
    return 0;
}
