#include "internal/cuda_profiler_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using Clock = std::chrono::steady_clock;

static double
measure_drain_ns(std::size_t capacity)
{
    PeakCudaSlotAllocator allocator(capacity);
    PeakCudaPendingQueue pending(capacity);
    std::vector<PeakCudaSlotLease> leases(capacity);

    for (std::size_t index = 0; index < capacity; ++index) {
        if (!allocator.acquire(7, index, &leases[index]) ||
            !pending.push(leases[index])) {
            std::abort();
        }
    }

    const Clock::time_point start = Clock::now();
    for (std::size_t index = 0; index < capacity; ++index) {
        PeakCudaSlotLease lease = {};
        if (!pending.pop(&lease) || !allocator.release(lease)) {
            std::abort();
        }
    }
    const Clock::time_point end = Clock::now();
    if (allocator.active_count() != 0) {
        std::abort();
    }
    return std::chrono::duration<double, std::nano>(end - start).count();
}

static double
best_drain_ns(std::size_t capacity)
{
    double best = std::numeric_limits<double>::max();
    for (int sample = 0; sample < 3; ++sample) {
        best = std::min(best, measure_drain_ns(capacity));
    }
    return best;
}

int
main()
{
    constexpr std::array<std::size_t, 3> capacities = {
        8192, 32768, 65536};
    std::array<double, capacities.size()> elapsed = {};
    for (std::size_t index = 0; index < capacities.size(); ++index) {
        elapsed[index] = best_drain_ns(capacities[index]);
    }

    const double small_per_record = elapsed[0] / capacities[0];
    const double medium_per_record = elapsed[1] / capacities[1];
    const double large_per_record = elapsed[2] / capacities[2];
    const double minimum_per_record = std::min(
        small_per_record, std::min(medium_per_record, large_per_record));
    const double maximum_per_record = std::max(
        small_per_record, std::max(medium_per_record, large_per_record));

    if (minimum_per_record <= 0.0 ||
        maximum_per_record > minimum_per_record * 3.0 ||
        elapsed[1] > elapsed[0] * 8.0 ||
        elapsed[2] > elapsed[0] * 16.0) {
        std::fprintf(stderr,
            "CUDA slot drain is not linear: "
            "8192=%.0fns 32768=%.0fns 65536=%.0fns\n",
            elapsed[0], elapsed[1], elapsed[2]);
        return 1;
    }

    std::printf(
        "cuda_slot_release_scaling_ok "
        "8192_ns=%.0f 32768_ns=%.0f 65536_ns=%.0f "
        "per_record_ns=%.3f,%.3f,%.3f\n",
        elapsed[0], elapsed[1], elapsed[2], small_per_record,
        medium_per_record, large_per_record);
    return 0;
}
