#!/usr/bin/env python3

import pathlib
import re
import subprocess
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


def repo_file_is_tracked(repo_root, relative_path):
    try:
        result = subprocess.run(
            ["git", "ls-files", "--error-unmatch", relative_path],
            cwd=repo_root,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return False
    return result.returncode == 0


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
    heartbeat_start = extract_function(source, "peak_runtime_start_heartbeat_thread")

    main_time_position = body.find("peak_main_time = peak_second();")
    heartbeat_position = body.find("peak_runtime_start_heartbeat_thread()")
    require(main_time_position != -1,
            "peak_init must initialize peak_main_time")
    require(heartbeat_position != -1,
            "peak_init must start the heartbeat thread explicitly")
    require(main_time_position < heartbeat_position,
            "peak_main_time must be initialized before heartbeat thread startup")
    require("pthread_create(&heartbeat_thread" in heartbeat_start,
            "heartbeat startup helper must create the heartbeat thread")


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
    env_guard = extract_function(peak_source, "peak_env_looks_like_intel_mpi")
    require("I_MPI_ROOT" in env_guard and
            "I_MPI_FABRICS" in env_guard and
            "I_MPI_HYDRA_BOOTSTRAP" in env_guard,
            "Intel MPI finalize guard must inspect Intel MPI environment markers")


def check_stop_window_trace_gating(repo_root):
    source = (repo_root / "src/peak_detach_controller.c").read_text(
        encoding="utf-8"
    )
    general = (repo_root / "src/general_listener.c").read_text(
        encoding="utf-8"
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

    require("getenv(" not in controller_trace_configure and
            "g_getenv(" not in controller_trace_configure,
            "detach controller trace configuration must not read environment")
    require("atomic_store_explicit(&trace_diagnostics_enabled" in source,
            "detach controller trace configuration must cache explicit state")
    for label, body in (("start", started), ("finish", finished)):
        require("getenv(" not in body and "g_getenv(" not in body,
                f"STOP-window {label} helper must not read environment")
    require("peak_detach_controller_monotonic_second()" in started,
            "STOP-window start helper must always capture policy timing")
    require("last_stop_window_us" in finished and
            "peak_detach_controller_monotonic_second()" in finished,
            "STOP-window finish helper must always publish policy timing")
    require("peak_detach_controller_lock_mutation_guard" in last_window and
            "return value;" in last_window,
            "last_stop_window_us must return stored timing for policy accounting")
    require("trace_diagnostics_enabled" not in started and
            "trace_diagnostics_enabled" not in finished and
            "trace_diagnostics_enabled" not in last_window,
            "STOP-window timing must not be trace-gated")
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
    syscall_policy = (repo_root / "src/syscall_interceptor_policy.h").read_text(
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
    require("peak_syscall_interceptor_has_inline_close_syscall_bytes" in syscall and
            "PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES" in syscall and
            "PEAK_CLOSE_PATCH_HAZARD_PREFIX_BYTES" in syscall_policy and
            "SYS_close" in syscall_policy and
            "0x0f" in syscall_policy and
            "0x05" in syscall_policy,
            "close support replacement must skip inline SYS_close syscall stubs")
    require("peak_general_listener_attach_target_is_supported" in dlopen_attach,
            "dlopen replacement must use normal target prologue policy so dynamic attach is not disabled by support-only early-return guards")
    require("peak_general_listener_support_attach_target_is_supported" not in dlopen_attach,
            "dlopen replacement must not use support-only prologue policy")
    require("peak_general_listener_startup_attach_can_skip_stop()" in dlopen_attach and
            "!startup_replace_can_skip_stop &&" in dlopen_attach,
            "startup dlopen replacement must skip the stop backend only after a single-thread proof")
    require("peak_general_listener_attach_target_is_supported" in dlopen_dynamic,
            "dynamic dlopen user targets must use normal target prologue policy")
    require("peak_general_listener_support_attach_target_is_supported" not in dlopen_dynamic,
            "dynamic dlopen user targets must not use support prologue policy")


def check_mpi_startup_helper_warmup(repo_root):
    source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    controller = (repo_root / "src/peak_detach_controller.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_init")
    warmup = extract_function(controller, "peak_detach_controller_warmup_backend")

    check_mpi_position = body.find("found_MPI = check_MPI();")
    warmup_position = body.find("peak_detach_controller_warmup_backend();")
    require(check_mpi_position != -1 and warmup_position != -1,
            "peak_init must keep explicit MPI detection and helper warmup")
    require(check_mpi_position < warmup_position,
            "helper warmup must not run before MPI detection")
    warmup_context = body[max(0, warmup_position - 80):warmup_position + 80]
    require("if (peak_hook_address_count > 0)" in warmup_context,
            "helper warmup must be enabled for configured target hooks")
    require("requested_backend == PEAK_DETACH_REQUESTED_BACKEND_SIGNAL" in warmup,
            "explicit signal backend must skip helper warmup")
    require("requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO" in warmup,
            "auto backend must skip eager helper warmup and remain lazy helper-first")
    auto_skip = warmup.find("requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO")
    init_atfork = warmup.find("peak_detach_controller_init_atfork_once")
    ensure_helper = warmup.find("peak_detach_controller_ensure_helper")
    require(auto_skip != -1 and init_atfork != -1 and ensure_helper != -1 and
            auto_skip < init_atfork < ensure_helper,
            "auto warmup skip must happen before any helper startup work")


def check_global_detach_overhead_selection(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    heartbeat = extract_function(source, "peak_heartbeat_monitor")
    comparator = extract_function(source, "compare_rate_de")
    wait_helper = extract_function(source, "peak_heartbeat_wait_us")

    global_detach_marker = "// 2) Global DETACH"
    reattach_marker = "// 3) Reattach"
    per_target_detach_marker = "// 1) Per-target DETACH"
    start = heartbeat.find(global_detach_marker)
    end = heartbeat.find(reattach_marker, start)
    require(start != -1 and end != -1,
            "heartbeat must keep explicit global detach and reattach sections")
    global_detach = heartbeat[start:end]
    per_target_start = heartbeat.find(per_target_detach_marker)
    per_target_end = heartbeat.find(global_detach_marker, per_target_start)
    require(per_target_start != -1 and per_target_end != -1,
            "heartbeat must keep explicit per-target and global detach sections")
    per_target_detach = heartbeat[per_target_start:per_target_end]

    require("compare_rate_de" in global_detach,
            "global detach must sort candidates by pressure contribution")
    per_target_gate_start = per_target_detach.find("if (detach_observation_ready")
    per_target_gate_end = per_target_detach.find("pthread_mutex_lock", per_target_gate_start)
    require(per_target_gate_start != -1 and per_target_gate_end != -1,
            "per-target detach must have an explicit admission gate")
    per_target_gate = per_target_detach[per_target_gate_start:per_target_gate_end]
    require("effective_global_overhead <=" not in per_target_gate and
            "global_target_ratio * peak_global_detach_factor" not in per_target_gate,
            "per-target detach must not yield to high global overhead; global detach is additive")
    require("ratio_snapshot[i] > target_profile_ratio" in per_target_detach,
            "per-target detach must keep the master-compatible per-hook overhead trigger")
    require("detach_slot_limit" in per_target_detach and
            "PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES" in per_target_detach and
            "admitted_requests >= detach_slot_limit" in per_target_detach,
            "per-target detach must cap each heartbeat to a bounded controller-sized wave")
    per_target_trigger_start = per_target_detach.find(
        "if (ratio_snapshot[i] > target_profile_ratio)")
    per_target_trigger_end = per_target_detach.find(
        "peak_general_listener_request_detach_with_context_unlocked",
        per_target_trigger_start)
    require(per_target_trigger_start != -1 and per_target_trigger_end != -1,
            "per-target detach must request hot hooks from the local threshold")
    per_target_before_request = per_target_detach[
        per_target_trigger_start:per_target_trigger_end]
    require("transition_cost_ratio" not in per_target_before_request and
            "peak_general_listener_heartbeat_mutation_budget_allows" not in per_target_before_request and
            "peak_general_listener_heartbeat_stop_window_burst_allows_unlocked" not in per_target_before_request and
            "peak_general_listener_transition_tokens_allow_with_reserve" not in per_target_before_request,
            "per-target hot-hook shedding must not be suppressed by synthetic stop-window budgets")
    require("PEAK_GLOBAL_DETACH_MIN_CALLS" in source and
            "calls_snapshot[i] < PEAK_GLOBAL_DETACH_MIN_CALLS" in global_detach and
            "continue;" in global_detach,
            "global detach must not enqueue one-shot/cold targets")
    require("ratio_snapshot[i] <= target_profile_ratio" not in global_detach,
            "global detach must not require candidates to exceed the per-target threshold")
    require("entries[k].ratio <= target_profile_ratio" not in global_detach,
            "global detach loop must not stop at the per-target threshold")
    require("attached_global_recent_overhead" in heartbeat and
            "attached_global_lifetime_overhead" in heartbeat and
            "projected_attached_recent_overhead" in global_detach and
            "projected_attached_lifetime_overhead" in global_detach,
            "global detach must project recent and lifetime callback pressure separately")
    require("candidate_pressure_drop > 0.0" in global_detach and
            "already_paid_batch_slot ||" in global_detach,
            "already-paid global detach batch slots must still reduce callback pressure")
    require("attached_global_overhead >" in global_detach and
            "effective_global_overhead >" not in global_detach[
                global_detach.find("if (detach_observation_ready"):
                global_detach.find("size_t n_attached")
            ],
            "global detach must be driven by attached callback pressure, not transition debt")
    require("peak_general_listener_heartbeat_mutation_budget_allows" in global_detach,
            "global detach must respect the heartbeat transition stop-window budget")
    require("hard_transition_budget" in global_detach and
            "projected_transition_after <= hard_transition_budget" in global_detach,
            "normal global detach must hard-cap projected transition stop-window overhead")
    require("peak_general_listener_global_detach_bootstrap_recovery_cost_ratio" in global_detach,
            "global detach must cap bootstrap stop-window cost for first-batch over-limit recovery")
    require("peak_general_listener_per_target_detach_can_precede_global_batch" in source and
            "peak_general_listener_per_target_detach_can_precede_global_batch" in heartbeat,
            "per-target detach must not steal the first global many-target stop-window")
    per_target_guard = extract_function(
        source, "peak_general_listener_per_target_detach_can_precede_global_batch")
    require("peak_transition_stop_window_success_batch_count != 0" in per_target_guard and
            "PEAK_GLOBAL_DETACH_WARMUP_MAX_WAIT_US" in per_target_guard,
            "per-target first-window guard must release after a measured batch or bounded warmup")
    bootstrap_recovery = extract_function(
        source, "peak_general_listener_global_detach_bootstrap_recovery_cost_ratio")
    require("peak_general_listener_global_detach_bootstrap_recovery_ready" in source and
            "peak_transition_stop_window_success_batch_count == 0" in source and
            "attached_recent_overhead > limit" in source,
            "bootstrap recovery must require no measured stop-window and attached overhead over the global limit")
    require("return hard_transition_budget;" in bootstrap_recovery and
            "projected_batch_transition_cost_ratio <= hard_transition_budget" in bootstrap_recovery,
            "bootstrap recovery must cap synthetic first-batch cost at the hard mutation budget")
    require("return 0.0" not in bootstrap_recovery,
            "bootstrap recovery must not make the first physical mutation free")
    require("peak_heartbeat_default_mutation_timeout_ms = 1000" in source and
            "peak_heartbeat_default_max_pending_age_s = 5.0" in source and
            "peak_heartbeat_mutation_max_pending_age_s = 5.0" in source,
            "heartbeat timeout and pending-age defaults must permit bounded retries on loaded nodes")
    bootstrap_cost = extract_function(
        source, "peak_general_listener_bootstrap_transition_batch_cost_s")
    require("PEAK_HEARTBEAT_BOOTSTRAP_BATCH_COST_MAX_MS" in bootstrap_cost and
            "peak_heartbeat_mutation_timeout_ms" in bootstrap_cost and
            "timeout_ms > PEAK_HEARTBEAT_BOOTSTRAP_BATCH_COST_MAX_MS" in bootstrap_cost,
            "heartbeat acquisition timeout must stay decoupled from bootstrap stop-window cost")
    pacing = extract_function(
        source, "peak_general_controller_heartbeat_batch_pacing_allows_unlocked")
    bootstrap_batch = extract_function(
        source, "peak_general_controller_batch_is_bootstrap_recovery_unlocked")
    collect_batch = extract_function(
        source, "peak_general_controller_collect_batch_unlocked")
    note_batch = extract_function(
        source, "peak_general_listener_note_transition_batch_unlocked")
    require("peak_general_controller_batch_is_bootstrap_recovery_unlocked" in pacing and
            "expected_batch_cost_s = reserved_s;" in pacing,
            "controller pacing must honor only whole-batch bootstrap recovery reservations")
    require("peak_general_listener_heartbeat_stop_window_burst_allows_unlocked" in pacing,
            "heartbeat batch pacing must enforce a recent stop-window burst shaper")
    require("peak_general_listener_expected_heartbeat_batch_cost_s_unlocked" in pacing,
            "heartbeat batch pacing must charge candidate-count-aware batch cost")
    require("peak_general_listener_heartbeat_batch_slot_limit_for_source_unlocked" in collect_batch and
            "operation == PEAK_DETACH_OPERATION_REATTACH ||" in collect_batch and
            "peak_general_controller_pending_is_probe_closeout_unlocked(i)" in collect_batch,
            "controller collection must cap heartbeat reattach/probe-closeout batches without throttling ordinary heartbeat detach evacuation")
    require("peak_general_controller_ready_global_recovery_pending_unlocked" in collect_batch and
            "source != PEAK_HOOK_REQUEST_SOURCE_GLOBAL_OVERHEAD_RECOVERY" in collect_batch,
            "controller collection must keep first global-overhead-recovery batches source-pure")
    source_slot = extract_function(
        source,
        "peak_general_listener_heartbeat_batch_slot_limit_for_source_unlocked")
    require("PEAK_HOOK_REQUEST_SOURCE_GLOBAL_OVERHEAD_RECOVERY" in source_slot and
            "peak_transition_stop_window_success_batch_count == 0" in source_slot and
            "PEAK_HEARTBEAT_RECOVERY_BOOTSTRAP_BATCH_CANDIDATES" in source_slot and
            "return MIN(requested_slots" in source_slot and
            "peak_general_listener_heartbeat_batch_slot_limit_unlocked" in source_slot,
            "first global-overhead recovery batches must use a bounded bootstrap slot cap")
    require("peak_transition_stop_window_success_count_ema" in note_batch,
            "transition accounting must track successful batch size for adaptive heartbeat batching")
    require("peak_general_listener_note_heartbeat_stop_window_unlocked" in source,
            "controller must record actual heartbeat stop-window bursts after completion")
    require("PEAK_HOOK_REQUEST_SOURCE_GLOBAL_OVERHEAD_RECOVERY" in bootstrap_batch and
            "PEAK_DETACH_OPERATION_DETACH" in bootstrap_batch and
            "has_reserved_slot = TRUE" in bootstrap_batch and
            "return has_reserved_slot;" in bootstrap_batch,
            "bootstrap recovery pacing bypass must require a source-pure recovery detach batch with at least one shared local reservation")
    recovery_assignment = global_detach[
        global_detach.find("bounded_overhead_recovery_batch ="):
        global_detach.find("within_soft_transition_budget =")
    ]
    require("batch_reduces_effective_overhead" in recovery_assignment,
            "bounded global-overhead recovery must be justified by the aggregate batch benefit")
    require("bootstrap_recovery_ready" in global_detach and
            "peak_general_controller_global_recovery_pending_or_inflight_unlocked" in global_detach and
            "size_t batch_slot_limit = requested_batch_slots;" in global_detach,
            "global detach recovery must avoid overlapping recovery batches while admitting a full controller-sized detach wave")
    require("global_attached_overhead_limit &&\n                        !already_paid_batch_slot" in global_detach,
            "global detach must keep filling an already-paid batch after the first hot hook crosses the aggregate budget")
    require("bootstrap_recovery_ready &&" in recovery_assignment,
            "bounded global-overhead recovery source must only be used for the explicit bootstrap recovery window")
    require("marginal_transition_cost_ratio <=" in recovery_assignment and
            "hard_transition_budget" in recovery_assignment,
            "bounded global-overhead recovery must hard-cap the single shared stop-window cost")
    require("entries[k].ratio > marginal_transition_cost_ratio" not in recovery_assignment,
            "bounded global-overhead recovery must not require one candidate to pay the whole shared stop-window cost")
    require("exceeds_hard_transition_budget" not in recovery_assignment,
            "bounded global-overhead recovery must recover from lifetime transition-ledger debt")
    recovery_pacing = global_detach[
        global_detach.find("peak_general_listener_heartbeat_stop_window_burst_allows_unlocked"):
        global_detach.find("if (!reduces_effective_overhead)")
    ]
    require("peak_general_listener_heartbeat_stop_window_burst_allows_unlocked" in recovery_pacing and
            "peak_general_listener_transition_tokens_allow_with_reserve" in recovery_pacing and
            "!bounded_overhead_recovery_batch" not in recovery_pacing,
            "bounded global-overhead recovery must obey dynamic stop-window/token pacing")
    require(comparator.find("x->ratio") < comparator.find("x->rate"),
            "global detach comparator must prioritize pressure contribution before current growth rate")
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


def check_hot_callback_detach_count_gate(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    on_enter = extract_function(source, "peak_general_listener_on_enter")

    require("peak_general_listener_detach_count_policy_active" in source,
            "hot callback must expose an explicit detach-count policy predicate")
    require("if (peak_general_listener_detach_count_policy_active())" in on_enter,
            "hot callback must gate detach-count lock/scan behind detach-count policy")
    gate_pos = on_enter.find("if (peak_general_listener_detach_count_policy_active())")
    require(gate_pos != -1, "missing detach-count hot-path gate")
    lock_pos = on_enter.find("pthread_mutex_lock(&lock)", gate_pos)
    scan_pos = on_enter.find("for (size_t j = 0; j < peak_max_num_threads; j++)", gate_pos)
    threshold_pos = on_enter.find(
        "peak_general_listener_next_detach_count_threshold_unlocked",
        gate_pos,
    )
    require(lock_pos != -1 and scan_pos != -1 and threshold_pos != -1,
            "detach-count gate must contain the lock, thread scan, and threshold check")
    before_gate = on_enter[:gate_pos]
    require("pthread_mutex_lock(&lock)" not in before_gate,
            "heartbeat-enabled hot callback must not take the controller lock before detach-count policy is known active")
    require("for (size_t j = 0; j < peak_max_num_threads; j++)" not in before_gate,
            "heartbeat-enabled hot callback must not scan all thread counters unless detach-count policy is active")


def check_reattach_probe_followup_budget(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    request_detach = extract_function(
        source, "peak_general_listener_request_detach_with_context_unlocked"
    )
    prepare_failure = extract_function(
        source, "peak_general_controller_handle_prepare_failure_unlocked"
    )
    pacing_defer = extract_function(
        source, "peak_general_controller_defer_pacing_blocked_batch_unlocked"
    )
    retry_abandon = extract_function(
        source,
        "peak_general_controller_abandon_retry_budget_exceeded_batch_unlocked",
    )
    batch_process = extract_function(
        source, "peak_general_controller_process_pending_batch_unlocked"
    )

    require("peak_general_controller_probe_followup_budget_unlocked" in source,
            "probe follow-up detach must expose its reserved budget")

    probe_active = request_detach.find("peak_hook_reattach_probe_active")
    peek_budget = request_detach.find(
        "peak_general_controller_probe_followup_budget_unlocked", probe_active
    )
    release_budget = request_detach.find(
        "peak_general_controller_release_probe_followup_budget_unlocked",
        probe_active,
    )
    set_context = request_detach.find(
        "peak_general_controller_set_pending_request_context_unlocked",
        release_budget,
    )
    set_local = request_detach.find(
        "peak_general_controller_set_pending_local_transition_budget_unlocked",
        set_context,
    )
    set_state = request_detach.find("PEAK_HOOK_DETACH_REQUESTED", set_context)

    require(probe_active != -1 and peek_budget != -1 and
            release_budget != -1 and set_context != -1 and
            set_local != -1 and set_state != -1,
            "probe follow-up detach must preserve already-paid local budget")
    require(probe_active < peek_budget < release_budget < set_context <
            set_local < set_state,
            "probe follow-up budget must be copied before release and applied before queueing")
    require("preserve_probe_closeout_budget" in prepare_failure and
            "peak_hook_reattach_probe_active" in prepare_failure,
            "retryable probe closeout prepare failures must preserve local budget")
    require("preserve_global_recovery_budget" in prepare_failure and
            "PEAK_HOOK_REQUEST_SOURCE_GLOBAL_OVERHEAD_RECOVERY" in prepare_failure,
            "retryable global recovery prepare failures must preserve admitted local budget")
    preserve_pos = prepare_failure.find("preserve_probe_closeout_budget")
    clear_pos = prepare_failure.find(
        "peak_general_controller_clear_pending_local_transition_budget_unlocked",
        preserve_pos,
    )
    require(clear_pos != -1 and
            "if (!preserve_probe_closeout_budget &&" in prepare_failure[preserve_pos:clear_pos] and
            "!preserve_global_recovery_budget" in prepare_failure[preserve_pos:clear_pos],
            "only non-closeout and non-global-recovery retryable failures should clear local transition budget")
    require("candidates[i].retry_count > 0" in pacing_defer and
            "candidates[i].last_retry_status != PEAK_DETACH_STATUS_SAFE" in pacing_defer,
            "pacing deferral must not erase heartbeat requests that already attempted the backend")
    preserve_deferred_pos = pacing_defer.find("preserve_probe_closeout")
    reserve_deferred_pos = pacing_defer.find(
        "peak_general_controller_reserve_probe_followup_budget_unlocked"
    )
    closeout_clear_pos = pacing_defer.find(
        "peak_general_listener_clear_reattach_probe_unlocked"
    )
    request_context_clear_pos = pacing_defer.find(
        "peak_general_controller_clear_pending_request_context_unlocked"
    )
    stable_state_pos = pacing_defer.find(
        "peak_general_controller_set_state_unlocked"
    )
    require(request_context_clear_pos != -1 and
            stable_state_pos != -1 and
            request_context_clear_pos < stable_state_pos,
            "fresh pacing deferral must release pending transition budget before restoring stable state")
    require(preserve_deferred_pos != -1 and reserve_deferred_pos != -1 and
            "peak_hook_pending_local_transition_budget_s" in
            pacing_defer[preserve_deferred_pos:request_context_clear_pos] and
            request_context_clear_pos < reserve_deferred_pos < stable_state_pos,
            "probe closeout pacing deferral must restore its follow-up reservation for retry")
    require(closeout_clear_pos != -1 and
            "!preserve_probe_closeout" in
            pacing_defer[closeout_clear_pos - 200:closeout_clear_pos],
            "pacing deferral must not clear active probe closeout state")
    require("peak_general_controller_retry_budget_exceeded_for_source_unlocked" in retry_abandon and
            '"retry-abandoned"' in retry_abandon and
            "peak_general_controller_arm_failure_cooldown_unlocked" in retry_abandon,
            "retry-state heartbeat requests blocked by pacing must still abandon through failure cooldown")
    require("peak_general_listener_clear_reattach_probe_unlocked(hook_id)" in retry_abandon and
            "candidates[i].stable_state == PEAK_HOOK_ATTACHED" in retry_abandon,
            "abandoned probe closeout retries must clear active/admitted follow-up state")
    abandon_pos = batch_process.find(
        "peak_general_controller_abandon_retry_budget_exceeded_batch_unlocked"
    )
    pacing_pos = batch_process.find(
        "peak_general_controller_heartbeat_batch_pacing_allows_unlocked"
    )
    require(abandon_pos != -1 and pacing_pos != -1 and abandon_pos < pacing_pos,
            "retry-budget abandonment must run before heartbeat pacing can defer fresh work")


def check_global_detach_probe_ownership(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    heartbeat = extract_function(source, "peak_heartbeat_monitor")

    per_target_marker = "// Per-target REATTACH"
    global_marker = "// Global REATTACH"
    start = heartbeat.find(per_target_marker)
    end = heartbeat.find(global_marker, start)
    require(start != -1 and end != -1,
            "heartbeat must keep explicit per-target and global reattach sections")
    per_target_reattach = heartbeat[start:end]

    require("last_detach_from_global_heartbeat" in per_target_reattach,
            "per-target reattach must identify hooks detached by global heartbeat")
    require("!last_detach_from_global_heartbeat" in per_target_reattach,
            "per-target probe scan must not consume global-detached hooks while global heartbeat is enabled")


def check_heartbeat_detached_hooks_reattach_as_probes(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    heartbeat = extract_function(source, "peak_heartbeat_monitor")

    per_target_marker = "// Per-target REATTACH"
    global_marker = "// Global REATTACH"
    start = heartbeat.find(per_target_marker)
    end = heartbeat.find(global_marker, start)
    require(start != -1 and end != -1,
            "heartbeat must keep explicit per-target and global reattach sections")
    per_target_reattach = heartbeat[start:end]

    normal_decision = per_target_reattach[
        per_target_reattach.find("should_request ="):
        per_target_reattach.find("if (!should_request", per_target_reattach.find("should_request ="))
    ]
    require("!last_detach_from_heartbeat" in normal_decision,
            "heartbeat-detached hooks must not use stale low-overhead estimates for normal per-target reattach")
    probe_decision = per_target_reattach[
        per_target_reattach.find("if (!should_request"):
        per_target_reattach.find("if (should_request)", per_target_reattach.find("if (!should_request"))
    ]
    require("last_detach_from_heartbeat" in probe_decision and
            "probe_request = TRUE" in per_target_reattach,
            "heartbeat-detached hooks must reattach through bounded probe sampling")


def check_reattach_probe_full_cost_accounting(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    record_success = extract_function(
        source, "peak_general_listener_note_transition_success_unlocked"
    )
    attached_limit = extract_function(
        source, "peak_general_listener_global_attached_overhead_limit"
    )
    pending = extract_function(
        source, "peak_general_controller_set_pending_request_context_unlocked"
    )
    retry_limits = extract_function(
        source, "peak_general_controller_init_retry_limits_once"
    )
    probe_ready = extract_function(
        source, "peak_general_listener_budgeted_probe_ready_unlocked"
    )
    probe_budget = extract_function(
        source, "peak_general_listener_probe_budget_ratio_unlocked"
    )
    probe_projection = extract_function(
        source, "peak_general_listener_global_probe_projection_allows"
    )
    cooldown_ready = extract_function(
        source, "peak_general_listener_reattach_cooldown_ready_unlocked"
    )
    max_ready = extract_function(
        source, "peak_general_listener_max_ready_global_probe_costs_unlocked"
    )
    token_cap = extract_function(
        source, "peak_general_listener_reattach_probe_token_cap_s"
    )
    probe_compare = extract_function(
        source, "compare_reattach_probe_priority"
    )
    heartbeat = extract_function(source, "peak_heartbeat_monitor")
    per_target_start = heartbeat.find("// Per-target REATTACH")
    global_start = heartbeat.find("// Global REATTACH")
    global_end = heartbeat.find("if (wake_controller)", global_start)
    require(per_target_start != -1 and global_start != -1 and global_end != -1,
            "heartbeat must keep explicit global reattach and wake-controller sections")
    per_target_reattach = heartbeat[per_target_start:global_start]
    global_reattach = heartbeat[global_start:global_end]

    helper_name = "peak_general_listener_reattach_probe_budget_cost_s_unlocked"
    require(helper_name in source,
            "reattach probe sample-budget helper is missing")
    helper_body = extract_function(source, helper_name)
    require("#define PEAK_REATTACH_PROBE_MAX_PER_HEARTBEAT 8U" in source and
            "PEAK_REATTACH_PROBE_MAX_PER_HEARTBEAT \\\n    PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES" not in source,
            "reattach probe waves must be smaller than the controller batch capacity to avoid synchronized closeout bursts")
    require("peak_general_listener_reattach_probe_sample_cost_s_unlocked" in
            helper_body and
            "transition_cost_ratio" in helper_body and
            "2.0 * transition_cost_s" not in helper_body,
            "reattach probe budget helper must charge sample exposure, not physical stop-window cost")
    require("(void)transition_overhead_ratio" in attached_limit and
            "return global_target_ratio" in attached_limit and
            "global_target_ratio - transition_overhead_ratio" not in attached_limit,
            "global detach pressure must use attached callback overhead; transition cost is bounded by mutation ledgers")
    require("peak_general_controller_replace_pending_transition_budget_unlocked" in source,
            "probe admission must be able to replace default pending budget with the admitted two-ledger cost")
    require(source.count("PEAK_REATTACH_COOLDOWN_MS_ENV") == 2 and
            "PEAK_REATTACH_COOLDOWN_MS_ENV,\n            PEAK_REATTACH_COMPAT_DEFAULT_COOLDOWN_MS" in retry_limits and
            "#define PEAK_REATTACH_COMPAT_DEFAULT_COOLDOWN_MS 60000U" in source,
            "legacy reattach cooldown env must be parsed exactly once with master-compatible 60s default")
    require("Unset keeps the old\n     * 60-second reattach cooldown" in retry_limits and
            "Explicit zero is reserved for aggressive sampling stress tests" in retry_limits and
            "sample-exposure cost" in retry_limits,
            "legacy reattach cooldown must stay documented as the master-compatible default gate")
    require("peak_reattach_compat_cooldown_floor_s <= 0.0" in cooldown_ready and
            "peak_general_listener_last_detach_from_heartbeat_unlocked" in cooldown_ready and
            "now - last_transition_time >=\n           peak_reattach_compat_cooldown_floor_s" in cooldown_ready,
            "legacy reattach cooldown must be an explicit master-compatible hard gate for heartbeat-detached hooks")
    require("peak_hook_detached_priority_estimate[hook_id] =\n                    request_ratio > 0.0 ? request_ratio : request_rate" in record_success,
            "detached probe priority must prefer sustained observed overhead over transient heartbeat rate")
    require("peak_general_listener_probe_should_yield_to_global_detach" in source and
            "global_target_ratio * peak_global_detach_factor" in
            extract_function(source, "peak_general_listener_probe_should_yield_to_global_detach"),
            "probe reattach must yield only at the global detach-pressure threshold, not the base target")
    require("request_followup_budget_s =\n                peak_hook_pending_local_transition_budget_s" in record_success and
            "request_followup_budget_s);" in record_success and
            "apportioned_stop_window_s);" not in record_success,
            "probe reattach success must carry the precharged closeout budget, not the measured reattach stop-window share")
    for label, body in (("pending reservation", pending),
                        ("probe interval", probe_ready),
                        ("max ready probe cost", max_ready),
                        ("heartbeat admission", heartbeat)):
        require(helper_name in body,
                f"{label} must use sample-budget reattach probe cost helper")

    require("request_spends_shared_tokens =\n                            probe_request ||\n                            last_detach_from_heartbeat" in heartbeat and
            "request_spends_shared_tokens =\n                        probe_request || last_detach_from_heartbeat" in heartbeat,
            "probe and heartbeat-detached reattach must spend shared transition tokens")
    require("request_current_transition_cost_s" in heartbeat and
            "request_followup_transition_cost_s" in heartbeat and
            "per_target_probe_physical_reserved_s" in heartbeat and
            "MAX(request_current_transition_cost_s -\n                                          per_target_probe_physical_reserved_s" in heartbeat and
            "MAX(request_current_transition_cost_s -\n                                      global_probe_physical_reserved_s" in heartbeat,
            "probe reattach must charge sample cost to probe tokens and only the incremental physical batch cost to shared mutation tokens in both per-target and global paths")
    require("per_target_probe_closeout_reserved_s" in heartbeat and
            "global_probe_closeout_reserved_s" in heartbeat and
            "MAX(request_followup_transition_cost_s -\n                                          per_target_probe_closeout_reserved_s" in heartbeat and
            "MAX(request_followup_transition_cost_s -\n                                      global_probe_closeout_reserved_s" in heartbeat and
            "request_shared_token_cost_s * 0.5" not in heartbeat,
            "probe admission must publish an amortized follow-up detach budget, not infer it from half of an immediate shared-token spend")
    require("peak_general_listener_probe_token_refill_ratio_unlocked" in probe_ready and
            "peak_general_listener_probe_token_refill_ratio_unlocked" in heartbeat,
            "probe readiness and token refill must use the same fraction-adjusted budget")
    heartbeat_priority_start = probe_compare.find(
        "if (x->last_detach_from_heartbeat && y->last_detach_from_heartbeat)"
    )
    heartbeat_priority = probe_compare[
        heartbeat_priority_start:
        probe_compare.find("return 0;", heartbeat_priority_start)
    ]
    require("x->transition_count < y->transition_count" in heartbeat_priority and
            heartbeat_priority.find("x->transition_count < y->transition_count") <
            heartbeat_priority.find("x->rate < y->rate"),
            "heartbeat-detached reattach probes must rotate by sample count before hot-rate tie-breaking")
    require("heartbeat_transition_min_probe_full_cost_s" in heartbeat and
            "heartbeat_transition_min_probe_shared_cost_s" in heartbeat,
            "heartbeat must keep sample probe-token cost separate from shared mutation-token reserve")
    reserve_slots_start = heartbeat.find("if (reattach_cycle_ready) {")
    reserve_slots_end = heartbeat.find(
        "if (reattach_cycle_ready && detached_ready_for_reattach > 0)",
        reserve_slots_start,
    )
    reserve_slots = heartbeat[reserve_slots_start:reserve_slots_end]
    reserve_tokens_start = heartbeat.find(
        "if (reattach_cycle_ready && reattach_probe_transition_tokens_s > 0.0"
    )
    reserve_tokens_end = heartbeat.find(
        "if (peak_general_controller_trace_enabled()",
        reserve_tokens_start,
    )
    reserve_tokens = heartbeat[reserve_tokens_start:reserve_tokens_end]
    require(reserve_slots_start != -1 and reserve_slots_end != -1 and
            "peak_general_listener_probe_should_yield_to_global_detach" in reserve_slots and
            "reattach_admission_reserve =\n                    MIN(detached_ready_for_reattach" in reserve_slots,
            "reattach admission-slot reserve must yield only above the global detach-pressure threshold")
    require(reserve_tokens_start != -1 and reserve_tokens_end != -1 and
            "peak_general_listener_probe_should_yield_to_global_detach" in reserve_tokens and
            "reattach_shared_token_reserve_s =\n                heartbeat_transition_min_probe_shared_cost_s" in reserve_tokens,
            "reattach shared-token reserve must yield only above the global detach-pressure threshold")
    require("global_probe_ready =\n                heartbeat_transition_min_probe_full_cost_s > 0.0" in heartbeat and
            "if (!should_request &&\n                            !global_probe_ready" in heartbeat,
            "per-target probes must not spend probe budget while global heartbeat probes are ready")
    require("peak_general_listener_transition_tokens_allow_with_reserve(\n                                peak_heartbeat_transition_tokens_s,\n                                request_shared_token_cost_s" in heartbeat and
            "shared_tokens_allowed =\n                            peak_general_listener_transition_tokens_allow_with_reserve(\n                                shared_tokens_snapshot_s,\n                                request_shared_token_cost_s" in heartbeat,
            "shared mutation token checks must not double-charge probe sample cost")
    require("!last_detach_from_api &&\n                             !last_detach_from_detach_count" in heartbeat,
            "API/detach-count reattach should not be starved by heartbeat probe storm pacing tokens")
    require("peak_general_listener_transition_tokens_allow_with_reserve" in global_reattach and
            "reattach_shared_token_reserve_s" in global_reattach,
            "global heartbeat reattach must reserve shared tokens for paired probe closeout")
    require("if (!last_detach_from_heartbeat) {\n                            continue;\n                        }" in global_reattach,
            "global heartbeat probe reattach must only apply to heartbeat-detached hooks")
    priority_ready = global_reattach[
        global_reattach.find("if (!peak_general_listener_budgeted_probe_ready_unlocked"):
        global_reattach.find("if (global_probe_requests >=", global_reattach.find("if (!peak_general_listener_budgeted_probe_ready_unlocked"))
    ]
    require("peak_general_listener_probe_priority_floor" in global_reattach and
            "sustained observed cost is now too small" in global_reattach,
            "global heartbeat probes must not spend sampling budget on sustained-cold detached hooks")
    cold_floor_start = global_reattach.find(
        "if (entries[k].rate <\n                            peak_general_listener_probe_priority_floor())"
    )
    cold_floor_section = global_reattach[
        cold_floor_start:
        global_reattach.find("if (!peak_general_listener_budgeted_probe_ready_unlocked",
                             cold_floor_start)
    ]
    require(cold_floor_start != -1 and
            "fairness-sort before heat" in cold_floor_section and
            "continue;" in cold_floor_section and
            "break;" not in cold_floor_section,
            "global heartbeat probes must skip sustained-cold fairness candidates and keep scanning")
    require("peak_general_listener_detached_priority_unlocked(hook_id) <\n            peak_general_listener_probe_priority_floor()" in max_ready,
            "global probe-ready pre-scan must apply the same sustained-cold floor as the global probe loop")
    require("fairness-sorted before heat" in priority_ready and
            "continue;" in priority_ready and
            "break;" not in priority_ready,
            "global heartbeat probes must skip not-ready candidates and keep scanning for a budget-ready fair probe")
    require("2.0 * transition_cost_s" not in probe_ready,
            "probe interval must include sample callback cost, not only two stop windows")
    require("peak_general_listener_reattach_cooldown_ready_unlocked(hook_id" in probe_ready and
            "required_interval_s = probe_cost_s / budget_ratio" in probe_ready,
            "budgeted probe readiness must enforce cooldown before adaptive sample-budget dwell")
    require(probe_ready.find("peak_general_listener_reattach_cooldown_ready_unlocked") <
            probe_ready.find("transition_cost_s = transition_cost_ratio * total_time_s"),
            "legacy cooldown must block heartbeat reattach before computing/spending probe cost")
    require("PEAK_REATTACH_COOLDOWN_MS_ENV" not in heartbeat and
            "PEAK_REATTACH_COOLDOWN_MS_ENV" not in token_cap and
            "peak_reattach_compat_cooldown_floor_s" not in token_cap and
            "PEAK_REATTACH_COOLDOWN_MS_ENV" not in max_ready and
            "peak_reattach_compat_cooldown_floor_s" not in max_ready,
            "legacy reattach cooldown must not drive token refill or token caps")
    require("2.0 * peak_transition_stop_window_ema_s" not in token_cap,
            "probe token cap must be driven by ready sample-budget probe cost")
    require("global_probe_physical_reserved_s" in global_reattach and
            "global_probe_admission_enabled" in global_reattach and
            "if (request_current_transition_cost_s >\n                            global_probe_physical_reserved_s)" in
            global_reattach and
            "request_pending_budget_s =\n                        probe_request\n                            ? request_token_cost_s +\n                                  request_shared_token_cost_s +\n                                  request_closeout_budget_s" in
            global_reattach and
            "peak_general_controller_replace_pending_transition_budget_unlocked(\n                                        i,\n                                        request_pending_budget_s)" in
            global_reattach and
            "peak_general_controller_replace_pending_transition_budget_unlocked" in
            global_reattach,
            "global probe admission must batch physical cost incrementally and publish the admitted pending budget")
    require("request_pending_budget_s" in heartbeat and
            "request_pending_budget_ratio" in heartbeat and
            "projected_global_overhead +=\n                                projected_ratio +\n                                request_pending_budget_ratio" in heartbeat and
            "projected_global_overhead +=\n                            request_pending_budget_ratio" in global_reattach and
            "projected_global_overhead +=\n                            entries[k].ratio +\n                            request_pending_budget_ratio" in global_reattach and
            "projected_global_overhead +=\n                            request_token_cost_ratio" not in heartbeat,
            "same-heartbeat projection must include the full published probe pending budget, not only sample tokens")
    require("headroom = (double)global_target_ratio - effective_global_overhead" in
            probe_budget and
            "if (headroom <= 0.0)" in probe_budget and
            "return 0.0;" in probe_budget,
            "probe token refill must stop when effective overhead has consumed the global budget")
    require("projected_global_overhead + request_pending_budget_ratio" in
            probe_projection and
            "<=\n           global_target_ratio" in probe_projection,
            "global probe admission must use an explicit projected-budget gate")
    require("peak_general_listener_global_probe_projection_allows" in
            per_target_reattach and
            "peak_general_listener_global_probe_projection_allows" in
            global_reattach,
            "both per-target and global reattach probes must pass the projected-budget gate")


def check_reattach_probe_closeout_drains_unbounded(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    heartbeat = extract_function(source, "peak_heartbeat_monitor")
    pacing = extract_function(
        source, "peak_general_controller_heartbeat_batch_pacing_allows_unlocked"
    )
    collect = extract_function(
        source, "peak_general_controller_collect_batch_unlocked"
    )
    ready_closeout = extract_function(
        source, "peak_general_controller_ready_probe_closeout_pending_unlocked"
    )
    start = heartbeat.find("// 0) Follow-up detach for heartbeat reattach probes.")
    end = heartbeat.find("// 1) Per-target DETACH", start)
    require(start != -1 and end != -1,
            "heartbeat must keep an explicit probe follow-up detach section")
    closeout_section = heartbeat[start:end]

    require("size_t closeout_limit =\n                MIN(admission_remaining,\n                    (size_t)PEAK_REATTACH_PROBE_MAX_PER_HEARTBEAT);" in closeout_section,
            "reattach probe closeout fast lane must be capped to the probe wave")
    require("peak_general_controller_batch_is_probe_closeout_unlocked" in source,
            "probe closeout fast lane must be explicitly recognized")
    closeout_pacing = pacing[
        pacing.find("peak_general_controller_batch_is_probe_closeout_unlocked"):
        pacing.find("/*\n     * Probe closeouts carry local reservation credit")
    ]
    require("cleanup half of an already-admitted sampling probe" in
            closeout_pacing and
            "strand hot hooks attached" in closeout_pacing and
            "return TRUE;" in closeout_pacing and
            "heartbeat_stop_window_burst_allows_unlocked" not in closeout_pacing,
            "all-probe-closeout batches must finish the already-admitted sample window without a second burst gate")
    require("prefer_probe_closeout" in collect and
            "peak_general_controller_ready_probe_closeout_pending_unlocked" in
            collect and
            "if (prefer_probe_closeout &&\n            !peak_general_controller_pending_is_probe_closeout_unlocked(i))" in
            collect and
            "prefer_global_recovery =\n        !prefer_probe_closeout" in collect,
            "ready reattach probe closeouts must be collected as closeout-only batches before fresh heartbeat work")
    require("peak_general_controller_pending_is_probe_closeout_unlocked" in
            ready_closeout and
            "peak_general_controller_retry_ready_unlocked" in ready_closeout and
            "peak_general_hook_is_published_unlocked" in ready_closeout,
            "closeout-only collection must require a ready and published pending probe closeout")
    budgeted_probe_ready = extract_function(
        source, "peak_general_listener_budgeted_probe_ready_unlocked"
    )
    dwell_check = (
        "elapsed_since_transition <\n        peak_general_listener_hook_probe_min_detached_s_unlocked(hook_id)"
    )
    require("PEAK_REATTACH_PROBE_MIN_DETACHED_MS_ENV" in source and
            "peak_hook_reattach_probe_min_detached_s" in source and
            dwell_check in budgeted_probe_ready,
            "reattach probe readiness must enforce per-hook minimum detached dwell")
    probe_max = extract_function(
        source, "peak_general_listener_probe_max_detached_s"
    )
    require("PEAK_REATTACH_PROBE_MAX_DETACHED_MS_ENV" in source and
            "#define PEAK_REATTACH_PROBE_DEFAULT_MIN_DETACHED_MS 5000U" in
            source and
            "#define PEAK_REATTACH_PROBE_DEFAULT_MAX_DETACHED_MS 60000U" in
            source and
            "peak_reattach_probe_max_detached_s" in probe_max,
            "reattach probe dwell must expose configurable 5s floor and 60s adaptive max defaults")
    require("reattach_probe_max_detached_ms == 0\n            ? 0.0" in source,
            "PEAK_REATTACH_PROBE_MAX_DETACHED_MS=0 must mean uncapped adaptive dwell")
    require("reattach_probe_max_detached_ms != 0 &&\n        reattach_probe_max_detached_ms < reattach_probe_min_detached_ms" in
            source,
            "reattach probe max below floor must be normalized up to the floor")
    require("max_s > 0.0 &&\n                       peak_hook_reattach_probe_min_detached_s[hook_id] > max_s" in
            source and
            "if (max_s > 0.0 && next_s > max_s)" in source,
            "reattach probe adaptive cap must only apply when max is positive")
    charge_debt = extract_function(
        source, "peak_general_listener_charge_heartbeat_stop_window_debt_unlocked"
    )
    require("peak_general_listener_heartbeat_mutation_budget_ratio()" in
            charge_debt and
            "peak_general_listener_heartbeat_transition_token_cap_s" in
            charge_debt and
            "debt_floor_s" in charge_debt and
            "peak_heartbeat_transition_tokens_s = debt_floor_s" in
            charge_debt,
            "heartbeat stop-window debt must be bounded so one large strict mutation cannot starve future detach")
    attempt_stop_window = extract_function(
        source,
        "peak_general_listener_note_heartbeat_attempt_stop_window_unlocked",
    )
    detach_if_requested = extract_function(
        source, "peak_general_controller_detach_if_requested_unlocked"
    )
    reattach_if_requested = extract_function(
        source, "peak_general_controller_reattach_if_requested_unlocked"
    )
    batch_process = extract_function(
        source, "peak_general_controller_process_pending_batch_unlocked"
    )
    require("peak_general_listener_is_heartbeat_request_source" in
            attempt_stop_window and
            "peak_general_listener_note_heartbeat_stop_window_unlocked" in
            attempt_stop_window and
            "peak_general_listener_charge_heartbeat_stop_window_debt_unlocked" in
            attempt_stop_window,
            "heartbeat stop-window attempt helper must charge both burst ledger and debt")
    for label, body in (
        ("detach", detach_if_requested),
        ("reattach", reattach_if_requested),
        ("batch", batch_process),
    ):
        require("peak_general_listener_note_heartbeat_attempt_stop_window_unlocked" in body,
                f"{label} prepare failures must charge heartbeat stop-window pacing debt")
    should_closeout = extract_function(
        source, "peak_general_listener_reattach_probe_should_closeout_unlocked"
    )
    pending_probe_closeout = extract_function(
        source, "peak_general_controller_pending_is_probe_closeout_unlocked"
    )
    require("peak_general_listener_reattach_probe_should_closeout_unlocked" in
            closeout_section and
            "peak_general_listener_reattach_probe_detach_ready_unlocked" not in
            closeout_section,
            "heartbeat must decide whether a probe is hot before scheduling its closeout detach")
    require("peak_general_listener_probe_sample_rate_unlocked" in
            should_closeout and
            "sample_rate <= target_profile_ratio" in should_closeout and
            "peak_general_controller_probe_followup_budget_is_shared_unlocked" in
            should_closeout and
            "\"reserved-wait\"" in should_closeout and
            "\"keep-attached\"" in should_closeout and
            "peak_general_listener_clear_reattach_probe_unlocked(hook_id)" in
            should_closeout and
            "return FALSE" in should_closeout,
            "cool reattach probes must keep shared closeout debt until it can be safely released")
    require("\"hot-closeout\"" in should_closeout and
            "\"invalid-closeout\"" in should_closeout and
            "\"reserved-closeout\"" in should_closeout and
            "\"probe-closeout\"" in source,
            "probe closeout decisions must be traced as keep-attached/hot/invalid/reserved")
    require("local_budget_s" in pending_probe_closeout and
            "followup_budget_s" in pending_probe_closeout and
            "(local_budget_s > 0.0 || followup_budget_s > 0.0)" in
            pending_probe_closeout,
            "probe closeout fast lane must require preserved local/follow-up budget, not flags alone")
    require("snapshot-unsafe" in source and
            "peak_general_listener_clear_reattach_probe_unlocked(hook_id);\n        peak_general_controller_reset_retry_unlocked(hook_id)" in
            source,
            "post-prepare snapshot abort must clear probe state before retry reset")
    require("pause-failed" in source and
            "peak_general_listener_clear_reattach_probe_unlocked(hook_id);\n        peak_general_controller_reset_retry_unlocked(hook_id)" in
            source,
            "post-prepare pause abort must clear probe state before retry reset")
    require("\"nonphysical-skipped\"" in source and
            "peak_general_listener_clear_reattach_probe_unlocked(\n                        candidates[i].hook_id);\n                    peak_general_controller_reset_retry_unlocked" in
            source,
            "batch nonphysical detach skip must clear probe state before retry reset")
    probe_sample_rate = extract_function(
        source, "peak_general_listener_probe_sample_rate_unlocked"
    )
    probe_heat = extract_function(
        source, "peak_general_listener_note_probe_sample_heat_unlocked"
    )
    note_transition = extract_function(
        source, "peak_general_listener_note_transition_success_unlocked"
    )
    require("static gboolean\npeak_general_listener_probe_sample_rate_unlocked" in
            source and
            "double* sample_rate_out" in probe_sample_rate and
            "delta_calls == 0" in probe_sample_rate and
            "return FALSE" in probe_sample_rate,
            "reattach probe sample-rate helper must distinguish invalid samples from zero hotness")
    require("sample_rate > 0.0 ? sample_rate" not in note_transition and
            "request_rate > 0.0 ? request_rate" not in note_transition and
            "request_ratio" not in note_transition[
                note_transition.find("if (closing_probe)"):
                note_transition.find("peak_general_listener_clear_reattach_probe_unlocked")
            ],
            "adaptive detached dwell must be updated only from the actual reattached probe sample")
    require("peak_general_listener_trace_probe_dwell_unlocked" in source and
            "\"probe-dwell\"" in source and
            "\"invalid\"" in probe_heat and
            "return;" in probe_heat,
            "probe dwell updates must trace hot/cool/invalid decisions and leave invalid samples unchanged")
    require("peak_hook_reattach_probe_followup_admitted[hook_id] = TRUE" in
            source and
            "peak_hook_reattach_probe_followup_admitted[hook_id] = FALSE" in
            source,
            "reattach probe follow-up admission must be marked when armed and cleared with probe state")
    require("peak_general_controller_set_pending_local_transition_budget_unlocked(\n                    hook_id,\n                    followup_budget_s)" in source,
            "probe closeout must carry pre-reserved local budget into the real detach request")


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


def check_strict_batch_stop_window_drops_listener_lock(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    body = extract_function(
        source, "peak_general_controller_process_pending_batch_unlocked"
    )
    has_pending = extract_function(
        source, "peak_general_controller_has_pending_unlocked"
    )
    pending_count = extract_function(
        source, "peak_general_controller_pending_count_unlocked"
    )

    prepare = body.find("peak_detach_controller_prepare_hook_mutation_batch")
    finish = body.find("peak_detach_controller_finish_hook_mutation_batch")
    require(prepare != -1 and finish != -1 and prepare < finish,
            "strict batch path must prepare before finish")

    unlock_before_prepare = body.rfind("pthread_mutex_unlock(&lock)", 0, prepare)
    require(unlock_before_prepare != -1,
            "strict batch prepare must drop the listener lock before STOP")

    relock_before_finish = body.find("pthread_mutex_lock(&lock)",
                                     unlock_before_prepare,
                                     finish)
    require(relock_before_finish == -1,
            "strict batch path must not reacquire the listener lock before resume")

    relock_after_finish = body.find("pthread_mutex_lock(&lock)", finish)
    require(relock_after_finish != -1,
            "strict batch path must reacquire the listener lock for bookkeeping")

    unlocked_window = body[unlock_before_prepare:relock_after_finish]
    held_thread_window = body[unlock_before_prepare:finish]
    prepared_finish_block = body[body.find("if (prepared_count > 0)", prepare):finish]
    require("return " not in unlocked_window and "goto " not in unlocked_window,
            "strict batch unlocked stop window must not contain early exits")
    require("peak_general_controller_flush_teardown" not in held_thread_window,
            "strict batch Gum flush must run after stopped threads are resumed")
    require("gum_interceptor_" not in held_thread_window,
            "strict batch Gum calls must run only after stopped threads are resumed")
    require("peak_detach_controller_last_stop_window_us" not in prepared_finish_block,
            "strict batch stop-window timing query must run only after resume")
    require("peak_general_controller_trace_event_detail" not in prepared_finish_block,
            "strict batch listener trace must run after stopped threads are resumed")

    final_return = body.rfind("return prepared_count > 0;")
    require(final_return != -1 and relock_after_finish < final_return,
            "strict batch path must return with the listener lock restored")

    comment_window = body[max(0, unlock_before_prepare - 420):finish]
    require("stop-the-world window must never run while this" in comment_window,
            "strict batch lock-drop invariant must be documented at the call site")

    for state in ("PEAK_HOOK_DETACHING", "PEAK_HOOK_REATTACHING"):
        require(state in has_pending and state in pending_count,
                "strict batch in-progress states must still count as pending")


def check_dynamic_attach_stop_window_drops_listener_lock(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_general_listener_dynamic_attach_symbol")

    prepare = body.find("peak_detach_controller_prepare_hook_mutation")
    finish = body.find("peak_detach_controller_finish_hook_mutation")
    require(prepare != -1 and finish != -1 and prepare < finish,
            "dynamic JIT attach must prepare before finish")

    unlock_before_prepare = body.rfind("pthread_mutex_unlock(&lock)", 0, prepare)
    require(unlock_before_prepare != -1,
            "dynamic JIT attach must drop the listener lock before STOP")

    success_relock = body.find("pthread_mutex_lock(&lock);\n\n        if (attach_status",
                               finish)
    require(success_relock != -1,
            "dynamic JIT attach must reacquire the listener lock for publish")


def check_startup_trace_deferred_until_after_finish(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    calibration = extract_function(source, "peak_general_overhead_bootstrapping")
    attach = extract_function(source, "peak_general_listener_attach")

    def require_after_finish(body, phase, description):
        phase_pos = body.find(f'"{phase}"')
        require(phase_pos != -1, f"missing startup phase {phase}")
        finish_pos = body.rfind("peak_detach_controller_finish_hook_mutation",
                                0,
                                phase_pos)
        require(finish_pos != -1,
                f"{description} trace must be emitted after strict finish")

    for phase in ("calibration-gum-attach-start",
                  "calibration-gum-attach-complete",
                  "calibration-gum-detach-start",
                  "calibration-gum-detach-complete"):
        require_after_finish(calibration, phase, phase)

    for phase in ("initial-gum-attach-start",
                  "initial-gum-attach-complete"):
        require_after_finish(attach, phase, phase)


def check_shutdown_stop_window_drops_listener_lock(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    detach = extract_function(source, "peak_general_listener_dettach")
    shutdown = extract_function(
        source, "peak_general_controller_shutdown_hook_unlocked"
    )

    suspend = detach.find("peak_general_listener_suspend_callbacks();")
    lock = detach.find("pthread_mutex_lock(&lock)")
    require(suspend != -1 and lock != -1 and suspend < lock,
            "shutdown teardown must suspend callbacks before taking listener lock")

    call = detach.find("peak_general_controller_shutdown_hook_unlocked")
    unlock_before_call = detach.rfind("pthread_mutex_unlock(&lock)", 0, call)
    relock_after_call = detach.find("pthread_mutex_lock(&lock)", call)
    require(call != -1 and unlock_before_call != -1 and relock_after_call != -1,
            "shutdown mutation attempts must run outside the listener lock")

    pause_failed = shutdown.find('"pause-failed"')
    resume_before_pause_failed = shutdown.rfind(
        "peak_general_controller_resume_called_threads",
        0,
        pause_failed,
    )
    require(pause_failed != -1 and resume_before_pause_failed != -1,
            "shutdown pause-failed trace must run after local resume")

    success = shutdown.find('"success"', pause_failed + 1)
    resume_before_success = shutdown.rfind(
        "peak_general_controller_resume_called_threads",
        0,
        success,
    )
    require(success != -1 and resume_before_success != -1,
            "shutdown success trace must run after local resume")


def check_detach_controller_defers_success_cleanup_until_finish(repo_root):
    source = (repo_root / "src/peak_detach_controller.c").read_text(encoding="utf-8")
    prepare_batch = extract_function(
        source, "peak_detach_controller_prepare_hook_mutation_batch"
    )
    finish = extract_function(source, "peak_detach_controller_finish_hook_mutation")

    ready = prepare_batch.find('"batch-held-mutation-ready"')
    success_return = prepare_batch.find("return TRUE;", ready)
    require(ready != -1 and success_return != -1,
            "batch prepare success path must publish held mutation then return")
    success_tail = prepare_batch[ready:success_return]
    require("free(" not in success_tail,
            "batch prepare must not free heap memory while threads are held")
    ownership_start = prepare_batch.rfind("aggregate.deferred_cleanup_1", 0, ready)
    ownership_end = prepare_batch.find("held_mutation = aggregate", ownership_start)
    require(ownership_start != -1 and ownership_end != -1 and
            "aggregate.deferred_cleanup_2" in prepare_batch[ownership_start:ready],
            "batch prepare must transfer cleanup ownership to held mutation")
    require("free(deferred_cleanup_1)" in finish and
            "free(deferred_cleanup_2)" in finish,
            "finish must release deferred batch cleanup after resume")
    resume = finish.find("peak_detach_controller_resume_backend")
    first_lock = finish.find("peak_detach_controller_lock_mutation_guard")
    unlock_before_resume = finish.rfind(
        "peak_detach_controller_unlock_mutation_guard", 0, resume
    )
    finishing_claim = finish.find("held_mutation.finishing = TRUE")
    first_claim_unlock = finish.find(
        "peak_detach_controller_unlock_mutation_guard", finishing_claim
    )
    request_match = finish.find(
        "peak_detach_controller_request_matches_held_mutation"
    )
    owner_match = finish.find(
        "peak_detach_controller_current_thread_owns_held_mutation"
    )
    lock_after_resume = finish.find("peak_detach_controller_lock_mutation_guard",
                                    resume)
    copy_cleanup = finish.find("deferred_cleanup_1 = held_mutation.deferred_cleanup_1")
    unlock = finish.find("peak_detach_controller_unlock_mutation_guard", copy_cleanup)
    free_cleanup = finish.find("free(deferred_cleanup_1)", unlock)
    require(resume != -1 and lock_after_resume != -1 and copy_cleanup != -1 and
            unlock != -1 and free_cleanup != -1 and first_lock != -1 and
            unlock_before_resume != -1 and finishing_claim != -1 and
            first_claim_unlock != -1 and request_match != -1 and
            owner_match != -1 and
            first_lock < request_match < owner_match < finishing_claim <
            first_claim_unlock < resume and
            unlock_before_resume < resume and
            resume < lock_after_resume < copy_cleanup < unlock < free_cleanup,
            "finish must claim held mutation ownership, release the guard before resume, then cleanup after resume")


def check_detach_controller_trace_not_written_while_held(repo_root):
    source = (repo_root / "src/peak_detach_controller.c").read_text(encoding="utf-8")
    trace_backend = extract_function(
        source, "peak_detach_controller_trace_backend_phase"
    )
    trace_signal = extract_function(
        source, "peak_detach_controller_trace_signal_phase"
    )

    held_guard = trace_backend.find("peak_detach_controller_stop_window_trace_deferred()")
    fd_load = trace_backend.find("trace_diagnostics_fd")
    write_call = trace_backend.find("write(fd")
    require(held_guard != -1,
            "controller diagnostic trace must explicitly suppress writes while a STOP window is active")
    require(fd_load != -1 and write_call != -1 and held_guard < fd_load < write_call,
            "controller diagnostic trace must check the held-window guard before touching the trace fd")

    signal_held_guard = trace_signal.find("peak_detach_controller_stop_window_trace_deferred()")
    signal_fd_load = trace_signal.find("trace_diagnostics_fd")
    signal_write_call = trace_signal.find("write(fd")
    require(signal_held_guard != -1,
            "signal diagnostic trace must explicitly suppress writes while a STOP window is active")
    require(signal_fd_load != -1 and signal_write_call != -1 and
            signal_held_guard < signal_fd_load < signal_write_call,
            "signal diagnostic trace must check the held-window guard before touching the trace fd")


def check_general_trace_uses_persistent_file(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    trace_mutation = extract_function(
        source, "peak_general_controller_trace_mutation_detail"
    )
    trace_event = extract_function(
        source, "peak_general_controller_trace_event_detail"
    )
    trace_file = extract_function(
        source, "peak_general_controller_trace_file_locked"
    )
    close_file = extract_function(
        source, "peak_general_controller_close_trace_file_locked"
    )

    for label, body in (("mutation", trace_mutation), ("event", trace_event)):
        require("fopen(" not in body and "fclose(" not in body,
                f"{label} trace path must not open/close the CSV per row")
        require("peak_general_controller_trace_file_locked()" in body,
                f"{label} trace path must use the persistent trace file")
        require("fflush(fp)" in body,
                f"{label} trace path must flush rows for crash diagnostics")

    require("O_CLOEXEC" in trace_file and "fdopen" in trace_file and
            "setvbuf" in trace_file,
            "general listener trace file must be persistent, close-on-exec, and line buffered")
    require("fstat(fileno(peak_controller_trace_fp)" in trace_file and
            "st.st_nlink == 0" in trace_file,
            "general listener trace file must reopen if the trace path is unlinked after startup")
    require("fclose(peak_controller_trace_fp)" in close_file,
            "general listener trace teardown must close the persistent trace file")
    require("peak_general_controller_close_trace_file_locked()" in source,
            "general listener teardown must call persistent trace cleanup")


def check_general_trace_wait_diagnostics_are_throttled(repo_root):
    source = (repo_root / "src/general_listener.c").read_text(encoding="utf-8")
    probe_closeout = extract_function(
        source, "peak_general_listener_trace_probe_closeout_unlocked"
    )
    heartbeat = extract_function(source, "peak_heartbeat_monitor")

    require("peak_heartbeat_last_global_reattach_wait_trace_time_s" in source and
            "peak_heartbeat_last_probe_closeout_wait_trace_time_s" in source,
            "high-frequency wait diagnostics must have explicit throttle timestamps")
    require("\"reserved-wait\"" in probe_closeout and
            "peak_heartbeat_last_probe_closeout_wait_trace_time_s" in probe_closeout and
            "peak_general_trace_liveness_interval_s()" in probe_closeout,
            "probe closeout reserved-wait trace must be throttled to the liveness interval")
    require("\"global-reattach\"" in heartbeat and
            "\"no-admission\"" in heartbeat and
            "peak_heartbeat_last_global_reattach_wait_trace_time_s" in heartbeat and
            "peak_general_trace_liveness_interval_s()" in heartbeat,
            "global reattach no-admission trace must be throttled to the liveness interval")
    require("peak_heartbeat_last_global_reattach_wait_trace_time_s = 0.0" in source and
            "peak_heartbeat_last_probe_closeout_wait_trace_time_s = 0.0" in source,
            "trace wait throttle timestamps must reset at listener teardown")


def check_auto_signal_fallback_is_stable_only(repo_root):
    source = (repo_root / "src/peak_detach_controller.c").read_text(encoding="utf-8")
    tests_cmake = (
        repo_root / "test/detach_controller/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    fallback = extract_function(
        source, "peak_detach_controller_status_allows_auto_signal_fallback"
    )
    single_prepare = extract_function(
        source, "peak_detach_controller_prepare_hook_mutation"
    )
    batch_prepare = extract_function(
        source, "peak_detach_controller_prepare_hook_mutation_batch"
    )

    require("PEAK_DETACH_STATUS_PERMISSION_DENIED" in fallback and
            "PEAK_DETACH_STATUS_UNSUPPORTED" in fallback,
            "auto helper fallback must still handle stable permission/unsupported helper failures")
    require("PEAK_DETACH_STATUS_TIMEOUT" in fallback,
            "auto helper fallback must handle bounded helper STOP timeout")
    require("stop-permission stop-timeout stop-timeout-delayed stop-hang stop-unsupported spawn-clone-fail" in tests_cmake and
            "stop-permission stop-timeout stop-timeout-delayed stop-hang stop-unsupported" in tests_cmake,
            "controller tests must prove helper STOP timeout/hang can fall back after cleanup")
    require("TIMEOUT 10" in tests_cmake,
            "hung fake-helper timeout tests must have bounded CTest timeouts")
    require("peak_detach_controller_cache_auto_signal_performance_fallback" in source and
            "PEAK_AUTO_HELPER_PERF_FALLBACK_STOP_WINDOW_US_ENV" in source and
            "PEAK_AUTO_HELPER_PERF_FALLBACK_DEFAULT_STOP_WINDOW_US" in source and
            "auto-performance-fallback" in tests_cmake and
            "FAKE_DETACH_HELPER_STOP_DELAY_MS=30" in tests_cmake,
            "auto helper fallback must cover configurable slow successful helper stop windows, not only hard failures")
    require("auto-fallback-not-cached-on-signal-stop-failure" in tests_cmake,
            "auto fallback tests must prove failed signal STOP does not cache signal fallback")
    require("success-zero-after-timeout" in
            (repo_root / "test/detach_controller/test_detach_controller.c").read_text(encoding="utf-8"),
            "deadline-exhausted auto fallback tests must retry helper instead of caching signal")
    for label, body, marker in (
        ("single", single_prepare, '"stop-helper-fallback-start"'),
        ("batch", batch_prepare, '"batch-stop-helper-fallback-start"'),
    ):
        start = body.find(marker)
        close_pos = body.find("helper_closed = peak_detach_controller_close_helper()",
                              start)
        cache_pos = body.find("peak_detach_controller_cache_auto_signal_fallback",
                              close_pos)
        signal_stop_pos = body.find(
            "peak_detach_controller_signal_stop_threads", close_pos
        )
        retry_deadline = body.find(
            "peak_detach_controller_deadline_remaining_ms", close_pos
        )
        require(start != -1 and close_pos != -1 and cache_pos != -1 and
                signal_stop_pos != -1 and retry_deadline != -1 and
                close_pos < retry_deadline < signal_stop_pos < cache_pos,
                f"{label} auto STOP fallback must cache signal only after signal STOP succeeds")


def check_helper_verify_unstopped_restarts_snapshot(repo_root):
    source = (repo_root / "src/peak_detach_helper.c").read_text(encoding="utf-8")
    body = extract_function(source, "stop_target_threads")

    verify_pos = body.find("verify_no_unstopped_threads")
    require(verify_pos != -1,
            "helper STOP must verify no unheld target threads remain")
    eagain_pos = body.find("*errno_out == EAGAIN", verify_pos)
    retry_pos = body.find("cleanup_or_retry_stop_snapshot", eagain_pos)
    restart_pos = body.find("goto restart_stop_snapshot", retry_pos)
    sleep_pos = body.find("usleep", eagain_pos, restart_pos if restart_pos != -1 else None)
    require(eagain_pos != -1 and retry_pos != -1 and restart_pos != -1,
            "helper verify-stage EAGAIN must release held threads and restart the STOP snapshot")
    require(sleep_pos == -1,
            "helper verify-stage EAGAIN must not wait on a stale held-thread snapshot")


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
            "peak_signal_policy_range_is_writable" in signal_policy and
            'fopen("/proc/self/maps", "r")' in signal_policy and
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
    require("peak_signal_policy_resolve_timer_create" in signal_policy and
            "dlvsym(RTLD_NEXT, \"timer_create\", \"GLIBC_2.3.3\")" in signal_policy,
            "timer_create interposer must preserve glibc's user-facing timer_t ABI")
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
    require("peak_detach_controller_end_thread_creation_gate" in controller,
            "controller must release the new-thread gate on finish/failure")
    single_prepare = extract_function(
        controller, "peak_detach_controller_prepare_hook_mutation"
    )
    batch_prepare = extract_function(
        controller, "peak_detach_controller_prepare_hook_mutation_batch"
    )
    for label, prepare_body, validate_marker in (
        ("single", single_prepare, '"validate-snapshots-start"'),
        ("batch", batch_prepare, '"batch-validate-snapshots-start"'),
    ):
        stop_start = prepare_body.find(
            "peak_detach_controller_note_stop_window_started"
        )
        gate_begin = prepare_body.find(
            "peak_detach_controller_begin_thread_creation_gate"
        )
        validate_start = prepare_body.find(validate_marker)
        require(stop_start != -1 and gate_begin != -1 and validate_start != -1,
                f"{label} strict mutation path must gate new threads, stop threads, then validate snapshots")
        require(gate_begin < stop_start < validate_start,
                f"{label} strict mutation path must begin the new-thread gate before STOP starts and validate after STOP")
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
            "invalid raw timer_create sigevent pointer was accepted" in runtime_hotloop and
            "invalid raw pselect6 sigmask pointer was accepted" in runtime_hotloop and
            "invalid pthread_sigmask pointer returned unexpected error" in runtime_hotloop and
            "read-only sigaction query returned unexpected errno" in runtime_hotloop and
            "invalid-pointer probes migrated PEAK signal" in runtime_hotloop,
            "signal policy must fail closed on unreadable raw/public pointers instead of crashing or migrating")
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
    benchmark_wrapper = (repo_root / "benchmarks/detach/hpc_extreme_detach_bench.sh").read_text(encoding="utf-8")
    hpc_config_path = repo_root / ".codex-hpc.toml"
    require("--detach-backend" in benchmark_runner,
            "hotloop benchmark must support backend-pinned signal stress")
    require("if args.require_reattach:" not in benchmark_runner or
            "PEAK_REATTACH_COOLDOWN_MS" not in benchmark_runner,
            "short reattach stress runs must not depend on deprecated reattach cooldown")
    require("PEAK_HPC_REATTACH_COOLDOWN_MS" not in benchmark_wrapper,
            "HPC smoke wrapper must not depend on deprecated reattach cooldown")
    require("--min-global-reattached-hot-hooks 64" in tests and
            "--global-hot-reattach-final-window-s 30" in tests and
            "--min-global-hot-reattach-final-window 8" in tests and
            "--min-global-hot-reattach-final-window-distinct-hooks 8" in tests and
            "--min-global-hot-probe-detached-dwell-count 128" in tests and
            "--min-probe-detached-dwell-s 4.5" in tests and
            "--min-probe-dwell-hot-increases 128" in tests and
            "--min-probe-dwell-hot-increase-global-hot-hooks 64" in tests and
            "--max-probe-dwell-invalid-count 0" in tests and
            "--min-trace-ordered-reattach-cycles 128" in tests and
            "--min-trace-ordered-reattach-distinct-hot-hooks 128" in tests and
            "--min-trace-ordered-global-hot-reattach-cycles 64" in tests and
            "--min-trace-ordered-global-hot-reattach-distinct-hooks 64" in tests and
            "--sample-counts-interval-ms 50" in tests and
            "--min-strict-global-reattach-ordered-distinct-hot-hooks 4" in tests and
            "--min-liveness-samples 8" in tests and
            "--min-liveness-window-count 3" in tests,
            "manual many-target stress proof must require broad sustained hot-target reattach coverage")
    if hpc_config_path.exists() and repo_file_is_tracked(
        repo_root,
        ".codex-hpc.toml",
    ):
        hpc_config = hpc_config_path.read_text(encoding="utf-8")
        require("PEAK_HPC_REATTACH_COOLDOWN_MS" not in hpc_config,
                "HPC suite config must not depend on deprecated reattach cooldown")
        require("--min-global-reattached-hot-hooks 64" in hpc_config and
                "--global-hot-reattach-final-window-s 30" in hpc_config and
                "--min-global-hot-reattach-final-window 8" in hpc_config and
                "--min-global-hot-reattach-final-window-distinct-hooks 8" in hpc_config and
                "--min-global-hot-probe-detached-dwell-count 128" in hpc_config and
                "--min-probe-detached-dwell-s 4.5" in hpc_config and
                "--min-probe-dwell-hot-increases 128" in hpc_config and
                "--min-probe-dwell-hot-increase-global-hot-hooks 64" in hpc_config and
                "--max-probe-dwell-invalid-count 0" in hpc_config and
                "--min-trace-ordered-reattach-cycles 128" in hpc_config and
                "--min-trace-ordered-reattach-distinct-hot-hooks 128" in hpc_config and
                "--min-trace-ordered-global-hot-reattach-cycles 64" in hpc_config and
                "--min-trace-ordered-global-hot-reattach-distinct-hooks 64" in hpc_config and
                "--min-strict-global-reattach-ordered-distinct-hot-hooks 4" in hpc_config and
                "--min-liveness-samples 8" in hpc_config and
                "--min-liveness-window-count 3" in hpc_config,
                "HPC many-target stress proof must require broad sustained hot-target reattach coverage")
        require("--target peak peak_detach_helper test_detach_many_targets_stress" in hpc_config,
                "HPC many-target proof must explicitly build the helper used by auto/backend validation")
    require("PEAK_DETACH_BACKEND=helper" in tests and
            "PEAK_DETACH_BACKEND=helper" in controller_tests_cmake,
            "fake-helper tests must force helper backend, not auto signal fallback")
    require("${PROJECT_SOURCE_DIR}/src/peak_signal_policy.c" in controller_tests_cmake and
            "PEAK_ENABLE_TEST_HOOKS=1" in controller_tests_cmake and
            "peak_signal_policy_test_block_reserved_for_current_thread() == 0" in controller_tests and
            "signal-reserved-blocked" in controller_tests,
            "controller tests must link signal policy and prove selected-signal blocked reason")


def check_exec_chain_syscall_bridge_invariants(repo_root):
    exec_interceptor = (repo_root / "src/exec_interceptor.c").read_text(
        encoding="utf-8"
    )
    general_listener = (repo_root / "src/general_listener.c").read_text(
        encoding="utf-8"
    )
    run_exec_checks = (
        repo_root / "test/exec_chain/run_exec_chain_check.py"
    ).read_text(encoding="utf-8")
    readme = (repo_root / "README.md").read_text(encoding="utf-8")
    syscall_bridge = extract_function(exec_interceptor, "peak_exec_handle_syscall")

    require("peak_exec_raw_syscall_depth" not in exec_interceptor,
            "raw clone syscall bridge must not mutate shared TLS recursion guards")
    clone_start = exec_interceptor.find("clone(int (*fn)(void*)")
    clone_end = exec_interceptor.find("static peak_execvpe_fn", clone_start)
    require(clone_start >= 0 and clone_end > clone_start,
            "missing clone wrapper")
    clone_body = exec_interceptor[clone_start:clone_end]
    constructor_body = extract_function(exec_interceptor, "peak_exec_register_atfork")
    require("peak_real_clone()" in constructor_body,
            "clone symbol must be resolved before possible multithreaded fork")
    require("in_fork_like_child" in clone_body and
            "stack_start" in clone_body and
            clone_body.find("in_fork_like_child") < clone_body.find("malloc("),
            "fork-like child clone wrapper must avoid heap allocation before exec")
    require("number == SYS_vfork" in syscall_bridge and
            "return 0;" in syscall_bridge[
                syscall_bridge.find("number == SYS_vfork"):
                syscall_bridge.find("number == SYS_execveat")
            ],
            "raw SYS_vfork must be passed through instead of emulated")
    readme_flat = " ".join(readme.split())
    require("syscall(SYS_vfork)" in readme_flat and
            "not part of PEAK's exec-chain support surface" in readme_flat,
            "README must document the raw SYS_vfork unsupported boundary")
    require("posix_spawn_preflight_unavailable_injection" in run_exec_checks and
            "PEAK_TEST_EXEC_PREFLIGHT_UNAVAILABLE" in run_exec_checks and
            "posix_spawn_bad_env_failure_non_destructive" in run_exec_checks,
            "posix_spawn tests must cover preflight fallback injection and conclusive bad-env failure")
    checkpoint_body = extract_function(
        general_listener, "peak_general_listener_stream_exec_checkpoint"
    )
    write_all_body = extract_function(general_listener, "peak_checkpoint_write_all")
    require("peak_checkpoint_open_exclusive" in checkpoint_body and
            "peak_checkpoint_close_fd" in checkpoint_body and
            "peak_checkpoint_unlink_path" in checkpoint_body and
            "peak_checkpoint_raw_syscall6(SYS_write" in write_all_body,
            "exec checkpoint file I/O must use raw syscall helpers on Linux")
    require("exec_checkpoint_write_target_no_reentry" in run_exec_checks,
            "exec-chain tests must cover checkpointing while profiling write")


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
    check_hot_callback_detach_count_gate(repo_root)
    check_reattach_probe_followup_budget(repo_root)
    check_global_detach_probe_ownership(repo_root)
    check_heartbeat_detached_hooks_reattach_as_probes(repo_root)
    check_reattach_probe_full_cost_accounting(repo_root)
    check_reattach_probe_closeout_drains_unbounded(repo_root)
    check_general_controller_dlopen_drain_order(repo_root)
    check_strict_batch_stop_window_drops_listener_lock(repo_root)
    check_dynamic_attach_stop_window_drops_listener_lock(repo_root)
    check_startup_trace_deferred_until_after_finish(repo_root)
    check_shutdown_stop_window_drops_listener_lock(repo_root)
    check_detach_controller_defers_success_cleanup_until_finish(repo_root)
    check_detach_controller_trace_not_written_while_held(repo_root)
    check_general_trace_uses_persistent_file(repo_root)
    check_general_trace_wait_diagnostics_are_throttled(repo_root)
    check_auto_signal_fallback_is_stable_only(repo_root)
    check_helper_verify_unstopped_restarts_snapshot(repo_root)
    check_exclusive_time_nonnegative(repo_root)
    check_dlopen_test_hook_visibility(repo_root)
    check_shutdown_fail_closed_docs(repo_root)
    check_signal_backend_strict_invariants(repo_root)
    check_exec_chain_syscall_bridge_invariants(repo_root)
    print("detach_lifecycle_invariants_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
