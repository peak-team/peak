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


def check_peak_init_heartbeat_order(repo_root):
    source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_init")

    main_time_position = body.find("peak_main_time = peak_second();")
    heartbeat_position = body.find("pthread_create(&heartbeat_thread")
    require(main_time_position != -1,
            "peak_init must initialize peak_main_time")
    require(heartbeat_position != -1,
            "peak_init must create the heartbeat thread explicitly")
    require(main_time_position < heartbeat_position,
            "peak_main_time must be initialized before heartbeat thread startup")


def check_mpi_finalize_trampoline_default(repo_root):
    source = (repo_root / "src/mpi_interceptor.c").read_text(encoding="utf-8")
    body = extract_function(source, "mpi_interceptor_direct_finalize_enabled")

    empty_env = body.find("value == NULL || value[0] == '\\0'")
    default_trampoline = body.find("return 0;", empty_env)
    default_direct = body.find("return 1;", empty_env)
    require(empty_env != -1 and default_trampoline != -1,
            "PMPI_Finalize default must use the Gum original trampoline")
    require(default_direct == -1 or default_trampoline < default_direct,
            "PMPI_Finalize must not restore the replacement by default")

    peak_source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    guard = extract_function(peak_source, "peak_mpi_real_finalize_default_allowed")
    require("PEAK_MPI_REAL_FINALIZE_ENV" in guard and
            "peak_env_value_truthy(value)" in guard,
            "real MPI finalizer policy must preserve explicit env override")
    require("peak_mpi_runtime_matches_intel_mpi" in guard and
            "return !peak_mpi_runtime_matches_intel_mpi();" in guard,
            "Intel MPI must fail closed by default after PEAK output")
    vendor = extract_function(peak_source, "peak_mpi_runtime_matches_intel_mpi")
    require("MPI_Get_library_version" in vendor and
            "Intel(R) MPI" in vendor and
            "Intel MPI" in vendor,
            "Intel MPI finalize guard must inspect MPI library version")


def check_stop_window_trace_gating(repo_root):
    source = (repo_root / "src/peak_detach_controller.c").read_text(
        encoding="utf-8"
    )
    general = (repo_root / "src/general_listener.c").read_text(
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
    controller_trace_configure = extract_function(
        source, "peak_detach_controller_configure_trace_diagnostics"
    )
    general_trace_enabled = extract_function(
        general, "peak_general_controller_trace_enabled"
    )
    general_trace_detail = extract_function(
        general, "peak_general_controller_trace_mutation_detail"
    )
    general_trace_init = extract_function(
        general, "peak_general_controller_init_trace_config_once"
    )
    general_attach_supported = extract_function(
        general, "peak_general_listener_attach_target_is_supported"
    )
    general_listener_attach = extract_function(
        general, "peak_general_listener_attach"
    )
    startup_skip = extract_function(
        general, "peak_general_listener_startup_attach_can_skip_stop"
    )
    general_attach_policy_init = extract_function(
        general, "peak_general_listener_init_attach_policy_once"
    )

    require("trace_diagnostics_enabled" in gate,
            "STOP-window diagnostics must be gated by cached trace diagnostics")
    require("getenv(" not in gate and "g_getenv(" not in gate,
            "STOP-window diagnostics must not read environment in mutation path")
    require("getenv(" not in controller_trace_configure and
            "g_getenv(" not in controller_trace_configure,
            "detach controller trace configuration must not read environment")
    require("atomic_store_explicit(&trace_diagnostics_enabled" in source,
            "detach controller trace configuration must cache explicit state")
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
    require("PEAK_DETACH_TRACE_PATH" in general_trace_init,
            "general listener must snapshot PEAK_DETACH_TRACE_PATH during init")
    require("peak_detach_controller_configure_trace_diagnostics" in general_trace_init,
            "general listener must configure detach-controller trace diagnostics")
    for label, body in (("general trace gate", general_trace_enabled),
                        ("general trace detail", general_trace_detail)):
        require("getenv(" not in body and "g_getenv(" not in body,
                f"{label} must use cached trace configuration")
    require("PEAK_ALLOW_UNSAFE_GUM_PROLOGUE" in general_attach_policy_init,
            "general listener must snapshot unsafe Gum prologue override during init")
    require("PEAK_UNSAFE_GUM_PROLOGUE_POLICY" in general_attach_policy_init,
            "general listener must snapshot unsafe Gum prologue policy during init")
    require("getenv(" not in general_attach_supported and
            "g_getenv(" not in general_attach_supported,
            "Gum attach support predicate must use cached attach policy")
    require("peak_unsafe_gum_prologue_check" in general_attach_supported,
            "Gum attach support predicate must delegate prologue policy checks")
    require("peak_general_listener_init_attach_policy();" in general,
            "general listener attach must initialize cached attach policy")
    require('opendir("/proc/self/task")' in startup_skip and
            "task_count > 1" in startup_skip and
            "return task_count == 1;" in startup_skip,
            "startup attach stop-skip must be proven by single-thread /proc task count")
    require("startup_attach_can_skip_stop" in general_listener_attach and
            "!startup_attach_can_skip_stop &&" in general_listener_attach,
            "initial attach must skip the stop backend only after a single-thread proof")

    support_attach_supported = extract_function(
        general, "peak_general_listener_support_attach_target_is_supported"
    )
    syscall = (repo_root / "src/syscall_interceptor.c").read_text(
        encoding="utf-8"
    )
    dlopen = (repo_root / "src/dlopen_interceptor.c").read_text(
        encoding="utf-8"
    )
    dlopen_attach = extract_function(dlopen, "dlopen_interceptor_attach")
    dlopen_dynamic = extract_function(
        dlopen, "dlopen_interceptor_attach_from_request"
    )
    require("peak_unsafe_gum_prologue_check" not in support_attach_supported and
            "peak_unsafe_gum_support_prologue_check" not in support_attach_supported and
            "peak_gum_prologue_too_short_for_attach" not in support_attach_supported,
            "support replacements must not apply user-target prologue guards")
    require("peak_general_listener_support_attach_target_is_supported" in syscall,
            "close support replacement must call the support attach predicate")
    require("peak_general_listener_attach_target_is_supported" in dlopen_attach,
            "dlopen replacement must use normal target prologue policy so dynamic attach is not disabled by support-only early-return guards")
    require("peak_general_listener_support_attach_target_is_supported" not in dlopen_attach,
            "dlopen replacement must not use support-only prologue policy")
    require("peak_general_listener_attach_target_is_supported" in dlopen_dynamic,
            "dynamic dlopen user targets must use normal target prologue policy")
    require("peak_general_listener_support_attach_target_is_supported" not in dlopen_dynamic,
            "dynamic dlopen user targets must not use support prologue policy")


def check_mpi_startup_helper_warmup(repo_root):
    source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_init")

    check_mpi_position = body.find("found_MPI = check_MPI();")
    warmup_position = body.find("peak_detach_controller_warmup_backend();")
    require(check_mpi_position != -1 and warmup_position != -1,
            "peak_init must keep explicit MPI detection and helper warmup")
    require(check_mpi_position < warmup_position,
            "helper warmup must not run before MPI detection")
    warmup_context = body[max(0, warmup_position - 180):warmup_position + 80]
    require("!found_MPI" in warmup_context,
            "helper warmup must be suppressed for MPI-linked programs before PMPI_Init")


def check_global_detach_overhead_selection(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    heartbeat = extract_function(source, "peak_heartbeat_monitor")
    comparator = extract_function(source, "compare_ratio_de")
    wait_helper = extract_function(source, "peak_heartbeat_wait_us")

    global_detach_marker = "// 2) Global DETACH"
    reattach_marker = "// 3) Reattach"
    start = heartbeat.find(global_detach_marker)
    end = heartbeat.find(reattach_marker, start)
    require(start != -1 and end != -1,
            "heartbeat must keep explicit global detach and reattach sections")
    global_detach = heartbeat[start:end]

    require("compare_rate_de" not in source,
            "global detach must not sort by transient overhead rate")
    require("compare_ratio_de" in global_detach,
            "global detach must sort candidates by actual overhead ratio")
    require("ratio_snapshot[i] <= target_profile_ratio" in global_detach and
            "continue;" in global_detach,
            "global detach must not enqueue below-threshold one-shot/cold targets")
    require("entries[k].ratio <= target_profile_ratio" in global_detach and
            "break;" in global_detach,
            "global detach loop must fail closed if a below-threshold candidate appears")
    require(comparator.find("x->ratio") < comparator.find("x->rate"),
            "global detach comparator must prioritize ratio before rate")
    initial_wait = heartbeat.find("peak_heartbeat_wait_us(initial_sleep_us)")
    loop_start = heartbeat.find("while (atomic_load(&heartbeat_running))")
    require("pthread_cond_timedwait" in wait_helper,
            "heartbeat wait helper must use the existing condition wait")
    require(initial_wait != -1 and loop_start != -1 and initial_wait < loop_start,
            "heartbeat must wait one interval before the first detach decision")
    require("PEAK_HEARTBEAT_MIN_OBSERVATION_US" in source and
            "min_detach_observation_time" in heartbeat and
            "detach_observation_ready" in heartbeat and
            "total_execution_time >= min_detach_observation_time" in heartbeat,
            "heartbeat detach must require a minimum observation window")
    require("detach_observation_ready && enable_per_target_heartbeat" in heartbeat and
            "detach_observation_ready && enable_global_heartbeat" in heartbeat,
            "heartbeat detach gates must use the minimum observation window")


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


def check_exclusive_time_nonnegative(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    helper = extract_function(
        source, "peak_general_listener_exclusive_duration"
    )
    pop = extract_function(source, "peak_general_listener_pop_invocation")
    enter = extract_function(source, "peak_general_listener_on_enter")
    leave = extract_function(source, "peak_general_listener_on_leave")
    output = extract_function(source, "peak_general_listener_print_result")
    sanitize = extract_function(
        source, "peak_general_listener_sanitize_output_times"
    )

    require("gulong stack_level" in source,
            "invocation data must remember its callback stack level")
    require("priv->stack_level = thread_data.level" in enter,
            "on_enter must snapshot the invocation stack level")
    require("thread_data.level < priv->stack_level" in pop and
            "thread_data.level > priv->stack_level" in pop,
            "pop helper must detect/collapse non-LIFO callback stack state")
    require("child_duration >= total_duration" in helper,
            "exclusive duration helper must clamp child-over-parent timing")
    require("return 0.0;" in helper,
            "exclusive duration helper must return zero for underflow")
    require("total_duration - child_duration" in helper,
            "exclusive duration helper must preserve positive self time")
    require(leave.count("peak_general_listener_exclusive_duration") >= 2,
            "on_leave must use the exclusive duration clamp in both fast and "
            "strict/detach paths")
    require("end_time - thread_data.child_time[thread_data.level]" not in leave,
            "on_leave must not accumulate open-coded negative exclusive time")
    require("sum_exclusive_time[i] < 0.0" in sanitize and
            "sum_exclusive_time[i] = 0.0" in sanitize,
            "output must clamp negative exclusive times after aggregation")
    require("sum_exclusive_time[i] > sum_total_time[i]" in sanitize,
            "output must clamp exclusive time to total time after aggregation")
    require("peak_general_listener_sanitize_output_times" in output and
            output.find("peak_general_listener_sanitize_output_times") <
            output.find("peak_general_listener_export_csv_result"),
            "output time sanitization must run before CSV/text printing")


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
    signal_policy = (repo_root / "src/peak_signal_policy.c").read_text(
        encoding="utf-8"
    )
    signal_public_header = (
        repo_root / "include/peak_signal_policy.h"
    ).read_text(encoding="utf-8")
    signal_internal_header = (
        repo_root / "src/peak_signal_policy_internal.h"
    ).read_text(encoding="utf-8")
    pthread_listener = (repo_root / "src/pthread_listener.c").read_text(
        encoding="utf-8"
    )
    tests = (repo_root / "test/CMakeLists.txt").read_text(encoding="utf-8")
    controller_tests_cmake = (
        repo_root / "test/detach_controller/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    controller_tests = (
        repo_root / "test/detach_controller/test_detach_controller.c"
    ).read_text(encoding="utf-8")
    runtime_hotloop = (
        repo_root / "test/detach_runtime/test_detach_hotloop.c"
    ).read_text(encoding="utf-8")
    signal_handler = extract_function(
        controller, "peak_detach_controller_signal_handler"
    )
    signal_wait_for_release = extract_function(
        controller, "peak_detach_controller_signal_wait_for_release"
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
    controller_mode = extract_function(controller, "peak_detach_controller_mode")

    require("PEAK_SAFE_DETACH_MODE_STRICT" in controller_mode and
            '"compatibility"' not in controller_mode and
            '"legacy"' not in controller_mode,
            "strict-auto must be the only runtime-selected detach mode")
    require("_Atomic int rewrite_status" in controller,
            "signal backend must keep observable per-thread rewrite status")
    require("peak_detach_controller_signal_wait_for_release" in signal_handler and
            "rewrite_ok ? 1 : -1" in signal_wait_for_release,
            "signal handler must publish PC rewrite success/failure")
    require("rewrite_status" in signal_release and "return FALSE" in signal_release,
            "signal release must fail if an intended PC rewrite did not succeed")
    require("peak_detach_controller_signal_release_or_fatal" in controller,
            "signal cleanup failures must use release-or-fatal helper")
    for token in (
        "signal stop send failure",
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
    require("peak_signal_policy_choose_reserved_signal" in controller and
            "peak_signal_policy_cookie_matches_async" in signal_handler and
            "peak_signal_policy_send_thread_signal" in signal_stop and
            "SYS_rt_tgsigqueueinfo" in signal_policy,
            "signal backend must reserve a signal and authenticate stop delivery with rt_tgsigqueueinfo cookies")
    require("peak_signal_policy_cookie_matches(" not in signal_handler and
            "peak_signal_policy_cookie_for_preinitialized" in signal_policy and
            "base == 0" in signal_policy,
            "signal handler cookie authentication must use only preinitialized async-safe state")
    require("peak_signal_policy_cookie_for" not in signal_public_header and
            "peak_signal_policy_cookie_matches" not in signal_public_header and
            "peak_signal_policy_send_thread_signal" not in signal_public_header and
            "PEAK_SIGNAL_POLICY_INTERNAL" in signal_internal_header,
            "signal stop cookie helpers must be internal, not public user-callable API")
    require("peak_signal_policy_atomics_lock_free" in signal_internal_header and
            "peak_signal_policy_atomics_lock_free()" in controller and
            "static _Atomic int signal_backend_signum" in controller and
            "atomic_is_lock_free(&signal_backend_signum)" in controller and
            "atomic_is_lock_free(&signal_slot_count)" in controller and
            "atomic_is_lock_free(&signal_slots[0].tid)" in controller and
            "atomic_is_lock_free(&unexpected_delivery_count)" in signal_policy and
            "atomic_is_lock_free(&cookie_base)" in signal_policy,
            "signal backend support must include signal-policy atomics used from handler context")
    require("peak_signal_policy_protective_handler" in signal_policy and
            "peak_signal_policy_install_protective_handler" in signal_policy and
            "peak_signal_policy_clear_reserved_signal" in signal_policy and
            "peak_signal_policy_clear_reserved_signal" in controller,
            "signal reservation must install a protective handler and clear dead leases on setup failure")
    require("peak_detach_controller_signal_handler_is_installed" in controller and
            "peak_signal_policy_unexpected_delivery_count" in controller,
            "signal backend must revalidate handler ownership and contamination before strict stops")
    require("signal-unexpected-delivery" in controller and
            "signal-handler-not-installed" in controller,
            "signal backend contamination and stolen-handler failures must have concrete trace reasons")
    require("peak_detach_controller_signal_tid_blocks_reserved" in signal_stop and
            "signal-reserved-blocked" in controller,
            "signal backend must fail fast when the reserved signal is truly blocked")
    for wrapper in (
        "sigaction",
        "signal",
        "pthread_sigmask",
        "sigprocmask",
        "sigwait",
        "sigwaitinfo",
        "sigtimedwait",
        "signalfd",
        "timer_create",
        "kill",
        "pthread_kill",
        "sigqueue",
        "raise",
        "sigsuspend",
        "pselect",
        "ppoll",
        "mq_notify",
        "aio_read",
        "aio_write",
        "aio_fsync",
        "lio_listio",
    ):
        require(f'__attribute__((visibility("default"))) int\n{wrapper}' in signal_policy or
                f'__attribute__((visibility("default"))) void (*{wrapper}' in signal_policy,
                f"signal policy must export {wrapper}")
    require('__asm__("syscall")' in signal_policy and
            "peak_signal_policy_syscall" in signal_policy and
            "peak_signal_policy_safe_read" in signal_policy and
            "/proc/self/maps" not in signal_policy and
            "SYS_pread64" in signal_policy and
            "SYS_process_vm_readv" in signal_policy and
            "syscall:rt_sigprocmask" in signal_policy and
            "syscall:rt_sigaction" in signal_policy and
            "syscall:rt_sigtimedwait" in signal_policy and
            "syscall:signalfd4" in signal_policy and
            "syscall:timer_create" in signal_policy and
            "syscall:mq_notify" in signal_policy and
            "syscall:tgkill" in signal_policy and
            "syscall:rt_tgsigqueueinfo" in signal_policy,
            "signal policy must guard common raw syscall routes for reserved RT signal collisions")
    require("peak_signal_policy_should_hide_raw_sigaction_query" in signal_policy and
            "PeakSignalPolicyRawSigaction action" in signal_policy and
            "errno = EFAULT" in signal_policy and
            "SYS_pwrite64" not in signal_policy and
            "raw_query_protected=1" in runtime_hotloop and
            "raw_query_readonly_failed=1" in runtime_hotloop and
            "raw rt_sigaction query leaked non-default PEAK action fields" in runtime_hotloop,
            "raw rt_sigaction query must hide or fail closed before exposing reserved-signal semantics")
    require("peak_signal_policy_event_signal" in signal_policy and
            '"timer_create"' in signal_policy and
            "peak_signal_policy_prepare_event_for_user(evp" in signal_policy and
            "SIGEV_THREAD_ID" in signal_policy,
            "timer_create wrapper must migrate explicit SIGEV_SIGNAL/SIGEV_THREAD_ID collisions")
    require('"mq_notify"' in signal_policy and
            '"aio_read"' in signal_policy and
            '"aio_write"' in signal_policy and
            '"aio_fsync"' in signal_policy and
            '"lio_listio"' in signal_policy and
            "peak_signal_policy_collect_lio_event_signals" in signal_policy,
            "signal policy must migrate POSIX mqueue/AIO sigevent collisions")
    require("peak_signal_policy_migrate_reserved_signal_locked" in signal_policy and
            "peak_signal_policy_prepare_reserved_set_for_user" in signal_policy and
            "peak_signal_policy_prepare_reserved_signal_for_user" in signal_policy and
            "peak_signal_policy_migration_count" in signal_public_header and
            "migration_candidate_signal" in signal_policy and
            "migration_releasing_signal" in signal_policy and
            "sigdelset" not in signal_policy,
            "signal policy must migrate away from explicit user collisions with transition guards instead of silently sanitizing user sets")
    require("real_symbols_once" in signal_policy and
            "pthread_once(&real_symbols_once" in signal_policy and
            "peak_signal_policy_ensure_real_symbols();\n    if (real_sigaction_fn == NULL)" in signal_policy,
            "signal policy wrapper resolution must be one-time and thread-safe")
    require("cookie_once" in signal_policy and
            "pthread_once(&cookie_once" in signal_policy,
            "signal policy cookie base must be initialized exactly once")
    require("PEAK_REQUIRE_SAFE_DETACH" not in signal_policy and
            "PEAK_SIGNAL_RESERVE_EARLY" in signal_policy and
            "forced-only" in signal_policy and
            "mode == NULL" in signal_policy and
            "strcasecmp(mode, \"strict\") == 0" in signal_policy and
            "strcasecmp(mode, \"auto\") == 0" in signal_policy and
            "strcasecmp(backend, \"signal\") == 0" in signal_policy and
            "strcasecmp(mode, \"helper\") == 0" in signal_policy,
            "signal policy must reserve early for strict-auto but not helper-only mode")
    require("test_detach_controller_signal_reserve_early_never" in controller_tests_cmake and
            "PEAK_SIGNAL_RESERVE_EARLY=never" in controller_tests_cmake and
            "signal-reserve-early-never" in controller_tests and
            "test_detach_controller_signal_reserve_helper_auto" in controller_tests_cmake and
            "PEAK_DETACH_BACKEND=helper;PEAK_DETACH_SIGNAL=auto" in controller_tests_cmake and
            "signal-reserve-helper-auto" in controller_tests,
            "signal reserve-early compatibility knob must have controller-level coverage")
    require("peak_detach_controller_note_thread_creation_gate_installed(TRUE)" in pthread_listener,
            "pthread listener must publish successful pthread_create hook installation")
    require("peak_detach_controller_begin_thread_creation_gate" in controller,
            "controller must begin the new-thread gate before STOP")
    require("peak_detach_controller_end_thread_creation_gate" in controller,
            "controller must release the new-thread gate on finish/failure")
    require("peak_signal_policy_push_migration_disabled" in controller and
            "peak_signal_policy_pop_migration_disabled" in controller and
            "peak_detach_controller_install_signal_backend_handler" in controller and
            "peak_signal_policy_reserved_signal" in controller,
            "controller must disable migration during mutation windows and resync migrated signal leases")
    require("peak_detach_controller_wait_for_mutation_window" in pthread_start,
            "pthread start wrapper must wait for strict mutation windows")
    require("peak_signal_policy_unblock_reserved_for_current_thread" in pthread_start,
            "pthread start wrapper must unblock PEAK reserved signal before user code")
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
        "test_detach_hotloop_signal_blocked_delivery_strict",
        "test_detach_hotloop_signal_user_collision_strict",
        "test_detach_hotloop_signal_bad_cookie_strict",
        "test_detach_hotloop_signal_pthread_create_gate_strict",
        "test_detach_stale_threads_signal_strict",
        "test_detach_stale_threads_signal_unrelated_spin_strict",
    ):
        require(test_name in tests,
                f"missing signal strict runtime test {test_name}")
    require("test_detach_controller_signal_backend_blocked_thread" in controller_tests_cmake,
            "missing blocked-signal fail-closed controller test")
    require("test_detach_controller_signal_backend_missing_thread_gate" in controller_tests_cmake,
            "missing signal backend missing-pthread-gate fail-closed controller test")
    require("PEAK_DETACH_TRACE_PATH" in tests and
            "signal-thread-spawn-trace" in tests and
            "trace_detach_success=1" in tests,
            "transient signal pthread test must prove trace-backed mutation evidence")
    require("--signal-blocked-delivery-check" in tests and
            "signal_blocked_delivery_ok" in tests and
            "--signal-user-collision-check" in tests and
            "signal_user_collision_ok" in tests and
            "--signal-bad-cookie-check" in tests and
            "signal_bad_cookie_ok" in tests and
            "--pthread-gate-race-check" in tests and
            "pthread_gate_race_ok" in tests,
            "missing signal runtime stress CTest runner coverage")
    require("--signal-blocked-delivery-check" in runtime_hotloop and
            "blocked_signal_threads" in runtime_hotloop and
            "trace_has_detach_prepare_blocked_signal" in runtime_hotloop and
            "signal-reserved-blocked" in runtime_hotloop,
            "blocked-signal runtime stress must prove fast fail-closed prepare failure")
    require("--signal-user-collision-check" in runtime_hotloop and
            "migration_count=" in runtime_hotloop and
            "handler_preserved=1" in runtime_hotloop and
            "raw_query_protected=1" in runtime_hotloop and
            "mask_preserved=1" in runtime_hotloop and
            "wait_preserved=1" in runtime_hotloop and
            "signalfd_preserved=1" in runtime_hotloop and
            "timer_preserved=1" in runtime_hotloop and
            "mq_migrated=1" in runtime_hotloop and
            "syscall_migrated=1" in runtime_hotloop and
            "worker_calls" in runtime_hotloop,
            "user signal collision runtime test must prove PEAK migrates and keeps working while preserving core libc signal behavior")
    require("aio_denied=1" in runtime_hotloop and
            "forced aio_read collision was not denied" in runtime_hotloop and
            "forced aio_write collision was not denied" in runtime_hotloop and
            "forced aio_fsync collision was not denied" in runtime_hotloop and
            "forced lio_listio collision was not denied" in runtime_hotloop,
            "forced signal collision runtime test must prove POSIX AIO collisions fail closed before libc AIO side effects")
    require("invalid_pointer_guard=1" in runtime_hotloop and
            "invalid raw rt_sigprocmask pointer was not rejected" in runtime_hotloop and
            "invalid raw pselect6 sigmask pointer was accepted" in runtime_hotloop and
            "invalid-pointer raw syscall probes migrated PEAK signal" in runtime_hotloop,
            "raw syscall wrapper must forward unreadable user pointers instead of crashing or migrating")
    require("--signal-bad-cookie-check" in runtime_hotloop and
            "peak_signal_policy_test_send_bad_cookie_to_current_thread" in runtime_hotloop and
            "trace_has_detach_prepare_unexpected_signal" in runtime_hotloop and
            "signal-unexpected-delivery" in runtime_hotloop and
            "contamination_seen=1" in runtime_hotloop and
            "detach_blocked=1" in runtime_hotloop,
            "bad-cookie runtime test must prove unauthenticated reserved-signal traffic blocks signal detach")
    require("peak_detach_controller_test_thread_creation_gate_epoch" in runtime_hotloop and
            "peak_detach_controller_test_gate_waiter_count" in runtime_hotloop and
            "gate_waiters" in runtime_hotloop and
            "create_attempted_during_gate" in runtime_hotloop and
            "child_started_while_gate" in runtime_hotloop,
            "pthread-create gate stress must prove blocked children and gate release ordering")
    benchmark_runner = (repo_root / "benchmarks/detach/run_detach_hotloop_stress.py").read_text(encoding="utf-8")
    require("--detach-backend" in benchmark_runner,
            "hotloop benchmark must support backend-pinned signal stress")
    require("PEAK_DETACH_BACKEND=helper" in tests and
            "PEAK_DETACH_BACKEND=helper" in controller_tests_cmake,
            "fake-helper tests must force helper backend, not auto signal fallback")
    require("${PROJECT_SOURCE_DIR}/src/peak_signal_policy.c" in controller_tests_cmake and
            "PEAK_ENABLE_TEST_HOOKS=1" in controller_tests_cmake and
            "peak_signal_policy_test_block_reserved_for_current_thread() == 0" in controller_tests and
            "signal-reserved-blocked" in controller_tests,
            "controller tests must link signal policy and prove selected-signal blocked reason")


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
    check_peak_init_heartbeat_order(repo_root)
    check_mpi_finalize_trampoline_default(repo_root)
    check_stop_window_trace_gating(repo_root)
    check_mpi_startup_helper_warmup(repo_root)
    check_global_detach_overhead_selection(repo_root)
    check_general_controller_dlopen_drain_order(repo_root)
    check_exclusive_time_nonnegative(repo_root)
    check_dlopen_test_hook_visibility(repo_root)
    check_shutdown_fail_closed_docs(repo_root)
    check_signal_backend_strict_invariants(repo_root)
    print("detach_lifecycle_invariants_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
