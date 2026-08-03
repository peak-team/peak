#include "internal/cuda_profiler_state.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static int
test_identity_cache_and_non_target_fast_path()
{
    PeakCudaProfilerState state(2);
    const std::vector<std::string> targets = {"wanted"};
    PeakCudaKernelIdentity first =
        state.identify(0x1000, false, "unwanted", "unwanted", false, targets);
    PeakCudaKernelIdentity cached =
        state.identify(0x1000, false, "wanted", "wanted", false, targets);
    PeakCudaKernelIdentity driver =
        state.identify(0x1000, true, "wanted", "wanted", false, targets);
    PeakCudaKernelIdentity lookup;

    if (first.target_match || cached.target_match ||
        cached.name != "unwanted" || !driver.target_match ||
        !state.cached_identity(0x1000, false, &lookup) ||
        lookup.name != "unwanted" || state.counters().active_slots != 0) {
        return 1;
    }
    return 0;
}

static int
test_bounded_pool_and_drop_accounting()
{
    PeakCudaProfilerState state(2);

    if (!state.acquire_slot() || !state.acquire_slot() ||
        state.acquire_slot()) {
        return 1;
    }
    PeakCudaProfilerCounters counters = state.counters();
    if (counters.active_slots != 2 || counters.high_water_slots != 2 ||
        counters.dropped_pool_full != 1) {
        return 1;
    }
    state.release_slot();
    state.release_slot();
    state.record_event_create_failure();
    state.record_timing_error();
    counters = state.counters();
    return counters.active_slots != 0 ||
           counters.dropped_event_create != 1 ||
           counters.dropped_timing_error != 1;
}

static int
test_identity_cache_is_bounded()
{
    PeakCudaProfilerState state(4);
    const std::vector<std::string> targets = {"wanted"};

    state.reset(4, 1);
    (void)state.identify(0x1, false, "wanted", "wanted", false, targets);
    PeakCudaKernelIdentity overflow =
        state.identify(0x2, false, "another", "another", true, targets);
    PeakCudaProfilerCounters counters = state.counters();
    return overflow.name != "<unknown>" || !overflow.target_match ||
           counters.cached_identities != 1 ||
           counters.dropped_identity_full != 1;
}

static int
test_concurrent_stress_remains_bounded()
{
    PeakCudaProfilerState state(8);
    std::atomic<bool> start(false);
    std::vector<std::thread> workers;

    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&state, &start]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int iteration = 0; iteration < 10000; ++iteration) {
                if (state.acquire_slot()) {
                    state.release_slot();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    PeakCudaProfilerCounters counters = state.counters();
    return counters.active_slots != 0 || counters.high_water_slots > 8;
}

static int
fake_launch_with_admission(PeakCudaProfilerState* state,
                           bool event_create_succeeds,
                           int* original_calls,
                           int* event_records)
{
    if (!state->acquire_slot()) {
        ++*original_calls;
        return 7;
    }
    if (!event_create_succeeds) {
        state->record_event_create_failure();
        state->release_slot();
        ++*original_calls;
        return 7;
    }
    ++*event_records;
    ++*original_calls;
    ++*event_records;
    state->release_slot();
    return 7;
}

static int
test_admission_failure_executes_original_once_without_events()
{
    PeakCudaProfilerState state(1);
    int original_calls = 0;
    int event_records = 0;

    if (!state.acquire_slot() ||
        fake_launch_with_admission(&state, true, &original_calls,
                                   &event_records) != 7 ||
        original_calls != 1 || event_records != 0) {
        return 1;
    }
    state.release_slot();
    original_calls = 0;
    event_records = 0;
    if (fake_launch_with_admission(&state, false, &original_calls,
                                   &event_records) != 7 ||
        original_calls != 1 || event_records != 0 ||
        state.counters().dropped_event_create != 1) {
        return 1;
    }
    return 0;
}

int
main()
{
    if (test_identity_cache_and_non_target_fast_path() != 0 ||
        test_bounded_pool_and_drop_accounting() != 0 ||
        test_identity_cache_is_bounded() != 0 ||
        test_concurrent_stress_remains_bounded() != 0 ||
        test_admission_failure_executes_original_once_without_events() != 0) {
        return 1;
    }
    std::puts("cuda_profiler_state_test_ok");
    return 0;
}
