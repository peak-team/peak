#!/usr/bin/env python3

import pathlib
import re
import sys


CUDA_HOOKS = [
    "cudaLaunchKernel",
    "cudaLaunchCooperativeKernel",
    "cudaLaunchCooperativeKernelMultiDevice",
    "cudaLaunchKernelExC",
    "cuLaunchKernel",
    "cuLaunchCooperativeKernel",
    "cuLaunchCooperativeKernelMultiDevice",
    "cuLaunchKernelEx",
    "cudaGraphLaunch",
    "cuGraphLaunch",
]

CUDA_WRAPPERS = [
    "peak_cuda_launch_kernel",
    "peak_cuda_launch_cooperative_kernel",
    "peak_cuda_launch_cooperative_kernel_multiple_device",
    "peak_cuda_launch_kernel_exc",
    "peak_cu_launch_kernel",
    "peak_cu_launch_cooperative_kernel",
    "peak_cu_launch_cooperative_kernel_multiple_device",
    "peak_cu_launch_kernel_ex",
    "peak_cuda_graph_launch",
    "peak_cu_graph_launch",
]

TIMED_CUDA_WRAPPERS = {
    "peak_cuda_launch_kernel": "original_cuda_launch_kernel",
    "peak_cuda_launch_cooperative_kernel":
        "original_cuda_launch_cooperative_kernel",
    "peak_cuda_launch_kernel_exc": "original_cuda_launch_kernel_exc",
    "peak_cu_launch_kernel": "original_cu_launch_kernel",
    "peak_cu_launch_cooperative_kernel":
        "original_cu_launch_cooperative_kernel",
    "peak_cu_launch_kernel_ex": "original_cu_launch_kernel_ex",
    "peak_cuda_graph_launch": "original_cuda_graph_launch",
    "peak_cu_graph_launch": "original_cu_graph_launch",
}

MULTI_DEVICE_CUDA_WRAPPERS = {
    "peak_cuda_launch_cooperative_kernel_multiple_device":
        "original_cuda_launch_cooperative_kernel_multiple_device",
    "peak_cu_launch_cooperative_kernel_multiple_device":
        "original_cu_launch_cooperative_kernel_multiple_device",
}

GENERAL_LISTENER_CUDA_TARGETS = [
    "cudaLaunchKernel",
    "cudaLaunchCooperativeKernel",
    "cudaLaunchCooperativeKernelMultiDevice",
    "cudaLaunchKernelEx",
    "cudaLaunchKernelExC",
    "cuLaunchKernel",
    "cuLaunchCooperativeKernel",
    "cuLaunchCooperativeKernelMultiDevice",
    "cuLaunchKernelEx",
    "cudaGraphLaunch",
    "cuGraphLaunch",
]

GENERAL_LISTENER_CUDA_WRAPPER_LOOKUPS = [
    ("cudaLaunchKernel", "peak_cuda_launch_kernel"),
    ("cudaLaunchCooperativeKernel", "peak_cuda_launch_cooperative_kernel"),
    ("cudaLaunchCooperativeKernelMultiDevice",
     "peak_cuda_launch_cooperative_kernel_multiple_device"),
    ("cudaLaunchKernelEx", "peak_cuda_launch_kernel_exc"),
    ("cudaLaunchKernelExC", "peak_cuda_launch_kernel_exc"),
    ("cuLaunchKernel", "peak_cu_launch_kernel"),
    ("cuLaunchCooperativeKernel", "peak_cu_launch_cooperative_kernel"),
    ("cuLaunchCooperativeKernelMultiDevice",
     "peak_cu_launch_cooperative_kernel_multiple_device"),
    ("cuLaunchKernelEx", "peak_cu_launch_kernel_ex"),
    ("cudaGraphLaunch", "peak_cuda_graph_launch"),
    ("cuGraphLaunch", "peak_cu_graph_launch"),
]


def function_body(source, name):
    match = re.search(rf'\b{name}\s*\([^)]*\)\s*(?:const\s*)?\{{', source)
    if not match:
        raise AssertionError(f"missing function: {name}")

    depth = 0
    start = match.end() - 1
    for pos in range(start, len(source)):
        char = source[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]

    raise AssertionError(f"unterminated function: {name}")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def require_order(body, *needles):
    offset = -1
    for needle in needles:
        next_offset = body.find(needle, offset + 1)
        require(next_offset != -1, f"missing ordered token: {needle}")
        require(next_offset > offset, f"out-of-order token: {needle}")
        offset = next_offset


def read_general_listener_source(repo_root):
    return (repo_root / "src" / "general_listener.c").read_text(
        encoding="utf-8")


def main():
    if len(sys.argv) != 2:
        print("usage: check_cuda_interceptor_consistency.py <repo-root>",
              file=sys.stderr)
        return 2

    repo_root = pathlib.Path(sys.argv[1]).resolve()
    cuda = (repo_root / "src" / "cuda_interceptor.cpp").read_text(
        encoding="utf-8")
    general = read_general_listener_source(repo_root)
    cuda_header = (repo_root / "include" / "cuda_interceptor.h").read_text(
        encoding="utf-8")
    cuda_state_header = (
        repo_root / "include" / "internal" /
        "cuda_profiler_state.h").read_text(encoding="utf-8")
    unsafe_gum_header = (
        repo_root / "include" / "internal" /
        "unsafe_gum_prologue.h").read_text(encoding="utf-8")
    cuda_benchmark = (
        repo_root / "test" / "cuda" /
        "benchmark_launch_overhead.cu").read_text(encoding="utf-8")
    cuda_regressions = (
        repo_root / "test" / "cuda" /
        "run_cuda_regressions.py").read_text(encoding="utf-8")
    cuda_test_cmake = (
        repo_root / "test" / "cuda" /
        "CMakeLists.txt").read_text(encoding="utf-8")
    cuda_state = (repo_root / "src" / "cuda_profiler_state.cpp").read_text(
        encoding="utf-8")
    c_api_headers = [
        "include/internal/general_listener/report_maxima.h",
        "include/internal/general_listener/report_model.h",
        "include/internal/general_listener/report_snapshot.h",
        "include/internal/general_listener/socket_report_transport.h",
        "include/internal/general_listener/mpi_report_transport.h",
    ]
    support_sources = {
        "src/dlopen_interceptor.c": ["dlopen"],
        "src/mpi_interceptor.c": ["PMPI_Finalize"],
        "src/pthread_listener.c": ["pthread_create", "pthread_join"],
    }

    require("peak_resolve_function" not in general,
            "general listener must not use a loader-first resolver")
    require("peak_symbol_resolver" not in general,
            "general listener must not include the removed symbol resolver")
    require("peak_general_listener_find_function(peak_hook_strings[i])" in general,
            "generic target lookup must use the Frida-native helper")
    generic_lookup = function_body(general,
                                   "peak_general_listener_find_function")
    require(generic_lookup.count("gum_find_function(symbol)") == 2,
            "generic target lookup must keep Gum dynamic-binary resolution")
    require(generic_lookup.count("dlsym(RTLD_DEFAULT, symbol)") == 1,
            "generic target lookup must keep the Darwin loader fallback")
    darwin_fallback = re.search(
        r"#if defined\(__APPLE__\)(.*?)#else(.*?)#endif",
        generic_lookup,
        re.DOTALL)
    require(darwin_fallback is not None,
            "Darwin loader fallback must remain Apple-only")
    apple_lookup, non_apple_lookup = darwin_fallback.groups()
    require_order(apple_lookup,
                  "address = gum_find_function(symbol);",
                  "if (address == NULL)",
                  "address = dlsym(RTLD_DEFAULT, symbol);",
                  "return address;")
    require("dlsym(" not in non_apple_lookup and
            "RTLD_" not in non_apple_lookup and
            "return gum_find_function(symbol);" in non_apple_lookup,
            "non-Apple generic lookup must remain Frida-native")
    require("gum_module_find_global_export_by_name" not in generic_lookup,
            "generic target lookup must not switch MPI ranks to export-only resolution")
    for relpath, symbols in support_sources.items():
        source = (repo_root / relpath).read_text(encoding="utf-8")
        for symbol in symbols:
            require(f'peak_general_listener_find_function("{symbol}")' in source,
                    f"{relpath} must use Frida-native support lookup for {symbol}")
            require(f'gum_find_function("{symbol}")' not in source,
                    f"{relpath} must not broad-scan support hook {symbol}")

    peak = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    require(re.search(r"PEAK_API\s+void\s+exit\s*\(", peak) is not None,
            "ELF exit handling must use an ordinary exported interposer")
    require(re.search(r"PEAK_API\s+void\s+peak_exit\s*\(", peak) is not None,
            "the exit target must retain an exported profiling handler")
    require('dlsym(RTLD_NEXT, "exit")' in peak,
            "the exit interposer must resolve the real function with RTLD_NEXT")
    require('peak_general_listener_find_function("exit")' not in peak and
            'gum_find_function("exit")' not in peak,
            "exit handling must not install a Gum support hook")

    require('gum_find_function("' not in cuda,
            "CUDA support hooks must use the Frida-native lookup helper")
    require('gum_find_function("' not in general,
            "general listener special cases must use the Frida-native lookup helper")
    require('#include "general_listener.h"' not in cuda and
            'extern "C" gpointer peak_general_listener_find_function('
            'const char* symbol);' in cuda,
            "CUDA must avoid C-only general-listener declarations and keep its C lookup ABI")
    for relpath in c_api_headers:
        header = (repo_root / relpath).read_text(encoding="utf-8")
        require(re.search(r'#ifdef __cplusplus\s+extern "C" \{\s+#endif',
                          header) is not None and
                re.search(r'#ifdef __cplusplus\s+}\s+#endif\s*\n\s*#endif',
                          header) is not None,
                f"{relpath} must preserve C linkage for CUDA C++ callers")
    require(re.search(r'#ifdef __cplusplus\s+extern "C" \{\s+#endif',
                      unsafe_gum_header) is not None and
            re.search(r'#ifdef __cplusplus\s+}\s+#endif\s*\n\s*#endif',
                      unsafe_gum_header) is not None,
            "unsafe Gum APIs must preserve C linkage for CUDA C++ callers")
    require('dlsym(driver_handle, "cuLaunchKernel")' not in cuda_benchmark and
            "launched = cuLaunchKernel(" in cuda_benchmark and
            "benchmark_launch_overhead.cu\n"
            "        ${PEAK_CUDA_DRIVER_TEST_LIBRARY}" in cuda_test_cmake,
            "the Driver benchmark must measure the linked public API entry")
    require('"--native-events"' in cuda_benchmark and
            "cuEventRecord(reinterpret_cast<CUevent>(start_event)" in
            cuda_benchmark and
            "cuEventRecord(reinterpret_cast<CUevent>(end_event)" in
            cuda_benchmark and
            "target_event_floors" in cuda_regressions and
            "target_event_floor_throughputs" in cuda_regressions and
            "target_ratio = target_profile / target_event_floor" in
            cuda_regressions and
            "target_increment = target_profile - target_event_floor" in
            cuda_regressions and
            "return profile / target_event_floor_throughputs" in
            cuda_regressions and
            "return target_event_floors[(api, threads)]" in cuda_regressions,
            "target performance must isolate PEAK software latency and "
            "throughput scaling from the mandatory native event-timing floor "
            "using the replacement's Driver record API")
    cxx_link_test = (repo_root / "test" / "CMakeLists.txt").read_text(
        encoding="utf-8")
    require("test_report_snapshot_cxx_link" in cxx_link_test and
            "test_report_snapshot_cxx_link.cpp" in cxx_link_test,
            "C++ report-snapshot linkage must remain a compiled regression test")
    socket_test = (repo_root / "test" / "report" /
                   "test_socket_report_transport.c").read_text(encoding="utf-8")
    require("bool early_drop_may_skip_accept =" in socket_test and
            "action == TEST_GATHER_DROP_FAILURE ||" in socket_test and
            "action == TEST_GATHER_PAYLOAD_DROP_FAILURE;" in socket_test and
            "(!early_drop_may_skip_accept &&" in socket_test and
            "root_telemetry.root_max_active != 1U" in socket_test and
            "(early_drop_may_skip_accept &&" in socket_test and
            "root_telemetry.root_max_active > 1U" in socket_test,
            "only immediate gather-drop injections may skip root accept telemetry")
    require("socketpair(AF_UNIX, SOCK_STREAM, 0, receipt_barrier_fds)" in
            socket_test and
            "peak_socket_report_test_receipt_barrier_set(" in socket_test and
            "root_telemetry.root_receipt_failure_injected" in socket_test,
            "receipt-failure test must synchronize the precise injected phase")
    socket_transport = (
        repo_root / "src" / "general_listener" /
        "socket_report_transport.c").read_text(encoding="utf-8")
    socket_header = (
        repo_root / "include" / "internal" / "general_listener" /
        "socket_report_transport.h").read_text(encoding="utf-8")
    socket_test_hook_section = socket_header.split(
        "#ifdef PEAK_ENABLE_TEST_HOOKS", 1)[1].split("#endif", 1)[0]
    require("root_receipt_failure_injected" in socket_test_hook_section and
            "peak_socket_report_test_receipt_barrier_set" in
            socket_test_hook_section,
            "receipt barrier telemetry and API must remain test-hook-only")
    receipt_prepare = function_body(socket_transport,
                                    "peak_socket_gather_prepare_receipt")
    receipt_write = function_body(socket_transport,
                                  "peak_socket_gather_write_ready")
    receipt_peer = function_body(socket_transport,
                                 "peak_socket_report_peer_begin")
    require_order(receipt_prepare,
                  "connection->phase = PEAK_SOCKET_GATHER_SENDING_RECEIPT;",
                  "peak_socket_test_receipt_barrier_signal_root()")
    require("peak_socket_test_telemetry.root_receipt_failure_injected = true;"
            in receipt_write and
            "peak_socket_test_receipt_barrier_wait_for_root()" in receipt_peer,
            "receipt failure must be injected only after the test barrier")
    require("PEAK_CUDA_OUTPUT_AGGREGATION_LOCAL = 0" in cuda and
            "PEAK_CUDA_OUTPUT_AGGREGATION_MPI = 1" in cuda and
            "PEAK_CUDA_OUTPUT_AGGREGATION_SOCKET = 2" in cuda,
            "CUDA transport mode values must match PeakOutputAggregationMode")

    for hook in CUDA_HOOKS:
        require(re.search(
                    rf'peak_general_listener_find_function\(\s*"{hook}"\s*\)',
                    cuda) is not None,
                f"missing Frida-native support CUDA hook lookup: {hook}")

    require("PEAK_CUDA_WRAPPER_EXPORT extern \"C\" __attribute__((visibility(\"default\")))" in cuda,
            "CUDA wrapper export macro must use C linkage and default visibility")
    for wrapper in CUDA_WRAPPERS:
        require(re.search(rf"PEAK_CUDA_WRAPPER_EXPORT[^\n;]*\b{wrapper}\s*\(", cuda),
                f"CUDA wrapper must be exported for gum_find_function: {wrapper}")

    for target in GENERAL_LISTENER_CUDA_TARGETS:
        require(f'strcmp(peak_hook_strings[i], "{target}") == 0' in general,
                f"missing general-listener CUDA target: {target}")

    for target, wrapper in GENERAL_LISTENER_CUDA_WRAPPER_LOOKUPS:
        branch_start = general.find(
            f'strcmp(peak_hook_strings[i], "{target}") == 0')
        require(branch_start != -1,
                f"missing general-listener CUDA target: {target}")
        next_branch = general.find("} else if", branch_start + 1)
        branch = general[branch_start:next_branch]
        require(f'peak_general_listener_find_function("{wrapper}")' in branch,
                f"general-listener target {target} must use the helper"
                f" wrapper lookup for {wrapper}")

    require(cuda.count("PeakCudaInflightGuard in_flight;") == 10 and
            cuda.count("if (!in_flight.entered())") == 10,
            "each known CUDA wrapper must reject closed lifecycle admission")
    require(cuda.count("PeakCudaRuntimeLaunchGuard runtime_launch;") == 5 and
            cuda.count(
                "if (peak_cuda_runtime_launch_wrapper_depth != 0)") == 5,
            "accepted Runtime launches must suppress duplicate Driver timing")
    require("PeakCudaNestedDriverBypassGuard" not in cuda and
            "gum_interceptor_ignore_current_thread(cuda_interceptor)" not in
            cuda and
            "gum_interceptor_unignore_current_thread(cuda_interceptor)" not in
            cuda,
            "Runtime duplicate suppression must remain a TLS fast path")
    for wrapper in (
            "peak_cu_launch_kernel",
            "peak_cu_launch_cooperative_kernel",
            "peak_cu_launch_cooperative_kernel_multiple_device",
            "peak_cu_launch_kernel_ex",
            "peak_cu_graph_launch"):
        require_order(
            function_body(cuda, wrapper),
            "if (peak_cuda_runtime_launch_wrapper_depth != 0)",
            "return original_",
            "PeakCudaInflightGuard in_flight;")
    attach = function_body(cuda, "cuda_interceptor_attach")
    require_order(
        attach,
        "if (driver_typed_replacements_are_isolated)",
        "hook_cuda_launch != NULL && hook_cu_launch != NULL",
        "gum_interceptor_revert(cuda_interceptor, hook_cuda_launch)",
        "hook_cuda_launch = NULL")
    require("hook_cuda_launch_cooperative != NULL &&" in attach and
            "hook_cu_launch_cooperative != NULL" in attach and
            "hook_cuda_launch_cooperative_multiple_device != NULL &&" in
            attach and
            "hook_cu_launch_cooperative_multiple_device != NULL" in attach and
            "hook_cuda_launch_exc != NULL && hook_cu_launch_ex != NULL" in
            attach and
            "hook_cuda_graph_launch != NULL &&" in attach and
            "hook_cu_graph_launch != NULL" in attach,
            "each available Driver entry must independently replace its "
            "Runtime timing layer")
    require(cuda.count("if (!peak_cuda_record_event(") ==
            2 * len(TIMED_CUDA_WRAPPERS),
            "each timed wrapper must handle both event-record failures")
    record_event = function_body(cuda, "peak_cuda_record_event")
    require("peak_cuda_backend_api.event_record(" in record_event and
            "cudaEventRecord(event, stream)" in record_event,
            "Driver timing must avoid re-entering the Runtime API while "
            "retaining the Runtime fallback")
    require("peak_cuda_acquire_event_pair" not in cuda and
            "peak_cuda_release_event_pair" not in cuda and
            "peak_cuda_drain_kernel_event_map" not in cuda and
            "peak_cuda_drain_graph_event_map" not in cuda,
            "CUDA timing must use reusable slots instead of launch maps")
    require(re.search(r'^\s*#\s*define\s+(?:min|max)\s*\(', cuda_header,
                      re.MULTILINE) is None,
            "CUDA public header must not poison C++ standard min/max names")
    require(cuda.find("#include <algorithm>") != -1 and
            cuda.find("#include <algorithm>") <
            cuda.find("#include \"cuda_interceptor.h\"") and
            "std::max(" in cuda and "std::min(" in cuda,
            "CUDA launch statistics must use standard min/max without macro pollution")
    require("recorded = cudaEventRecord(event, stream) == cudaSuccess" in cuda and
            "if (!recorded)" in cuda and
            "cudaEventQuery(slot.end)" in cuda and
            "cudaEventElapsedTime(&ms" in cuda and
            "!= cudaSuccess" in cuda,
            "CUDA event record/query/elapsed failures must be checked")
    require("record_timing_error" in cuda and
            '"[timing_error]"' in cuda,
            "timing failures must be counted and transported")
    require("record_harvester_unavailable" in cuda and
            '"[harvester_unavailable]"' in cuda,
            "deferred helper initialization skips must be counted and "
            "transported")
    require("peak_cuda_new_event_slot" not in cuda,
            "per-launch CUDA event allocation must not bypass the bounded pool")
    require("PEAK_CUDA_EVENT_POOL_CAPACITY" in cuda and
            "peak_cuda_event_pool_capacity" in cuda,
            "CUDA event-pool capacity must be configurable and bounded")
    require("PeakCudaSlotLease" in cuda_state_header and
            "std::uint64_t generation;" in cuda_state_header and
            "std::uintptr_t context;" in cuda_state_header and
            "std::size_t shard;" in cuda_state_header and
            "kShardCount = 256" in cuda_state_header and
            "kCacheLineBytes = 64" in cuda_state_header and
            "sizeof(FreeHead) == kCacheLineBytes" in cuda_state_header and
            "sizeof(CounterShard) == kCacheLineBytes" in cuda_state_header and
            "sizeof(QueueShard) ==" in cuda_state_header and
            "sizeof(LaunchCounterShard) ==" in cuda_state_header and
            "struct alignas(64) FreeHead" not in cuda_state_header and
            "std::unique_ptr<Slot[]> slots_;" in cuda_state_header and
            "free_heads_" in cuda_state_header,
            "event slots must use fixed sharded context-owned generation "
            "leases without imposing an over-aligned containing-object ABI")
    require(re.search(
                r"struct PeakCudaRetainedState\s*\{\s*"
                r"PeakCudaProfilerState profiler_state;\s*"
                r"PeakCudaSlotAllocator slot_allocator;\s*"
                r"PeakCudaPendingQueue pending_queue;\s*"
                r"std::vector<PeakCudaEventSlot> event_pool;\s*"
                r"std::vector<PeakCudaActiveCapture> active_captures;\s*\};",
                cuda) is not None and
            "static PeakCudaRetainedState& peak_cuda_retained_state =\n"
            "    *new PeakCudaRetainedState;" in cuda,
            "a timed-out helper must retain every reachable C++ object for "
            "process lifetime")
    allocator_acquire = function_body(cuda_state, "acquire")
    allocator_admit = function_body(cuda_state, "acquire_if_accepting")
    allocator_release = function_body(cuda_state, "release")
    allocator_reserve = function_body(cuda_state, "reserve_unassigned")
    require(cuda_state.count(
                "accepting.load(std::memory_order_seq_cst)") == 2 and
            "*admission_closed = true;" in cuda_state and
            "acquire(context, shard, lease)" in cuda_state and
            "(void)release(*lease);" in cuda_state,
            "lock-free slot admission must roll back a lease that crosses close")
    lifecycle_enter = function_body(cuda, "peak_cuda_lifecycle_try_enter")
    lifecycle_leave = function_body(cuda, "peak_cuda_lifecycle_leave")
    lifecycle_count = function_body(cuda, "peak_cuda_active_wrapper_count")
    lifecycle_close = function_body(cuda, "peak_cuda_lifecycle_close")
    lifecycle_open = function_body(cuda, "peak_cuda_lifecycle_open")
    require("kPeakCudaLifecycleShardCount = 256" in cuda and
            "kPeakCudaLifecycleExclusiveShardCount = 192" in cuda and
            "struct alignas(64) PeakCudaLifecycleShard" in cuda and
            "peak_cuda_lifecycle_shards[kPeakCudaLifecycleShardCount]" in
            cuda and
            "static thread_local size_t peak_cuda_lifecycle_shard_index" in
            cuda and
            "static thread_local unsigned int peak_cuda_lifecycle_depth" in
            cuda and
            "static thread_local gboolean "
            "peak_cuda_lifecycle_shard_exclusive" in cuda and
            "kPeakCudaLifecycleShardCount;" in cuda and
            "std::atomic_ullong peak_cuda_lifecycle_epoch{1}" in cuda and
            "Odd epochs are closed; even epochs are open." in cuda,
            "wrapper lifetime accounting must use fixed cache-line shards "
            "and begin in a closed epoch")
    require_order(
        lifecycle_enter,
        "unsigned long long epoch = peak_cuda_lifecycle_epoch.load(",
        "std::memory_order_seq_cst",
        "if ((epoch & 1ULL) != 0)",
        "if (peak_cuda_lifecycle_depth != 0)",
        "++peak_cuda_lifecycle_depth",
        "peak_cuda_lifecycle_shard_index == kPeakCudaLifecycleShardCount",
        "peak_cuda_next_lifecycle_shard.fetch_add(",
        "std::memory_order_relaxed",
        "ticket < kPeakCudaLifecycleExclusiveShardCount",
        "kPeakCudaLifecycleExclusiveShardCount +",
        "shard->active.store(1, std::memory_order_seq_cst)",
        "shard->active.fetch_add(1, std::memory_order_seq_cst)",
        "peak_cuda_lifecycle_epoch.load(std::memory_order_seq_cst) != epoch",
        "shard->active.store(0, std::memory_order_seq_cst)",
        "shard->active.fetch_sub(1, std::memory_order_seq_cst)",
        "return NULL;",
        "peak_cuda_lifecycle_depth = 1",
        "return shard;",
    )
    require_order(lifecycle_leave,
                  "--peak_cuda_lifecycle_depth",
                  "peak_cuda_lifecycle_shard_exclusive",
                  "shard->active.store(0, std::memory_order_seq_cst)",
                  "shard->active.fetch_sub(1, std::memory_order_seq_cst)")
    require("ticket - kPeakCudaLifecycleExclusiveShardCount" in
            lifecycle_enter and
            "kPeakCudaLifecycleShardCount -\n"
            "                      kPeakCudaLifecycleExclusiveShardCount" in
            lifecycle_enter and
            "for (PeakCudaLifecycleShard& shard : "
            "peak_cuda_lifecycle_shards)" in lifecycle_count and
            "shard.active.load(std::memory_order_seq_cst)" in
            lifecycle_count and
            "if (active > UINT_MAX - total)" in lifecycle_count,
            "exclusive lifecycle slots, disjoint overflow counters, release, "
            "and close-side scans must remain SC and bounded")
    require_order(
        lifecycle_close,
        "unsigned long long epoch = peak_cuda_lifecycle_epoch.load(",
        "std::memory_order_seq_cst",
        "if ((epoch & 1ULL) == 0)",
        "peak_cuda_lifecycle_epoch.store(epoch + 1",
        "std::memory_order_seq_cst",
    )
    require_order(
        lifecycle_open,
        "unsigned long long epoch = peak_cuda_lifecycle_epoch.load(",
        "std::memory_order_seq_cst",
        "if ((epoch & 1ULL) != 0)",
        "peak_cuda_lifecycle_epoch.store(epoch + 1",
        "std::memory_order_seq_cst",
    )
    require(cuda.count("peak_cuda_lifecycle_epoch.store(") == 2 and
            cuda.count("peak_cuda_lifecycle_open();") == 1 and
            cuda.count("peak_cuda_lifecycle_close();") == 3,
            "lifecycle epochs must advance only through serialized "
            "odd/even close and open transitions")
    require(cuda.count(
                "for (PeakCudaLifecycleShard& shard : "
                "peak_cuda_lifecycle_shards)") == 1 and
            re.search(
                r"peak_cuda_lifecycle_shards\s*\[[^\]]+\]\s*"
                r"\.active\.store\s*\(", cuda) is None,
            "lifecycle reader shards must never be reset across reattach")
    require("peak_cuda_in_flight" not in cuda,
            "the unsafe scalar CUDA in-flight handshake must not return")
    slot_shard = function_body(cuda, "peak_cuda_current_slot_shard")
    observed = function_body(cuda_state, "record_launch_observed")
    accepted = function_body(cuda_state, "record_launch_accepted")
    require("kPeakCudaSlotExclusiveShardCount = 192" in cuda and
            "peak_cuda_slot_shard_exclusive" in cuda and
            "ticket < kPeakCudaSlotExclusiveShardCount" in slot_shard and
            "ticket - kPeakCudaSlotExclusiveShardCount" in slot_shard and
            "PeakCudaSlotAllocator::kShardCount -\n"
            "                    kPeakCudaSlotExclusiveShardCount" in
            slot_shard and
            "peak_cuda_current_slot_shard(), "
            "peak_cuda_slot_shard_exclusive" in cuda and
            "timing.lease.shard < kPeakCudaSlotExclusiveShardCount" in cuda,
            "launch accounting must use unique producer shards and disjoint "
            "overflow counters")
    require("if (exclusive)" in observed and
            "observed.store(observed.load(std::memory_order_relaxed) + 1" in
            observed and
            "observed.fetch_add(1, std::memory_order_relaxed)" in observed and
            "if (exclusive)" in accepted and
            "accepted.store(accepted.load(std::memory_order_relaxed) + 1" in
            accepted and
            "accepted.fetch_add(1, std::memory_order_relaxed)" in accepted,
            "exclusive launch counters must avoid RMWs while overflow "
            "counters remain multi-producer safe")
    require("slot.context = context;" in allocator_reserve and
            "slot.shard = static_cast<std::uint16_t>(shard);" in
            allocator_reserve and
            "slot.generation.fetch_add(" in allocator_reserve and
            "push_back" not in allocator_reserve and
            "resize" not in allocator_reserve,
            "slot acquisition must partition the fixed pool by context and shard")
    require_order(allocator_release,
                  "slot.generation.load(std::memory_order_acquire)",
                  "counter_shards_[lease.shard].handoff.fetch_add(",
                  "slot.state.compare_exchange_strong(",
                  "counter_shards_[lease.shard].active.fetch_sub(",
                  "push_free(bucket, lease.shard",
                  "counter_shards_[lease.shard].handoff.fetch_sub(")
    require("for (" not in allocator_release and
            "while (" not in allocator_release,
            "slot release must remain O(1), without a pool scan")
    require("slot.context != lease.context" in allocator_release and
            "slot.shard != lease.shard" in allocator_release and
            "lease.generation" in allocator_release,
            "slot release must reject stale and cross-context leases")
    discard = function_body(cuda, "peak_cuda_discard_lease_current")
    require("peak_cuda_event_pool[lease.index]" in discard and
            "peak_cuda_slot_allocator.release(lease)" in discard and
            "for (" not in discard and "while (" not in discard,
            "the interceptor must release slots by validated token index")
    attach = function_body(cuda, "cuda_interceptor_attach")
    require("peak_cuda_event_pool.assign(peak_cuda_event_pool_capacity, {})"
            in attach and
            "peak_cuda_pending_queue.reset(peak_cuda_event_pool_capacity)"
            in attach,
            "event slots and pending tokens must have one fixed shared bound")
    pending_push = function_body(cuda, "peak_cuda_pending_push")
    queue_push = function_body(cuda_state, "push")
    queue_requeue = function_body(cuda_state, "requeue_local")
    require("peak_cuda_pending_queue.push(lease, &shard_was_empty)" in
            pending_push and
            "if (shard_was_empty &&" in pending_push and
            "lease.shard == kPeakCudaWakeShard" in pending_push and
            "peak_cuda_harvester_waiting.compare_exchange_strong" in
            pending_push and
            "pthread_mutex_lock(&peak_cuda_harvester_mutex)" in
            pending_push and
            "pthread_cond_signal(&peak_cuda_harvester_cond)" in
            pending_push and
            "wake_pending" not in cuda and
            "shards_[lease.shard].head" in queue_push and
            "free_head_index(current) == kInvalidSlotIndex" in queue_push and
            ".count" not in queue_push and
            "compare_exchange_weak" in queue_push and
            "push_back" not in queue_push,
            "pending producers must use a per-shard empty-to-nonempty "
            "handoff and synchronize only with a sleeping helper")
    require("local_heads_[lease.shard]" in queue_requeue and
            "compare_exchange" not in queue_requeue and
            "shards_[lease.shard].head" not in queue_requeue and
            "peak_cuda_pending_queue.requeue_local" in
            function_body(cuda, "peak_cuda_harvest_pass"),
            "harvester retries must remain consumer-local so they do not "
            "suppress the next producer empty-to-nonempty notification")
    publish_timing = function_body(cuda, "peak_cuda_publish_timing")
    require("peak_cuda_pending_push(timing.lease)" in publish_timing and
            "slot->lease" not in publish_timing,
            "publishing must reuse the lease stored at acquisition instead "
            "of rewriting the large event-slot handoff state")
    require("kPeakCudaHarvestRecordBudget = 64" in cuda and
            "kPeakCudaHarvestTimeBudgetNs = 1000000" in cuda and
            "kPeakCudaHarvestRetryNs = 1000000L" in cuda,
            "the harvester must retain its 64-record/1-ms pass and bounded "
            "1-ms retry interval")
    harvest_pass = function_body(cuda, "peak_cuda_harvest_pass")
    require_order(harvest_pass,
                  "checked < kPeakCudaHarvestRecordBudget",
                  "!peak_cuda_harvester_running.load(",
                  "peak_cuda_pending_pop(&lease)",
                  "peak_cuda_activate_context(lease_context, &activation)",
                  "peak_cuda_harvest_one_current(lease)",
                  "now_ns - started_ns >= kPeakCudaHarvestTimeBudgetNs",
                  "peak_cuda_restore_context(&activation)",
                  "peak_cuda_slot_allocator.release(leases[index])")
    require("peak_cuda_pending_size" not in cuda,
            "the harvester must not scan producer-owned queue counts")
    require("outcomes[index] == PEAK_CUDA_HARVEST_RETRY &&\n"
            "                       peak_cuda_harvester_running.load(" in
            harvest_pass,
            "a stopped harvester must not requeue or continue incomplete "
            "records")
    require("peak_cuda_pending_queue.requeue_local(lease)" in harvest_pass and
            "peak_cuda_pending_queue.requeue_local(leases[index])" in
            harvest_pass and
            "peak_cuda_pending_push(lease)" not in harvest_pass,
            "helper retries must remain consumer-local and leave producer "
            "wake transitions available")
    require("lease_context != batch_context" in harvest_pass and
            "PeakCudaSlotLease leases[kPeakCudaHarvestRecordBudget]" in
            harvest_pass,
            "the helper must amortize context activation across one bounded "
            "same-context batch")
    harvest_one = function_body(cuda, "peak_cuda_harvest_one_current")
    require("cudaEventQuery(slot.end)" in harvest_one and
            "cudaEventElapsedTime(&ms, slot.start, slot.end)" in harvest_one and
            "pthread_mutex_lock" not in harvest_one and
            "peak_cuda_activate_context" not in harvest_one and
            "peak_cuda_restore_context" not in harvest_one,
            "CUDA completion queries must run outside locks and reuse the "
            "pass-owned current context")
    require("peak_cuda_test_force_incomplete.load(" in harvest_one and
            "return PEAK_CUDA_HARVEST_RETRY;" in harvest_one and
            "peak_cuda_test_force_incomplete.store(" in
            function_body(cuda, "peak_cuda_test_force_incomplete_events"),
            "the permanent-incomplete test seam must stop before CUDA query")
    require_order(
        harvest_one,
        "peak_cuda_test_force_query_error.exchange(",
        "cudaEventQuery(slot.end)",
        "peak_cuda_test_harvester_queries.fetch_add(",
        "!peak_cuda_harvester_running.load(",
        "record_event_query_failure()",
        "peak_cuda_destroy_slot_events_current(&slot)",
        "return PEAK_CUDA_HARVEST_RELEASE;",
    )
    harvester = function_body(cuda, "peak_cuda_harvester_main")
    require("pthread_cond_timedwait" in harvester and
            "peak_cuda_harvester_waiting.store(true" in harvester and
            "peak_cuda_harvester_waiting.store(false" in harvester and
            "kPeakCudaHarvestRetryNs" in harvester and
            "peak_general_listener_exclude_current_thread()" in harvester,
            "the bounded harvester must be helper-excluded and condition-driven")
    require_order(
        harvester,
        "peak_general_listener_fast_ignore_current_thread()",
        "!peak_cuda_harvester_initialization_requested",
        "cudaThreadExchangeStreamCaptureMode(&runtime_mode)",
        ".driver_thread_exchange_stream_capture_mode(&driver_mode)",
        "peak_cuda_harvester_capture_mode_ready = capture_mode_ready",
        "peak_cuda_harvester_initialization_done = TRUE",
        "peak_cuda_accepting_events.store(true",
        "for (;;)",
    )
    start_harvester = function_body(cuda, "peak_cuda_start_harvester")
    require_order(
        start_harvester,
        "peak_cuda_harvester_initialization_requested = FALSE",
        "peak_cuda_harvester_initialization_allowed = TRUE",
        "peak_cuda_harvester_initialization_done = FALSE",
        "pthread_create(",
        "peak_cuda_harvester_started.store(true",
        "return TRUE;",
    )
    require("peak_cuda_accepting_events" not in start_harvester,
            "helper creation must not publish CUDA timing admission")
    request_harvester = function_body(
        cuda, "peak_cuda_request_harvester_initialization")
    require("peak_cuda_harvester_initialization_terminal.load(" in
            request_harvester,
            "terminal helper initialization failure must bypass the mutex on "
            "later matching launches")
    require_order(
        request_harvester,
        "peak_cuda_harvester_started.load(",
        "pthread_mutex_trylock(",
        "peak_cuda_harvester_initialization_allowed",
        "peak_cuda_harvester_initialization_requested = TRUE",
        "pthread_cond_broadcast",
    )
    require("pthread_cond_wait" not in request_harvester and
            "cudaThreadExchangeStreamCaptureMode" not in request_harvester,
            "matching launches must request deferred helper initialization "
            "without waiting or entering CUDA")
    require_order(
        harvester,
        "peak_cuda_harvester_initialization_inflight.store(\n"
        "            true",
        "cudaThreadExchangeStreamCaptureMode(&runtime_mode)",
        ".driver_thread_exchange_stream_capture_mode(&driver_mode)",
        "peak_cuda_harvester_initialization_inflight.store(\n"
        "        false",
    )
    require("*(char**)((size_t)func" not in cuda and
            "(size_t)func + 8" not in cuda,
            "Driver CUfunction handles must not be read through private layouts")
    driver_name = function_body(cuda, "peak_cuda_driver_kernel_name")
    require('dlsym(driver, "cuFuncGetName")' in cuda and
            "dlsym(" not in driver_name and
            "peak_cuda_func_get_name(&name, function)" in driver_name,
            "Driver names must use a once-cached cuFuncGetName lookup")
    identify_kernel = function_body(cuda, "peak_cuda_identify_kernel")
    require(identify_kernel.count(
                "demangled_name != NULL ? demangled_name : resolved_name") == 2,
            "plain Driver symbols must retain their resolved name when "
            "demangling is not applicable")
    for forbidden_mpi_call in ("MPI_Init(", "MPI_Reduce(", "MPI_Gather(",
                               "MPI_Gatherv("):
        require(forbidden_mpi_call not in cuda,
                f"CUDA interceptor must not own {forbidden_mpi_call} transport")

    backend_resolver = function_body(cuda, "peak_cuda_resolve_backend_api")
    require('dlsym(driver, "cuStreamIsCapturing")' in backend_resolver and
            'dlsym(driver, "cuThreadExchangeStreamCaptureMode")' in
            backend_resolver,
            "Driver capture APIs must be resolved once for lifecycle listeners")
    acquire_timing = function_body(cuda, "peak_cuda_acquire_timing")
    require_order(acquire_timing,
                  "!capture_guard->try_enter()",
                  "!peak_cuda_accepting_events.load(",
                  "peak_cuda_request_harvester_initialization()",
                  "record_harvester_unavailable()",
                  "return FALSE;",
                  "peak_cuda_current_context(&context)",
                  "peak_cuda_slot_allocator.acquire_if_accepting(")
    require("driver_stream_is_capturing" not in acquire_timing and
            "if (!admission_closed)" in acquire_timing and
            "record_pool_full()" in acquire_timing,
            "the fail-closed capture gate must avoid redundant per-launch "
            "queries and closed admission must not look like pool exhaustion")

    capture_begin = function_body(cuda, "peak_cuda_capture_begin_transition")
    capture_end = function_body(cuda,
                                "peak_cuda_capture_end_transition_locked")
    capture_enter = function_body(cuda,
                                  "peak_cuda_capture_listener_on_enter")
    capture_leave = function_body(cuda,
                                  "peak_cuda_capture_listener_on_leave")
    require("kPeakCudaTimingShardCount = 256" in cuda and
            "kPeakCudaTimingExclusiveShardCount = 192" in cuda and
            "struct alignas(64) PeakCudaTimingShard" in cuda and
            "thread_local unsigned int peak_cuda_timing_depth" in cuda and
            "thread_local gboolean peak_cuda_timing_shard_exclusive" in cuda and
            "peak_cuda_next_timing_shard.fetch_add(" in cuda and
            "ticket < kPeakCudaTimingExclusiveShardCount" in cuda and
            "kPeakCudaTimingExclusiveShardCount +" in cuda and
            "shard_->active.fetch_add(1, std::memory_order_seq_cst)" in cuda and
            "shard->active.fetch_sub(" in cuda,
            "capture coordination must reserve disjoint exclusive and "
            "overflow cache-line reader shards")
    timing_enter = function_body(cuda, "try_enter")
    timing_leave = function_body(cuda, "release_reader")
    require_order(timing_enter,
                  "peak_cuda_capture_blocked.load(std::memory_order_acquire)",
                  "if (peak_cuda_timing_depth != 0)",
                  "peak_cuda_next_timing_shard.fetch_add(",
                  "shard_->active.store(1, std::memory_order_seq_cst)",
                  "peak_cuda_timing_depth = 1",
                  "peak_cuda_capture_blocked.load(std::memory_order_seq_cst)",
                  "release_reader()",
                  "return false;")
    require_order(timing_leave,
                  "if (--peak_cuda_timing_depth != 0)",
                  "shard->active.store(0, std::memory_order_seq_cst)",
                  "shard->active.fetch_sub(",
                  "peak_cuda_capture_blocked.load(",
                  "pthread_cond_broadcast")
    require_order(capture_begin,
                  "peak_cuda_capture_blocked.store(true",
                  "while (peak_cuda_timing_sections_active())",
                  "pthread_cond_wait")
    require("peak_cuda_capture_any_active_locked()" in capture_end and
            "peak_cuda_capture_tracking_terminal.load(" in capture_end and
            "peak_cuda_capture_blocked.store(false" in capture_end,
            "capture timing may reopen only after proven quiescence")
    require_order(capture_enter,
                  "invocation->lifecycle_shard = "
                  "peak_cuda_lifecycle_try_enter()",
                  "if (invocation->lifecycle_shard == NULL)",
                  "peak_cuda_capture_teardown_mutex.lock()",
                  "invocation->teardown_lock_held = TRUE",
                  "peak_cuda_capture_tracking_terminal.store(",
                  "invocation->fail_closed_transition = TRUE",
                  "peak_cuda_capture_begin_transition()",
                  "return;",
                  "peak_cuda_capture_begin_transition()",
                  "peak_cuda_capture_status(",
                  "ctx_get_current")
    rejected_listener = capture_enter[
        capture_enter.find("if (invocation->lifecycle_shard == NULL)"):
        capture_enter.find("return;",
                           capture_enter.find(
                               "if (invocation->lifecycle_shard == NULL)"))]
    require("peak_cuda_capture_status" not in rejected_listener and
            "peak_cuda_profiler_state" not in rejected_listener and
            "ctx_get_current" not in rejected_listener,
            "a lifecycle-rejected capture listener must fail closed without "
            "calling CUDA or profiler-state APIs")
    require_order(capture_leave,
                  "if (invocation->lifecycle_shard == NULL)",
                  "if (invocation->fail_closed_transition)",
                  "pthread_mutex_lock(&peak_cuda_capture_mutex)",
                  "peak_cuda_capture_end_transition_locked()",
                  "pthread_mutex_unlock(&peak_cuda_capture_mutex)",
                  "if (invocation->teardown_lock_held)",
                  "peak_cuda_capture_teardown_mutex.unlock()",
                  "return;",
                  "peak_cuda_capture_status(",
                  "peak_cuda_capture_end_transition_locked()",
                  "peak_cuda_lifecycle_leave(invocation->lifecycle_shard)")
    require("peak_cuda_capture_register_locked(" in capture_leave and
            "peak_cuda_capture_remove_locked(" in capture_leave and
            "result == 0 && !expected_edge" in capture_leave and
            "peak_cuda_capture_end_transition_locked()" in capture_leave,
            "capture listeners must validate and publish each lifecycle edge")
    require("kPeakCudaCaptureRegistryCapacity = 1024" in cuda and
            "peak_cuda_active_captures.assign(\n"
            "        kPeakCudaCaptureRegistryCapacity, {})" in attach and
            "peak_cuda_active_captures.assign(peak_cuda_event_pool_capacity"
            not in attach,
            "capture tracking must be bounded independently of timing slots")

    capture_census = function_body(
        cuda, "peak_cuda_capture_entry_point_census")
    require('"cuGetProcAddress_v2"' in capture_census and
            '"cuGetProcAddress"' in capture_census and
            "static const unsigned long long flags[] = {0, 1, 2};" in
            capture_census and
            "query.introduction_version > driver_version" in capture_census and
            "peak_cuda_capture_entry_point_is_covered(\n"
            "                         entry, query.operation)" in capture_census and
            "peak_cuda_install_direct_capture_hook(\n"
            "                         entry, query.symbol, query.operation)" in
            capture_census and
            "if (!begin_covered || !end_covered)" in capture_census,
            "capture pointer census must cover ABI versions and stream modes "
            "without querying future APIs")
    direct_capture_attach = function_body(
        cuda, "peak_cuda_install_direct_capture_hook")
    direct_capture_detach = function_body(cuda, "cuda_interceptor_dettach")
    require("kPeakCudaDirectCaptureHookCapacity = 42" in cuda and
            "peak_cuda_direct_capture_hook_count >=\n"
            "        kPeakCudaDirectCaptureHookCapacity" in
            direct_capture_attach and
            "peak_cuda_attach_capture_entry(entry, listener)" in
            direct_capture_attach and
            "peak_cuda_direct_capture_hook_count" in direct_capture_detach and
            "peak_cuda_direct_capture_listeners[index]" in
            direct_capture_detach,
            "direct Driver capture entries must use a fixed attach-time "
            "listener set with symmetric teardown")
    require("entry_point_listener" not in cuda and
            "capture_begin_transition_except_current" not in cuda,
            "capture pointer coverage must remain an attach-time census")
    driver_isolation = function_body(
        cuda, "peak_cuda_driver_typed_replacements_are_isolated")
    require("gum_interceptor_peak_resolve_redirect_chain(" in
            driver_isolation and
            "if (canonical != address)" in driver_isolation and
            "Driver launch timing remains disabled" in driver_isolation,
            "typed Driver replacements must reject redirecting dispatch stubs")
    for support_symbol in (
            "cuCtxGetCurrent", "cuCtxGetDevice", "cuCtxPushCurrent_v2",
            "cuCtxPopCurrent_v2", "cuStreamIsCapturing",
            "cuThreadExchangeStreamCaptureMode", "cuFuncGetName",
            "cuDriverGetVersion", "cuGetProcAddress",
            "cuGetProcAddress_v2"):
        require(f'"{support_symbol}"' in driver_isolation,
                f"Driver isolation census must include {support_symbol}")
    require("for (const char* symbol : support_symbols)" in driver_isolation and
            "for (const PeakCudaCaptureHookDescriptor& descriptor :" in
            driver_isolation and
            "descriptor.api == PEAK_CUDA_CAPTURE_API_DRIVER" in
            driver_isolation and
            "!non_timed_entry_is_isolated(descriptor.symbol)" in
            driver_isolation and
            "address == timed[index].canonical ||" in driver_isolation and
            "canonical == timed[index].canonical" in driver_isolation,
            "Driver isolation must resolve support/getproc/capture entries and "
            "reject raw or canonical collisions with timed hooks")

    kernel_wrappers = [
        "peak_cuda_launch_kernel",
        "peak_cuda_launch_cooperative_kernel",
        "peak_cuda_launch_kernel_exc",
        "peak_cu_launch_kernel",
        "peak_cu_launch_cooperative_kernel",
        "peak_cu_launch_kernel_ex",
    ]
    for wrapper in kernel_wrappers:
        body = function_body(cuda, wrapper)
        require("peak_cuda_identify_kernel" in body and
                "if (!identity->target_match)" in body,
                f"{wrapper} must filter a cached kernel identity")
        require_order(body, "peak_cuda_identify_kernel",
                      "if (!identity->target_match)",
                      "PeakCudaCaptureTimingGuard capture_guard",
                      "peak_cuda_acquire_timing(")
        if wrapper.startswith("peak_cu_"):
            require_order(body, "if (!in_flight.entered())",
                          "peak_cuda_cached_driver_identity(func, &context)",
                          "if (identity == NULL)",
                          "peak_cuda_current_context(&context)",
                          "peak_cuda_identify_kernel")
            require_order(body, "if (!identity->target_match)",
                          "PeakCudaCaptureTimingGuard capture_guard",
                          "if (!capture_guard.try_enter())",
                          "peak_cuda_acquire_timing(")
    cached_driver = function_body(
        cuda, "peak_cuda_cached_driver_identity")
    require("peak_cuda_identity_epoch.load(" in cached_driver and
            "peak_cuda_thread_identity.epoch != epoch" in cached_driver and
            "peak_cuda_thread_identity.driver_function" in cached_driver and
            "peak_cuda_thread_identity.context" in cached_driver and
            "&peak_cuda_thread_identity.value" in cached_driver and
            "peak_cuda_current_context" not in cached_driver,
            "repeated Driver identities must use the epoch-bounded TLS cache "
            "without a serialized context query")
    require("PEAK_CUDA_LAUNCH_BACKEND_RUNTIME" in
            function_body(cuda, "peak_cuda_launch_kernel") and
            "PEAK_CUDA_LAUNCH_BACKEND_DRIVER" in
            function_body(cuda, "peak_cu_launch_kernel"),
            "both CUDA launch families must pass through capture validation")

    for wrapper, original in TIMED_CUDA_WRAPPERS.items():
        body = function_body(cuda, wrapper)
        first_guard = body.find("if (!peak_cuda_record_event(")
        second_guard = body.find("if (!peak_cuda_record_event(", first_guard + 1)
        require(first_guard != -1 and second_guard != -1,
                f"{wrapper} must check start and end event records")
        start_block = body[first_guard:second_guard]
        end_block = body[second_guard:]
        start_return = ("return call_original();"
                        if wrapper.startswith("peak_cuda_")
                        else f"return {original}(")
        require("peak_cuda_discard_lease_current(timing.lease)" in start_block and
                start_return in start_block,
                f"{wrapper} start-record failure must release and call original once")
        require("peak_cuda_discard_lease_current(timing.lease)" in end_block and
                "return result;" in end_block and
                f"return {original}(" not in end_block,
                f"{wrapper} end-record failure must release without rerunning original")

    for wrapper, original in MULTI_DEVICE_CUDA_WRAPPERS.items():
        body = function_body(cuda, wrapper)
        expected_original_calls = 3 if wrapper.startswith("peak_cu_") else 2
        require("record_unsupported_multi_device" in body and
                body.count(original) == expected_original_calls and
                "peak_cuda_acquire_timing" not in body and
                "peak_cuda_record_event" not in body,
                f"{wrapper} must safely skip unsupported multi-device timing")

    for wrapper in CUDA_WRAPPERS:
        body = function_body(cuda, wrapper)
        original = TIMED_CUDA_WRAPPERS.get(
            wrapper, MULTI_DEVICE_CUDA_WRAPPERS.get(wrapper))
        require(original is not None,
                f"missing original-function mapping for {wrapper}")
        rejection = body.find("if (!in_flight.entered())")
        rejection_end = body.find("}", rejection)
        require_order(body,
                      "PeakCudaInflightGuard in_flight;",
                      "if (!in_flight.entered())",
                      f"return {original}(")
        require(rejection != -1 and rejection_end != -1 and
                f"return {original}(" in body[rejection:rejection_end] and
                all(token not in body[:rejection_end] for token in (
                    "peak_cuda_profiler_state",
                    "peak_cuda_identify_kernel",
                    "peak_cuda_current_context",
                    "PeakCudaCaptureTimingGuard",
                    "peak_cuda_acquire_timing",
                )),
                f"{wrapper} must delegate immediately when lifecycle "
                "admission is closed")

    require("std::atomic_bool peak_cuda_accepting_events" in cuda,
            "missing accepting-events gate")
    require("std::atomic_bool peak_cuda_harvester_started" in cuda,
            "helper publication must be race-free with newly active wrappers")
    pin_harvester = function_body(
        cuda, "peak_cuda_pin_harvester_to_last_allowed_cpu")
    require_order(pin_harvester,
                  "sched_getaffinity(0, sizeof(allowed), &allowed)",
                  "CPU_ISSET(cpu, &allowed)",
                  "pthread_setaffinity_np(")
    require_order(function_body(cuda, "peak_cuda_harvester_main"),
                  "peak_cuda_pin_harvester_to_last_allowed_cpu()",
                  "peak_general_listener_exclude_current_thread()")
    require("std::atomic_ullong peak_cuda_lifecycle_epoch" in cuda and
            "PeakCudaLifecycleShard" in cuda,
            "missing epoch-based lifecycle reader gate")
    require("static gboolean peak_cuda_hooks_reverted" in cuda,
            "missing physical-detach state flag")
    require(re.search(
                r'g_hash_table_new_full\(\s*g_str_hash,\s*str_equal_function,'
                r'\s*g_free,\s*g_free\)', attach) is not None and
            re.search(
                r'g_hash_table_new_full\(\s*peak_cuda_graph_key_hash,'
                r'\s*peak_cuda_graph_key_equal,\s*g_free,\s*g_free\)',
                attach) is not None,
            "CUDA aggregation maps must own and free their heap keys")
    require("g_int_equal" not in cuda,
            "CUDA graph pointer maps must not use g_int_equal")
    require("std::uintptr_t context;" in cuda and
            "a->context == b->context && a->graph == b->graph" in cuda,
            "graph identity must include its owning CUDA context")
    graph_insert = function_body(cuda, "insert_cuda_graph_record")
    require("g_hash_table_size(cuda_graph_local_mapping) >=\n"
            "            peak_cuda_graph_identity_capacity" in graph_insert and
            "record_identity_full()" in graph_insert and
            "g_try_new(PeakCudaGraphKey, 1)" in graph_insert and
            "g_try_new(GraphRecordInfo, 1)" in graph_insert,
            "graph aggregation must retain a fixed fail-open identity bound")
    require("peak_cuda_graph_identity_capacity = "
            "peak_cuda_event_pool_capacity * 4;" in attach,
            "graph identity capacity must derive from the configured pool")
    require("Graph executable handles are rank-local" in cuda,
            "graph records must document their rank-local policy")
    require(re.search(
                r'struct PeakCudaLaunchDimensions\s*\{\s*'
                r'std::uint64_t total_threads;\s*'
                r'std::uint64_t grid_size;\s*'
                r'std::uint64_t block_size;', cuda_state_header) is not None and
            "std::uint64_t total_gpu_threads;" in cuda and
            "std::uint64_t total_kernel_call_cnt;" in cuda and
            "std::uint64_t total_block_size;" in cuda and
            "std::uint64_t total_grid_size;" in cuda,
            "CUDA launch dimensions and aggregates must use 64-bit counters")
    saturating_multiply = function_body(cuda_state, "saturating_multiply")
    saturating_add = function_body(cuda_state, "peak_cuda_saturating_add_u64")
    require("std::numeric_limits<std::uint64_t>::max() / left" in
            saturating_multiply and
            "std::numeric_limits<std::uint64_t>::max() - left" in
            saturating_add and
            "record_dimension_overflow" in cuda,
            "CUDA dimension arithmetic must saturate and report overflow")
    require("peak_cuda_build_report_snapshot" in cuda and
            "peak_cuda_render_report_snapshot" in cuda,
            "CUDA must own an isolated transport snapshot and renderer")
    cuda_snapshot = function_body(cuda, "peak_cuda_build_report_snapshot")
    require("snapshot->overhead = peak_report_snapshot_get_transport_overhead();"
            in cuda_snapshot,
            "CUDA snapshots must copy the CPU transport-overhead context")
    require("snapshot->rank_count = 1;" in cuda_snapshot,
            "CUDA local snapshots must begin with a single local rank")
    cuda_counter_names = [
        "observed", "accepted", "completed", "pool_high_water", "pool_full",
        "identity_full", "event_create_failed", "timing_error",
        "harvester_unavailable",
        "stream_capture_skipped", "capture_query_failed",
        "capture_query_unsupported", "unsupported_multi_device",
        "event_query_failed", "elapsed_failed", "context_query_failed",
        "context_switch_failed", "context_restore_failed",
        "finalization_timeout", "finalization_incomplete",
        "dimension_overflow",
    ]
    for counter in cuda_counter_names:
        require(f'"CUDA profiler counter [{counter}]"' in cuda_snapshot,
                f"CUDA snapshot must transport the {counter} counter")
    require("peak_socket_report_transport_begin_channel" in cuda and
            "PEAK_SOCKET_REPORT_CHANNEL_CUDA" in cuda,
            "CUDA socket output must use its own port channel")
    require("cuda_interceptor_print_kernel_result" not in cuda,
            "only the snapshot-based CUDA kernel renderer may remain")
    general_source = (repo_root / "src/general_listener.c").read_text(
        encoding="utf-8")
    source_cmake = (repo_root / "src/CMakeLists.txt").read_text(
        encoding="utf-8")
    require("cuda_interceptor_prepare_report_snapshot" not in general_source and
            "auxiliary" not in general_source,
            "CPU report snapshots must not depend on CUDA rows")
    require("if (CUDA_FOUND OR CUDAToolkit_FOUND)" in source_cmake and
            "cuda_profiler_state.cpp" in source_cmake,
            "CUDA profiling state must remain an optional CUDA-only object")
    cpu_capture = function_body(
        general_source, "peak_general_listener_print_with_mpi_job_policy")
    require_order(cpu_capture,
                  "PeakReportOverhead local_report =",
                  "peak_general_listener_local_report_overhead(sum_num_calls)",
                  "peak_report_snapshot_set_transport_overhead(&local_report)",
                  "peak_general_listener_build_report_snapshot(")

    cuda_mpi_gate = peak.find(
        "cuda_interceptor_print_with_mpi_job_policy(\n"
        "            (mpi_reducer_failed_closed ||")
    cuda_failed_closed_recheck = peak.find(
        "mpi_reducer_failed_closed = found_MPI &&\n"
        "            peak_general_listener_mpi_reducer_failed_closed();",
        cuda_mpi_gate + 1)
    report_release_gate = peak.find(
        "if (report_release_gate_allowed && !mpi_reducer_failed_closed)",
        cuda_failed_closed_recheck + 1)
    require(cuda_mpi_gate != -1 and
            "output_mode == PEAK_OUTPUT_AGGREGATION_MPI" in
            peak[cuda_mpi_gate:cuda_failed_closed_recheck] and
            "!used_mpi_aggregation" in
            peak[cuda_mpi_gate:cuda_failed_closed_recheck],
            "CUDA MPI output must be gated on CPU MPI aggregation success")
    require(cuda_failed_closed_recheck > cuda_mpi_gate and
            report_release_gate > cuda_failed_closed_recheck,
            "CUDA MPI failed-closed state must be rechecked before report release")

    printer = function_body(cuda, "cuda_interceptor_print_with_mpi_job_policy")
    require(re.search(
                r"cuda_interceptor_print_with_mpi_job_policy\s*\(\s*"
                r"int aggregation_mode,\s*int active_mpi_job\s*\)", cuda)
            is not None,
            "CUDA policy reporting must receive explicit active-MPI context")
    require("PeakSocketReportRankSource socket_rank_source = active_mpi_job\n"
            "                ? PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED\n"
            "                : PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV;" in printer and
            "local, socket_rank_source," in printer,
            "active-MPI CUDA socket output must require launcher rank metadata")
    mpi_transport = printer.find("peak_mpi_report_transport_reduce(local, &aggregate)")
    mpi_guard = printer.rfind(
        "if (aggregation_mode == PEAK_CUDA_OUTPUT_AGGREGATION_MPI)", 0,
        mpi_transport)
    require(mpi_transport != -1 and mpi_guard != -1 and
            mpi_guard < mpi_transport,
            "CUDA local or failed-closed output must not enter MPI transport")
    require("found_MPI ? TRUE : FALSE" in
            peak[cuda_mpi_gate:cuda_failed_closed_recheck],
            "CUDA output must receive the active-MPI policy from peak.c")
    legacy_printer = function_body(cuda, "cuda_interceptor_print")
    require(re.search(r"cuda_interceptor_print\s*\(\s*int is_MPI\s*\)",
                      cuda) is not None and
            "cuda_interceptor_print_with_mpi_job_policy(" in legacy_printer and
            "is_MPI ? PEAK_CUDA_OUTPUT_AGGREGATION_MPI :" in legacy_printer and
            "PEAK_CUDA_OUTPUT_AGGREGATION_LOCAL" in legacy_printer and
            "PEAK_CUDA_OUTPUT_AGGREGATION_SOCKET" not in legacy_printer,
            "legacy CUDA reporting ABI must retain boolean MPI/local semantics")

    attach = function_body(cuda, "cuda_interceptor_attach")
    require(attach.count("hook_replace_check != GUM_REPLACE_OK") == 10,
            "attach must track replacement success for each known CUDA hook")
    require_order(
        attach,
        "std::lock_guard<std::mutex> lifecycle_lock",
        "if (!peak_cuda_timing_requested())",
        "return GUM_REPLACE_OK;",
        "peak_cuda_finalization_timed_out",
        "refusing to reinitialize active or retained CUDA profiler state",
        "peak_cuda_lifecycle_close()",
        "peak_cuda_accepting_events.store(false",
        "if (!peak_cuda_destroy_event_pool())",
        "g_hash_table_new_full",
    )
    require_order(
        attach,
        "peak_cuda_accepting_events.store(false",
        "peak_cuda_resolve_backend_api()",
        "gum_interceptor_begin_transaction(cuda_interceptor)",
        "peak_cuda_install_capture_hook(",
        "peak_cuda_capture_entry_point_census(",
        "peak_cuda_lifecycle_open()",
        "gum_interceptor_end_transaction(cuda_interceptor)",
        "if (peak_cuda_has_timed_hook() && peak_cuda_capture_hooks_ready)",
        "peak_cuda_start_harvester()",
    )
    require(attach.count("peak_cuda_resolve_backend_api()") == 1,
            "CUDA backend entry points must be immutable before hook "
            "publication")
    require(attach.count(
                "peak_cuda_driver_typed_replacements_are_isolated()") == 1 and
            cuda.count(
                "peak_cuda_driver_typed_replacements_are_isolated()") == 2,
            "Driver collision census must run once at attach time only")
    require_order(
        attach,
        "for (size_t hook_index = 0;",
        "if (!driver_typed_replacements_are_isolated &&",
        "peak_cuda_capture_descriptors[index].api ==",
        "PEAK_CUDA_CAPTURE_API_DRIVER)",
        "continue;",
        "gboolean installed = peak_cuda_install_capture_hook(",
    )
    require("driver_typed_replacements_are_isolated\n"
            "        ? (gpointer*) peak_general_listener_find_function("
            in attach,
            "a failed Driver ABI-isolation census must suppress typed launch "
            "replacements and generic Driver capture listeners")
    for wrapper in CUDA_WRAPPERS:
        require("peak_cuda_resolve_backend_api" not in
                function_body(cuda, wrapper),
                f"{wrapper} must not resolve Driver APIs on the launch path")

    detach = function_body(cuda, "cuda_interceptor_dettach")
    require_order(
        detach,
        "peak_cuda_lifecycle_close()",
        "peak_cuda_disable_harvester_initialization()",
        "gum_interceptor_begin_transaction(cuda_interceptor)",
        "gum_interceptor_end_transaction(cuda_interceptor)",
        "gum_interceptor_flush(cuda_interceptor)",
        "peak_cuda_hooks_reverted = TRUE",
        "peak_cuda_clear_hook_pointers()",
        "peak_cuda_finalize_pending()",
        "if (peak_cuda_finalization_timed_out)",
        "peak_cuda_destroy_event_pool()",
        "g_hash_table_destroy(cuda_kernel_local_dim_mapping)",
        "g_hash_table_destroy(cuda_graph_local_mapping)",
    )
    require("g_object_unref(cuda_interceptor)" not in detach,
            "CUDA physical detach must retain Gum trampoline state")
    require("cuda_interceptor = NULL" not in detach,
            "CUDA physical detach must keep interceptor state referenced")
    active_guard = detach.find("if (active_cuda_wrappers != 0)")
    finalize = detach.find("peak_cuda_finalize_pending()")
    require(active_guard != -1 and finalize != -1 and
            active_guard < finalize and
            "return;" in detach[active_guard:finalize],
            "active CUDA wrappers must retain pool/maps for a safe cleanup retry")
    timed_out = detach.find("if (peak_cuda_finalization_timed_out)")
    destroy_pool = detach.find("peak_cuda_destroy_event_pool()", timed_out)
    require(timed_out != -1 and destroy_pool > timed_out and
            "return;" in detach[timed_out:destroy_pool],
            "a bounded finalization timeout must retain incomplete CUDA state")
    require("peak_cuda_capture_listeners[index] != NULL" in detach and
            "gum_interceptor_detach(" in detach,
            "capture lifecycle listeners must detach in the Gum transaction")

    clear_hooks = function_body(cuda, "peak_cuda_clear_hook_pointers")
    for hook in [
        "hook_cuda_launch",
        "hook_cuda_launch_cooperative",
        "hook_cuda_launch_cooperative_multiple_device",
        "hook_cuda_launch_exc",
        "hook_cu_launch",
        "hook_cu_launch_cooperative",
        "hook_cu_launch_cooperative_multiple_device",
        "hook_cu_launch_ex",
        "hook_cuda_graph_launch",
        "hook_cu_graph_launch",
    ]:
        require(f"{hook} = NULL;" in clear_hooks,
                f"hook cleanup helper must clear {hook}")
    require("peak_cuda_capture_hooks[index] = NULL;" in clear_hooks and
            "g_object_unref(peak_cuda_capture_listeners[index])" in
            clear_hooks,
            "capture lifecycle hook storage must be cleared after flush")

    for forbidden_sync in ("cudaDeviceSynchronize(",
                           "cudaStreamSynchronize(",
                           "cuCtxSynchronize("):
        require(forbidden_sync not in cuda,
                f"CUDA timing and finalization must not call {forbidden_sync}")

    timeout_parser = function_body(cuda,
                                   "peak_cuda_parse_finalization_timeout_ms")
    require("PEAK_CUDA_FINALIZATION_TIMEOUT_MS" in timeout_parser and
            '"milliseconds", 1000, 1,' in timeout_parser and
            "60000" in timeout_parser,
            "CUDA finalization timeout must default to 1000 ms in [1, 60000]")
    monotonic = function_body(cuda, "peak_cuda_monotonic_ns")
    require("clock_gettime(CLOCK_MONOTONIC" in monotonic,
            "CUDA finalization must use a monotonic clock")
    finalizer = function_body(cuda, "peak_cuda_finalize_pending")
    require("peak_cuda_disable_harvester_initialization();" in finalizer,
            "finalization must cancel a deferred helper handshake before "
            "waiting for CUDA wrapper quiescence")
    disable_initialization = function_body(
        cuda, "peak_cuda_disable_harvester_initialization")
    require_order(
        disable_initialization,
        "peak_cuda_harvester_initialization_allowed = FALSE",
        "peak_cuda_harvester_initialization_terminal.store(",
        "peak_cuda_accepting_events.store(false",
        "!peak_cuda_harvester_initialization_done",
        "peak_cuda_harvester_running.store(false",
    )
    require_order(
        finalizer,
        "peak_cuda_lifecycle_close()",
        "peak_cuda_monotonic_ns()",
        "peak_cuda_disable_harvester_initialization()",
        "deadline_ns",
        "unsigned int wrappers = peak_cuda_active_wrapper_count()",
        "size_t active_slots = peak_cuda_slot_allocator.active_count()",
        "now_ns >= deadline_ns",
        "peak_cuda_request_harvester_stop_no_wait()",
        "deadline_wrappers = peak_cuda_active_wrapper_count()",
        "deadline_incomplete = peak_cuda_slot_allocator.active_count()",
        "deadline_initialization_inflight =",
        "if (deadline_incomplete == 0 && deadline_wrappers == 0 &&",
        "peak_cuda_finalization_complete = TRUE",
        "if (peak_cuda_finalization_complete)",
        "record_finalization_timeout(\n            deadline_incomplete)",
        "retaining incomplete event state",
        "peak_cuda_log_incomplete_records(deadline_wrappers)",
    )
    require("peak_cuda_harvester_initialization_inflight.load(" in
            finalizer and
            "helper_initialization_inflight=%d" in finalizer,
            "a helper blocked while establishing capture mode must consume "
            "the same bounded finalization deadline")
    complete_branch = finalizer.find("if (peak_cuda_finalization_complete)")
    timeout_branch = finalizer.find("} else {", complete_branch)
    require(complete_branch != -1 and timeout_branch != -1 and
            "peak_cuda_request_harvester_stop_no_wait()" in
            finalizer[:complete_branch] and
            "peak_cuda_stop_harvester()" in
            finalizer[complete_branch:timeout_branch] and
            "peak_cuda_stop_harvester()" not in finalizer[timeout_branch:] and
            "pthread_join" not in finalizer[timeout_branch:],
            "deadline expiry must stop without a blocking lock or join")
    incomplete_log = function_body(cuda, "peak_cuda_log_incomplete_records")
    require("kDiagnosticLimit = 16" in incomplete_log and
            "try_snapshot_active_leases" in incomplete_log and
            "diagnostics unavailable because slot ownership is busy" in
            incomplete_log and
            "CUDA incomplete event context=%p device=%d" in
            incomplete_log,
            "bounded timeout diagnostics must identify affected contexts/devices")
    harvest_one = function_body(cuda, "peak_cuda_harvest_one_current")
    require_order(
        harvest_one,
        "record_event_query_failure()",
        "peak_cuda_destroy_slot_events_current(&slot)",
        "if (destroyed)",
        "return PEAK_CUDA_HARVEST_RELEASE;",
    )
    harvest_pass = function_body(cuda, "peak_cuda_harvest_pass")
    require_order(
        harvest_pass,
        "peak_cuda_restore_context(&activation)",
        "if (restored)",
        "outcomes[index] == PEAK_CUDA_HARVEST_RELEASE",
        "peak_cuda_slot_allocator.release(leases[index])",
    )
    destroy_events = function_body(cuda, "peak_cuda_destroy_event_pool")
    destroy_slot = function_body(cuda,
                                 "peak_cuda_destroy_slot_events_current")
    require("slot->start != NULL" in destroy_slot and
            "slot->end != NULL" in destroy_slot and
            "if (destroyed)" in destroy_slot and
            "slot->initialized = FALSE;" in destroy_slot,
            "partial CUDA event creation must retain and retry each live handle")
    require("slot.start == NULL && slot.end == NULL" in destroy_events and
            "peak_cuda_activate_context(slot.owner_context" in destroy_events and
            "if (!peak_cuda_destroy_slot_events_current(&slot))" in
            destroy_events and
            "if (!peak_cuda_restore_context(&activation))" in destroy_events and
            "retaining CUDA event state" in destroy_events and
            "return FALSE;" in destroy_events and
            "return TRUE;" in destroy_events,
            "event destruction must occur in the owning context or retain state")
    require("if (capture_unsafe)" in destroy_events and
            "capture_unsafe && !peak_cuda_event_pool.empty()" not in
            destroy_events and
            destroy_events.find("if (capture_unsafe)") <
            destroy_events.find("for (PeakCudaEventSlot& slot"),
            "capture-unsafe teardown must retain state even when the event "
            "pool is empty")

    graph_renderer = function_body(cuda, "cuda_interceptor_print_graph_result")
    require_order(graph_renderer,
                  "g_mutex_lock(&cuda_graph_local_mapping_mutex)",
                  "g_hash_table_iter_init",
                  "g_mutex_unlock(&cuda_graph_local_mapping_mutex)")

    require_order(
        printer,
        "std::lock_guard<std::mutex> lifecycle_lock",
        "peak_cuda_finalize_pending()",
        "peak_cuda_build_report_snapshot()",
    )
    require("cuda_interceptor_print_kernel_result" not in printer,
            "kernel output must flow through the shared snapshot transport")

    print("cuda_interceptor_consistency_ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"CUDA interceptor consistency check failed: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
