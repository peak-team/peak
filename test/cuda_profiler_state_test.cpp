#include "internal/cuda_profiler_state.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

static int
test_identity_cache_and_non_target_fast_path()
{
    PeakCudaProfilerState state(2);
    const std::vector<std::string> targets = {"wanted"};
    state.reset(2, 2, false, targets);
    PeakCudaKernelIdentity first =
        state.identify(0x1000, false, "unwanted", "unwanted");
    PeakCudaKernelIdentity cached =
        state.identify(0x1000, false, "wanted", "wanted");
    PeakCudaKernelIdentity driver =
        state.identify(0x1000, true, "wanted", "wanted");
    PeakCudaKernelIdentity lookup;

    if (first.target_match || cached.target_match ||
        std::strcmp(cached.name.data(), "unwanted") != 0 ||
        !driver.target_match ||
        !state.cached_identity(0x1000, false, &lookup) ||
        std::strcmp(lookup.name.data(), "unwanted") != 0) {
        return 1;
    }
    return 0;
}

static int
test_identity_overflow_is_bounded_and_suppressed()
{
    PeakCudaProfilerState state(4);
    const std::vector<std::string> targets;

    state.reset(4, 1, true, targets);
    (void)state.identify(0x1, false, "first", "first");
    PeakCudaKernelIdentity overflow =
        state.identify(0x2, false, "another", "another");
    PeakCudaKernelIdentity cached;
    if (!state.cached_identity(0x2, false, &cached)) {
        return 1;
    }
    PeakCudaProfilerCounters counters = state.counters();
    return std::strcmp(overflow.name.data(), "<identity-overflow>") != 0 ||
           !overflow.target_match ||
           std::strcmp(cached.name.data(), "<identity-overflow>") != 0 ||
           counters.cached_identities != 1 ||
           counters.cached_overflow_identities != 1 ||
           counters.dropped_identity_full != 1 ||
           counters.monitor_all_identity_overflow != 1 ||
           counters.repeated_identity_overflow_suppressed != 1;
}

static int
test_target_survives_negative_identity_saturation()
{
    PeakCudaProfilerState state(1);
    const std::vector<std::string> targets = {"wanted"};

    state.reset(1, 1, false, targets);
    PeakCudaKernelIdentity negative =
        state.identify(0x1, false, "unwanted", "unwanted");
    PeakCudaKernelIdentity target =
        state.identify(0x2, false, "wanted_display", "wanted");
    PeakCudaKernelIdentity cached_target;
    PeakCudaKernelIdentity suppressed_negative;
    PeakCudaProfilerCounters counters = state.counters();
    return negative.target_match || !target.target_match ||
           std::strcmp(target.name.data(), "wanted_display") != 0 ||
           !state.cached_identity(0x2, false, &cached_target) ||
           !cached_target.target_match ||
           !state.cached_identity(0x1, false, &suppressed_negative) ||
           suppressed_negative.target_match ||
           counters.cached_identities != 1 ||
           counters.cached_overflow_identities != 1 ||
           counters.negative_identity_overflow != 1 ||
           counters.positive_identity_admission_failures != 0;
}

static int
test_concurrent_target_after_saturation_is_unique()
{
    PeakCudaProfilerState state(8);
    const std::vector<std::string> targets = {"wanted"};
    state.reset(8, 8, false, targets);
    for (std::uintptr_t identity = 1; identity <= 8; ++identity) {
        (void)state.identify(identity, false, "unwanted", "unwanted");
    }

    std::atomic<bool> start(false);
    std::atomic<int> failures(0);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 16; ++worker) {
        workers.emplace_back([&state, &start, &failures]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            PeakCudaKernelIdentity result =
                state.identify(0x1000, false, "wanted_display", "wanted");
            if (!result.target_match ||
                std::strcmp(result.name.data(), "wanted_display") != 0) {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }

    PeakCudaKernelIdentity cached;
    PeakCudaProfilerCounters counters = state.counters();
    return failures.load(std::memory_order_relaxed) != 0 ||
           !state.cached_identity(0x1000, false, &cached) ||
           !cached.target_match || counters.cached_identities != 8 ||
           counters.cached_overflow_identities != 1 ||
           counters.positive_identity_admission_failures != 0;
}

static int
test_driver_identity_cache_is_context_aware()
{
    PeakCudaProfilerState state(4);
    const std::vector<std::string> targets = {"wanted"};
    state.reset(4, 4, false, targets);
    PeakCudaKernelIdentity first = state.identify(
        0x1000, true, "context-a", "not-wanted", 0xa);
    PeakCudaKernelIdentity second = state.identify(
        0x1000, true, "context-b", "wanted", 0xb);
    PeakCudaKernelIdentity cached_first;
    PeakCudaKernelIdentity cached_second;

    if (first.target_match ||
        std::strcmp(first.name.data(), "context-a") != 0 ||
        !second.target_match ||
        std::strcmp(second.name.data(), "context-b") != 0 ||
        !state.cached_identity(0x1000, true, &cached_first, 0xa) ||
        !state.cached_identity(0x1000, true, &cached_second, 0xb) ||
        std::strcmp(cached_first.name.data(), "context-a") != 0 ||
        cached_first.target_match ||
        std::strcmp(cached_second.name.data(), "context-b") != 0 ||
        !cached_second.target_match ||
        state.counters().cached_identities != 2) {
        return 1;
    }
    return 0;
}

static int
test_concurrent_stress_remains_bounded()
{
    PeakCudaSlotAllocator allocator(8);
    std::atomic<bool> start(false);
    std::vector<std::thread> workers;

    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&allocator, &start, worker]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int iteration = 0; iteration < 10000; ++iteration) {
                PeakCudaSlotLease lease = {};
                if (allocator.acquire(
                        1, static_cast<std::size_t>(worker), &lease)) {
                    if (!allocator.release(lease)) {
                        std::abort();
                    }
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    PeakCudaSlotAllocatorCounters counters = allocator.counters();
    return counters.active_slots != 0 || counters.high_water_slots > 8;
}

static int
test_pending_queue_shards_preserve_exact_handoff()
{
    constexpr std::size_t kWorkers = 8;
    constexpr std::size_t kPerWorker = 64;
    constexpr std::size_t kTotal = kWorkers * kPerWorker;
    PeakCudaPendingQueue queue(kTotal);
    std::atomic<bool> start(false);
    std::vector<std::thread> producers;
    std::vector<unsigned char> seen(kTotal, 0);

    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        producers.emplace_back([&queue, &start, worker]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (std::size_t item = 0; item < kPerWorker; ++item) {
                const std::size_t index = worker * kPerWorker + item;
                PeakCudaSlotLease lease = {
                    index, index + 1, 0x1000 + worker, worker,
                };
                if (!queue.push(lease)) {
                    std::abort();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);

    std::size_t received = 0;
    while (received != kTotal) {
        PeakCudaSlotLease lease = {};
        if (!queue.pop(&lease)) {
            std::this_thread::yield();
            continue;
        }
        if (lease.index >= kTotal || seen[lease.index] != 0 ||
            lease.generation != lease.index + 1 ||
            lease.shard >= kWorkers) {
            return 1;
        }
        seen[lease.index] = 1;
        ++received;
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    if (queue.pop(nullptr)) {
        return 1;
    }
    for (unsigned char value : seen) {
        if (value != 1) {
            return 1;
        }
    }
    return 0;
}

static int
test_pending_queue_local_retry_preserves_producer_empty_transition()
{
    PeakCudaPendingQueue queue(2);
    const PeakCudaSlotLease first = {0, 1, 0x1000, 0};
    const PeakCudaSlotLease second = {1, 1, 0x1000, 0};
    PeakCudaSlotLease received = {};
    bool shard_was_empty = false;

    if (!queue.push(first, &shard_was_empty) || !shard_was_empty ||
        !queue.pop(&received) || received.index != first.index ||
        !queue.requeue_local(received)) {
        return 1;
    }
    shard_was_empty = false;
    if (!queue.push(second, &shard_was_empty) || !shard_was_empty ||
        !queue.pop(&received) || received.index != first.index ||
        !queue.pop(&received) || received.index != second.index ||
        queue.pop(&received)) {
        return 1;
    }
    return 0;
}

static int
fake_launch_with_admission(PeakCudaSlotAllocator* allocator,
                           PeakCudaProfilerState* state,
                           bool event_create_succeeds,
                           int* original_calls,
                           int* event_records)
{
    PeakCudaSlotLease lease = {};
    if (!allocator->acquire(1, &lease)) {
        ++*original_calls;
        return 7;
    }
    if (!event_create_succeeds) {
        state->record_event_create_failure();
        if (!allocator->release(lease)) {
            return 8;
        }
        ++*original_calls;
        return 7;
    }
    ++*event_records;
    ++*original_calls;
    ++*event_records;
    if (!allocator->release(lease)) {
        return 8;
    }
    return 7;
}

static int
test_admission_failure_executes_original_once_without_events()
{
    PeakCudaSlotAllocator allocator(1);
    PeakCudaProfilerState state(1);
    PeakCudaSlotLease held = {};
    int original_calls = 0;
    int event_records = 0;

    if (!allocator.acquire(1, &held) ||
        fake_launch_with_admission(&allocator, &state, true, &original_calls,
                                   &event_records) != 7 ||
        original_calls != 1 || event_records != 0) {
        return 1;
    }
    if (!allocator.release(held)) {
        return 1;
    }
    original_calls = 0;
    event_records = 0;
    if (fake_launch_with_admission(&allocator, &state, false, &original_calls,
                                   &event_records) != 7 ||
        original_calls != 1 || event_records != 0 ||
        state.counters().dropped_event_create != 1) {
        return 1;
    }
    return 0;
}

static int
test_slot_allocator_partitions_contexts_and_reuses_slots()
{
    PeakCudaSlotAllocator allocator(3);
    PeakCudaSlotLease first = {};
    PeakCudaSlotLease other = {};
    PeakCudaSlotLease reused = {};

    if (!allocator.acquire(0xa, &first) || !allocator.release(first) ||
        !allocator.acquire(0xb, &other) ||
        other.index == first.index ||
        !allocator.acquire(0xa, &reused) ||
        reused.index != first.index ||
        reused.generation == first.generation) {
        return 1;
    }

    PeakCudaSlotAllocatorCounters counters = allocator.counters();
    PeakCudaSlotLease snapshot[1] = {};
    if (counters.capacity != 3 || counters.assigned_slots != 2 ||
        counters.context_count != 2 || counters.active_slots != 2 ||
        counters.high_water_slots != 2 ||
        allocator.snapshot_active_leases(snapshot, 1) != 2 ||
        (snapshot[0].context != 0xa && snapshot[0].context != 0xb)) {
        return 1;
    }
    return !allocator.release(other) || !allocator.release(reused);
}

static int
test_slot_allocator_bounds_exhaustion_and_reset()
{
    PeakCudaSlotAllocator allocator(2);
    PeakCudaSlotLease first = {};
    PeakCudaSlotLease second = {};
    PeakCudaSlotLease third = {};

    if (!allocator.acquire(1, &first) ||
        !allocator.acquire(2, &second) ||
        allocator.acquire(3, &third) ||
        !allocator.release(first) || !allocator.release(second) ||
        allocator.acquire(3, &third)) {
        return 1;
    }
    PeakCudaSlotAllocatorCounters counters = allocator.counters();
    if (counters.assigned_slots != 2 || counters.context_count != 2 ||
        counters.active_slots != 0 || counters.high_water_slots != 2) {
        return 1;
    }

    allocator.reset(1);
    counters = allocator.counters();
    if (counters.capacity != 1 || counters.assigned_slots != 0 ||
        counters.context_count != 0 || counters.active_slots != 0 ||
        counters.high_water_slots != 0 ||
        !allocator.acquire(3, &third) ||
        third.generation == first.generation || allocator.release(first)) {
        return 1;
    }
    return !allocator.release(third);
}

static int
test_slot_allocator_rejects_stale_and_double_release()
{
    PeakCudaSlotAllocator allocator(1);
    PeakCudaSlotLease first = {};
    PeakCudaSlotLease second = {};

    if (!allocator.acquire(7, &first) || !allocator.release(first) ||
        allocator.release(first) || !allocator.acquire(7, &second) ||
        second.index != first.index || second.generation == first.generation ||
        allocator.release(first)) {
        return 1;
    }

    PeakCudaSlotLease wrong_context = second;
    wrong_context.context = 8;
    if (allocator.release(wrong_context) ||
        allocator.counters().active_slots != 1) {
        return 1;
    }
    return !allocator.release(second) || allocator.release(second);
}

static int
test_slot_allocator_admission_close_is_fail_open()
{
    PeakCudaSlotAllocator allocator(1);
    std::atomic_bool accepting(false);
    PeakCudaSlotLease lease = {};
    bool admission_closed = false;

    if (allocator.acquire_if_accepting(
            7, accepting, &lease, &admission_closed) ||
        !admission_closed ||
        allocator.counters().active_slots != 0) {
        return 1;
    }
    accepting.store(true, std::memory_order_release);
    if (!allocator.acquire_if_accepting(
            7, accepting, &lease, &admission_closed) ||
        admission_closed) {
        return 1;
    }
    accepting.store(false, std::memory_order_release);
    if (allocator.acquire_if_accepting(
            7, accepting, &lease, &admission_closed) ||
        !admission_closed ||
        allocator.counters().active_slots != 1) {
        return 1;
    }
    return !allocator.release(lease);
}

static int
test_explicit_lifecycle_counters()
{
    PeakCudaProfilerState state(1);

    state.record_launch_observed();
    state.record_launch_observed();
    state.record_launch_accepted();
    state.record_launch_completed();
    state.record_pool_full();
    state.record_identity_full();
    state.record_event_create_failure();
    state.record_timing_error();
    state.record_harvester_unavailable();
    state.record_stream_capture_skip();
    state.record_capture_query_failure();
    state.record_capture_query_unsupported();
    state.record_unsupported_multi_device();
    state.record_event_query_failure();
    state.record_elapsed_time_failure();
    state.record_context_query_failure();
    state.record_context_switch_failure();
    state.record_context_restore_failure();
    state.record_finalization_timeout(3);
    state.record_dimension_overflow();

    PeakCudaProfilerCounters counters = state.counters();
    if (counters.observed_launches != 2 ||
        counters.accepted_launches != 1 ||
        counters.completed_launches != 1 ||
        counters.dropped_pool_full != 1 ||
        counters.dropped_identity_full != 1 ||
        counters.dropped_event_create != 1 ||
        counters.dropped_timing_error != 3 ||
        counters.dropped_harvester_unavailable != 1 ||
        counters.dropped_stream_capture != 1 ||
        counters.dropped_capture_query != 1 ||
        counters.dropped_capture_query_unsupported != 1 ||
        counters.dropped_unsupported_multi_device != 1 ||
        counters.dropped_event_query != 1 ||
        counters.dropped_elapsed_time != 1 ||
        counters.dropped_context_query != 1 ||
        counters.dropped_context_switch != 1 ||
        counters.dropped_context_restore != 1 ||
        counters.finalization_timeouts != 1 ||
        counters.finalization_incomplete != 3 ||
        counters.dropped_dimension_overflow != 1) {
        return 1;
    }

    state.reset(1);
    counters = state.counters();
    return counters.observed_launches != 0 ||
           counters.accepted_launches != 0 ||
           counters.completed_launches != 0 ||
           counters.dropped_pool_full != 0 ||
           counters.dropped_identity_full != 0 ||
           counters.dropped_timing_error != 0 ||
           counters.dropped_harvester_unavailable != 0 ||
           counters.finalization_timeouts != 0 ||
           counters.finalization_incomplete != 0 ||
           counters.dropped_dimension_overflow != 0;
}

static int
test_concurrent_counters_are_exact()
{
    PeakCudaProfilerState state(1);
    std::vector<std::thread> workers;
    constexpr int kWorkers = 8;
    constexpr int kIterations = 10000;

    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&state, worker]() {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                state.record_launch_observed(
                    static_cast<std::size_t>(worker), true);
                state.record_launch_accepted(
                    static_cast<std::size_t>(worker), true);
                state.record_pool_full();
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    PeakCudaProfilerCounters counters = state.counters();
    return counters.observed_launches != kWorkers * kIterations ||
           counters.accepted_launches != kWorkers * kIterations ||
           counters.dropped_pool_full != kWorkers * kIterations;
}

static int
test_dimension_arithmetic_is_wide_and_saturating()
{
    PeakCudaLaunchDimensions legal = peak_cuda_compute_launch_dimensions(
        65538U, 65535U, 1U, 1024U, 1U, 1U);
    const std::uint64_t expected_grid =
        static_cast<std::uint64_t>(65538U) * 65535U;
    if (legal.overflow || legal.grid_size != expected_grid ||
        legal.block_size != 1024U ||
        legal.total_threads != expected_grid * 1024U ||
        legal.grid_size <= std::numeric_limits<std::uint32_t>::max()) {
        return 1;
    }

    PeakCudaLaunchDimensions overflow = peak_cuda_compute_launch_dimensions(
        std::numeric_limits<unsigned int>::max(),
        std::numeric_limits<unsigned int>::max(),
        std::numeric_limits<unsigned int>::max(),
        std::numeric_limits<unsigned int>::max(), 2U, 2U);
    if (!overflow.overflow ||
        overflow.grid_size != std::numeric_limits<std::uint64_t>::max() ||
        overflow.total_threads != std::numeric_limits<std::uint64_t>::max()) {
        return 1;
    }

    bool add_overflow = false;
    std::uint64_t sum = peak_cuda_saturating_add_u64(
        std::numeric_limits<std::uint64_t>::max() - 1, 2, &add_overflow);
    return !add_overflow ||
           sum != std::numeric_limits<std::uint64_t>::max();
}

int
main()
{
    if (test_identity_cache_and_non_target_fast_path() != 0 ||
        test_identity_overflow_is_bounded_and_suppressed() != 0 ||
        test_target_survives_negative_identity_saturation() != 0 ||
        test_concurrent_target_after_saturation_is_unique() != 0 ||
        test_driver_identity_cache_is_context_aware() != 0 ||
        test_concurrent_stress_remains_bounded() != 0 ||
        test_pending_queue_shards_preserve_exact_handoff() != 0 ||
        test_pending_queue_local_retry_preserves_producer_empty_transition() !=
            0 ||
        test_admission_failure_executes_original_once_without_events() != 0 ||
        test_slot_allocator_partitions_contexts_and_reuses_slots() != 0 ||
        test_slot_allocator_bounds_exhaustion_and_reset() != 0 ||
        test_slot_allocator_rejects_stale_and_double_release() != 0 ||
        test_slot_allocator_admission_close_is_fail_open() != 0 ||
        test_explicit_lifecycle_counters() != 0 ||
        test_concurrent_counters_are_exact() != 0 ||
        test_dimension_arithmetic_is_wide_and_saturating() != 0) {
        return 1;
    }
    std::puts("cuda_profiler_state_test_ok");
    return 0;
}
