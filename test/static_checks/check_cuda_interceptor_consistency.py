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
    match = re.search(rf'\b{name}\s*\([^)]*\)\s*\{{', source)
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
        require(f'peak_general_listener_find_function("{hook}")' in cuda,
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

    require(cuda.count("PeakCudaInflightGuard in_flight;") == 10,
            "each known CUDA wrapper must use in-flight accounting")
    require(cuda.count("peak_cuda_acquire_event_pair(&start, &end)") == 10,
            "each known CUDA wrapper must admit one bounded event-pool lease")
    require(cuda.count("if (!peak_cuda_record_event(") == 20,
            "each wrapper must handle both start and end event-record failures")
    require("launches.reserve(" not in cuda,
            "per-key launch vectors must not reserve beyond the shared pool budget")
    require(re.search(r'^\s*#\s*define\s+(?:min|max)\s*\(', cuda_header,
                      re.MULTILINE) is None,
            "CUDA public header must not poison C++ standard min/max names")
    require(cuda.find("#include <algorithm>") != -1 and
            cuda.find("#include <algorithm>") <
            cuda.find("#include \"cuda_interceptor.h\"") and
            cuda.count("std::max(") == 5 and cuda.count("std::min(") == 5,
            "CUDA launch statistics must use standard min/max without macro pollution")
    require("cudaEventRecord(*event, stream) != cudaSuccess" in cuda and
            "cudaEventElapsedTime(&ms" in cuda and
            "!= cudaSuccess" in cuda,
            "CUDA event record/elapsed failures must be checked")
    require("record_timing_error" in cuda and
            "CUDA profiler drops [timing_error]" in cuda,
            "timing failures must be counted and transported")
    require("peak_cuda_new_event_slot" not in cuda,
            "per-launch CUDA event allocation must not bypass the bounded pool")
    require("PEAK_CUDA_EVENT_POOL_CAPACITY" in cuda and
            "peak_cuda_event_pool_capacity" in cuda,
            "CUDA event-pool capacity must be configurable and bounded")
    require("PeakCudaProfilerState" in cuda and
            "peak_cuda_profiler_state.acquire_slot()" in cuda,
            "CUDA wrappers must share bounded identity/admission state")
    require("*(char**)((size_t)func" not in cuda and
            "(size_t)func + 8" not in cuda,
            "Driver CUfunction handles must not be read through private layouts")
    require('peak_general_listener_find_function("cuFuncGetName")' in cuda,
            "Driver names must use dynamically-resolved cuFuncGetName")
    for forbidden_mpi_call in ("MPI_Init(", "MPI_Reduce(", "MPI_Gather(",
                               "MPI_Gatherv("):
        require(forbidden_mpi_call not in cuda,
                f"CUDA interceptor must not own {forbidden_mpi_call} transport")

    kernel_wrappers = CUDA_WRAPPERS[:8]
    for wrapper in kernel_wrappers:
        body = function_body(cuda, wrapper)
        require("peak_cuda_identify_kernel" in body and
                "if (!identity.target_match)" in body,
                f"{wrapper} must filter a cached kernel identity")
        require_order(body, "if (!identity.target_match)",
                      "peak_cuda_acquire_event_pair(&start, &end)")
    for wrapper in CUDA_WRAPPERS[8:]:
        require("peak_cuda_acquire_event_pair(&start, &end)" in
                function_body(cuda, wrapper),
                f"{wrapper} must use the shared bounded graph-event pool")
    for wrapper in CUDA_WRAPPERS:
        body = function_body(cuda, wrapper)
        first_guard = body.find("if (!peak_cuda_record_event(")
        second_guard = body.find("if (!peak_cuda_record_event(", first_guard + 1)
        require(first_guard != -1 and second_guard != -1,
                f"{wrapper} must check start and end event records")
        start_block = body[first_guard:second_guard]
        end_block = body[second_guard:]
        require("peak_cuda_release_event_pair(start, end);" in start_block and
                "return original_" in start_block,
                f"{wrapper} start-record failure must release and call original once")
        require("peak_cuda_release_event_pair(start, end);" in end_block and
                "return result;" in end_block,
                f"{wrapper} end-record failure must release without rerunning original")

    require("std::mutex peak_kernel_event_map_mutex" in cuda,
            "missing kernel event-map mutex")
    require("std::mutex peak_graph_event_map_mutex" in cuda,
            "missing graph event-map mutex")
    require("std::atomic_bool peak_cuda_accepting_events" in cuda,
            "missing accepting-events gate")
    require("std::atomic_uint peak_cuda_in_flight" in cuda,
            "missing in-flight counter")
    require("static gboolean peak_cuda_hooks_reverted" in cuda,
            "missing physical-detach state flag")
    require("g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free)" in cuda,
            "CUDA graph maps must use pointer equality")
    require("g_int_equal" not in cuda,
            "CUDA graph pointer maps must not use g_int_equal")
    require("Graph executable handles are rank-local" in cuda,
            "graph records must document their rank-local policy")
    require("peak_cuda_build_report_snapshot" in cuda and
            "peak_cuda_render_report_snapshot" in cuda,
            "CUDA must own an isolated transport snapshot and renderer")
    cuda_snapshot = function_body(cuda, "peak_cuda_build_report_snapshot")
    require("snapshot->overhead = peak_report_snapshot_get_transport_overhead();"
            in cuda_snapshot,
            "CUDA snapshots must copy the CPU transport-overhead context")
    require("snapshot->rank_count = 1;" in cuda_snapshot,
            "CUDA local snapshots must begin with a single local rank")
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
        "peak_cuda_accepting_events.store(false",
        "gum_interceptor_begin_transaction(cuda_interceptor)",
        "gum_interceptor_end_transaction(cuda_interceptor)",
        "peak_cuda_accepting_events.store(true",
    )

    detach = function_body(cuda, "cuda_interceptor_dettach")
    require_order(
        detach,
        "peak_cuda_accepting_events.store(false",
        "gum_interceptor_begin_transaction(cuda_interceptor)",
        "gum_interceptor_end_transaction(cuda_interceptor)",
        "gum_interceptor_flush(cuda_interceptor)",
        "peak_cuda_hooks_reverted = TRUE",
        "peak_cuda_clear_hook_pointers()",
        "peak_cuda_drain_kernel_event_map(FALSE)",
        "peak_cuda_drain_graph_event_map(FALSE)",
        "g_hash_table_destroy(cuda_kernel_local_dim_mapping)",
        "g_hash_table_destroy(cuda_graph_local_mapping)",
    )
    require("g_object_unref(cuda_interceptor)" not in detach,
            "CUDA physical detach must retain Gum trampoline state")
    require("cuda_interceptor = NULL" not in detach,
            "CUDA physical detach must keep interceptor state referenced")
    active_guard = detach.find("if (active_cuda_wrappers != 0)")
    drain = detach.find("peak_cuda_drain_kernel_event_map(FALSE)")
    require(active_guard != -1 and drain != -1 and active_guard < drain and
            "return;" in detach[active_guard:drain],
            "active CUDA wrappers must retain pool/maps for a safe cleanup retry")

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

    sync = function_body(cuda, "cuda_sync_kernel_event")
    require_order(
        sync,
        "cudaDeviceSynchronize()",
        "peak_cuda_drain_kernel_event_map(TRUE)",
        "peak_cuda_drain_graph_event_map(TRUE)",
    )
    require("free(launch.start_event)" not in sync,
            "sync must drain through ownership helpers, not free inline")

    require_order(
        printer,
        "std::lock_guard<std::mutex> lifecycle_lock",
        "peak_cuda_accepting_events.store(false",
        "cuda_sync_kernel_event()",
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
