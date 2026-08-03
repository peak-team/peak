#ifndef PEAK_CUDA_PROFILER_STATE_H
#define PEAK_CUDA_PROFILER_STATE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/** CUDA-free admission and identity state used by the CUDA interceptor. */
struct PeakCudaProfilerCounters {
    std::size_t capacity;
    std::size_t identity_capacity;
    std::size_t cached_identities;
    std::size_t active_slots;
    std::size_t high_water_slots;
    std::uint64_t dropped_pool_full;
    std::uint64_t dropped_identity_full;
    std::uint64_t dropped_event_create;
    std::uint64_t dropped_timing_error;
};

struct PeakCudaKernelIdentity {
    std::string name;
    bool target_match;
};

struct PeakCudaIdentityKey {
    std::uintptr_t value;
    bool driver_function;

    bool operator==(const PeakCudaIdentityKey& other) const
    {
        return value == other.value && driver_function == other.driver_function;
    }
};

struct PeakCudaIdentityKeyHash {
    std::size_t operator()(const PeakCudaIdentityKey& key) const
    {
        return std::hash<std::uintptr_t>()(key.value) ^
               (std::hash<bool>()(key.driver_function) << 1);
    }
};

class PeakCudaProfilerState {
public:
    explicit PeakCudaProfilerState(std::size_t capacity = 256);

    void reset(std::size_t capacity, std::size_t identity_capacity = 0);

    PeakCudaKernelIdentity identify(
        std::uintptr_t identity,
        bool driver_function,
        const char* display_name,
        const char* target_name,
        bool monitor_all,
        const std::vector<std::string>& targets);

    bool cached_identity(std::uintptr_t identity,
                         bool driver_function,
                         PeakCudaKernelIdentity* result) const;

    bool acquire_slot();
    void release_slot();
    void record_event_create_failure();
    void record_timing_error();
    PeakCudaProfilerCounters counters() const;

private:
    static bool target_matches(const std::string& name,
                               const std::vector<std::string>& targets);

    mutable std::mutex mutex_;
    std::unordered_map<PeakCudaIdentityKey, PeakCudaKernelIdentity,
                       PeakCudaIdentityKeyHash> identities_;
    std::size_t capacity_;
    std::size_t identity_capacity_;
    std::size_t active_slots_;
    std::size_t high_water_slots_;
    std::uint64_t dropped_pool_full_;
    std::uint64_t dropped_identity_full_;
    std::uint64_t dropped_event_create_;
    std::uint64_t dropped_timing_error_;
};

#endif /* PEAK_CUDA_PROFILER_STATE_H */
