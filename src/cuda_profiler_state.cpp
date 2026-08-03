#include "internal/cuda_profiler_state.h"

#include <algorithm>

namespace {
constexpr const char* kUnknownKernelName = "<unknown>";
constexpr std::size_t kMaximumKernelNameLength = 255;
}

PeakCudaProfilerState::PeakCudaProfilerState(std::size_t capacity)
    : capacity_(capacity),
      identity_capacity_(capacity),
      active_slots_(0),
      high_water_slots_(0),
      dropped_pool_full_(0),
      dropped_identity_full_(0),
      dropped_event_create_(0),
      dropped_timing_error_(0)
{
}

void
PeakCudaProfilerState::reset(std::size_t capacity,
                             std::size_t identity_capacity)
{
    std::lock_guard<std::mutex> lock(mutex_);

    identities_.clear();
    capacity_ = capacity;
    identity_capacity_ = identity_capacity == 0 ? capacity : identity_capacity;
    active_slots_ = 0;
    high_water_slots_ = 0;
    dropped_pool_full_ = 0;
    dropped_identity_full_ = 0;
    dropped_event_create_ = 0;
    dropped_timing_error_ = 0;
}

PeakCudaKernelIdentity
PeakCudaProfilerState::identify(
    std::uintptr_t identity,
    bool driver_function,
    const char* display_name,
    const char* target_name,
    bool monitor_all,
    const std::vector<std::string>& targets)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const PeakCudaIdentityKey key = {identity, driver_function};
    const auto existing = identities_.find(key);

    if (existing != identities_.end()) {
        return existing->second;
    }

    if (identities_.size() >= identity_capacity_) {
        ++dropped_identity_full_;
        return {kUnknownKernelName, monitor_all};
    }

    PeakCudaKernelIdentity result = {
        display_name != nullptr && display_name[0] != '\0'
            ? display_name
            : kUnknownKernelName,
        false,
    };
    if (result.name.size() > kMaximumKernelNameLength) {
        result.name.resize(kMaximumKernelNameLength);
    }
    result.target_match = monitor_all ||
                          (target_name != nullptr && target_name[0] != '\0' &&
                           target_matches(target_name, targets));
    identities_.emplace(key, result);
    return result;
}

bool
PeakCudaProfilerState::cached_identity(
    std::uintptr_t identity,
    bool driver_function,
    PeakCudaKernelIdentity* result) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = identities_.find({identity, driver_function});

    if (existing == identities_.end()) {
        return false;
    }
    if (result != nullptr) {
        *result = existing->second;
    }
    return true;
}

bool
PeakCudaProfilerState::acquire_slot()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_slots_ >= capacity_) {
        ++dropped_pool_full_;
        return false;
    }
    ++active_slots_;
    high_water_slots_ = std::max(high_water_slots_, active_slots_);
    return true;
}

void
PeakCudaProfilerState::release_slot()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_slots_ != 0) {
        --active_slots_;
    }
}

void
PeakCudaProfilerState::record_event_create_failure()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_event_create_;
}

void
PeakCudaProfilerState::record_timing_error()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_timing_error_;
}

PeakCudaProfilerCounters
PeakCudaProfilerState::counters() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return {
        capacity_,
        identity_capacity_,
        identities_.size(),
        active_slots_,
        high_water_slots_,
        dropped_pool_full_,
        dropped_identity_full_,
        dropped_event_create_,
        dropped_timing_error_,
    };
}

bool
PeakCudaProfilerState::target_matches(const std::string& name,
                                      const std::vector<std::string>& targets)
{
    return std::find(targets.begin(), targets.end(), name) != targets.end();
}
