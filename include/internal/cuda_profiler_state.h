#ifndef PEAK_CUDA_PROFILER_STATE_H
#define PEAK_CUDA_PROFILER_STATE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PeakCudaSlotLease {
    std::size_t index;
    std::uint64_t generation;
    std::uintptr_t context;
    std::size_t shard;
};

struct PeakCudaSlotAllocatorCounters {
    std::size_t capacity;
    std::size_t assigned_slots;
    std::size_t context_count;
    std::size_t active_slots;
    std::size_t high_water_slots;
};

struct PeakCudaLaunchDimensions {
    std::uint64_t total_threads;
    std::uint64_t grid_size;
    std::uint64_t block_size;
    bool overflow;
};

std::uint64_t peak_cuda_saturating_add_u64(std::uint64_t left,
                                           std::uint64_t right,
                                           bool* overflow = nullptr);
PeakCudaLaunchDimensions peak_cuda_compute_launch_dimensions(
    unsigned int grid_x, unsigned int grid_y, unsigned int grid_z,
    unsigned int block_x, unsigned int block_y, unsigned int block_z);

/** Fixed-capacity, context-partitioned logical CUDA event-slot allocator. */
class PeakCudaSlotAllocator {
public:
    static constexpr std::size_t kShardCount = 256;
    static constexpr std::size_t kCacheLineBytes = 64;

    explicit PeakCudaSlotAllocator(std::size_t capacity = 0);

    void reset(std::size_t capacity);
    bool acquire(std::uintptr_t context,
                 PeakCudaSlotLease* lease);
    bool acquire(std::uintptr_t context, std::size_t shard,
                 PeakCudaSlotLease* lease);
    bool acquire_if_accepting(std::uintptr_t context,
                              const std::atomic_bool& accepting,
                              PeakCudaSlotLease* lease,
                              bool* admission_closed);
    bool acquire_if_accepting(std::uintptr_t context, std::size_t shard,
                              const std::atomic_bool& accepting,
                              PeakCudaSlotLease* lease,
                              bool* admission_closed);
    bool release(const PeakCudaSlotLease& lease);
    std::size_t active_count() const noexcept;
    std::size_t snapshot_active_leases(PeakCudaSlotLease* leases,
                                       std::size_t lease_capacity) const;
    bool try_snapshot_active_leases(PeakCudaSlotLease* leases,
                                    std::size_t lease_capacity,
                                    std::size_t* active) const;
    PeakCudaSlotAllocatorCounters counters() const;

private:
    struct FreeHead {
        std::atomic<std::uint64_t> value;
        std::array<unsigned char,
                   kCacheLineBytes - sizeof(std::atomic<std::uint64_t>)>
            padding;
    };

    struct CounterShard {
        std::atomic<std::size_t> active;
        std::atomic<std::size_t> handoff;
        std::atomic<std::size_t> high_water;
        std::array<unsigned char,
                   kCacheLineBytes - 3 * sizeof(std::atomic<std::size_t>)>
            padding;
    };

    static_assert(sizeof(FreeHead) == kCacheLineBytes,
                  "free-list heads must occupy separate cache lines");
    static_assert(sizeof(CounterShard) == kCacheLineBytes,
                  "allocator counters must occupy separate cache lines");

    struct Slot {
        std::atomic<std::uint32_t> next_free;
        std::atomic<std::uint64_t> generation;
        std::uintptr_t context;
        std::uint16_t shard;
        std::atomic<unsigned char> state;
    };

    std::size_t find_or_claim_context(std::uintptr_t context);
    bool pop_free(std::size_t context_index, std::size_t shard,
                  PeakCudaSlotLease* lease);
    void push_free(std::size_t context_index, std::size_t shard,
                   std::uint32_t index);
    bool reserve_unassigned(std::size_t context_index,
                            std::uintptr_t context, std::size_t shard,
                            PeakCudaSlotLease* lease);
    std::size_t context_index(std::uintptr_t context) const;
    void record_active_acquire(std::size_t shard);

    std::unique_ptr<Slot[]> slots_;
    std::unique_ptr<std::atomic<std::uintptr_t>[]> contexts_;
    std::unique_ptr<FreeHead[]> free_heads_;
    std::size_t capacity_;
    std::size_t context_capacity_;
    std::atomic<std::size_t> next_unassigned_;
    std::atomic<std::size_t> assigned_slots_;
    std::atomic<std::size_t> context_count_;
    std::array<CounterShard, kShardCount> counter_shards_;
    std::uint64_t generation_seed_;
};

/** Fixed-capacity MPMC handoff from launch wrappers to the harvester. */
class PeakCudaPendingQueue {
public:
    explicit PeakCudaPendingQueue(std::size_t capacity = 0);

    void reset(std::size_t capacity);
    bool push(const PeakCudaSlotLease& lease, bool* shard_was_empty = nullptr);
    bool pop(PeakCudaSlotLease* lease);
    bool requeue_local(const PeakCudaSlotLease& lease);

private:
    struct QueueShard {
        std::atomic<std::uint64_t> head;
        std::array<unsigned char,
                   PeakCudaSlotAllocator::kCacheLineBytes -
                       sizeof(std::atomic<std::uint64_t>)>
            padding;
    };

    static_assert(sizeof(QueueShard) ==
                      PeakCudaSlotAllocator::kCacheLineBytes,
                  "pending queues must occupy separate cache lines");

    std::unique_ptr<std::atomic<std::uint32_t>[]> next_;
    std::unique_ptr<PeakCudaSlotLease[]> leases_;
    std::array<QueueShard, PeakCudaSlotAllocator::kShardCount> shards_;
    std::array<std::uint32_t,
               PeakCudaSlotAllocator::kShardCount> local_heads_;
    std::size_t capacity_;
    std::size_t dequeue_shard_;
};

/** CUDA-free admission and identity state used by the CUDA interceptor. */
struct PeakCudaProfilerCounters {
    std::size_t identity_capacity;
    std::size_t cached_identities;
    std::uint64_t observed_launches;
    std::uint64_t accepted_launches;
    std::uint64_t completed_launches;
    std::uint64_t dropped_pool_full;
    std::uint64_t dropped_identity_full;
    std::uint64_t dropped_event_create;
    /** Legacy aggregate for event record/query/elapsed-time failures. */
    std::uint64_t dropped_timing_error;
    std::uint64_t dropped_harvester_unavailable;
    std::uint64_t dropped_stream_capture;
    std::uint64_t dropped_capture_query;
    std::uint64_t dropped_capture_query_unsupported;
    std::uint64_t dropped_unsupported_multi_device;
    std::uint64_t dropped_event_query;
    std::uint64_t dropped_elapsed_time;
    std::uint64_t dropped_context_query;
    std::uint64_t dropped_context_switch;
    std::uint64_t dropped_context_restore;
    std::uint64_t finalization_timeouts;
    std::uint64_t finalization_incomplete;
    std::uint64_t dropped_dimension_overflow;
};

struct PeakCudaKernelIdentity {
    std::array<char, 256> name;
    bool target_match;
};

struct PeakCudaIdentityKey {
    std::uintptr_t value;
    bool driver_function;
    std::uintptr_t context;

    bool operator==(const PeakCudaIdentityKey& other) const
    {
        return value == other.value &&
               driver_function == other.driver_function &&
               context == other.context;
    }
};

struct PeakCudaIdentityKeyHash {
    std::size_t operator()(const PeakCudaIdentityKey& key) const
    {
        return std::hash<std::uintptr_t>()(key.value) ^
               (std::hash<bool>()(key.driver_function) << 1) ^
               (std::hash<std::uintptr_t>()(key.context) << 2);
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
        const std::vector<std::string>& targets,
        std::uintptr_t context = 0);

    bool cached_identity(std::uintptr_t identity,
                         bool driver_function,
                         PeakCudaKernelIdentity* result,
                         std::uintptr_t context = 0) const;

    void record_launch_observed(std::size_t shard = 0,
                                bool exclusive = false);
    void record_launch_accepted(std::size_t shard = 0,
                                bool exclusive = false);
    void record_launch_completed();
    void record_pool_full();
    void record_identity_full();
    void record_event_create_failure();
    void record_timing_error();
    void record_harvester_unavailable();
    void record_stream_capture_skip();
    void record_capture_query_failure();
    void record_capture_query_unsupported();
    void record_unsupported_multi_device();
    void record_event_query_failure();
    void record_elapsed_time_failure();
    void record_context_query_failure();
    void record_context_switch_failure();
    void record_context_restore_failure();
    void record_finalization_timeout(std::size_t incomplete_records);
    void record_dimension_overflow();
    PeakCudaProfilerCounters counters() const;

private:
    struct LaunchCounterShard {
        std::atomic<std::uint64_t> observed;
        std::atomic<std::uint64_t> accepted;
        std::array<unsigned char,
                   PeakCudaSlotAllocator::kCacheLineBytes -
                       2 * sizeof(std::atomic<std::uint64_t>)>
            padding;
    };

    static_assert(sizeof(LaunchCounterShard) ==
                      PeakCudaSlotAllocator::kCacheLineBytes,
                  "launch counters must occupy separate cache lines");

    static bool target_matches(const std::string& name,
                               const std::vector<std::string>& targets);

    mutable std::mutex mutex_;
    std::unordered_map<PeakCudaIdentityKey, PeakCudaKernelIdentity,
                       PeakCudaIdentityKeyHash> identities_;
    std::size_t identity_capacity_;
    std::array<LaunchCounterShard,
               PeakCudaSlotAllocator::kShardCount> launch_counter_shards_;
    std::atomic<std::uint64_t> completed_launches_;
    std::atomic<std::uint64_t> dropped_pool_full_;
    std::atomic<std::uint64_t> dropped_identity_full_;
    std::atomic<std::uint64_t> dropped_event_create_;
    std::atomic<std::uint64_t> dropped_timing_error_;
    std::atomic<std::uint64_t> dropped_harvester_unavailable_;
    std::atomic<std::uint64_t> dropped_stream_capture_;
    std::atomic<std::uint64_t> dropped_capture_query_;
    std::atomic<std::uint64_t> dropped_capture_query_unsupported_;
    std::atomic<std::uint64_t> dropped_unsupported_multi_device_;
    std::atomic<std::uint64_t> dropped_event_query_;
    std::atomic<std::uint64_t> dropped_elapsed_time_;
    std::atomic<std::uint64_t> dropped_context_query_;
    std::atomic<std::uint64_t> dropped_context_switch_;
    std::atomic<std::uint64_t> dropped_context_restore_;
    std::atomic<std::uint64_t> finalization_timeouts_;
    std::atomic<std::uint64_t> finalization_incomplete_;
    std::atomic<std::uint64_t> dropped_dimension_overflow_;
};

#endif /* PEAK_CUDA_PROFILER_STATE_H */
