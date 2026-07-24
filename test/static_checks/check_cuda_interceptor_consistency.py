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
    support_sources = {
        "src/dlopen_interceptor.c": ["dlopen"],
        "src/mpi_interceptor.c": ["PMPI_Finalize"],
        "src/peak.c": ["exit"],
        "src/pthread_listener.c": ["pthread_create", "pthread_join"],
        "src/syscall_interceptor.c": ["close"],
    }

    require("peak_resolve_function" not in general,
            "general listener must not use a loader-first resolver")
    require("peak_symbol_resolver" not in general,
            "general listener must not include the removed symbol resolver")
    require("dlsym(" not in general and "RTLD_" not in general,
            "general listener generic lookup must remain Frida-native")
    require("peak_general_listener_find_function(peak_hook_strings[i])" in general,
            "generic target lookup must use the Frida-native helper")
    generic_lookup = function_body(general,
                                   "peak_general_listener_find_function")
    require("gum_find_function(symbol)" in generic_lookup,
            "generic target lookup must keep Gum dynamic-binary resolution")
    require("gum_module_find_global_export_by_name" not in generic_lookup,
            "generic target lookup must not switch MPI ranks to export-only resolution")
    for relpath, symbols in support_sources.items():
        source = (repo_root / relpath).read_text(encoding="utf-8")
        for symbol in symbols:
            require(f'peak_general_listener_find_function("{symbol}")' in source,
                    f"{relpath} must use Frida-native support lookup for {symbol}")
            require(f'gum_find_function("{symbol}")' not in source,
                    f"{relpath} must not broad-scan support hook {symbol}")

    require('gum_find_function("' not in cuda,
            "CUDA support hooks must use the Frida-native lookup helper")
    require('gum_find_function("' not in general,
            "general listener special cases must use the Frida-native lookup helper")

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
    require(cuda.count("= peak_cuda_new_event_slot();") == 20,
            "each known CUDA wrapper must allocate start/end events through the safe helper")
    require("malloc(sizeof(cudaEvent_t))" not in cuda,
            "per-launch CUDA events must not use raw malloc")
    require("cudaEventCreate(start)" not in cuda and
            "cudaEventCreate(end)" not in cuda,
            "per-launch CUDA events must use peak_cuda_new_event_slot")

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
    require("keys[index] = (CUgraphExec_st*) key;" in cuda,
            "MPI graph reduction must not dereference direct pointer keys")
    require("cuda_graph_global_mapping" not in cuda,
            "MPI graph reduction must not aggregate process-local graph pointers globally")
    require("cuda_interceptor_print_graph_mpi_result" in cuda,
            "MPI graph reduction must print rank-qualified graph records")

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

    printer = function_body(cuda, "cuda_interceptor_print")
    require_order(
        printer,
        "std::lock_guard<std::mutex> lifecycle_lock",
        "peak_cuda_accepting_events.store(false",
        "cuda_sync_kernel_event()",
    )

    print("cuda_interceptor_consistency_ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"CUDA interceptor consistency check failed: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
