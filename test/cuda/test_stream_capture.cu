#include "cuda_test_common.cuh"

#include <cuda.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace {
constexpr int kCaptureCycles = 40;

using PeakCudaForceIncompleteFn = void (*)(int);
using PeakCudaHarvesterQueryCountFn = unsigned long long (*)(void);
using PeakCudaHarvesterReadyFn = int (*)(void);
using PeakCudaActiveSlotCountFn = unsigned long long (*)(void);
using PeakCudaPauseFn = void (*)(int);
using PeakCudaBoolFn = int (*)(void);
using PeakCudaWaitingCountFn = unsigned int (*)(void);
using PeakCudaFinalizeFn = void (*)(void);

struct PeakCudaCaptureTestSeams {
    PeakCudaForceIncompleteFn force_incomplete;
    PeakCudaHarvesterQueryCountFn harvester_query_count;
    PeakCudaHarvesterReadyFn harvester_ready;
    PeakCudaActiveSlotCountFn active_slot_count;
    PeakCudaPauseFn pause_capture_begin;
    PeakCudaBoolFn capture_begin_waiting;
    PeakCudaPauseFn pause_cuda_section;
    PeakCudaWaitingCountFn cuda_sections_waiting;
    PeakCudaBoolFn capture_blocked;
    PeakCudaFinalizeFn finalize;
    PeakCudaPauseFn pause_lifecycle_before_increment;
    PeakCudaWaitingCountFn lifecycle_before_increment_waiting;
    PeakCudaPauseFn pause_lifecycle_after_admission;
    PeakCudaWaitingCountFn lifecycle_after_admission_waiting;
    PeakCudaBoolFn lifecycle_closing;
};

__global__ void
peak_cuda_capture_topology_kernel(int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1);
    }
}

__global__ void
peak_cuda_driver_capture_kernel(int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1);
    }
}

__global__ void
peak_cuda_runtime_pending_capture_marker()
{
}

__global__ void
peak_cuda_capture_race_target(int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1);
    }
}

__global__ void
peak_cuda_capture_race_control(int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1);
    }
}

PeakCudaCaptureTestSeams
load_test_seams()
{
    PeakCudaCaptureTestSeams seams = {};
    seams.force_incomplete = reinterpret_cast<PeakCudaForceIncompleteFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_force_incomplete_events"));
    seams.harvester_query_count =
        reinterpret_cast<PeakCudaHarvesterQueryCountFn>(
            dlsym(RTLD_DEFAULT,
                  "peak_cuda_test_harvester_query_count"));
    seams.harvester_ready = reinterpret_cast<PeakCudaHarvesterReadyFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_harvester_ready"));
    seams.active_slot_count = reinterpret_cast<PeakCudaActiveSlotCountFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_active_slot_count"));
    seams.pause_capture_begin = reinterpret_cast<PeakCudaPauseFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_pause_capture_begin"));
    seams.capture_begin_waiting = reinterpret_cast<PeakCudaBoolFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_capture_begin_waiting"));
    seams.pause_cuda_section = reinterpret_cast<PeakCudaPauseFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_pause_cuda_section"));
    seams.cuda_sections_waiting =
        reinterpret_cast<PeakCudaWaitingCountFn>(
            dlsym(RTLD_DEFAULT,
                  "peak_cuda_test_cuda_sections_waiting"));
    seams.capture_blocked = reinterpret_cast<PeakCudaBoolFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_capture_blocked"));
    seams.finalize = reinterpret_cast<PeakCudaFinalizeFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_finalize"));
    seams.pause_lifecycle_before_increment =
        reinterpret_cast<PeakCudaPauseFn>(
            dlsym(RTLD_DEFAULT,
                  "peak_cuda_test_pause_lifecycle_before_increment"));
    seams.lifecycle_before_increment_waiting =
        reinterpret_cast<PeakCudaWaitingCountFn>(
            dlsym(RTLD_DEFAULT,
                  "peak_cuda_test_lifecycle_before_increment_waiting"));
    seams.pause_lifecycle_after_admission =
        reinterpret_cast<PeakCudaPauseFn>(
            dlsym(RTLD_DEFAULT,
                  "peak_cuda_test_pause_lifecycle_after_admission"));
    seams.lifecycle_after_admission_waiting =
        reinterpret_cast<PeakCudaWaitingCountFn>(
            dlsym(RTLD_DEFAULT,
                  "peak_cuda_test_lifecycle_after_admission_waiting"));
    seams.lifecycle_closing = reinterpret_cast<PeakCudaBoolFn>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_lifecycle_closing"));
    return seams;
}

bool
wait_for_harvester_quiescence(const PeakCudaCaptureTestSeams& seams,
                              unsigned long long* query_count)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    unsigned long long previous = seams.harvester_query_count();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        unsigned long long current = seams.harvester_query_count();
        if (current == previous) {
            *query_count = current;
            return true;
        }
        previous = current;
    }
    return false;
}

bool
wait_for_harvester_query(const PeakCudaCaptureTestSeams& seams,
                         unsigned long long previous)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.harvester_query_count() > previous) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
wait_for_harvester_idle(const PeakCudaCaptureTestSeams& seams)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.active_slot_count() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
wait_for_active_slots(const PeakCudaCaptureTestSeams& seams,
                      unsigned long long minimum)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.active_slot_count() >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
harvester_queries_stay_at(const PeakCudaCaptureTestSeams& seams,
                          unsigned long long expected)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(25);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.harvester_query_count() != expected) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return seams.harvester_query_count() == expected;
}

bool
initialize_harvester(const PeakCudaCaptureTestSeams& seams,
                     cudaStream_t stream,
                     int* warmup_attempts)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (seams.harvester_ready() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        peak_cuda_runtime_pending_capture_marker<<<1, 1, 0, stream>>>();
        if (!peak_cuda_test_check(cudaPeekAtLastError(),
                                  "harvester initialization launch") ||
            !peak_cuda_test_check(cudaStreamSynchronize(stream),
                                  "harvester initialization synchronize")) {
            return false;
        }
        ++*warmup_attempts;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (seams.harvester_ready() == 0) {
        return false;
    }

    unsigned long long prior_queries = seams.harvester_query_count();
    peak_cuda_runtime_pending_capture_marker<<<1, 1, 0, stream>>>();
    return peak_cuda_test_check(cudaPeekAtLastError(),
                                "ready harvester marker launch") &&
           peak_cuda_test_check(cudaStreamSynchronize(stream),
                                "ready harvester marker synchronize") &&
           wait_for_harvester_query(seams, prior_queries) &&
           wait_for_harvester_idle(seams);
}

bool
check_driver(CUresult result, const char* operation)
{
    if (result == CUDA_SUCCESS) {
        return true;
    }
    const char* name = nullptr;
    (void)cuGetErrorName(result, &name);
    std::fprintf(stderr, "cuda_test_error: %s failed: %s\n", operation,
                 name != nullptr ? name : "unknown CUDA Driver error");
    return false;
}

template <typename Predicate>
bool
wait_for_condition(Predicate predicate, const char* operation)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "cuda_test_error: timed out waiting for %s\n",
                 operation);
    return false;
}

bool
execute_single_node_graph(cudaGraph_t graph, cudaStream_t stream,
                          const char* operation)
{
    size_t node_count = 0;
    cudaGraphExec_t executable = nullptr;
    if (!peak_cuda_test_check(cudaGraphGetNodes(graph, nullptr, &node_count),
                              "race cudaGraphGetNodes") ||
        node_count != 1) {
        std::fprintf(stderr,
                     "cuda_test_error: %s graph has %zu nodes, expected 1\n",
                     operation, node_count);
        return false;
    }
    if (!peak_cuda_test_check(
            cudaGraphInstantiate(&executable, graph, nullptr, nullptr, 0),
            "race cudaGraphInstantiate") ||
        !peak_cuda_test_check(cudaGraphLaunch(executable, stream),
                              "race cudaGraphLaunch") ||
        !peak_cuda_test_check(cudaStreamSynchronize(stream),
                              "race cudaStreamSynchronize")) {
        if (executable != nullptr) {
            (void)cudaGraphExecDestroy(executable);
        }
        return false;
    }
    (void)cudaGraphExecDestroy(executable);
    return true;
}

bool
run_begin_first_race(const PeakCudaCaptureTestSeams& seams,
                     cudaStream_t stream, int* target_value,
                     int* control_value)
{
    std::atomic<int> begin_result{static_cast<int>(cudaErrorNotReady)};
    std::atomic<int> launch_result{static_cast<int>(cudaErrorNotReady)};
    std::atomic<int> end_result{static_cast<int>(cudaErrorNotReady)};
    cudaGraph_t graph = nullptr;

    seams.pause_capture_begin(1);
    std::thread capture_thread([&]() {
        cudaError_t result = cudaSetDevice(0);
        if (result == cudaSuccess) {
            result = cudaStreamBeginCapture(stream,
                                            cudaStreamCaptureModeGlobal);
        }
        begin_result.store(static_cast<int>(result),
                           std::memory_order_release);
        if (result == cudaSuccess) {
            peak_cuda_capture_race_control<<<1, 1, 0, stream>>>(
                control_value);
            cudaError_t launch = cudaPeekAtLastError();
            launch_result.store(static_cast<int>(launch),
                                std::memory_order_release);
            cudaError_t end = cudaStreamEndCapture(stream, &graph);
            end_result.store(static_cast<int>(end),
                             std::memory_order_release);
        }
    });

    bool waiting = wait_for_condition(
        [&]() {
            return seams.capture_begin_waiting() != 0 &&
                   seams.capture_blocked() != 0;
        },
        "paused capture begin");
    bool target_ok = false;
    if (waiting) {
        peak_cuda_capture_race_target<<<1, 1, 0, stream>>>(target_value);
        target_ok = peak_cuda_test_check(cudaPeekAtLastError(),
                                         "begin-first target launch") &&
                    peak_cuda_test_check(cudaStreamSynchronize(stream),
                                         "begin-first target synchronize");
    }
    seams.pause_capture_begin(0);
    capture_thread.join();

    bool capture_ok =
        begin_result.load(std::memory_order_acquire) == cudaSuccess &&
        launch_result.load(std::memory_order_acquire) == cudaSuccess &&
        end_result.load(std::memory_order_acquire) == cudaSuccess &&
        graph != nullptr;
    bool graph_ok = capture_ok &&
                    execute_single_node_graph(graph, stream, "begin-first");
    bool graph_drained = graph_ok && wait_for_harvester_idle(seams);
    if (graph != nullptr) {
        (void)cudaGraphDestroy(graph);
    }
    return waiting && target_ok && graph_drained;
}

bool
run_reader_first_race(const PeakCudaCaptureTestSeams& seams,
                      cudaStream_t stream, int* target_value,
                      int* control_value, int* helper_quiesced,
                      int* helper_recycled)
{
    std::atomic<bool> worker_ready{false};
    std::atomic<bool> begin_ready{false};
    std::atomic<bool> start_worker{false};
    std::atomic<bool> start_begin{false};
    std::atomic<bool> capture_active{false};
    std::atomic<bool> allow_end{false};
    std::atomic<int> worker_result{static_cast<int>(cudaErrorNotReady)};
    std::atomic<int> begin_result{static_cast<int>(cudaErrorNotReady)};
    std::atomic<int> end_result{static_cast<int>(cudaErrorNotReady)};
    cudaGraph_t graph = nullptr;

    std::thread worker([&]() {
        cudaError_t result = cudaSetDevice(0);
        worker_ready.store(true, std::memory_order_release);
        while (!start_worker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (result == cudaSuccess) {
            peak_cuda_capture_race_target<<<1, 1, 0, stream>>>(target_value);
            result = cudaPeekAtLastError();
        }
        worker_result.store(static_cast<int>(result),
                            std::memory_order_release);
    });
    std::thread capture_thread([&]() {
        cudaError_t result = cudaSetDevice(0);
        begin_ready.store(true, std::memory_order_release);
        while (!start_begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (result == cudaSuccess) {
            result = cudaStreamBeginCapture(stream,
                                            cudaStreamCaptureModeGlobal);
        }
        begin_result.store(static_cast<int>(result),
                           std::memory_order_release);
        if (result == cudaSuccess) {
            capture_active.store(true, std::memory_order_release);
            while (!allow_end.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            peak_cuda_capture_race_control<<<1, 1, 0, stream>>>(
                control_value);
            cudaError_t launch = cudaPeekAtLastError();
            cudaError_t end = cudaStreamEndCapture(stream, &graph);
            end_result.store(static_cast<int>(
                                 launch == cudaSuccess ? end : launch),
                             std::memory_order_release);
        }
    });

    bool setup_ready = wait_for_condition(
        [&]() {
            return worker_ready.load(std::memory_order_acquire) &&
                   begin_ready.load(std::memory_order_acquire);
        },
        "race worker setup");
    seams.force_incomplete(1);
    seams.pause_cuda_section(1);
    seams.pause_capture_begin(1);
    start_worker.store(true, std::memory_order_release);
    bool reader_waiting = setup_ready && wait_for_condition(
        [&]() { return seams.cuda_sections_waiting() > 0; },
        "paused CUDA timing section");
    start_begin.store(true, std::memory_order_release);
    bool writer_waiting = reader_waiting && wait_for_condition(
        [&]() {
            return seams.capture_blocked() != 0 &&
                   seams.capture_begin_waiting() == 0;
        },
        "capture begin waiting for CUDA section");

    seams.pause_cuda_section(0);
    bool writer_drained = writer_waiting && wait_for_condition(
        [&]() { return seams.capture_begin_waiting() != 0; },
        "capture begin after CUDA section drain");
    worker.join();
    bool pending = writer_drained &&
                   worker_result.load(std::memory_order_acquire) ==
                       cudaSuccess &&
                   wait_for_active_slots(seams, 1);
    unsigned long long pending_queries = seams.harvester_query_count();

    seams.pause_capture_begin(0);
    bool began = pending && wait_for_condition(
        [&]() { return capture_active.load(std::memory_order_acquire); },
        "reader-first active capture");
    seams.force_incomplete(0);
    bool quiesced = began && seams.capture_blocked() != 0 &&
                    harvester_queries_stay_at(seams, pending_queries) &&
                    seams.active_slot_count() > 0;
    if (quiesced) {
        *helper_quiesced = 1;
    }
    allow_end.store(true, std::memory_order_release);
    capture_thread.join();
    bool recycled = quiesced &&
                    wait_for_harvester_query(seams, pending_queries) &&
                    wait_for_harvester_idle(seams);
    if (recycled) {
        *helper_recycled = 1;
    }

    bool capture_ok =
        begin_result.load(std::memory_order_acquire) == cudaSuccess &&
        end_result.load(std::memory_order_acquire) == cudaSuccess &&
        graph != nullptr;
    bool graph_ok = capture_ok && execute_single_node_graph(
                                      graph, stream, "reader-first");
    if (graph != nullptr) {
        (void)cudaGraphDestroy(graph);
    }
    seams.force_incomplete(0);
    seams.pause_cuda_section(0);
    seams.pause_capture_begin(0);
    return setup_ready && reader_waiting && writer_waiting &&
           writer_drained && pending && began && recycled && graph_ok;
}

bool
run_global_cross_stream_cycles(const PeakCudaCaptureTestSeams& seams,
                               cudaStream_t capture_stream,
                               cudaStream_t target_stream,
                               CUfunction target_function,
                               int* target_value, int* control_value,
                               int* node_count_total)
{
    constexpr int kRaceCycles = 32;
    std::atomic<bool> worker_ready{false};
    std::atomic<bool> stop{false};
    std::atomic<int> requested{0};
    std::atomic<int> completed{0};
    std::atomic<int> worker_result{static_cast<int>(CUDA_SUCCESS)};
    std::atomic<bool> context_unchanged{false};
    CUdeviceptr target_pointer = reinterpret_cast<CUdeviceptr>(target_value);

    std::thread worker([&]() {
        CUcontext before = nullptr;
        CUcontext after = nullptr;
        cudaError_t runtime_result = cudaSetDevice(0);
        CUresult result = runtime_result == cudaSuccess
                              ? cuCtxGetCurrent(&before)
                              : CUDA_ERROR_INVALID_CONTEXT;
        worker_result.store(static_cast<int>(result),
                            std::memory_order_release);
        worker_ready.store(true, std::memory_order_release);
        int handled = 0;
        while (!stop.load(std::memory_order_acquire)) {
            int next = requested.load(std::memory_order_acquire);
            if (next <= handled) {
                std::this_thread::yield();
                continue;
            }
            void* arguments[] = {&target_pointer};
            result = cuLaunchKernel(
                target_function, 1, 1, 1, 1, 1, 1, 0,
                reinterpret_cast<CUstream>(target_stream), arguments,
                nullptr);
            if (result != CUDA_SUCCESS) {
                worker_result.store(static_cast<int>(result),
                                    std::memory_order_release);
            }
            handled = next;
            completed.store(handled, std::memory_order_release);
        }
        if (cuCtxGetCurrent(&after) == CUDA_SUCCESS && before == after &&
            before != nullptr) {
            context_unchanged.store(true, std::memory_order_release);
        }
    });

    bool ok = wait_for_condition(
        [&]() { return worker_ready.load(std::memory_order_acquire); },
        "cross-stream worker setup");
    for (int cycle = 0; ok && cycle < kRaceCycles; ++cycle) {
        cudaGraph_t graph = nullptr;
        cudaError_t begin_result = cudaStreamBeginCapture(
            capture_stream, cudaStreamCaptureModeGlobal);
        cudaError_t control_result = begin_result;
        if (begin_result == cudaSuccess) {
            peak_cuda_capture_race_control<<<1, 1, 0, capture_stream>>>(
                control_value);
            control_result = cudaPeekAtLastError();
        }
        bool gate_active = begin_result == cudaSuccess &&
                           seams.capture_blocked() != 0;
        requested.store(cycle + 1, std::memory_order_release);
        bool launched = wait_for_condition(
            [&]() {
                return completed.load(std::memory_order_acquire) >= cycle + 1;
            },
            "cross-stream target launch");
        cudaError_t end_result = begin_result;
        if (begin_result == cudaSuccess) {
            end_result = cudaStreamEndCapture(capture_stream, &graph);
        }
        ok = gate_active && launched && begin_result == cudaSuccess &&
             control_result == cudaSuccess && end_result == cudaSuccess &&
             graph != nullptr &&
             worker_result.load(std::memory_order_acquire) == CUDA_SUCCESS;
        if (ok) {
            size_t nodes = 0;
            ok = peak_cuda_test_check(
                     cudaGraphGetNodes(graph, nullptr, &nodes),
                     "cross-stream cudaGraphGetNodes") &&
                 nodes == 1;
            if (ok) {
                *node_count_total += static_cast<int>(nodes);
                ok = execute_single_node_graph(
                    graph, capture_stream, "cross-stream");
            }
        }
        if (graph != nullptr) {
            (void)cudaGraphDestroy(graph);
        }
        if (ok) {
            ok = peak_cuda_test_check(cudaStreamSynchronize(target_stream),
                                      "cross-stream target synchronize");
        }
        if (ok) {
            ok = wait_for_harvester_idle(seams);
        }
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    return ok && context_unchanged.load(std::memory_order_acquire) &&
           seams.capture_blocked() == 0;
}

int
run_lifecycle_gate_race(bool reader_wins)
{
    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }
    PeakCudaCaptureTestSeams seams = load_test_seams();
    int seam_count =
        (seams.finalize != nullptr ? 1 : 0) +
        (seams.pause_lifecycle_before_increment != nullptr ? 1 : 0) +
        (seams.lifecycle_before_increment_waiting != nullptr ? 1 : 0) +
        (seams.pause_lifecycle_after_admission != nullptr ? 1 : 0) +
        (seams.lifecycle_after_admission_waiting != nullptr ? 1 : 0) +
        (seams.lifecycle_closing != nullptr ? 1 : 0) +
        (seams.pause_cuda_section != nullptr ? 1 : 0) +
        (seams.cuda_sections_waiting != nullptr ? 1 : 0) +
        (seams.capture_blocked != nullptr ? 1 : 0);
    if (seam_count != 9) {
        std::fprintf(stderr,
                     "cuda_test_error: lifecycle race mode requires all "
                     "PEAK CUDA lifecycle seams\n");
        return 1;
    }

    cudaStream_t timing_stream = nullptr;
    cudaStream_t capture_stream = nullptr;
    int* device_value = nullptr;
    if (!peak_cuda_test_check(cudaSetDevice(0),
                              "lifecycle cudaSetDevice") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &timing_stream, cudaStreamNonBlocking),
                              "lifecycle timing stream create") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &capture_stream, cudaStreamNonBlocking),
                              "lifecycle capture stream create") ||
        !peak_cuda_test_check(cudaMalloc(&device_value, sizeof(int)),
                              "lifecycle value allocation") ||
        !peak_cuda_test_check(cudaMemset(device_value, 0, sizeof(int)),
                              "lifecycle value memset")) {
        return 1;
    }

    std::atomic<bool> worker_ready{false};
    std::atomic<bool> start_worker{false};
    std::atomic<bool> finalize_done{false};
    std::atomic<int> worker_result{static_cast<int>(cudaErrorNotReady)};
    std::thread worker([&]() {
        cudaError_t result = cudaSetDevice(0);
        worker_ready.store(true, std::memory_order_release);
        while (!start_worker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (result == cudaSuccess) {
            peak_cuda_capture_race_target<<<1, 1, 0, timing_stream>>>(
                device_value);
            result = cudaPeekAtLastError();
        }
        worker_result.store(static_cast<int>(result),
                            std::memory_order_release);
    });

    bool setup_ready = wait_for_condition(
        [&]() { return worker_ready.load(std::memory_order_acquire); },
        "lifecycle worker setup");
    if (reader_wins) {
        seams.pause_lifecycle_after_admission(1);
        seams.pause_cuda_section(1);
    } else {
        seams.pause_lifecycle_before_increment(1);
    }
    start_worker.store(true, std::memory_order_release);
    bool reader_paused = setup_ready && wait_for_condition(
        [&]() {
            return reader_wins
                       ? seams.lifecycle_after_admission_waiting() != 0
                       : seams.lifecycle_before_increment_waiting() != 0;
        },
        reader_wins ? "admitted lifecycle reader"
                    : "pre-increment lifecycle reader");

    std::thread finalizer([&]() {
        seams.finalize();
        finalize_done.store(true, std::memory_order_release);
    });
    bool closing = reader_paused && wait_for_condition(
        [&]() { return seams.lifecycle_closing() != 0; },
        "lifecycle closing publication");
    bool ordering_proved = false;
    bool timing_reader_paused = !reader_wins;
    bool capture_waited_for_reader = !reader_wins;
    std::atomic<bool> capture_begin_done{false};
    std::atomic<int> capture_begin_result{
        static_cast<int>(cudaErrorNotReady)};
    std::thread capture_begin_thread;
    bool capture_begin_started = false;
    if (reader_wins) {
        seams.pause_lifecycle_after_admission(0);
        timing_reader_paused = closing && wait_for_condition(
            [&]() { return seams.cuda_sections_waiting() != 0; },
            "admitted CUDA timing reader");
        if (timing_reader_paused) {
            capture_begin_started = true;
            capture_begin_thread = std::thread([&]() {
                cudaError_t result = cudaSetDevice(0);
                if (result == cudaSuccess) {
                    result = cudaStreamBeginCapture(
                        capture_stream, cudaStreamCaptureModeRelaxed);
                }
                capture_begin_result.store(static_cast<int>(result),
                                           std::memory_order_release);
                capture_begin_done.store(true, std::memory_order_release);
            });
            bool transition_blocked = wait_for_condition(
                [&]() { return seams.capture_blocked() != 0; },
                "closed lifecycle capture transition");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            capture_waited_for_reader = transition_blocked &&
                !capture_begin_done.load(std::memory_order_acquire);
        }
        ordering_proved = closing && timing_reader_paused &&
            capture_waited_for_reader &&
            !finalize_done.load(std::memory_order_acquire);
        seams.pause_cuda_section(0);
    } else {
        ordering_proved = closing && wait_for_condition(
            [&]() { return finalize_done.load(std::memory_order_acquire); },
            "closer completion before pre-increment reader");
        seams.pause_lifecycle_before_increment(0);
    }
    seams.pause_cuda_section(0);
    seams.pause_lifecycle_after_admission(0);
    seams.pause_lifecycle_before_increment(0);
    worker.join();
    if (capture_begin_thread.joinable()) {
        capture_begin_thread.join();
    }
    finalizer.join();

    bool target_ok = worker_result.load(std::memory_order_acquire) ==
        cudaSuccess;
    if (!capture_begin_started) {
        target_ok = target_ok && peak_cuda_test_check(
            cudaStreamSynchronize(timing_stream),
            "lifecycle target synchronize");
    }
    cudaGraph_t graph = nullptr;
    int node_count = 0;
    bool listener_ok = target_ok && seams.lifecycle_closing() != 0;
    bool fail_closed_transition = false;
    if (listener_ok) {
        size_t nodes = 0;
        if (capture_begin_started) {
            listener_ok = capture_begin_done.load(std::memory_order_acquire) &&
                capture_begin_result.load(std::memory_order_acquire) ==
                    cudaSuccess;
        } else {
            listener_ok = seams.capture_blocked() == 0 &&
                peak_cuda_test_check(
                cudaStreamBeginCapture(capture_stream,
                                       cudaStreamCaptureModeGlobal),
                "closed lifecycle cudaStreamBeginCapture");
        }
        if (listener_ok) {
            peak_cuda_capture_race_control<<<1, 1, 0, capture_stream>>>(
                device_value);
            listener_ok = peak_cuda_test_check(
                cudaPeekAtLastError(), "closed lifecycle captured launch");
        }
        if (listener_ok) {
            listener_ok = peak_cuda_test_check(
                cudaStreamEndCapture(capture_stream, &graph),
                "closed lifecycle cudaStreamEndCapture");
        }
        if (listener_ok) {
            listener_ok = peak_cuda_test_check(
                              cudaGraphGetNodes(graph, nullptr, &nodes),
                              "closed lifecycle cudaGraphGetNodes") &&
                          nodes == 1;
            node_count = static_cast<int>(nodes);
        }
        if (listener_ok) {
            listener_ok = execute_single_node_graph(
                graph, capture_stream, "closed-lifecycle");
        }
        listener_ok = listener_ok && peak_cuda_test_check(
            cudaStreamSynchronize(timing_stream),
            "lifecycle timing stream synchronize");
        fail_closed_transition = seams.capture_blocked() != 0;
        listener_ok = listener_ok && fail_closed_transition;
    }

    int host_value = 0;
    bool result_ok = target_ok && listener_ok &&
        peak_cuda_test_check(cudaMemcpy(
            &host_value, device_value, sizeof(host_value),
            cudaMemcpyDeviceToHost), "lifecycle result copy");
    if (graph != nullptr) {
        (void)cudaGraphDestroy(graph);
    }
    (void)cudaFree(device_value);
    (void)cudaStreamDestroy(capture_stream);
    (void)cudaStreamDestroy(timing_stream);

    int expected_value = 2;
    if (!reader_paused || !closing || !ordering_proved || !result_ok ||
        host_value != expected_value || node_count != 1) {
        std::fprintf(stderr,
                     "cuda_test_error: lifecycle race failed: mode=%s "
                     "paused=%d closing=%d ordering=%d listener=%d "
                     "timing_reader_paused=%d capture_waited=%d "
                     "fail_closed_transition=%d nodes=%d result=%d "
                     "expected=%d\n",
                     reader_wins ? "reader-wins" : "closer-wins",
                     reader_paused ? 1 : 0, closing ? 1 : 0,
                     ordering_proved ? 1 : 0, listener_ok ? 1 : 0,
                     timing_reader_paused ? 1 : 0,
                     capture_waited_for_reader ? 1 : 0,
                     fail_closed_transition ? 1 : 0,
                     node_count, host_value, expected_value);
        return 1;
    }

    if (reader_wins) {
        std::printf("cuda_lifecycle_reader_wins_ok admitted_before_close=1 "
                    "closer_waited=1 capture_waited_for_reader=1 "
                    "fail_closed_transition=1 nodes=%d result=%d\n",
                    node_count, host_value);
    } else {
        std::printf("cuda_lifecycle_closer_wins_ok close_before_increment=1 "
                    "finalize_before_release=1 listener_rejected=1 "
                    "fail_closed_transition=1 nodes=%d result=%d\n",
                    node_count, host_value);
    }
    return 0;
}

int
run_capture_races()
{
    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }
    PeakCudaCaptureTestSeams seams = load_test_seams();
    int seam_count =
        (seams.force_incomplete != nullptr ? 1 : 0) +
        (seams.harvester_query_count != nullptr ? 1 : 0) +
        (seams.harvester_ready != nullptr ? 1 : 0) +
        (seams.active_slot_count != nullptr ? 1 : 0) +
        (seams.pause_capture_begin != nullptr ? 1 : 0) +
        (seams.capture_begin_waiting != nullptr ? 1 : 0) +
        (seams.pause_cuda_section != nullptr ? 1 : 0) +
        (seams.cuda_sections_waiting != nullptr ? 1 : 0) +
        (seams.capture_blocked != nullptr ? 1 : 0);
    if (seam_count != 9) {
        std::fprintf(stderr,
                     "cuda_test_error: capture race mode requires all PEAK "
                     "CUDA test seams\n");
        return 1;
    }

    cudaStream_t stream_a = nullptr;
    cudaStream_t stream_b = nullptr;
    int* begin_target = nullptr;
    int* begin_control = nullptr;
    int* reader_target = nullptr;
    int* reader_control = nullptr;
    int* global_target = nullptr;
    int* global_control = nullptr;
    if (!peak_cuda_test_check(cudaSetDevice(0), "race cudaSetDevice") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &stream_a, cudaStreamNonBlocking),
                              "race stream A create") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &stream_b, cudaStreamNonBlocking),
                              "race stream B create") ||
        !peak_cuda_test_check(cudaMalloc(&begin_target, sizeof(int)),
                              "race begin target allocation") ||
        !peak_cuda_test_check(cudaMalloc(&begin_control, sizeof(int)),
                              "race begin control allocation") ||
        !peak_cuda_test_check(cudaMalloc(&reader_target, sizeof(int)),
                              "race reader target allocation") ||
        !peak_cuda_test_check(cudaMalloc(&reader_control, sizeof(int)),
                              "race reader control allocation") ||
        !peak_cuda_test_check(cudaMalloc(&global_target, sizeof(int)),
                              "race global target allocation") ||
        !peak_cuda_test_check(cudaMalloc(&global_control, sizeof(int)),
                              "race global control allocation") ||
        !peak_cuda_test_check(cudaMemset(begin_target, 0, sizeof(int)),
                              "race begin target memset") ||
        !peak_cuda_test_check(cudaMemset(begin_control, 0, sizeof(int)),
                              "race begin control memset") ||
        !peak_cuda_test_check(cudaMemset(reader_target, 0, sizeof(int)),
                              "race reader target memset") ||
        !peak_cuda_test_check(cudaMemset(reader_control, 0, sizeof(int)),
                              "race reader control memset") ||
        !peak_cuda_test_check(cudaMemset(global_target, 0, sizeof(int)),
                              "race global target memset") ||
        !peak_cuda_test_check(cudaMemset(global_control, 0, sizeof(int)),
                              "race global control memset")) {
        return 1;
    }

    int initialization_warmups = 0;
    if (!initialize_harvester(seams, stream_a, &initialization_warmups)) {
        std::fprintf(stderr,
                     "cuda_test_error: CUDA harvester did not become ready "
                     "for capture races\n");
        return 1;
    }
    bool begin_ok = run_begin_first_race(
        seams, stream_a, begin_target, begin_control);
    int reader_quiesced = 0;
    int reader_recycled = 0;
    bool reader_ok = begin_ok && run_reader_first_race(
        seams, stream_a, reader_target, reader_control,
        &reader_quiesced, &reader_recycled);

    cudaFunction_t runtime_function = nullptr;
    bool function_ok = reader_ok && peak_cuda_test_check(
        cudaGetFuncBySymbol(
            &runtime_function,
            reinterpret_cast<const void*>(peak_cuda_capture_race_target)),
        "race cudaGetFuncBySymbol");
    int global_nodes = 0;
    bool global_ok = function_ok && run_global_cross_stream_cycles(
        seams, stream_a, stream_b,
        reinterpret_cast<CUfunction>(runtime_function), global_target,
        global_control, &global_nodes);

    int begin_target_value = 0;
    int begin_control_value = 0;
    int reader_target_value = 0;
    int reader_control_value = 0;
    int global_target_value = 0;
    int global_control_value = 0;
    bool copies_ok = global_ok &&
        peak_cuda_test_check(cudaMemcpy(
            &begin_target_value, begin_target, sizeof(int),
            cudaMemcpyDeviceToHost), "race begin target copy") &&
        peak_cuda_test_check(cudaMemcpy(
            &begin_control_value, begin_control, sizeof(int),
            cudaMemcpyDeviceToHost), "race begin control copy") &&
        peak_cuda_test_check(cudaMemcpy(
            &reader_target_value, reader_target, sizeof(int),
            cudaMemcpyDeviceToHost), "race reader target copy") &&
        peak_cuda_test_check(cudaMemcpy(
            &reader_control_value, reader_control, sizeof(int),
            cudaMemcpyDeviceToHost), "race reader control copy") &&
        peak_cuda_test_check(cudaMemcpy(
            &global_target_value, global_target, sizeof(int),
            cudaMemcpyDeviceToHost), "race global target copy") &&
        peak_cuda_test_check(cudaMemcpy(
            &global_control_value, global_control, sizeof(int),
            cudaMemcpyDeviceToHost), "race global control copy");

    (void)cudaFree(global_control);
    (void)cudaFree(global_target);
    (void)cudaFree(reader_control);
    (void)cudaFree(reader_target);
    (void)cudaFree(begin_control);
    (void)cudaFree(begin_target);
    (void)cudaStreamDestroy(stream_b);
    (void)cudaStreamDestroy(stream_a);

    if (!copies_ok || begin_target_value != 1 || begin_control_value != 1 ||
        reader_target_value != 1 || reader_control_value != 1 ||
        global_target_value != 32 || global_control_value != 32 ||
        global_nodes != 32 || reader_quiesced != 1 ||
        reader_recycled != 1) {
        std::fprintf(stderr,
                     "cuda_test_error: capture race results are incomplete: "
                     "begin=%d/%d reader=%d/%d global=%d/%d nodes=%d "
                     "quiesced=%d recycled=%d\n",
                     begin_target_value, begin_control_value,
                     reader_target_value, reader_control_value,
                     global_target_value, global_control_value, global_nodes,
                     reader_quiesced, reader_recycled);
        return 1;
    }

    std::printf(
        "cuda_capture_races_ok begin_first_nodes=1 "
        "begin_first_results=%d/%d reader_first_nodes=1 "
        "reader_first_results=%d/%d reader_pending_quiesced=%d "
        "reader_pending_recycled=%d global_cycles=32 "
        "global_nodes_per_cycle=1 global_results=%d/%d "
        "initialization_warmups=%d\n",
        begin_target_value, begin_control_value,
        reader_target_value, reader_control_value,
        reader_quiesced, reader_recycled,
        global_target_value, global_control_value,
        initialization_warmups);
    return 0;
}
}

int
main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--capture-races") == 0) {
        return run_capture_races();
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--lifecycle-reader-wins") == 0) {
        return run_lifecycle_gate_race(true);
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--lifecycle-closer-wins") == 0) {
        return run_lifecycle_gate_race(false);
    }
    if (argc != 1) {
        return 2;
    }
    int requirement = peak_cuda_test_require_devices(1, nullptr);
    if (requirement != 0) {
        return requirement;
    }

    cudaStream_t stream = nullptr;
    int* device_value = nullptr;
    int runtime_value = 0;
    int driver_value = 0;
    std::vector<cudaGraph_t> runtime_graphs;
    std::vector<cudaGraphExec_t> runtime_executables;
    std::vector<CUgraph> driver_graphs;
    std::vector<CUgraphExec> driver_executables;
    PeakCudaCaptureTestSeams seams = load_test_seams();
    bool have_test_seams = seams.force_incomplete != nullptr &&
                           seams.harvester_query_count != nullptr &&
                           seams.harvester_ready != nullptr &&
                           seams.active_slot_count != nullptr;
    int runtime_helper_quiesced = 0;
    int runtime_helper_recycled = 0;
    int driver_helper_quiesced = 0;
    int driver_helper_recycled = 0;
    int initialization_warmups = 0;
    unsigned long long runtime_pending_queries = 0;
    unsigned long long driver_pending_queries = 0;
    int seam_count = (seams.force_incomplete != nullptr ? 1 : 0) +
                     (seams.harvester_query_count != nullptr ? 1 : 0) +
                     (seams.harvester_ready != nullptr ? 1 : 0) +
                     (seams.active_slot_count != nullptr ? 1 : 0);
    if (seam_count != 0 && seam_count != 4) {
        std::fprintf(stderr,
                     "cuda_test_error: incomplete PEAK CUDA test seam set\n");
        return 1;
    }
    int race_seam_count =
        (seams.pause_capture_begin != nullptr ? 1 : 0) +
        (seams.capture_begin_waiting != nullptr ? 1 : 0) +
        (seams.pause_cuda_section != nullptr ? 1 : 0) +
        (seams.cuda_sections_waiting != nullptr ? 1 : 0) +
        (seams.capture_blocked != nullptr ? 1 : 0);
    if (race_seam_count != 0 && race_seam_count != 5) {
        std::fprintf(stderr,
                     "cuda_test_error: incomplete PEAK CUDA race seam set\n");
        return 1;
    }
    runtime_graphs.reserve(kCaptureCycles);
    runtime_executables.reserve(kCaptureCycles);
    driver_graphs.reserve(kCaptureCycles);
    driver_executables.reserve(kCaptureCycles);
    if (!peak_cuda_test_check(cudaSetDevice(0), "cudaSetDevice") ||
        !peak_cuda_test_check(cudaStreamCreateWithFlags(
                                  &stream, cudaStreamNonBlocking),
                              "cudaStreamCreateWithFlags") ||
        !peak_cuda_test_check(cudaMalloc(&device_value, sizeof(*device_value)),
                              "cudaMalloc") ||
        !peak_cuda_test_check(cudaMemsetAsync(device_value, 0,
                                              sizeof(*device_value), stream),
                              "cudaMemsetAsync") ||
        !peak_cuda_test_check(cudaStreamSynchronize(stream),
                              "initial cudaStreamSynchronize")) {
        return 1;
    }

    if (have_test_seams) {
        if (!initialize_harvester(seams, stream,
                                  &initialization_warmups)) {
            std::fprintf(stderr,
                         "cuda_test_error: CUDA harvester did not become "
                         "ready\n");
            return 1;
        }
        seams.force_incomplete(1);
        peak_cuda_runtime_pending_capture_marker<<<1, 1, 0, stream>>>();
        if (!peak_cuda_test_check(cudaPeekAtLastError(),
                                  "runtime pending marker launch") ||
            !peak_cuda_test_check(cudaStreamSynchronize(stream),
                                  "runtime pending marker synchronize") ||
            !wait_for_active_slots(seams, 1) ||
            !wait_for_harvester_quiescence(
                seams, &runtime_pending_queries) ||
            !peak_cuda_test_check(
                cudaStreamBeginCapture(stream,
                                       cudaStreamCaptureModeGlobal),
                "runtime concurrent cudaStreamBeginCapture")) {
            seams.force_incomplete(0);
            return 1;
        }
        seams.force_incomplete(0);
        if (!harvester_queries_stay_at(seams, runtime_pending_queries) ||
            seams.active_slot_count() == 0) {
            cudaGraph_t ignored = nullptr;
            (void)cudaStreamEndCapture(stream, &ignored);
            std::fprintf(stderr,
                         "cuda_test_error: harvester was not quiescent "
                         "during Runtime capture\n");
            return 1;
        }
        runtime_helper_quiesced = 1;
    }

    for (int cycle = 0; cycle < kCaptureCycles; ++cycle) {
        cudaGraph_t graph = nullptr;
        cudaGraphExec_t executable = nullptr;
        size_t node_count = 0;

        if ((cycle != 0 || !have_test_seams) &&
            !peak_cuda_test_check(
                cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal),
                "cudaStreamBeginCapture")) {
            return 1;
        }
        peak_cuda_capture_topology_kernel<<<1, 1, 0, stream>>>(device_value);
        if (!peak_cuda_test_check(cudaPeekAtLastError(),
                                  "captured kernel launch") ||
            !peak_cuda_test_check(cudaStreamEndCapture(stream, &graph),
                                  "cudaStreamEndCapture") ||
            !peak_cuda_test_check(cudaGraphGetNodes(graph, nullptr,
                                                    &node_count),
                                  "cudaGraphGetNodes")) {
            return 1;
        }
        if (node_count != 1) {
            std::fprintf(stderr,
                         "cuda_test_error: capture cycle %d has %zu nodes, "
                         "expected 1\n",
                         cycle, node_count);
            return 1;
        }
        if (cycle == 0 && have_test_seams) {
            if (!wait_for_harvester_query(seams,
                                          runtime_pending_queries) ||
                !wait_for_harvester_idle(seams)) {
                std::fprintf(
                    stderr,
                    "cuda_test_error: Runtime pending event did not recycle "
                    "after capture ended\n");
                return 1;
            }
            runtime_helper_recycled = 1;
        }
        if (!peak_cuda_test_check(
                cudaGraphInstantiate(&executable, graph, nullptr, nullptr, 0),
                "cudaGraphInstantiate") ||
            !peak_cuda_test_check(cudaGraphLaunch(executable, stream),
                                  "cudaGraphLaunch") ||
            !peak_cuda_test_check(cudaStreamSynchronize(stream),
                                  "cudaStreamSynchronize")) {
            return 1;
        }
        runtime_executables.push_back(executable);
        runtime_graphs.push_back(graph);
    }

    if (!peak_cuda_test_check(cudaMemcpy(&runtime_value, device_value,
                                         sizeof(runtime_value),
                                         cudaMemcpyDeviceToHost),
                              "runtime cudaMemcpy") ||
        !peak_cuda_test_check(cudaMemset(device_value, 0,
                                         sizeof(*device_value)),
                              "driver setup cudaMemset")) {
        return 1;
    }
    if (runtime_value != kCaptureCycles) {
        std::fprintf(stderr,
                     "cuda_test_error: runtime captured graph result is %d, "
                     "expected %d\n",
                     runtime_value, kCaptureCycles);
        return 1;
    }

    cudaFunction_t runtime_function = nullptr;
    cudaFunction_t runtime_marker_function = nullptr;
    CUstream driver_stream = nullptr;
    if (!peak_cuda_test_check(
            cudaGetFuncBySymbol(
                &runtime_function,
                reinterpret_cast<const void*>(peak_cuda_driver_capture_kernel)),
            "cudaGetFuncBySymbol") ||
        !peak_cuda_test_check(
            cudaGetFuncBySymbol(
                &runtime_marker_function,
                reinterpret_cast<const void*>(
                    peak_cuda_runtime_pending_capture_marker)),
            "marker cudaGetFuncBySymbol") ||
        !check_driver(cuStreamCreate(&driver_stream, CU_STREAM_NON_BLOCKING),
                      "cuStreamCreate")) {
        return 1;
    }
    CUfunction driver_function =
        reinterpret_cast<CUfunction>(runtime_function);
    CUfunction driver_marker_function =
        reinterpret_cast<CUfunction>(runtime_marker_function);
    CUdeviceptr driver_pointer =
        reinterpret_cast<CUdeviceptr>(device_value);
    void* driver_arguments[] = {&driver_pointer};

    if (have_test_seams) {
        seams.force_incomplete(1);
        if (!check_driver(
                cuLaunchKernel(driver_marker_function, 1, 1, 1, 1, 1, 1,
                               0, driver_stream, nullptr, nullptr),
                "Driver pending marker launch") ||
            !check_driver(cuStreamSynchronize(driver_stream),
                          "Driver pending marker synchronize") ||
            !wait_for_active_slots(seams, 1) ||
            !wait_for_harvester_quiescence(
                seams, &driver_pending_queries) ||
            !check_driver(
                cuStreamBeginCapture(driver_stream,
                                     CU_STREAM_CAPTURE_MODE_GLOBAL),
                "Driver concurrent cuStreamBeginCapture")) {
            seams.force_incomplete(0);
            return 1;
        }
        seams.force_incomplete(0);
        if (!harvester_queries_stay_at(seams, driver_pending_queries) ||
            seams.active_slot_count() == 0) {
            CUgraph ignored = nullptr;
            (void)cuStreamEndCapture(driver_stream, &ignored);
            std::fprintf(stderr,
                         "cuda_test_error: harvester was not quiescent "
                         "during Driver capture\n");
            return 1;
        }
        driver_helper_quiesced = 1;
    }

    for (int cycle = 0; cycle < kCaptureCycles; ++cycle) {
        CUgraph graph = nullptr;
        CUgraphExec executable = nullptr;
        size_t node_count = 0;

        if ((cycle != 0 || !have_test_seams) &&
            !check_driver(
                cuStreamBeginCapture(driver_stream,
                                     CU_STREAM_CAPTURE_MODE_GLOBAL),
                "cuStreamBeginCapture")) {
            return 1;
        }
        if (!check_driver(cuLaunchKernel(driver_function, 1, 1, 1, 1, 1, 1,
                                         0, driver_stream, driver_arguments,
                                         nullptr),
                          "cuLaunchKernel") ||
            !check_driver(cuStreamEndCapture(driver_stream, &graph),
                          "cuStreamEndCapture") ||
            !check_driver(cuGraphGetNodes(graph, nullptr, &node_count),
                          "cuGraphGetNodes")) {
            return 1;
        }
        if (node_count != 1) {
            std::fprintf(stderr,
                         "cuda_test_error: driver capture cycle %d has %zu "
                         "nodes, expected 1\n",
                         cycle, node_count);
            return 1;
        }
        if (cycle == 0 && have_test_seams) {
            if (!wait_for_harvester_query(seams,
                                          driver_pending_queries) ||
                !wait_for_harvester_idle(seams)) {
                std::fprintf(
                    stderr,
                    "cuda_test_error: Driver pending event did not recycle "
                    "after capture ended\n");
                return 1;
            }
            driver_helper_recycled = 1;
        }
#if CUDA_VERSION >= 11040
        CUresult instantiate =
            cuGraphInstantiateWithFlags(&executable, graph, 0);
#else
        CUresult instantiate = cuGraphInstantiate(
            &executable, graph, nullptr, nullptr, 0);
#endif
        if (!check_driver(instantiate,
                          "cuGraphInstantiate") ||
            !check_driver(cuGraphLaunch(executable, driver_stream),
                          "cuGraphLaunch") ||
            !check_driver(cuStreamSynchronize(driver_stream),
                          "cuStreamSynchronize")) {
            return 1;
        }
        driver_executables.push_back(executable);
        driver_graphs.push_back(graph);
    }

    if (!check_driver(cuMemcpyDtoH(&driver_value, driver_pointer,
                                   sizeof(driver_value)),
                      "cuMemcpyDtoH")) {
        return 1;
    }
    for (CUgraphExec executable : driver_executables) {
        (void)cuGraphExecDestroy(executable);
    }
    for (CUgraph graph : driver_graphs) {
        (void)cuGraphDestroy(graph);
    }
    for (cudaGraphExec_t executable : runtime_executables) {
        (void)cudaGraphExecDestroy(executable);
    }
    for (cudaGraph_t graph : runtime_graphs) {
        (void)cudaGraphDestroy(graph);
    }
    (void)cuStreamDestroy(driver_stream);
    cudaFree(device_value);
    cudaStreamDestroy(stream);
    if (driver_value != kCaptureCycles) {
        std::fprintf(stderr,
                     "cuda_test_error: driver captured graph result is %d, "
                     "expected %d\n",
                     driver_value, kCaptureCycles);
        return 1;
    }

    std::printf("cuda_stream_capture_ok runtime_cycles=%d driver_cycles=%d "
                "live_graph_identities=%d nodes_per_cycle=1 "
                "runtime_result=%d driver_result=%d "
                "runtime_helper_quiesced=%d "
                "runtime_helper_recycled=%d "
                "driver_helper_quiesced=%d "
                "driver_helper_recycled=%d "
                "initialization_warmups=%d\n",
                kCaptureCycles, kCaptureCycles, kCaptureCycles * 2,
                runtime_value, driver_value,
                runtime_helper_quiesced,
                runtime_helper_recycled,
                driver_helper_quiesced,
                driver_helper_recycled,
                initialization_warmups);
    return 0;
}
