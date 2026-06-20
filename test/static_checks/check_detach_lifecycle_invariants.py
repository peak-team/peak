#!/usr/bin/env python3

import pathlib
import re
import sys


def require(condition, message):
    if not condition:
        print(message, file=sys.stderr)
        raise SystemExit(1)


def extract_function(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    require(match is not None, f"missing function {name}")
    start = match.start()
    brace = source.find("{", match.end() - 1)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    require(False, f"unterminated function {name}")


def check_shutdown_order(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_general_controller_shutdown_hook_unlocked")
    detach_positions = [
        match.start()
        for match in re.finditer(r"gum_interceptor_detach\s*\(\s*interceptor", body)
    ]
    require(detach_positions, "shutdown path has no Gum detach calls")
    for position in detach_positions:
        following_finish = body.find(
            "peak_general_controller_finish_hook_mutation",
            position,
        )
        require(following_finish != -1,
                "shutdown Gum detach must happen before controller finish/resume")


def check_safe_pc_alignment(repo_root):
    gum_source = (repo_root / "cmake/peak-gum/gum_peak_pc_api.c").read_text(
        encoding="utf-8"
    )
    controller_source = (repo_root / "src/peak_detach_controller.c").read_text(
        encoding="utf-8"
    )
    gum_body = extract_function(gum_source, "gum_interceptor_peak_safe_pc")
    controller_body = extract_function(
        controller_source, "peak_detach_controller_safe_pc_from_snapshot"
    )

    for label, body in (("Gum", gum_body), ("controller", controller_body)):
        require("state == GUM_PEAK_PC_IN_ENTER_TRAMPOLINE" in body,
                f"{label} safe-PC rule is not limited to enter trampoline state")
        require("pc == " in body and "on_enter_trampoline" in body,
                f"{label} safe-PC rule is not exact on_enter_trampoline")
        require("return NULL;" in body,
                f"{label} safe-PC rule must fail closed by default")

    require("return private_context->function_address;" in gum_body,
            "Gum safe-PC rule must redirect only to function entry")
    require("return snapshot->diagnostics.function_address;" in controller_body,
            "controller safe-PC rule must redirect only to function entry")


def check_support_hook_lifetimes(repo_root):
    checks = [
        ("src/mpi_interceptor.c", "mpi_interceptor_dettach", "mpi_interceptor"),
        ("src/syscall_interceptor.c", "syscall_interceptor_dettach", "syscall_interceptor"),
        ("src/peak.c", "exit_interceptor_detach", "exit_interceptor"),
    ]
    for rel, function, object_name in checks:
        source = (repo_root / rel).read_text(encoding="utf-8")
        body = extract_function(source, function)
        require(f"g_object_unref({object_name})" not in body,
                f"{function} must not unref Gum state during shutdown")
        require(f"{object_name} = NULL" not in body,
                f"{function} must keep Gum state pinned after revert")


def check_dlopen_revert_transactions(repo_root):
    source = (repo_root / "src/dlopen_interceptor.c").read_text(encoding="utf-8")
    body = extract_function(source, "dlopen_interceptor_dettach")
    for match in re.finditer(r"gum_interceptor_revert\s*\(\s*dlopen_interceptor", body):
        before = body[max(0, match.start() - 180):match.start()]
        after = body[match.end():match.end() + 180]
        require("gum_interceptor_begin_transaction(dlopen_interceptor)" in before,
                "dlopen revert is missing nearby begin_transaction")
        require("gum_interceptor_end_transaction(dlopen_interceptor)" in after,
                "dlopen revert is missing nearby end_transaction")


def check_stop_window_trace_gating(repo_root):
    source = (repo_root / "src/peak_detach_controller.c").read_text(
        encoding="utf-8"
    )
    gate = extract_function(
        source, "peak_detach_controller_trace_diagnostics_enabled"
    )
    started = extract_function(
        source, "peak_detach_controller_note_stop_window_started"
    )
    finished = extract_function(
        source, "peak_detach_controller_note_stop_window_finished"
    )
    last_window = extract_function(
        source, "peak_detach_controller_last_stop_window_us"
    )

    require("PEAK_DETACH_TRACE_PATH" in gate,
            "STOP-window diagnostics must be gated by PEAK_DETACH_TRACE_PATH")
    for label, body in (("start", started), ("finish", finished)):
        require("peak_detach_controller_trace_diagnostics_enabled()" in body,
                f"STOP-window {label} helper must check trace diagnostics")
        require("last_stop_window_us = 0.0" in body,
                f"STOP-window {label} helper must clear timing when disabled")
    require("return 0.0;" in last_window,
            "last_stop_window_us must return zero when diagnostics are disabled")
    require(last_window.find("return 0.0;") <
            last_window.find("peak_detach_controller_lock_mutation_guard"),
            "last_stop_window_us must gate before reading stored timing")


def check_general_controller_dlopen_drain_order(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_general_controller_thread_main")
    process_positions = [
        match.start()
        for match in re.finditer(
            r"\bpeak_general_controller_process_pending_unlocked\s*\(",
            body,
        )
    ]
    drain_positions = [
        match.start()
        for match in re.finditer(
            r"\bdlopen_interceptor_drain_dynamic_attach_queue\s*\(",
            body,
        )
    ]

    require(process_positions,
            "general controller thread must process pending target hooks")
    require(drain_positions,
            "general controller thread must drain dynamic dlopen attach queue")
    for position in drain_positions:
        require(any(process_position < position
                    for process_position in process_positions),
                "general controller must process pending target hooks before "
                "draining dynamic dlopen attach work")


def check_dlopen_test_hook_visibility(repo_root):
    header = (repo_root / "include/dlopen_interceptor.h").read_text(
        encoding="utf-8"
    )
    cmake = (repo_root / "src/CMakeLists.txt").read_text(encoding="utf-8")

    for function in (
        "dlopen_interceptor_drain_dynamic_attach_queue",
        "dlopen_interceptor_release_retained_dynamic_handles",
    ):
        declaration = re.search(
            r"(?:PEAK_DLOPEN_API\s+)?void\s+"
            + re.escape(function)
            + r"\s*\(",
            header,
        )
        require(declaration is not None,
                f"missing dlopen lifecycle declaration for {function}")
        require("PEAK_DLOPEN_API" not in declaration.group(0),
                f"{function} must not be default-visible production ABI")

    require("target_compile_definitions(peak PRIVATE PEAK_ENABLE_TEST_HOOKS=1)"
            in cmake,
            "PEAK_ENABLE_TEST_HOOKS must be private to libpeak test builds")


def check_shutdown_fail_closed_docs(repo_root):
    docs = (repo_root / "docs/physical-detach-controller.md").read_text(
        encoding="utf-8"
    )
    general = (repo_root / "src/general_listener.c").read_text(
        encoding="utf-8"
    )
    controller = (repo_root / "src/peak_detach_controller.c").read_text(
        encoding="utf-8"
    )

    require("A missing\nidle `SHUTDOWN` response fails closed" in docs,
            "shutdown docs must describe idle SHUTDOWN fail-closed retention")
    require("helper release/resume failure\nafter a STOP window" in docs,
            "shutdown docs must keep post-mutation release failure fatal")
    require("missing response\nor failed detach is fatal" not in docs,
            "shutdown docs still describe idle SHUTDOWN failure as fatal")
    require("bucket=%s status=%s attempts=%u; leaving listener state alive"
            in general,
            "shutdown fail-closed log must include bucket/status/attempts")
    require("detach helper shutdown failed: %s; leaving listener state alive"
            in general,
            "idle helper shutdown failure must retain listener state")
    require("detach helper was unavailable during idle shutdown" in controller,
            "controller must identify idle helper shutdown failure separately")


def check_signal_backend_strict_invariants(repo_root):
    controller = (repo_root / "src/peak_detach_controller.c").read_text(
        encoding="utf-8"
    )
    pthread_listener = (repo_root / "src/pthread_listener.c").read_text(
        encoding="utf-8"
    )
    tests = (repo_root / "test/CMakeLists.txt").read_text(encoding="utf-8")
    controller_tests = (
        repo_root / "test/detach_controller/CMakeLists.txt"
    ).read_text(encoding="utf-8")

    signal_handler = extract_function(
        controller, "peak_detach_controller_signal_handler"
    )
    signal_release = extract_function(
        controller, "peak_detach_controller_signal_release"
    )
    signal_stop = extract_function(
        controller, "peak_detach_controller_signal_stop_threads"
    )
    signal_evacuate = extract_function(
        controller, "peak_detach_controller_signal_evacuate"
    )
    pthread_start = extract_function(pthread_listener, "peak_pthread_start")

    require("_Atomic int rewrite_status" in controller,
            "signal backend must keep observable per-thread rewrite status")
    require("rewrite_ok ? 1 : -1" in signal_handler,
            "signal handler must publish PC rewrite success/failure")
    require("rewrite_status" in signal_release and "return FALSE" in signal_release,
            "signal release must fail if an intended PC rewrite did not succeed")
    require("peak_detach_controller_signal_release_or_fatal" in controller,
            "signal cleanup failures must use release-or-fatal helper")
    for token in (
        "signal stop tgkill failure",
        "signal stop timeout",
        "signal stop verification failure",
        "signal stop snapshot overflow",
    ):
        require(token in signal_stop,
                f"signal stop cleanup path missing fatal context: {token}")
    require("strict_mutation_thread_gate" in controller,
            "strict signal/helper mutation windows must gate new pthread starts")
    require("strict_mutation_thread_gate_installed" in controller,
            "signal backend must know whether pthread_create gate is installed")
    require("peak_detach_controller_note_thread_creation_gate_installed" in controller,
            "pthread listener must be able to publish gate installation")
    require("pthread_create interception is not installed" in controller,
            "signal backend must fail closed when pthread_create gate is unavailable")
    require("peak_detach_controller_note_thread_creation_gate_installed(TRUE)" in pthread_listener,
            "pthread listener must publish successful pthread_create hook installation")
    require("peak_detach_controller_begin_thread_creation_gate" in controller,
            "controller must begin the new-thread gate before STOP")
    require("peak_detach_controller_end_thread_creation_gate" in controller,
            "controller must release the new-thread gate on finish/failure")
    require("peak_detach_controller_wait_for_mutation_window" in pthread_start,
            "pthread start wrapper must wait for strict mutation windows")
    require(pthread_start.find("peak_detach_controller_wait_for_mutation_window") <
            pthread_start.find("ret = start_routine"),
            "pthread start wrapper must gate before entering user code")
    require(signal_evacuate.find(
                "peak_detach_controller_signal_verify_no_unheld_threads") <
            signal_evacuate.find(
                "peak_detach_controller_signal_write_memory"),
            "signal evacuate must revalidate held threads before patch writes")
    capture_snapshot = extract_function(
        controller, "peak_detach_controller_capture_gum_snapshot"
    )
    require("needs_existing_hook_context" in capture_snapshot and
            "PEAK_DETACH_OPERATION_REATTACH" in capture_snapshot and
            "PEAK_DETACH_STATUS_CLASSIFY_FAILED" in capture_snapshot and
            "return FALSE" in capture_snapshot,
            "missing Gum PC diagnostics must fail closed for existing-hook mutations")
    for test_name in (
        "test_detach_hotloop_signal_strict",
        "test_detach_hotloop_signal_thread_spawn_strict",
        "test_detach_stale_threads_signal_strict",
        "test_detach_stale_threads_signal_unrelated_spin_strict",
    ):
        require(test_name in tests,
                f"missing signal strict runtime test {test_name}")
    require("test_detach_controller_signal_backend_blocked_thread" in controller_tests,
            "missing blocked-signal fail-closed controller test")
    require("test_detach_controller_signal_backend_missing_thread_gate" in controller_tests,
            "missing signal backend missing-pthread-gate fail-closed controller test")
    require("PEAK_DETACH_TRACE_PATH" in tests and
            "signal-thread-spawn-trace" in tests and
            "trace_detach_success=1" in tests,
            "transient signal pthread test must prove trace-backed mutation evidence")
    benchmark_runner = (repo_root / "benchmarks/detach/run_detach_hotloop_stress.py").read_text(encoding="utf-8")
    require("--detach-backend" in benchmark_runner,
            "hotloop benchmark must support backend-pinned signal stress")
    require("PEAK_DETACH_BACKEND=helper" in tests and
            "PEAK_DETACH_BACKEND=helper" in controller_tests,
            "fake-helper tests must force helper backend, not auto signal fallback")


def main():
    if len(sys.argv) != 2:
        print("usage: check_detach_lifecycle_invariants.py <repo-root>",
              file=sys.stderr)
        return 2
    repo_root = pathlib.Path(sys.argv[1]).resolve()
    check_shutdown_order(repo_root)
    check_safe_pc_alignment(repo_root)
    check_support_hook_lifetimes(repo_root)
    check_dlopen_revert_transactions(repo_root)
    check_stop_window_trace_gating(repo_root)
    check_general_controller_dlopen_drain_order(repo_root)
    check_dlopen_test_hook_visibility(repo_root)
    check_shutdown_fail_closed_docs(repo_root)
    check_signal_backend_strict_invariants(repo_root)
    print("detach_lifecycle_invariants_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
