#include "internal/cuda_profiler_state.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr const char* kUnknownKernelName = "<unknown>";
constexpr const char* kIdentityOverflowName = "<identity-overflow>";
constexpr std::size_t kMaxOverflowIdentityCapacity = 256;
constexpr std::size_t kInvalidSlot = std::numeric_limits<std::size_t>::max();
constexpr std::uint32_t kInvalidSlotIndex =
    std::numeric_limits<std::uint32_t>::max();
constexpr unsigned char kSlotUnassigned = 0;
constexpr unsigned char kSlotLeased = 1;
constexpr unsigned char kSlotFree = 2;

std::uint64_t
pack_free_head(std::uint32_t index, std::uint32_t tag)
{
    return (static_cast<std::uint64_t>(tag) << 32) | index;
}

std::uint32_t
free_head_index(std::uint64_t head)
{
    return static_cast<std::uint32_t>(head);
}

std::uint32_t
free_head_tag(std::uint64_t head)
{
    return static_cast<std::uint32_t>(head >> 32);
}

PeakCudaKernelIdentity
make_kernel_identity(const char* name, bool target_match)
{
    PeakCudaKernelIdentity result = {};
    const char* source = name != nullptr && name[0] != '\0'
        ? name
        : kUnknownKernelName;
    std::size_t length = std::min(
        std::strlen(source), result.name.size() - 1);
    std::memcpy(result.name.data(), source, length);
    result.name[length] = '\0';
    result.target_match = target_match;
    return result;
}

std::uint64_t
saturating_multiply(std::uint64_t left, std::uint64_t right, bool* overflow)
{
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        if (overflow != nullptr) {
            *overflow = true;
        }
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}
}

std::uint64_t
peak_cuda_saturating_add_u64(std::uint64_t left, std::uint64_t right,
                             bool* overflow)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        if (overflow != nullptr) {
            *overflow = true;
        }
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

PeakCudaLaunchDimensions
peak_cuda_compute_launch_dimensions(
    unsigned int grid_x, unsigned int grid_y, unsigned int grid_z,
    unsigned int block_x, unsigned int block_y, unsigned int block_z)
{
    bool overflow = false;
    std::uint64_t grid_size = saturating_multiply(
        static_cast<std::uint64_t>(grid_x),
        static_cast<std::uint64_t>(grid_y), &overflow);
    grid_size = saturating_multiply(
        grid_size, static_cast<std::uint64_t>(grid_z), &overflow);
    std::uint64_t block_size = saturating_multiply(
        static_cast<std::uint64_t>(block_x),
        static_cast<std::uint64_t>(block_y), &overflow);
    block_size = saturating_multiply(
        block_size, static_cast<std::uint64_t>(block_z), &overflow);
    std::uint64_t total_threads = saturating_multiply(
        grid_size, block_size, &overflow);
    return {total_threads, grid_size, block_size, overflow};
}

PeakCudaSlotAllocator::PeakCudaSlotAllocator(std::size_t capacity)
    : capacity_(0),
      context_capacity_(0),
      next_unassigned_(0),
      assigned_slots_(0),
      context_count_(0),
      generation_seed_(0)
{
    reset(capacity);
}

void
PeakCudaSlotAllocator::reset(std::size_t capacity)
{
    if (capacity >= kInvalidSlotIndex) {
        capacity = kInvalidSlotIndex - 1;
    }
    capacity_ = capacity;
    context_capacity_ = std::min<std::size_t>(capacity, kShardCount);
    slots_.reset(capacity == 0 ? nullptr : new Slot[capacity]);
    contexts_.reset(context_capacity_ == 0
                        ? nullptr
                        : new std::atomic<std::uintptr_t>[context_capacity_]);
    const std::size_t head_count = context_capacity_ * kShardCount;
    free_heads_.reset(head_count == 0
                          ? nullptr
                          : new FreeHead[head_count]);
    generation_seed_ += UINT64_C(1) << 32;
    for (std::size_t index = 0; index < capacity_; ++index) {
        slots_[index].next_free.store(kInvalidSlotIndex,
                                     std::memory_order_relaxed);
        slots_[index].generation.store(generation_seed_,
                                      std::memory_order_relaxed);
        slots_[index].context = 0;
        slots_[index].shard = 0;
        slots_[index].state.store(kSlotUnassigned,
                                  std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < context_capacity_; ++index) {
        contexts_[index].store(0, std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < head_count; ++index) {
        free_heads_[index].value.store(
            pack_free_head(kInvalidSlotIndex, 0),
            std::memory_order_relaxed);
    }
    next_unassigned_.store(0, std::memory_order_relaxed);
    assigned_slots_.store(0, std::memory_order_relaxed);
    context_count_.store(0, std::memory_order_relaxed);
    for (std::size_t shard = 0; shard < kShardCount; ++shard) {
        counter_shards_[shard].active.store(0,
                                             std::memory_order_relaxed);
        counter_shards_[shard].handoff.store(0,
                                              std::memory_order_relaxed);
        counter_shards_[shard].high_water.store(
            0, std::memory_order_relaxed);
    }
}

bool
PeakCudaSlotAllocator::acquire(std::uintptr_t context,
                               PeakCudaSlotLease* lease)
{
    return acquire(context, 0, lease);
}

bool
PeakCudaSlotAllocator::acquire(std::uintptr_t context, std::size_t shard,
                               PeakCudaSlotLease* lease)
{
    if (lease == nullptr || context == 0 || context_capacity_ == 0) {
        return false;
    }
    shard %= kShardCount;
    const std::size_t bucket = find_or_claim_context(context);
    if (bucket == kInvalidSlot) {
        return false;
    }
    if (pop_free(bucket, shard, lease) ||
        reserve_unassigned(bucket, context, shard, lease)) {
        return true;
    }
    for (std::size_t offset = 1; offset < kShardCount; ++offset) {
        if (pop_free(bucket, (shard + offset) % kShardCount, lease)) {
            return true;
        }
    }
    return false;
}

bool
PeakCudaSlotAllocator::acquire_if_accepting(
    std::uintptr_t context, const std::atomic_bool& accepting,
    PeakCudaSlotLease* lease, bool* admission_closed)
{
    return acquire_if_accepting(context, 0, accepting, lease,
                                admission_closed);
}

bool
PeakCudaSlotAllocator::acquire_if_accepting(
    std::uintptr_t context, std::size_t shard,
    const std::atomic_bool& accepting,
    PeakCudaSlotLease* lease, bool* admission_closed)
{
    if (admission_closed != nullptr) {
        *admission_closed = false;
    }
    if (lease == nullptr ||
        !accepting.load(std::memory_order_seq_cst)) {
        if (admission_closed != nullptr) {
            *admission_closed = true;
        }
        return false;
    }
    if (!acquire(context, shard, lease)) {
        return false;
    }
    if (!accepting.load(std::memory_order_seq_cst)) {
        (void)release(*lease);
        if (admission_closed != nullptr) {
            *admission_closed = true;
        }
        return false;
    }
    return true;
}

bool
PeakCudaSlotAllocator::release(const PeakCudaSlotLease& lease)
{
    if (lease.index >= capacity_) {
        return false;
    }
    Slot& slot = slots_[lease.index];
    if (slot.context != lease.context || slot.shard != lease.shard ||
        slot.generation.load(std::memory_order_acquire) !=
            lease.generation) {
        return false;
    }
    counter_shards_[lease.shard].handoff.fetch_add(
        1, std::memory_order_relaxed);
    unsigned char expected = kSlotLeased;
    if (!slot.state.compare_exchange_strong(
            expected, kSlotFree, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        counter_shards_[lease.shard].handoff.fetch_sub(
            1, std::memory_order_relaxed);
        return false;
    }
    const std::size_t bucket = context_index(lease.context);
    if (bucket == kInvalidSlot) {
        slot.state.store(kSlotLeased, std::memory_order_release);
        counter_shards_[lease.shard].handoff.fetch_sub(
            1, std::memory_order_relaxed);
        return false;
    }
    counter_shards_[lease.shard].active.fetch_sub(
        1, std::memory_order_relaxed);
    push_free(bucket, lease.shard, static_cast<std::uint32_t>(lease.index));
    counter_shards_[lease.shard].handoff.fetch_sub(
        1, std::memory_order_release);
    return true;
}

std::size_t
PeakCudaSlotAllocator::active_count() const noexcept
{
    std::size_t total = 0;
    for (std::size_t shard = 0; shard < kShardCount; ++shard) {
        total += counter_shards_[shard].active.load(
            std::memory_order_acquire);
        total += counter_shards_[shard].handoff.load(
            std::memory_order_acquire);
    }
    return total;
}

std::size_t
PeakCudaSlotAllocator::snapshot_active_leases(
    PeakCudaSlotLease* leases, std::size_t lease_capacity) const
{
    std::size_t active = 0;
    for (std::size_t index = 0; index < capacity_; ++index) {
        const Slot& slot = slots_[index];
        if (slot.state.load(std::memory_order_acquire) != kSlotLeased) {
            continue;
        }
        if (leases != nullptr && active < lease_capacity) {
            leases[active] = {
                index,
                slot.generation.load(std::memory_order_acquire),
                slot.context,
                slot.shard,
            };
        }
        ++active;
    }
    return active;
}

bool
PeakCudaSlotAllocator::try_snapshot_active_leases(
    PeakCudaSlotLease* leases, std::size_t lease_capacity,
    std::size_t* active) const
{
    const std::size_t count = snapshot_active_leases(leases,
                                                      lease_capacity);
    if (active != nullptr) {
        *active = count;
    }
    return true;
}

PeakCudaSlotAllocatorCounters
PeakCudaSlotAllocator::counters() const
{
    std::size_t active = 0;
    std::size_t high_water = 0;
    for (std::size_t shard = 0; shard < kShardCount; ++shard) {
        active += counter_shards_[shard].active.load(
            std::memory_order_relaxed);
        high_water += counter_shards_[shard].high_water.load(
            std::memory_order_relaxed);
    }
    return {
        capacity_,
        assigned_slots_.load(std::memory_order_relaxed),
        context_count_.load(std::memory_order_relaxed),
        active,
        high_water,
    };
}

std::size_t
PeakCudaSlotAllocator::find_or_claim_context(std::uintptr_t context)
{
    const std::size_t start = std::hash<std::uintptr_t>()(context) %
                              context_capacity_;
    for (std::size_t offset = 0; offset < context_capacity_; ++offset) {
        const std::size_t index = (start + offset) % context_capacity_;
        std::uintptr_t current = contexts_[index].load(
            std::memory_order_acquire);
        if (current == context) {
            return index;
        }
        if (current == 0 && contexts_[index].compare_exchange_strong(
                                current, context,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
            context_count_.fetch_add(1, std::memory_order_relaxed);
            return index;
        }
        if (current == context) {
            return index;
        }
    }
    return kInvalidSlot;
}

std::size_t
PeakCudaSlotAllocator::context_index(std::uintptr_t context) const
{
    if (context == 0 || context_capacity_ == 0) {
        return kInvalidSlot;
    }
    const std::size_t start = std::hash<std::uintptr_t>()(context) %
                              context_capacity_;
    for (std::size_t offset = 0; offset < context_capacity_; ++offset) {
        const std::size_t index = (start + offset) % context_capacity_;
        const std::uintptr_t current = contexts_[index].load(
            std::memory_order_acquire);
        if (current == context) {
            return index;
        }
        if (current == 0) {
            return kInvalidSlot;
        }
    }
    return kInvalidSlot;
}

bool
PeakCudaSlotAllocator::pop_free(std::size_t context_index,
                                std::size_t shard,
                                PeakCudaSlotLease* lease)
{
    std::atomic<std::uint64_t>& head =
        free_heads_[context_index * kShardCount + shard].value;
    std::uint64_t current = head.load(std::memory_order_acquire);
    for (;;) {
        const std::uint32_t index = free_head_index(current);
        if (index == kInvalidSlotIndex) {
            return false;
        }
        const std::uint32_t next = slots_[index].next_free.load(
            std::memory_order_relaxed);
        const std::uint64_t desired = pack_free_head(
            next, free_head_tag(current) + 1);
        if (!head.compare_exchange_weak(current, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            continue;
        }
        unsigned char expected = kSlotFree;
        if (!slots_[index].state.compare_exchange_strong(
                expected, kSlotLeased, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }
        const std::uint64_t generation =
            slots_[index].generation.fetch_add(
                1, std::memory_order_relaxed) + 1;
        record_active_acquire(slots_[index].shard);
        *lease = {index, generation, slots_[index].context,
                  slots_[index].shard};
        return true;
    }
}

void
PeakCudaSlotAllocator::push_free(std::size_t context_index,
                                 std::size_t shard,
                                 std::uint32_t index)
{
    std::atomic<std::uint64_t>& head =
        free_heads_[context_index * kShardCount + shard].value;
    std::uint64_t current = head.load(std::memory_order_relaxed);
    do {
        slots_[index].next_free.store(free_head_index(current),
                                     std::memory_order_relaxed);
    } while (!head.compare_exchange_weak(
        current, pack_free_head(index, free_head_tag(current) + 1),
        std::memory_order_release, std::memory_order_relaxed));
}

bool
PeakCudaSlotAllocator::reserve_unassigned(
    std::size_t context_bucket, std::uintptr_t context, std::size_t shard,
    PeakCudaSlotLease* lease)
{
    (void)context_bucket;
    std::size_t index = next_unassigned_.load(std::memory_order_relaxed);
    while (index < capacity_ &&
           !next_unassigned_.compare_exchange_weak(
               index, index + 1, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
    }
    if (index >= capacity_) {
        return false;
    }
    Slot& slot = slots_[index];
    slot.context = context;
    slot.shard = static_cast<std::uint16_t>(shard);
    const std::uint64_t generation = slot.generation.fetch_add(
        1, std::memory_order_relaxed) + 1;
    slot.state.store(kSlotLeased, std::memory_order_release);
    assigned_slots_.fetch_add(1, std::memory_order_relaxed);
    record_active_acquire(shard);
    *lease = {index, generation, context, shard};
    return true;
}

void
PeakCudaSlotAllocator::record_active_acquire(std::size_t shard)
{
    const std::size_t active = counter_shards_[shard].active.fetch_add(
        1, std::memory_order_relaxed) + 1;
    std::size_t high_water = counter_shards_[shard].high_water.load(
        std::memory_order_relaxed);
    while (high_water < active &&
           !counter_shards_[shard].high_water.compare_exchange_weak(
               high_water, active, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

PeakCudaPendingQueue::PeakCudaPendingQueue(std::size_t capacity)
    : capacity_(0), dequeue_shard_(0)
{
    reset(capacity);
}

void
PeakCudaPendingQueue::reset(std::size_t capacity)
{
    if (capacity >= kInvalidSlotIndex) {
        capacity = kInvalidSlotIndex - 1;
    }
    capacity_ = capacity;
    next_.reset(capacity == 0
                    ? nullptr
                    : new std::atomic<std::uint32_t>[capacity]);
    leases_.reset(capacity == 0 ? nullptr
                                : new PeakCudaSlotLease[capacity]);
    for (std::size_t index = 0; index < capacity_; ++index) {
        next_[index].store(kInvalidSlotIndex, std::memory_order_relaxed);
        leases_[index] = {};
    }
    for (std::size_t shard = 0;
         shard < PeakCudaSlotAllocator::kShardCount; ++shard) {
        shards_[shard].head.store(pack_free_head(kInvalidSlotIndex, 0),
                                  std::memory_order_relaxed);
        local_heads_[shard] = kInvalidSlotIndex;
    }
    dequeue_shard_ = 0;
}

bool
PeakCudaPendingQueue::push(const PeakCudaSlotLease& lease,
                           bool* shard_was_empty)
{
    if (shard_was_empty != nullptr) {
        *shard_was_empty = false;
    }
    if (lease.index >= capacity_ ||
        lease.shard >= PeakCudaSlotAllocator::kShardCount) {
        return false;
    }
    leases_[lease.index] = lease;
    std::atomic<std::uint64_t>& head = shards_[lease.shard].head;
    std::uint64_t current = head.load(std::memory_order_relaxed);
    do {
        next_[lease.index].store(free_head_index(current),
                                 std::memory_order_relaxed);
    } while (!head.compare_exchange_weak(
        current,
        pack_free_head(static_cast<std::uint32_t>(lease.index),
                       free_head_tag(current) + 1),
        std::memory_order_release, std::memory_order_relaxed));
    if (shard_was_empty != nullptr) {
        *shard_was_empty = free_head_index(current) == kInvalidSlotIndex;
    }
    return true;
}

bool
PeakCudaPendingQueue::pop(PeakCudaSlotLease* lease)
{
    if (lease == nullptr || capacity_ == 0) {
        return false;
    }
    for (std::size_t offset = 0;
         offset < PeakCudaSlotAllocator::kShardCount; ++offset) {
        const std::size_t shard =
            (dequeue_shard_ + offset) % PeakCudaSlotAllocator::kShardCount;
        std::uint32_t index = local_heads_[shard];
        if (index == kInvalidSlotIndex) {
            std::atomic<std::uint64_t>& head = shards_[shard].head;
            std::uint64_t current = head.load(std::memory_order_acquire);
            while (free_head_index(current) != kInvalidSlotIndex) {
                const std::uint64_t desired = pack_free_head(
                    kInvalidSlotIndex, free_head_tag(current) + 1);
                if (head.compare_exchange_weak(
                        current, desired, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    break;
                }
            }
            std::uint32_t detached = free_head_index(current);
            std::uint32_t reversed = kInvalidSlotIndex;
            while (detached != kInvalidSlotIndex) {
                const std::uint32_t next = next_[detached].load(
                    std::memory_order_relaxed);
                next_[detached].store(reversed, std::memory_order_relaxed);
                reversed = detached;
                detached = next;
            }
            local_heads_[shard] = reversed;
            index = reversed;
        }
        if (index != kInvalidSlotIndex) {
            local_heads_[shard] = next_[index].load(
                std::memory_order_relaxed);
            *lease = leases_[index];
            dequeue_shard_ = (shard + 1) %
                             PeakCudaSlotAllocator::kShardCount;
            return true;
        }
    }
    return false;
}

bool
PeakCudaPendingQueue::requeue_local(const PeakCudaSlotLease& lease)
{
    if (lease.index >= capacity_ ||
        lease.shard >= PeakCudaSlotAllocator::kShardCount) {
        return false;
    }
    leases_[lease.index] = lease;
    next_[lease.index].store(local_heads_[lease.shard],
                             std::memory_order_relaxed);
    local_heads_[lease.shard] = static_cast<std::uint32_t>(lease.index);
    return true;
}

PeakCudaProfilerState::PeakCudaProfilerState(std::size_t capacity)
    : identity_capacity_(capacity),
      overflow_identity_capacity_(std::min(
          capacity, kMaxOverflowIdentityCapacity)),
      monitor_all_(false),
      completed_launches_(0),
      dropped_pool_full_(0),
      dropped_identity_full_(0),
      positive_identity_admission_failures_(0),
      negative_identity_overflow_(0),
      monitor_all_identity_overflow_(0),
      repeated_identity_overflow_suppressed_(0),
      dropped_event_create_(0),
      dropped_timing_error_(0),
      dropped_harvester_unavailable_(0),
      dropped_stream_capture_(0),
      dropped_capture_query_(0),
      dropped_capture_query_unsupported_(0),
      dropped_unsupported_multi_device_(0),
      dropped_event_query_(0),
      dropped_elapsed_time_(0),
      dropped_context_query_(0),
      dropped_context_switch_(0),
      dropped_context_restore_(0),
      finalization_timeouts_(0),
      finalization_incomplete_(0),
      dropped_dimension_overflow_(0)
{
    for (std::size_t shard = 0;
         shard < PeakCudaSlotAllocator::kShardCount; ++shard) {
        launch_counter_shards_[shard].observed.store(
            0, std::memory_order_relaxed);
        launch_counter_shards_[shard].accepted.store(
            0, std::memory_order_relaxed);
    }
}

void
PeakCudaProfilerState::reset(std::size_t capacity,
                             std::size_t identity_capacity,
                             bool monitor_all,
                             const std::vector<std::string>& targets)
{
    std::lock_guard<std::mutex> lock(mutex_);

    identities_.clear();
    overflow_identities_.clear();
    identity_capacity_ = identity_capacity == 0 ? capacity : identity_capacity;
    overflow_identity_capacity_ = std::min(
        identity_capacity_, kMaxOverflowIdentityCapacity);
    monitor_all_ = monitor_all;
    targets_ = targets;
    for (std::size_t shard = 0;
         shard < PeakCudaSlotAllocator::kShardCount; ++shard) {
        launch_counter_shards_[shard].observed.store(
            0, std::memory_order_relaxed);
        launch_counter_shards_[shard].accepted.store(
            0, std::memory_order_relaxed);
    }
    completed_launches_.store(0, std::memory_order_relaxed);
    dropped_pool_full_.store(0, std::memory_order_relaxed);
    dropped_identity_full_.store(0, std::memory_order_relaxed);
    positive_identity_admission_failures_.store(
        0, std::memory_order_relaxed);
    negative_identity_overflow_.store(0, std::memory_order_relaxed);
    monitor_all_identity_overflow_.store(0, std::memory_order_relaxed);
    repeated_identity_overflow_suppressed_.store(
        0, std::memory_order_relaxed);
    dropped_event_create_.store(0, std::memory_order_relaxed);
    dropped_timing_error_.store(0, std::memory_order_relaxed);
    dropped_harvester_unavailable_.store(0, std::memory_order_relaxed);
    dropped_stream_capture_.store(0, std::memory_order_relaxed);
    dropped_capture_query_.store(0, std::memory_order_relaxed);
    dropped_capture_query_unsupported_.store(0, std::memory_order_relaxed);
    dropped_unsupported_multi_device_.store(0, std::memory_order_relaxed);
    dropped_event_query_.store(0, std::memory_order_relaxed);
    dropped_elapsed_time_.store(0, std::memory_order_relaxed);
    dropped_context_query_.store(0, std::memory_order_relaxed);
    dropped_context_switch_.store(0, std::memory_order_relaxed);
    dropped_context_restore_.store(0, std::memory_order_relaxed);
    finalization_timeouts_.store(0, std::memory_order_relaxed);
    finalization_incomplete_.store(0, std::memory_order_relaxed);
    dropped_dimension_overflow_.store(0, std::memory_order_relaxed);
}

PeakCudaKernelIdentity
PeakCudaProfilerState::identify(
    std::uintptr_t identity,
    bool driver_function,
    const char* display_name,
    const char* target_name,
    std::uintptr_t context)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const PeakCudaIdentityKey key = {identity, driver_function, context};
    const auto existing = identities_.find(key);

    if (existing != identities_.end()) {
        return existing->second;
    }
    const auto existing_overflow = overflow_identities_.find(key);
    if (existing_overflow != overflow_identities_.end()) {
        repeated_identity_overflow_suppressed_.fetch_add(
            1, std::memory_order_relaxed);
        return existing_overflow->second;
    }

    PeakCudaKernelIdentity result = make_kernel_identity(display_name, false);
    result.target_match = monitor_all_ ||
                          (target_name != nullptr && target_name[0] != '\0' &&
                           target_matches(target_name, targets_));
    if (identities_.size() >= identity_capacity_) {
        record_identity_full();
        if (result.target_match && !monitor_all_) {
            const auto negative = std::find_if(
                identities_.begin(), identities_.end(),
                [](const auto& entry) {
                    return !entry.second.target_match;
                });
            if (negative != identities_.end()) {
                if (overflow_identities_.size() <
                    overflow_identity_capacity_) {
                    overflow_identities_.emplace(
                        negative->first, negative->second);
                }
                negative_identity_overflow_.fetch_add(
                    1, std::memory_order_relaxed);
                identities_.erase(negative);
                identities_.emplace(key, result);
                return result;
            }
            positive_identity_admission_failures_.fetch_add(
                1, std::memory_order_relaxed);
        }

        PeakCudaKernelIdentity overflow = monitor_all_
            ? make_kernel_identity(kIdentityOverflowName, true)
            : result;
        if (overflow_identities_.size() < overflow_identity_capacity_) {
            overflow_identities_.emplace(key, overflow);
        }
        if (monitor_all_) {
            monitor_all_identity_overflow_.fetch_add(
                1, std::memory_order_relaxed);
        } else if (!result.target_match) {
            negative_identity_overflow_.fetch_add(
                1, std::memory_order_relaxed);
        }
        return overflow;
    }

    identities_.emplace(key, result);
    return result;
}

bool
PeakCudaProfilerState::cached_identity(
    std::uintptr_t identity,
    bool driver_function,
    PeakCudaKernelIdentity* result,
    std::uintptr_t context)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = identities_.find(
        {identity, driver_function, context});

    if (existing != identities_.end()) {
        if (result != nullptr) {
            *result = existing->second;
        }
        return true;
    }
    const auto overflow = overflow_identities_.find(
        {identity, driver_function, context});
    if (overflow == overflow_identities_.end()) {
        return false;
    }
    repeated_identity_overflow_suppressed_.fetch_add(
        1, std::memory_order_relaxed);
    if (result != nullptr) {
        *result = overflow->second;
    }
    return true;
}

void
PeakCudaProfilerState::record_launch_observed(std::size_t shard,
                                              bool exclusive)
{
    std::atomic<std::uint64_t>& observed =
        launch_counter_shards_[shard % PeakCudaSlotAllocator::kShardCount]
            .observed;
    if (exclusive) {
        observed.store(observed.load(std::memory_order_relaxed) + 1,
                       std::memory_order_relaxed);
    } else {
        observed.fetch_add(1, std::memory_order_relaxed);
    }
}

void
PeakCudaProfilerState::record_launch_accepted(std::size_t shard,
                                              bool exclusive)
{
    std::atomic<std::uint64_t>& accepted =
        launch_counter_shards_[shard % PeakCudaSlotAllocator::kShardCount]
            .accepted;
    if (exclusive) {
        accepted.store(accepted.load(std::memory_order_relaxed) + 1,
                       std::memory_order_relaxed);
    } else {
        accepted.fetch_add(1, std::memory_order_relaxed);
    }
}

void
PeakCudaProfilerState::record_launch_completed()
{
    completed_launches_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_pool_full()
{
    dropped_pool_full_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_identity_full()
{
    dropped_identity_full_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_event_create_failure()
{
    dropped_event_create_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_timing_error()
{
    dropped_timing_error_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_harvester_unavailable()
{
    dropped_harvester_unavailable_.fetch_add(
        1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_stream_capture_skip()
{
    dropped_stream_capture_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_capture_query_failure()
{
    dropped_capture_query_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_capture_query_unsupported()
{
    dropped_capture_query_unsupported_.fetch_add(
        1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_unsupported_multi_device()
{
    dropped_unsupported_multi_device_.fetch_add(
        1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_event_query_failure()
{
    dropped_event_query_.fetch_add(1, std::memory_order_relaxed);
    dropped_timing_error_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_elapsed_time_failure()
{
    dropped_elapsed_time_.fetch_add(1, std::memory_order_relaxed);
    dropped_timing_error_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_context_query_failure()
{
    dropped_context_query_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_context_switch_failure()
{
    dropped_context_switch_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_context_restore_failure()
{
    dropped_context_restore_.fetch_add(1, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_finalization_timeout(
    std::size_t incomplete_records)
{
    finalization_timeouts_.fetch_add(1, std::memory_order_relaxed);
    finalization_incomplete_.fetch_add(
        incomplete_records, std::memory_order_relaxed);
}

void
PeakCudaProfilerState::record_dimension_overflow()
{
    dropped_dimension_overflow_.fetch_add(1, std::memory_order_relaxed);
}

PeakCudaProfilerCounters
PeakCudaProfilerState::counters() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    PeakCudaProfilerCounters result = {};
    result.identity_capacity = identity_capacity_;
    result.cached_identities = identities_.size();
    result.cached_overflow_identities = overflow_identities_.size();
    for (std::size_t shard = 0;
         shard < PeakCudaSlotAllocator::kShardCount; ++shard) {
        result.observed_launches +=
            launch_counter_shards_[shard].observed.load(
            std::memory_order_relaxed);
        result.accepted_launches +=
            launch_counter_shards_[shard].accepted.load(
            std::memory_order_relaxed);
    }
    result.completed_launches =
        completed_launches_.load(std::memory_order_relaxed);
    result.dropped_pool_full =
        dropped_pool_full_.load(std::memory_order_relaxed);
    result.dropped_identity_full =
        dropped_identity_full_.load(std::memory_order_relaxed);
    result.positive_identity_admission_failures =
        positive_identity_admission_failures_.load(
            std::memory_order_relaxed);
    result.negative_identity_overflow =
        negative_identity_overflow_.load(std::memory_order_relaxed);
    result.monitor_all_identity_overflow =
        monitor_all_identity_overflow_.load(std::memory_order_relaxed);
    result.repeated_identity_overflow_suppressed =
        repeated_identity_overflow_suppressed_.load(
            std::memory_order_relaxed);
    result.dropped_event_create =
        dropped_event_create_.load(std::memory_order_relaxed);
    result.dropped_timing_error =
        dropped_timing_error_.load(std::memory_order_relaxed);
    result.dropped_harvester_unavailable =
        dropped_harvester_unavailable_.load(std::memory_order_relaxed);
    result.dropped_stream_capture =
        dropped_stream_capture_.load(std::memory_order_relaxed);
    result.dropped_capture_query =
        dropped_capture_query_.load(std::memory_order_relaxed);
    result.dropped_capture_query_unsupported =
        dropped_capture_query_unsupported_.load(std::memory_order_relaxed);
    result.dropped_unsupported_multi_device =
        dropped_unsupported_multi_device_.load(std::memory_order_relaxed);
    result.dropped_event_query =
        dropped_event_query_.load(std::memory_order_relaxed);
    result.dropped_elapsed_time =
        dropped_elapsed_time_.load(std::memory_order_relaxed);
    result.dropped_context_query =
        dropped_context_query_.load(std::memory_order_relaxed);
    result.dropped_context_switch =
        dropped_context_switch_.load(std::memory_order_relaxed);
    result.dropped_context_restore =
        dropped_context_restore_.load(std::memory_order_relaxed);
    result.finalization_timeouts =
        finalization_timeouts_.load(std::memory_order_relaxed);
    result.finalization_incomplete =
        finalization_incomplete_.load(std::memory_order_relaxed);
    result.dropped_dimension_overflow =
        dropped_dimension_overflow_.load(std::memory_order_relaxed);
    return result;
}

bool
PeakCudaProfilerState::target_matches(const std::string& name,
                                      const std::vector<std::string>& targets)
{
    return std::find(targets.begin(), targets.end(), name) != targets.end();
}
