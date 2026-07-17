#!/usr/bin/env python3

import pathlib
import re
import sys


def require(condition, message):
    if not condition:
        print(message, file=sys.stderr)
        raise SystemExit(1)


def read_source(repo_root, rel):
    source = (repo_root / rel).read_text(encoding="utf-8")
    if rel != "src/general_listener.c":
        return source

    def include_fragment(match):
        fragment = match.group(1)
        return (
            repo_root / "src/general_listener" / fragment
        ).read_text(encoding="utf-8")

    return re.sub(
        r'^#include "general_listener/([^"]+\.inc)"$',
        include_fragment,
        source,
        flags=re.MULTILINE,
    )


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
    source = read_source(repo_root, "src/general_listener.c")
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
    controller_source = (repo_root / "src/detach_controller.c").read_text(
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


def check_dlopen_detach_transaction(repo_root):
    source = (repo_root / "src/dlopen_interceptor.c").read_text(encoding="utf-8")
    body = extract_function(source, "dlopen_interceptor_dettach")
    match = re.search(r"gum_interceptor_detach\s*\(\s*dlopen_interceptor", body)
    require(match is not None, "dlopen listener teardown must detach its listener")
    before = body[max(0, match.start() - 180):match.start()]
    after = body[match.end():match.end() + 180]
    require("gum_interceptor_begin_transaction(dlopen_interceptor)" in before,
            "dlopen listener detach is missing nearby begin_transaction")
    require("gum_interceptor_end_transaction(dlopen_interceptor)" in after,
            "dlopen listener detach is missing nearby end_transaction")


def check_dlopen_resolution_lock_order(repo_root):
    source = (repo_root / "src/dlopen_interceptor.c").read_text(encoding="utf-8")
    body = extract_function(source, "dlopen_interceptor_attach_from_request")
    dlsym_position = body.find("dlsym(request->handle")
    require(dlsym_position != -1,
            "dynamic dlopen resolution must use the request handle")

    before_dlsym = body[:dlsym_position]
    last_lock = before_dlsym.rfind("peak_general_listener_controller_lock();")
    last_unlock = before_dlsym.rfind("peak_general_listener_controller_unlock();")
    next_lock = body.find("peak_general_listener_controller_lock();", dlsym_position)
    require(last_unlock > last_lock,
            "dynamic dlsym must not hold the general-listener lock")
    require(next_lock > dlsym_position,
            "dynamic attach must revalidate state under the general-listener lock")


def check_dlopen_fftw_scope_and_fork_guard(repo_root):
    source = (repo_root / "src/dlopen_interceptor.c").read_text(encoding="utf-8")
    membership = extract_function(
        source, "dlopen_interceptor_is_fftw_group_symbol"
    )
    attach = extract_function(source, "dlopen_interceptor_attach_from_request")
    sync = extract_function(source, "dlopen_interceptor_attach_fftw_before_return")
    admission = extract_function(source, "dlopen_interceptor_callback_is_admitted")
    on_enter = extract_function(source, "dlopen_interceptor_on_enter")
    on_leave = extract_function(source, "dlopen_interceptor_on_leave")
    enable = extract_function(source, "dlopen_interceptor_enable_dynamic_attach")
    begin_callback_body = extract_function(
        source, "dlopen_interceptor_begin_callback"
    )
    shutdown = extract_function(
        source, "dlopen_interceptor_shutdown_dynamic_attach"
    )
    detach = extract_function(source, "dlopen_interceptor_dettach")
    provider_probe = extract_function(
        source, "dlopen_interceptor_handle_may_resolve_fftw"
    )
    unresolved_counts = extract_function(
        source, "dlopen_interceptor_unresolved_counts"
    )
    resolved_mark = extract_function(
        source, "dlopen_interceptor_mark_target_resolved_unlocked"
    )
    completed_scan = extract_function(
        source, "dlopen_interceptor_fftw_module_scan_completed"
    )
    retain_handle = extract_function(
        source, "dlopen_interceptor_retain_dynamic_handle"
    )

    require("source_target_array_FFTW[i]" in membership and
            "strcmp(name, source_target_array_FFTW[i]) == 0" in membership and
            "strncmp(" not in membership,
            "synchronous FFTW scope must use exact built-in group membership")
    require("dlopen_interceptor_target_matches_scope_unlocked" in attach and
            "request->scope" in attach,
            "dynamic dlopen resolution must filter every request by scope")
    duplicate_check = attach.find("if (duplicate_address)")
    duplicate_mark = attach.find("peak_demangled_strings[i] = g_strdup(",
                                 duplicate_check)
    candidate_init = attach.find(
        "dlopen_interceptor_initialize_attach_candidate(",
        duplicate_check,
    )
    require(duplicate_check != -1 and duplicate_mark != -1 and
            candidate_init != -1 and
            duplicate_check < duplicate_mark < candidate_init,
            "duplicate dynamic addresses must be terminal-skipped before attach")
    require("request.scope = PEAK_DLOPEN_ATTACH_FFTW_ONLY" in sync and
            "request.scope = retry_scope" in sync,
            "synchronous resolution must remain FFTW-only and preserve one retry scope")
    require(on_leave.count("dlopen_interceptor_enqueue_dynamic_attach_request(") == 1,
            "one dlopen callback must have only one asynchronous enqueue site")
    require("if (dlopen_sync_fftw_enabled)" in on_leave and
            "Preserve the original asynchronous non-FFTW fast path" in on_leave,
            "non-FFTW-only dlopen callbacks must not gain a synchronous target scan")
    require("dlopen_interceptor_module_identity" in on_leave and
            "PEAK_DLOPEN_SYNC_REQUEUED" not in on_leave,
            "on-leave must use canonical identity and let a successful requeue suppress duplicate work")
    require("dlerror();" in attach and "dlerror();" in on_leave,
            "PEAK loader lookups must not leak dlerror state after successful dlopen")
    probe_position = on_leave.find(
        "dlopen_interceptor_handle_may_resolve_fftw(handle)"
    )
    identity_position = on_leave.find(
        "dlopen_interceptor_module_identity("
    )
    require(probe_position != -1 and identity_position != -1 and
            probe_position < identity_position and
            all(name in provider_probe for name in (
                "fftw_malloc", "fftwf_malloc", "fftwl_malloc",
                "fftwq_malloc", "rfftw_create_plan", "fftw_threads",
                "rfftw_threads")) and
            '"fftw_create_plan"' not in provider_probe and
            "strstr(" not in provider_probe,
            "FFTW dlopen must use filename-independent ABI probes before module reopen and full scan")
    maybe_load = unresolved_counts.find(
        "atomic_load_explicit(&dlopen_may_have_unresolved_non_fftw"
    )
    early_return = unresolved_counts.find("return result;", maybe_load)
    mixed_lock = unresolved_counts.find(
        "peak_general_listener_controller_lock();"
    )
    require(unresolved_counts.count("atomic_load_explicit(") == 2 and
            maybe_load != -1 and early_return != -1 and mixed_lock != -1 and
            maybe_load < early_return < mixed_lock and
            "for (" in unresolved_counts and
            "dlopen_unresolved_non_fftw_count" not in source and
            "dlopen_unresolved_non_fftw_count" not in resolved_mark,
            "pure FFTW callbacks must use the atomic fast path while mixed targets are classified from live listener state")

    pid_check = on_enter.find("dlopen_interceptor_callback_is_admitted()")
    cancel_disable = on_enter.find("pthread_setcancelstate(PTHREAD_CANCEL_DISABLE")
    begin_callback_position = on_enter.find("dlopen_interceptor_begin_callback()")
    filename_capture = on_enter.find("invocation->filename = filename")
    require(pid_check != -1 and cancel_disable != -1 and
            begin_callback_position != -1 and filename_capture != -1 and
            pid_check < cancel_disable < begin_callback_position < filename_capture,
            "fork-child guard and cancellation disable must precede callback state")
    require("g_strdup(filename)" not in on_enter and
            "g_free(invocation->filename)" not in on_leave,
            "unrelated dlopen callbacks must not allocate merely to preserve a live call argument")
    completed_lookup = on_leave.find(
        "dlopen_interceptor_fftw_module_scan_completed(handle)"
    )
    require(completed_lookup != -1 and
            completed_lookup < identity_position and
            "dlopen_interceptor_primary_module_token(handle)" in completed_scan and
            "g_hash_table_contains(dlopen_completed_fftw_modules" in completed_scan,
            "only an identical completed primary FFTW module may skip a full scan")
    require("request->scope == PEAK_DLOPEN_ATTACH_FFTW_ONLY" in attach and
            "request->scope == PEAK_DLOPEN_ATTACH_ALL" in attach and
            "resolved_fftw_from_handle" in attach and
            "peak_hook_address_count == target_count" in attach and
            "resolved_targets[i].address != NULL" in attach and
            "dlopen_interceptor_target_is_unresolved_unlocked(i)" in attach and
            "completed_fftw_scan" in retain_handle and
            "g_hash_table_add(dlopen_completed_fftw_modules, module_token)" in retain_handle,
            "FFTW module cache publication must require a complete scan and a retained primary handle")
    require("atomic_load_explicit(&dlopen_listener_owner_pid" in admission and
            "getpid() == owner" in admission and
            "pthread_mutex" not in admission and "g_" not in admission,
            "fork-child admission predicate must be PID-only and lock-free")
    require("atomic_store_explicit(&dlopen_listener_owner_pid" in enable and
            "getpid()" in enable,
            "dlopen callback admission must open only with dynamic attach")
    owner_open = enable.find(
        "atomic_store_explicit(&dlopen_listener_owner_pid"
    )
    state_open = enable.find(
        "dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_OPEN"
    )
    enable_unlock = enable.find(
        "pthread_mutex_unlock(&dynamic_attach_gate_mutex)"
    )
    require(owner_open != -1 and state_open != -1 and enable_unlock != -1 and
            owner_open < state_open < enable_unlock,
            "owner PID must publish before OPEN while holding the gate mutex")
    require("dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN" in begin_callback_body and
            "atomic_load_explicit(&dlopen_listener_owner_pid" in begin_callback_body and
            "active_dlopen_callback_count++" in begin_callback_body,
            "callback count admission must be revalidated under the gate mutex")
    owner_close = detach.find(
        "atomic_store_explicit(&dlopen_listener_owner_pid"
    )
    close_lock = detach.rfind(
        "pthread_mutex_lock(&dynamic_attach_gate_mutex)",
        0,
        owner_close,
    )
    close_unlock = detach.find(
        "pthread_mutex_unlock(&dynamic_attach_gate_mutex)",
        owner_close,
    )
    require(owner_close != -1 and close_lock != -1 and close_unlock != -1 and
            close_lock < owner_close < close_unlock,
            "teardown must close callback admission under the gate mutex")
    pinned_owner_close = shutdown.find(
        "atomic_store_explicit(&dlopen_listener_owner_pid"
    )
    pinned_state_close = shutdown.find(
        "dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN"
    )
    pinned_unlock = shutdown.find(
        "pthread_mutex_unlock(&dynamic_attach_gate_mutex)"
    )
    require(pinned_owner_close != -1 and pinned_state_close != -1 and
            pinned_unlock != -1 and
            pinned_owner_close < pinned_state_close < pinned_unlock,
            "pinned dlopen shutdown must make the listener inert under the gate mutex")

    peak_source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    fini = extract_function(peak_source, "peak_fini_impl")
    pinned_shutdown = fini.find(
        "dlopen_interceptor_shutdown_dynamic_attach()"
    )
    shutdown_failure = fini.find(
        "if (!dlopen_shutdown_flushed)", pinned_shutdown
    )
    report = fini.find("peak_general_listener_print(", pinned_shutdown)
    require(pinned_shutdown != -1 and shutdown_failure != -1 and report != -1 and
            pinned_shutdown < shutdown_failure < report,
            "MPI pinned-listener path must drain dlopen callbacks before report metadata is freed")
    require("if (!invocation->callback_admitted)" in on_leave,
            "dlopen on-leave must skip callbacks rejected by the fork PID guard")
    end_callback = on_leave.find("dlopen_interceptor_end_callback()")
    cancel_restore = on_leave.find(
        "pthread_setcancelstate(invocation->previous_cancel_state"
    )
    require(end_callback != -1 and cancel_restore != -1 and
            end_callback < cancel_restore,
            "dlopen cancellation must be restored only after callback cleanup")


def check_safe_arm64_plt_reads_and_close_overlap_guard(repo_root):
    unsafe = (repo_root / "src/unsafe_gum_prologue.c").read_text(
        encoding="utf-8"
    )
    arm64 = extract_function(unsafe, "peak_arm64_target_branches_to_elf_plt")
    require(arm64.count("gum_memory_read(") == 2 and
            "UINTPTR_MAX" in arm64 and "saved_errno" in arm64,
            "Arm64 branch-to-PLT detection must safely read both ranges and preserve errno")
    require("memcpy(plt, plt_address" not in arm64,
            "Arm64 branch-to-PLT detection must not directly read a computed target")
    plan = extract_function(unsafe, "peak_gum_target_attach_plan")
    require("plan_out->mutation_address = plt_address" in plan and
            "plan_out->mutation_guard_size = GUM_PEAK_MAX_PROLOGUE_SIZE" in plan and
            "defined(GUM_PEAK_MAX_PROLOGUE_SIZE)" in plan,
            "Arm64 B-to-PLT attach plan must guard Gum's real mutation address")
    target_attach = extract_function(
        unsafe, "peak_gum_interceptor_attach_target"
    )
    require("plan->attach_exact_entry" in target_attach and
            "gum_interceptor_peak_attach_exact" in target_attach and
            "&plan->options" in target_attach and
            "gum_interceptor_attach" in target_attach,
            "target attach helper must apply exact-entry and fallback plans")

    general = read_source(repo_root, "src/general_listener.c")
    initial_attach = extract_function(general, "peak_general_listener_attach")
    jit_attach = extract_function(
        general, "peak_general_listener_dynamic_attach_symbol"
    )
    dlopen_source = (repo_root / "src/dlopen_interceptor.c").read_text(
        encoding="utf-8"
    )
    dlopen_plan = extract_function(
        dlopen_source, "dlopen_interceptor_initialize_attach_candidate"
    )
    normalized_dlopen_plan = re.sub(r"\s+", " ", dlopen_plan)
    for label, body in (
        ("initial", initial_attach),
        ("JIT", jit_attach),
    ):
        normalized = re.sub(r"\s+", " ", body)
        plan_position = body.find("peak_gum_target_attach_plan(")
        blocked_position = body.find(".blocked_pc_start =")
        prepare_position = body.find("peak_detach_controller_prepare_hook_mutation")
        require(plan_position != -1 and blocked_position != -1 and
                prepare_position != -1 and
                plan_position < blocked_position < prepare_position,
                f"{label} first attach must guard its planned mutation range")
        require(
            ".blocked_pc_start = attach_plan.mutation_guard_size > 0 ? "
            "attach_plan.mutation_address : NULL" in normalized and
            ".blocked_pc_size = attach_plan.mutation_guard_size" in normalized and
            "peak_gum_interceptor_attach_target(" in normalized and
            "&attach_plan" in normalized,
            f"{label} attach must use one plan for both strict guard and Gum options",
        )
    require(
        "peak_gum_target_attach_plan(" in dlopen_plan and
        ".blocked_pc_start = candidate->attach_plan.mutation_guard_size > 0 ? "
        "candidate->attach_plan.mutation_address : NULL" in normalized_dlopen_plan and
        ".blocked_pc_size = candidate->attach_plan.mutation_guard_size" in normalized_dlopen_plan,
        "dlopen first attach must put the exact planned mutation range in the request",
    )
    dlopen_scalar_attach = extract_function(
        dlopen_source, "dlopen_interceptor_attach_candidate_scalar"
    )
    dlopen_batch_attach = extract_function(
        dlopen_source, "dlopen_interceptor_attach_candidate_batch"
    )
    require("peak_gum_interceptor_attach_target(" in dlopen_scalar_attach and
            "&candidate->attach_plan" in dlopen_scalar_attach and
            "peak_gum_interceptor_attach_target(" in dlopen_batch_attach and
            "&candidates[i].attach_plan" in dlopen_batch_attach,
            "dlopen Gum attach must use the same plan as its strict guard")
    dlopen_attach = extract_function(
        dlopen_source, "dlopen_interceptor_attach_from_request"
    )
    require(dlopen_attach.find(
                "dlopen_interceptor_initialize_attach_candidate(") != -1 and
            dlopen_attach.find("dlopen_interceptor_attach_candidates(") != -1,
            "dlopen mutation requests must be planned and processed through the guarded helpers")

    syscall = (repo_root / "src/syscall_interceptor.c").read_text(
        encoding="utf-8"
    )
    overlap = extract_function(syscall, "peak_close_overlaps_nocancel_entry")
    attach = extract_function(syscall, "syscall_interceptor_attach")
    guard = attach.find("peak_close_overlaps_nocancel_entry(hook_address)")
    replace = attach.find("gum_interceptor_replace_fast")
    require("__close_nocancel" in overlap and
            overlap.count("gum_process_find_function_range") == 2 and
            "PEAK_GUM_X86_MAX_REDIRECT_SIZE" in overlap,
            "close support hook must detect overlapping and nearby __close_nocancel entries")
    require(guard != -1 and replace != -1 and guard < replace,
            "close overlap guard must run before Gum mutates the close entry")


def check_peak_init_heartbeat_order(repo_root):
    source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    body = extract_function(source, "peak_init")
    fini = extract_function(source, "peak_fini_impl")

    group_load_position = body.find("load_symbols_from_array(PEAK_TARGET_GROUP_ENV")
    deduplicate_position = body.find("peak_deduplicate_target_names(")
    require(group_load_position != -1 and deduplicate_position != -1 and
            group_load_position < deduplicate_position,
            "explicit and group target names must be deduplicated before setup")

    main_time_position = body.find("peak_main_time = peak_second();")
    runtime_start_position = body.find("peak_general_listener_note_runtime_start")
    heartbeat_position = body.find("pthread_create(&heartbeat_thread")
    require(main_time_position != -1,
            "peak_init must initialize peak_main_time")
    require(runtime_start_position != -1 and
            main_time_position < runtime_start_position,
            "peak_init must publish the runtime start timestamp after peak_main_time")
    require(heartbeat_position != -1,
            "peak_init must create the heartbeat thread explicitly")
    require(runtime_start_position < heartbeat_position,
            "runtime start timestamp must be initialized before heartbeat thread startup")

    general = read_source(repo_root, "src/general_listener.c")
    general_attach = extract_function(general, "peak_general_listener_attach")
    require("peak_general_listener_controller_start" not in general_attach,
            "general listener attach must not start mutation processing")
    general_attach_position = body.find("peak_general_listener_attach()")
    syscall_attach_position = body.find("syscall_interceptor_attach()")
    dlopen_attach_position = body.find("dlopen_interceptor_attach()")
    malloc_attach_position = body.find("malloc_interceptor_attach()")
    controller_start_position = body.find(
        "peak_general_listener_controller_start()"
    )
    dynamic_enable_position = body.find("dlopen_interceptor_enable_dynamic_attach()")
    require(-1 not in (general_attach_position, syscall_attach_position,
                       dlopen_attach_position, malloc_attach_position,
                       controller_start_position, dynamic_enable_position) and
            general_attach_position < syscall_attach_position <
            dlopen_attach_position < malloc_attach_position <
            controller_start_position < dynamic_enable_position <
            heartbeat_position,
            "startup Gum hooks must finish before controller and dlopen admission")
    elapsed_position = fini.find("peak_main_time = peak_second() - peak_runtime_start_time")
    controller_stop_position = fini.find("peak_general_listener_controller_stop()")
    require(elapsed_position != -1 and controller_stop_position != -1 and
            controller_stop_position < elapsed_position,
            "application elapsed time must freeze after controller drain")


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


def check_final_report_snapshot_order(repo_root):
    peak_source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    general = read_source(repo_root, "src/general_listener.c")
    fini = extract_function(peak_source, "peak_fini_impl")
    local_report = extract_function(
        general, "peak_general_listener_local_report_overhead"
    )
    freeze_report = extract_function(
        general, "peak_general_listener_freeze_final_report_snapshot"
    )
    print_result = extract_function(
        general, "peak_general_listener_print_result"
    )
    print_text = extract_function(
        general, "peak_general_listener_print_text_result"
    )
    print_mpi_maxima = extract_function(
        general, "peak_general_listener_print_mpi_maximum_reports"
    )
    print_entry = extract_function(general, "peak_general_listener_print")
    local_ranks = extract_function(
        general, "peak_general_listener_local_mpi_ranks"
    )
    parse_positive_uint = extract_function(
        general, "peak_general_listener_parse_positive_uint_text"
    )
    control_risk = extract_function(
        general, "peak_general_listener_control_risk_seconds"
    )

    elapsed_position = fini.find(
        "peak_main_time = peak_second() - peak_runtime_start_time"
    )
    controller_stop_position = fini.find(
        "peak_general_listener_controller_stop()"
    )
    freeze_position = fini.find(
        "peak_general_listener_freeze_final_report_snapshot()"
    )
    require(elapsed_position != -1 and controller_stop_position != -1 and
            freeze_position != -1 and
            controller_stop_position < elapsed_position < freeze_position,
            "final report snapshot must freeze after controller drain and elapsed time")
    heartbeat_free_position = fini.find("g_free(heartbeat_overhead)")
    require(heartbeat_free_position != -1 and
            controller_stop_position < heartbeat_free_position,
            "heartbeat sample storage must outlive the active detach controller")

    accounting_snapshot_position = freeze_report.find(
        "peak_general_listener_runtime_accounting_snapshot(&accounting)"
    )
    elapsed_snapshot_position = freeze_report.find(
        "snapshot.elapsed_seconds = peak_main_time"
    )
    assignment_position = freeze_report.find(
        "peak_general_listener_final_report_snapshot = snapshot"
    )
    require(accounting_snapshot_position != -1 and
            elapsed_snapshot_position != -1 and
            assignment_position != -1 and
            accounting_snapshot_position < elapsed_snapshot_position <
            assignment_position,
            "final report snapshot must capture accounting and frozen elapsed before publishing")
    require("snapshot.profile_seconds =" in freeze_report and
            "snapshot.control_seconds =" in freeze_report and
            "snapshot.profile_ratio =" in freeze_report and
            "snapshot.control_ratio =" in freeze_report and
            "snapshot.profile_control_risk_ratio =" in freeze_report and
            "snapshot.control_risk_ratio =" in freeze_report and
            "peak_general_listener_control_risk_seconds(snapshot.control_seconds)" in freeze_report and
            "snapshot.local_ranks = peak_general_listener_local_mpi_ranks()" in freeze_report and
            "snapshot.control_risk_seconds = control_risk_seconds" in freeze_report and
            "snapshot.profile_control_risk_seconds = profile_control_risk_seconds" in freeze_report and
            "snapshot.profile_seconds / snapshot.elapsed_seconds" in freeze_report and
            "snapshot.control_seconds / snapshot.elapsed_seconds" in freeze_report and
            "(snapshot.profile_seconds + snapshot.control_seconds) /\n            snapshot.elapsed_seconds" in freeze_report,
            "final report snapshot must publish explicit profile/control fields from frozen elapsed")
    require("peak_detach_controller_accounting_snapshot" not in freeze_report,
            "final report snapshot must use the general listener accounting boundary")
    require("peak_general_listener_final_report_snapshot.valid" in local_report and
            "peak_general_listener_final_report_snapshot.profile_seconds" in local_report and
            "peak_general_listener_final_report_snapshot.control_seconds" in local_report and
            "peak_general_listener_final_report_snapshot.profile_ratio" in local_report and
            "peak_general_listener_final_report_snapshot.control_ratio" in local_report and
            "peak_general_listener_final_report_snapshot.profile_control_risk_ratio" in local_report and
            "peak_general_listener_final_report_snapshot.control_risk_ratio" in local_report,
            "final local report must consume separate frozen raw and risk fields")

    require("parsed == 0" in parse_positive_uint and
            "parsed > G_MAXUINT" in parse_positive_uint and
            "MPI_LOCALNRANKS" in local_ranks and
            "OMPI_COMM_WORLD_LOCAL_SIZE" in local_ranks and
            "MV2_COMM_WORLD_LOCAL_SIZE" in local_ranks and
            "PMI_LOCAL_SIZE" in local_ranks and
            "return parsed;" in local_ranks and
            "return 1U;" in local_ranks,
            "local MPI rank discovery must reject invalid values and fall back to one")
    require("peak_general_listener_multiply_nonnegative_finite" in control_risk and
            "peak_general_listener_local_mpi_ranks()" in control_risk and
            "return DBL_MAX;" in control_risk,
            "control risk must be local ranks times raw control and fail closed")

    require("const PeakReportOverhead* report_overhead" in print_result and
            "report_overhead);" in print_result,
            "final reporting wrapper must forward the report snapshot")
    require("const PeakReportOverhead* report_overhead" in print_text and
            "report_overhead != NULL && report_overhead->valid" in print_text and
            "report_overhead->profile_seconds" in print_text and
            "report_overhead->control_seconds" in print_text and
            "report_overhead->profile_ratio" in print_text and
            "report_overhead->control_ratio" in print_text and
            "report_overhead->profile_control_risk_ratio" in print_text and
            "report_overhead->control_risk_ratio" in print_text and
            "report_overhead->management_ratio" in print_text,
            "text output must consume explicit raw and risk report fields")
    require("profile_ratio=%.9f control_ratio=%.9f ratio=%.9f" in print_text,
            "local text output must include explicit profile/control ratio fields")
    require("[peak] local profile+local-rank-control risk: profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u risk_control_seconds=%.9f ratio=%.9f" in print_text and
            "[peak] per-rank maximum profile+control risk overhead: owner_rank=%d profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f" in print_mpi_maxima and
            "[peak] per-rank maximum control risk overhead: owner_rank=%d raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f" in print_mpi_maxima and
            "[peak] per-rank maximum profile+control overhead: owner_rank=%d profile_seconds=%.9f control_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f" in print_mpi_maxima and
            "peak_general_listener_print_mpi_maximum_reports" in print_text,
            "text output must keep strict, separate, and owner-consistent raw/risk ratio contracts")
    require("[peak] %s final transition coverage: detached_targets=%zu reattached_targets=%zu revisited_targets=%zu" in print_text and
            "rank_count > 1 ? \"aggregate\" : \"local\"" in print_text,
            "final output must expose exact aggregate ever-revisited coverage")
    require("[peak] per-rank elapsed range: min_seconds=%.9f max_seconds=%.9f" in print_text and
            "report_overhead->elapsed_min_seconds" in print_text and
            "report_overhead->elapsed_max_seconds" in print_text,
            "final output must expose the exact per-rank elapsed range contract")
    require("PeakReportOverhead local_report =" in print_entry and
            "peak_general_listener_local_report_overhead(sum_num_calls)" in print_entry and
            "&local_report" in print_entry,
            "local final output must consume the frozen report snapshot")

    if "peak_general_listener_reduce_result" in general:
        reduce_result = extract_function(
            general, "peak_general_listener_reduce_result"
        )
        tuple_reduce = extract_function(
            general, "peak_mpi_reduce_report_rank_tuples"
        )
        require("peak_general_listener_report_rank_tuple(&local_report)" in reduce_result and
                "peak_mpi_reduce_report_rank_tuples" in reduce_result and
                "mpi_report.per_rank_max_owner_ranks[i] = max_ratio_owner_ranks[i]" in reduce_result and
                "local_tuple->profile_ratio" in tuple_reduce and
                "local_tuple->control_ratio" in tuple_reduce and
                "local_tuple->profile_control_risk_ratio" in tuple_reduce and
                "local_tuple->control_risk_ratio" in tuple_reduce and
                "MPI_DOUBLE_INT" in tuple_reduce and
                "MPI_MAXLOC" in tuple_reduce and
                "profile-control-ratio-maxloc" in tuple_reduce and
                "profile-control-ratio-owner" in tuple_reduce and
                "peak_mpi_bcast_report_rank_tuple" in tuple_reduce,
                "MPI final reporting must retain the owner and local tuple for each maximum ratio")
        tuple_bcast = extract_function(
            general, "peak_mpi_bcast_report_rank_tuple"
        )
        require("MPI_BYTE" not in tuple_bcast and
                "MPI_INT" in tuple_bcast and
                "MPI_UNSIGNED" in tuple_bcast and
                "PEAK_MPI_UINT64_DATATYPE" in tuple_bcast and
                "MPI_DOUBLE" in tuple_bcast,
                "MPI report tuples must use field-wise typed broadcasts")
        require("_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64" in general and
                "MPI_UINT64_T" in general and
                "UINT64_MAX == ULONG_MAX" in general and
                "UINT64_MAX == ULLONG_MAX" in general,
                "MPI reporting must select an exact uint64 datatype with a compile-time fallback")
        require("local_elapsed_valid" in reduce_result and
                "\"elapsed-valid\"" in reduce_result and
                "MPI_MIN" in reduce_result and
                "\"elapsed-min\"" in reduce_result and
                "MPI_MAX" in reduce_result and
                "\"elapsed-max\"" in reduce_result and
                "mpi_report.elapsed_min_seconds = mpi_min_elapsed_seconds" in reduce_result and
                "mpi_report.elapsed_max_seconds = mpi_max_elapsed_seconds" in reduce_result,
                "MPI final reporting must validate and reduce exact elapsed endpoints")
        require("\"accounting-valid\"" in reduce_result and
                "MPI_MIN" in reduce_result and
                "\"failed-stop-window-max\"" in reduce_result and
                "(UINT64_MAX - 1) / (uint64_t)size" in reduce_result and
                "\"failed-stop-window-count\"" in reduce_result and
                "MPI_SUM" in reduce_result and
                "mpi_report.accounting_valid = all_accounting_valid != 0" in reduce_result and
                "mpi_report.failed_stop_window_count = mpi_failed_stop_window_count" in reduce_result,
                "MPI final reporting must carry all-rank accounting validity and failed-window evidence")
    if "peak_general_listener_socket_reduce_result_with_rank_source" in general:
        socket_result = extract_function(
            general, "peak_general_listener_socket_reduce_result_with_rank_source"
        )
        require("#define PEAK_SOCKET_REDUCE_VERSION 8U" in general and
                "header.profile_ratio = local_report.profile_ratio" in socket_result and
                "header.control_ratio = local_report.control_ratio" in socket_result and
                "header.profile_control_risk_ratio =\n        local_report.profile_control_risk_ratio" in socket_result and
                "header.control_risk_ratio = local_report.control_risk_ratio" in socket_result and
                "header.failed_stop_window_count = local_report.failed_stop_window_count" in socket_result and
                "header.accounting_valid = local_report.accounting_valid ? 1U : 0U" in socket_result and
                "socket_report.profile_ratio = socket_max_profile_ratio" in socket_result and
                "socket_report.control_ratio = socket_max_control_ratio" in socket_result and
                "socket_report.profile_control_risk_ratio =\n        socket_max_profile_control_risk_ratio" in socket_result and
                "socket_report.control_risk_ratio = socket_max_control_risk_ratio" in socket_result and
                "socket_report.accounting_valid = socket_accounting_valid" in socket_result and
                "peak_general_listener_add_uint64_saturated" in socket_result,
                "socket reducer version and aggregation must carry raw, risk, and accounting health")


def check_stop_window_accounting_sidecar(repo_root):
    source = (repo_root / "src/detach_controller.c").read_text(
        encoding="utf-8"
    )
    general = read_source(repo_root, "src/general_listener.c")
    started = extract_function(
        source, "peak_detach_controller_note_stop_window_started"
    )
    finished = extract_function(
        source, "peak_detach_controller_note_stop_window_finished"
    )
    failed = extract_function(
        source, "peak_detach_controller_note_stop_window_failed"
    )
    publish = extract_function(
        source, "peak_detach_controller_publish_stop_window_accounting"
    )
    accounting_begin = extract_function(
        source, "peak_detach_controller_accounting_begin_publication"
    )
    accounting_end = extract_function(
        source, "peak_detach_controller_accounting_end_publication"
    )
    accounting_add = extract_function(
        source, "peak_detach_controller_accounting_add_saturated"
    )
    last_window = extract_function(
        source, "peak_detach_controller_last_stop_window_us"
    )
    snapshot = extract_function(
        source, "peak_detach_controller_accounting_snapshot"
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
    general_trace = extract_function(
        general, "peak_general_controller_trace_mutation"
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
    heartbeat = extract_function(general, "peak_heartbeat_monitor")
    note_runtime_start = extract_function(
        general, "peak_general_listener_note_runtime_start"
    )
    print_text = extract_function(
        general, "peak_general_listener_print_text_result"
    )
    clear_pending_context = extract_function(
        general,
        "peak_general_controller_clear_pending_request_context_unlocked",
    )
    set_pending_context = extract_function(
        general,
        "peak_general_controller_set_pending_request_context_unlocked",
    )
    expand_dynamic_hooks = extract_function(
        general,
        "peak_general_listener_expand_dynamic_hook_tables_unlocked",
    )
    nonnegative_finite = extract_function(
        general,
        "peak_general_listener_nonnegative_finite",
    )
    checked_add = extract_function(
        general,
        "peak_general_listener_add_nonnegative_finite",
    )
    checked_subtract = extract_function(
        general,
        "peak_general_listener_subtract_nonnegative_finite",
    )
    checked_multiply = extract_function(
        general,
        "peak_general_listener_multiply_nonnegative_finite",
    )
    checked_positive_multiply = extract_function(
        general,
        "peak_general_listener_multiply_positive_finite",
    )
    profile_seconds_floor = extract_function(
        general,
        "peak_general_listener_profile_seconds_floor",
    )
    note_reattach_success = extract_function(
        general,
        "peak_general_listener_note_reattach_success_unlocked",
    )
    refresh_revisited = extract_function(
        general,
        "peak_general_listener_refresh_revisited_markers",
    )
    reduce_result = extract_function(
        general,
        "peak_general_listener_reduce_result",
    )
    scalar_reattach = extract_function(
        general,
        "peak_general_controller_reattach_if_requested_unlocked",
    )
    batch_mutation = extract_function(
        general,
        "peak_general_controller_process_pending_batch_unlocked",
    )
    handle_prepare_failure = extract_function(
        general,
        "peak_general_controller_handle_prepare_failure_unlocked",
    )

    require("getenv(" not in controller_trace_configure and
            "g_getenv(" not in controller_trace_configure,
            "detach controller trace configuration must not read environment")
    require("atomic_store_explicit(&trace_diagnostics_enabled" in source,
            "detach controller trace configuration must cache explicit state")
    require("peak_detach_controller_trace_diagnostics_enabled" not in source,
            "stop-window accounting must not be gated by trace diagnostics")
    require("held_mutation_started_at = peak_detach_controller_monotonic_second()" in started and
            "last_stop_window_us = 0.0" not in started,
            "control-window start must record monotonic time without discarding the last successful predictor")
    require("peak_detach_accounting_completed_stop_window_count" in source and
            "peak_detach_accounting_failed_stop_window_count" in source and
            "peak_detach_accounting_stop_window_wall_ns" in source and
            "peak_detach_accounting_sequence" in source and
            "peak_detach_controller_publish_stop_window_accounting(elapsed_ns" in finished and
            "peak_detach_controller_publish_stop_window_accounting(elapsed_ns" in failed and
            "peak_detach_controller_accounting_begin_publication" in publish and
            "peak_detach_controller_accounting_add_saturated" in publish and
            "peak_detach_controller_accounting_end_publication" in publish and
            "last_stop_window_us = (double)elapsed_ns / 1000.0" in finished,
            "completed and failed control windows must update total accounting without changing the failed predictor")
    snapshot_bound = snapshot.find(
        "attempt < PEAK_DETACH_ACCOUNTING_SNAPSHOT_MAX_ATTEMPTS"
    )
    snapshot_first_sequence = snapshot.find(
        "sequence_before =\n            atomic_load_explicit"
    )
    snapshot_odd_check = snapshot.find("(sequence_before & 1U) != 0")
    snapshot_completed = snapshot.find("snapshot.completed_stop_window_count =")
    snapshot_failed = snapshot.find("snapshot.failed_stop_window_count =")
    snapshot_wall = snapshot.find(
        "snapshot.stop_window_wall_ns ="
    )
    snapshot_final_sequence = snapshot.find(
        "sequence_after =\n            atomic_load_explicit"
    )
    snapshot_validation = snapshot.find("sequence_before == sequence_after")
    snapshot_success = snapshot.find("return TRUE;", snapshot_validation)
    snapshot_failure = snapshot.find("return FALSE;", snapshot_success)
    require("peak_detach_controller_lock_mutation_guard" not in snapshot and
            snapshot_bound != -1 and
            snapshot_first_sequence != -1 and
            snapshot_odd_check != -1 and
            snapshot_completed != -1 and
            snapshot_failed != -1 and
            snapshot_wall != -1 and
            snapshot_final_sequence != -1 and
            snapshot_validation != -1 and
            snapshot_success != -1 and
            snapshot_failure != -1 and
            snapshot_bound < snapshot_first_sequence < snapshot_odd_check <
            snapshot_completed < snapshot_failed < snapshot_wall <
            snapshot_final_sequence < snapshot_validation < snapshot_success <
            snapshot_failure and
            "memory_order_seq_cst" in accounting_begin and
            "memory_order_seq_cst" in accounting_end and
            accounting_add.count("memory_order_seq_cst") == 3 and
            snapshot.count("memory_order_seq_cst") == 5 and
            "memory_order_relaxed" not in snapshot and
            "atomic_thread_fence" not in snapshot and
            "UINT64_MAX" not in snapshot,
            "accounting snapshot must be bounded, seq_cst coherent, and return explicit validity")
    require("return 0.0;" not in last_window,
            "last_stop_window_us must not be trace-gated")
    require("PEAK_DETACH_TRACE_PATH" in general_trace_init,
            "general listener must snapshot PEAK_DETACH_TRACE_PATH during init")
    require("peak_detach_controller_configure_trace_diagnostics" in general_trace_init,
            "general listener must configure detach-controller trace diagnostics")
    require("PeakDetachAccountingSnapshot detach_accounting" in general_trace_detail and
            "accounting_stop_window_count" not in general_trace_detail and
            "trace_elapsed_time = peak_general_listener_elapsed_at(trace_now)" in general_trace_detail and
            "peak_general_listener_control_wall_ns_since_heartbeat" in general_trace_detail and
            "accounting_ratio = accounting_wall_s / trace_elapsed_time" in general_trace_detail and
            "peak_general_listener_control_window_count_since_heartbeat" in general_trace_detail,
            "trace rows must append accounting fields after existing request fields with current elapsed denominator")
    require("strcmp(result, \"prepare-failed\") == 0" in general_trace and
            "? 0.0" in general_trace and
            "\n            0.0,\n            0," in handle_prepare_failure and
            "stop_window_us = peak_detach_controller_last_stop_window_us();" not in
                batch_mutation[:batch_mutation.find("if (prepared_count > 0)")] and
            "stop_window_us = peak_detach_controller_last_stop_window_us();" in
                batch_mutation[batch_mutation.find("if (prepared_count > 0)"):],
            "prepare-failed traces must use zero unless they share a completed partial-batch window")
    require("PeakDetachAccountingSnapshot detach_accounting" in print_text and
            "control stop-window overhead:" in print_text and
            "stop_window_seconds / peak_main_time" in print_text,
            "text output must report measured stop-window overhead")
    require("double profile_spent_seconds = 0.0" in heartbeat and
            "double control_spent_seconds = 0.0" in heartbeat and
            "double spent_ratio = 0.0" in heartbeat and
            "double attached_recent_sum = 0.0" in heartbeat and
            "double attached_lifetime_sum = 0.0" in heartbeat and
            "double attached_pressure = 0.0" in heartbeat,
            "heartbeat must split profile spend, control spend, and both attached pressure signals")
    require("profile_spent_seconds += hook_profile_spent_seconds" in heartbeat and
            "control_pause_wall_ns =\n            peak_general_listener_control_wall_ns_since_heartbeat" in heartbeat and
            "control_spent_seconds = (double)control_pause_wall_ns / 1e9" in heartbeat and
            "spent_ratio =\n            (profile_spent_seconds + control_spent_seconds) /\n            total_execution_time" in heartbeat,
            "heartbeat spent ratio must add measured workload stop-window seconds")
    reattach_before = heartbeat.find("reattach_accounting_before_valid =")
    reattach_predictor = heartbeat.find("peak_detach_controller_last_stop_window_us()")
    reattach_after = heartbeat.find("reattach_accounting_after_valid =")
    reattach_coherent = heartbeat.find("reattach_accounting_coherent =")
    reattach_budget = heartbeat.find("reattach_accounting_coherent &&")
    reattach_coherence_contract = heartbeat[
        reattach_coherent:reattach_budget
    ]
    unsaturated_accounting_fields = [
        f"reattach_accounting_{side}.{field} <\n"
        "                    (UINT64_MAX - 1)"
        for side in ("before", "after")
        for field in (
            "completed_stop_window_count",
            "failed_stop_window_count",
            "stop_window_wall_ns",
        )
    ]
    require(reattach_before != -1 and reattach_predictor != -1 and
            reattach_after != -1 and reattach_coherent != -1 and
            reattach_budget != -1 and
            reattach_before < reattach_predictor < reattach_after <
            reattach_coherent < reattach_budget and
            all(field in reattach_coherence_contract
                for field in unsaturated_accounting_fields) and
            "reattach_accounting_before.completed_stop_window_count ==\n                    reattach_accounting_after.completed_stop_window_count" in heartbeat and
            "reattach_accounting_before.failed_stop_window_count ==\n                    reattach_accounting_after.failed_stop_window_count" in heartbeat and
            "reattach_accounting_before.stop_window_wall_ns ==\n                    reattach_accounting_after.stop_window_wall_ns" in heartbeat,
            "reattach admission must use unsaturated accounting snapshots that bracket and match the predictor read")
    require("peak_general_listener_accounting_snapshot(&accounting_baseline)" in note_runtime_start and
            "&peak_general_listener_heartbeat_control_baseline_ns" in note_runtime_start and
            "accounting_baseline.stop_window_wall_ns" in note_runtime_start and
            "&peak_general_listener_heartbeat_control_baseline_count" in note_runtime_start and
            "accounting_baseline.completed_stop_window_count" in note_runtime_start and
            "&peak_general_listener_heartbeat_control_baseline_failed_count" in note_runtime_start and
            "accounting_baseline.failed_stop_window_count" in note_runtime_start and
            "&peak_general_listener_heartbeat_control_baseline_valid" in note_runtime_start and
            "baseline_valid" in note_runtime_start and
            "&peak_general_listener_heartbeat_control_baseline_ns" not in heartbeat,
            "control accounting baseline must be captured synchronously before heartbeat startup")
    require("_Atomic unsigned long long peak_general_listener_runtime_start_ns" in general and
            "_Atomic unsigned long long\n    peak_general_listener_heartbeat_control_baseline_ns" in general and
            "atomic_load_explicit(&peak_general_listener_runtime_start_ns" in general and
            "peak_general_listener_count_since_baseline" in general and
            "&peak_general_listener_heartbeat_control_baseline_ns" in general and
            "peak_general_listener_runtime_accounting_snapshot" in general and
            "peak_general_listener_heartbeat_control_baseline_valid" in general,
            "heartbeat accounting baseline must be shared with trace through atomics")
    per_target_detach = heartbeat[
        heartbeat.find("// 1) Per-target DETACH"):
        heartbeat.find("// 2) Global DETACH")
    ]
    global_detach_for_budget = heartbeat[
        heartbeat.find("// 2) Global DETACH"):
        heartbeat.find("// 3) Reattach")
    ]
    adaptive_sleep_start = heartbeat.find("// Adaptive heartbeat sleep")
    adaptive_sleep = heartbeat[
        adaptive_sleep_start:heartbeat.find("cleanup:", adaptive_sleep_start)
    ]
    require("ratio_snapshot[i] = ratio" in heartbeat and
            "rate_snapshot[i] = recent_rate" in heartbeat and
            "attached_recent_sum += recent_rate" in heartbeat and
            "attached_lifetime_sum += ratio" in heartbeat,
            "heartbeat must snapshot and aggregate both recent and lifetime contributions")
    require("attached_pressure = MAX(attached_recent_sum, attached_lifetime_sum)" in heartbeat and
            "profile_global_overhead = attached_pressure" in heartbeat and
            "double projected_attached_recent_sum = attached_recent_sum" in heartbeat and
            "double projected_attached_lifetime_sum = attached_lifetime_sum" in heartbeat,
            "global detach pressure must be the max of attached recent and lifetime sums")
    request_context_start = heartbeat.find(
        "profile_global_overhead = attached_pressure;"
    )
    require(request_context_start != -1 and
            "profile_global_overhead =" not in heartbeat[
                request_context_start + len(
                    "profile_global_overhead = attached_pressure;"
                ):
            ] and
            re.search(
                r"PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT,\s*"
                r"calls_snapshot\[i\],\s*lifetime_snapshot\[i\],\s*"
                r"profile_global_overhead,\s*total_execution_time,\s*"
                r"rate_snapshot\[i\]",
                per_target_detach,
                re.DOTALL,
            ) is not None and
            re.search(
                r"PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT,\s*"
                r"calls_snapshot\[idx\],\s*lifetime_snapshot\[idx\],\s*"
                r"profile_global_overhead,\s*total_execution_time,\s*"
                r"rate_snapshot\[idx\]",
                global_detach_for_budget,
                re.DOTALL,
            ) is not None,
            "detach requests must preserve the immutable hybrid global-overhead context and both hook signals")
    require("#define PEAK_GLOBAL_DETACH_MIN_CALLS 2U" in general and
            "calls_snapshot[i] >= PEAK_GLOBAL_DETACH_MIN_CALLS" in per_target_detach and
            "if (calls_snapshot[i] < PEAK_GLOBAL_DETACH_MIN_CALLS)" in global_detach_for_budget,
            "per-target and global detach must preserve the robust-reference two-call guard")
    require("ratio_snapshot[i] > target_profile_ratio" in per_target_detach and
            "rate_snapshot[i] > target_profile_ratio" not in per_target_detach and
            "gboolean accepted =" in per_target_detach and
            "projected_attached_recent_sum -= rate_snapshot[i]" in per_target_detach and
            "projected_attached_lifetime_sum -= ratio_snapshot[i]" in per_target_detach and
            "control_spent_seconds" not in per_target_detach and
            "risk" not in per_target_detach and
            "local_mpi_ranks" not in per_target_detach and
            "spent_ratio" not in per_target_detach,
            "per-target detach must remain cumulative-only and independent of reattach risk")
    require("double projected_global_overhead =\n                MAX(projected_attached_recent_sum,\n                    projected_attached_lifetime_sum)" in global_detach_for_budget and
            "if (projected_global_overhead >\n                global_target_ratio * peak_global_detach_factor)" in global_detach_for_budget and
            "double contribution =\n                        MAX(rate_snapshot[i], ratio_snapshot[i])" in global_detach_for_budget and
            "if (contribution <= 0.0) continue;" in global_detach_for_budget and
            "entries[n_attached].ratio = contribution" in global_detach_for_budget and
            "entries[n_attached].rate = rate_snapshot[i]" in global_detach_for_budget and
            "entries[n_attached].lifetime = ratio_snapshot[i]" in global_detach_for_budget and
            "double reduced_recent_sum = projected_attached_recent_sum" in global_detach_for_budget and
            "double reduced_lifetime_sum = projected_attached_lifetime_sum" in global_detach_for_budget and
            "double reduced_global_overhead = projected_global_overhead" in global_detach_for_budget and
            "if (reduced_global_overhead <= global_target_ratio)" in global_detach_for_budget and
            "reduced_recent_sum -= entries[k].rate" in global_detach_for_budget and
            "reduced_lifetime_sum -= entries[k].lifetime" in global_detach_for_budget and
            "reduced_global_overhead =\n                            MAX(reduced_recent_sum, reduced_lifetime_sum)" in global_detach_for_budget and
            "projected_attached_recent_sum = reduced_recent_sum" in global_detach_for_budget and
            "projected_attached_lifetime_sum = reduced_lifetime_sum" in global_detach_for_budget and
            "spent_ratio" not in global_detach_for_budget and
            "observed_global_overhead" not in global_detach_for_budget and
            "control_spent_seconds" not in global_detach_for_budget and
            "risk" not in global_detach_for_budget and
            "local_mpi_ranks" not in global_detach_for_budget,
            "global detach must carry both profile signals and exclude raw and risk control spend")
    require("profile_global_overhead - prev_global_overhead" in adaptive_sleep and
            "profile_global_overhead / global_target_ratio" in adaptive_sleep and
            "recent_rate" not in adaptive_sleep and
            "rate_snapshot" not in adaptive_sleep and
            "risk" not in adaptive_sleep and
            "local_mpi_ranks" not in adaptive_sleep,
            "adaptive heartbeat sleep must use only the immutable hybrid global overhead")
    reattach_section = heartbeat[heartbeat.find("// 3) Reattach"):]
    require("value >= 0.0" in nonnegative_finite and
            "value == value" in nonnegative_finite and
            "value <= DBL_MAX" in nonnegative_finite and
            "lhs > DBL_MAX - rhs" in checked_add and
            "amount > *value" in checked_subtract and
            "result = *value - amount" in checked_subtract and
            "peak_general_listener_nonnegative_finite(result)" in checked_subtract and
            "rhs > 0.0 && lhs > DBL_MAX / rhs" in checked_multiply and
            "result = lhs * rhs" in checked_multiply and
            "peak_general_listener_nonnegative_finite(result)" in checked_multiply,
            "checked headroom helpers must reject nonfinite, overflow, and negative results")
    require("double result" in checked_positive_multiply and
            "result = lhs * rhs" in checked_positive_multiply and
            "!peak_general_listener_positive_finite(result)" in checked_positive_multiply and
            "*out = result" in checked_positive_multiply,
            "positive multiplication must reject zero underflow after the product")
    require("peak_general_listener_positive_finite(peak_general_overhead)" in profile_seconds_floor and
            "return peak_general_overhead" in profile_seconds_floor and
            "return 1e-12" in profile_seconds_floor,
            "profile-seconds floor must use calibrated cost with 1e-12 fallback")
    require("const unsigned int local_mpi_ranks =\n        peak_general_listener_local_mpi_ranks()" in heartbeat and
            "double reattach_spent_seconds = 0.0" in reattach_section and
            "double reattach_control_spent_seconds =" in reattach_section and
            "double reattach_control_risk_seconds = DBL_MAX" in reattach_section and
            "peak_general_listener_multiply_nonnegative_finite(\n"
            "                    reattach_control_spent_seconds,\n"
            "                    (double)local_mpi_ranks,\n"
            "                    &reattach_control_risk_seconds)" in reattach_section and
            "peak_general_listener_add_nonnegative_finite(\n"
            "                    profile_spent_seconds,\n"
            "                    reattach_control_risk_seconds,\n"
            "                    &reattach_spent_seconds)" in reattach_section and
            "double reattach_risk_spent_ratio = 0.0" in reattach_section and
            "reattach_risk_spent_ratio =\n"
            "                    reattach_spent_seconds / total_execution_time" in reattach_section and
            "peak_general_listener_nonnegative_finite(\n"
            "                        reattach_risk_spent_ratio)" in reattach_section and
            "reattach_risk_spent_ratio <= reattach_gate_ratio" in reattach_section,
            "reattach gate must use checked profile plus local-rank-scaled raw control risk")
    require("double reattach_gate_ratio = 0.0" in reattach_section and
            "peak_general_listener_nonnegative_finite(\n"
            "                    peak_global_reattach_factor)" in reattach_section and
            "peak_general_listener_nonnegative_finite(global_target_ratio)" in reattach_section and
            "peak_general_listener_multiply_nonnegative_finite(\n"
            "                    peak_global_reattach_factor,\n"
            "                    global_target_ratio,\n"
            "                    &reattach_gate_ratio)" in reattach_section,
            "reattach gate must be built with checked factor-times-target arithmetic")
    require("double headroom_seconds = 0.0" in reattach_section and
            "peak_general_listener_multiply_nonnegative_finite(\n"
            "                    global_target_ratio,\n"
            "                    total_execution_time,\n"
            "                    &headroom_seconds)" in reattach_section and
            "peak_general_listener_subtract_nonnegative_finite(\n"
            "                    &headroom_seconds,\n"
            "                    profile_spent_seconds)" in reattach_section and
            "peak_general_listener_subtract_nonnegative_finite(\n"
            "                    &headroom_seconds,\n"
            "                    reattach_control_risk_seconds)" in reattach_section and
            "headroom_seconds > 0.0" in reattach_section,
            "reattach headroom must debit local-rank risk through checked arithmetic")
    require("double predicted_batch_stop_seconds = DBL_MAX" in reattach_section and
            "peak_general_listener_multiply_nonnegative_finite(\n"
            "                    last_stop_seconds,\n"
            "                    (double)local_mpi_ranks,\n"
            "                    &predicted_batch_stop_seconds)" in reattach_section,
            "future stop-window reservations must use the same local-rank risk bound")
    require("reattach_control_risk_seconds" not in scalar_reattach and
            "reattach_control_risk_seconds" not in batch_mutation and
            "profile_control_risk_ratio" not in heartbeat,
            "risk accounting must remain an admission input, not hook or lifecycle state")
    require("PEAK_HOOK_REATTACH_REQUESTED" in reattach_section and
            "PEAK_HOOK_REATTACHING" in reattach_section and
            "pending_lease_seconds == DBL_MAX" in reattach_section and
            "peak_general_listener_add_nonnegative_finite(\n"
            "                                projected_pending_reattach_seconds,\n"
            "                                pending_lease_seconds,\n"
            "                                &projected_pending_reattach_seconds)" in reattach_section and
            "projected_pending_reattach_seconds != DBL_MAX" in reattach_section and
            "peak_general_listener_subtract_nonnegative_finite(\n"
            "                        &headroom_seconds,\n"
            "                        projected_pending_reattach_seconds)" in reattach_section,
            "pending reattach leases must use checked shared headroom accounting")
    require("pending_batch_windows" in reattach_section and
            "peak_general_listener_multiply_nonnegative_finite(\n"
            "                        (double)pending_batch_windows,\n"
            "                        predicted_batch_stop_seconds,\n"
            "                        &pending_stop_seconds)" in reattach_section and
            "peak_general_listener_subtract_nonnegative_finite(\n"
            "                        &headroom_seconds,\n"
            "                        pending_stop_seconds)" in reattach_section and
            "(double)(after_windows - before_windows)" in reattach_section and
            "incremental_lease_seconds != DBL_MAX" in reattach_section and
            "candidate_seconds == DBL_MAX" in reattach_section and
            "&remaining_headroom_seconds,\n"
            "                            candidate_seconds" in reattach_section,
            "batch stop reservations and candidate leases must reject overflow via DBL_MAX")
    rate_write_pattern = re.compile(
        r"peak_hook_pending_request_rate\s*\[[^]]+\]\s*="
    )
    require(rate_write_pattern.search(clear_pending_context) is not None and
            rate_write_pattern.search(set_pending_context) is not None and
            rate_write_pattern.search(expand_dynamic_hooks) is not None,
            "pending request rate must be written by context clear/set and slot initialization")
    rate_write_remainder = general
    for allowed_body in (clear_pending_context,
                         set_pending_context,
                         expand_dynamic_hooks):
        rate_write_remainder = rate_write_remainder.replace(allowed_body, "", 1)
    require(rate_write_pattern.search(rate_write_remainder) is None,
            "pending_request_rate must not be rewritten after reset/context clear")
    require("peak_hook_reattach_request_calls_valid[hook_id]" in note_reattach_success and
            "peak_hook_pending_request_calls[hook_id] >\n"
            "            peak_hook_reattach_request_calls[hook_id]" in note_reattach_success and
            "array_listener_revisited[hook_id] = TRUE" in note_reattach_success and
            "peak_hook_reattach_request_calls[hook_id] =\n"
            "        peak_hook_pending_request_calls[hook_id]" in note_reattach_success and
            "peak_hook_reattach_request_calls_valid[hook_id] = TRUE" in note_reattach_success and
            "array_listener_revisited[hook_id] = FALSE" not in note_reattach_success,
            "reattach success must preserve an OR latch with a valid call baseline")
    revisit_compare = note_reattach_success.find(
        "peak_hook_pending_request_calls[hook_id] >"
    )
    revisit_latch = note_reattach_success.find(
        "array_listener_revisited[hook_id] = TRUE",
        revisit_compare,
    )
    baseline_overwrite = note_reattach_success.find(
        "peak_hook_reattach_request_calls[hook_id] =",
        revisit_latch,
    )
    baseline_valid = note_reattach_success.find(
        "peak_hook_reattach_request_calls_valid[hook_id] = TRUE",
        baseline_overwrite,
    )
    require(revisit_compare != -1 and revisit_latch != -1 and
            baseline_overwrite != -1 and baseline_valid != -1 and
            revisit_compare < revisit_latch < baseline_overwrite < baseline_valid,
            "revisit growth must be compared and latched before baseline overwrite")
    require("peak_hook_reattach_request_calls_valid =\n"
            "        g_new0(gboolean, peak_hook_address_count)" in general and
            "peak_hook_reattach_request_calls_valid[old_count] = FALSE" in expand_dynamic_hooks,
            "revisit call baselines must start invalid for static and dynamic hooks")
    require("if (!array_listener_revisited[i]" in refresh_revisited and
            "peak_hook_reattach_request_calls_valid[i]" in refresh_revisited and
            "local_final_calls[i] > peak_hook_reattach_request_calls[i]" in refresh_revisited and
            "array_listener_revisited[i] = TRUE" in refresh_revisited and
            "array_listener_revisited[i] = FALSE" not in refresh_revisited,
            "final revisit refresh must only OR in growth from a valid baseline")
    require("array_listener_revisited, mpi_array_listener_revisited" in reduce_result and
            "MPI_INT, MPI_MAX, 0, \"revisited-marker\"" in reduce_result and
            "array_listener_revisited = mpi_array_listener_revisited" in reduce_result,
            "MPI revisit aggregation must use logical OR via integer maximum")
    scalar_mark = scalar_reattach.find(
        "array_listener_reattached[hook_id] = TRUE"
    )
    scalar_note = scalar_reattach.find(
        "peak_general_listener_note_reattach_success_unlocked(hook_id)",
        scalar_mark,
    )
    scalar_reset = scalar_reattach.find(
        "peak_general_controller_reset_retry_unlocked(hook_id)",
        scalar_note,
    )
    batch_mark = batch_mutation.find(
        "array_listener_reattached[candidates[i].hook_id] = TRUE"
    )
    batch_note = batch_mutation.find(
        "peak_general_listener_note_reattach_success_unlocked(\n"
        "                candidates[i].hook_id)",
        batch_mark,
    )
    batch_reset = batch_mutation.find(
        "peak_general_controller_reset_retry_unlocked(candidates[i].hook_id)",
        batch_note,
    )
    require(scalar_mark != -1 and scalar_note != -1 and scalar_reset != -1 and
            scalar_mark < scalar_note < scalar_reset,
            "scalar reattach success must latch revisit state before context reset")
    require(batch_mark != -1 and batch_note != -1 and batch_reset != -1 and
            batch_mark < batch_note < batch_reset,
            "batch reattach success must latch revisit state before context reset")
    require(general.count(
                "peak_general_listener_note_reattach_success_unlocked("
            ) == 3,
            "revisit baseline hook must have exactly scalar and batch success callsites")
    require("peak_hook_reattach_projected_overhead_seconds" not in general and
            "peak_hook_pending_reattach_reserved_ratio" not in general and
            "peak_general_listener_estimated_transition_ratio" not in general and
            "reattach_transition_charge" not in reattach_section,
            "heartbeat fix must not add projection, reservation, or transition admission sidecars")
    require("global_reattach_queued_this_cycle" not in reattach_section,
            "global reattach must remain budget-driven rather than fixed one-per-cycle")
    require("entries[detached_cnt].rate =\n"
            "                        peak_hook_last_detach_time" in reattach_section,
            "global reattach fairness must remain explicit when enabled")
    require("control_management_ratio" not in heartbeat and
            "reattach_budget_token" not in source + general and
            "reattach_probe" not in source + general and
            "admission_cap" not in source + general,
            "heartbeat fix must not add management guards, token buckets, probes, or admission caps")
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
        dlopen, "dlopen_interceptor_initialize_attach_candidate"
    )
    require("peak_unsafe_gum_prologue_check" not in support_attach_supported and
            "peak_unsafe_gum_support_prologue_check" not in support_attach_supported and
            "peak_gum_prologue_too_short_for_attach" not in support_attach_supported,
            "support replacements must not apply user-target prologue guards")
    require("peak_general_listener_support_attach_target_is_supported" in syscall,
            "close support replacement must call the support attach predicate")
    require("peak_general_listener_attach_target_is_supported" in dlopen_attach,
            "dlopen listener must use normal target prologue policy so dynamic attach is not disabled by support-only early-return guards")
    require("peak_general_listener_support_attach_target_is_supported" not in dlopen_attach,
            "dlopen listener must not use support-only prologue policy")
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


def check_detach_profile_accounting_order(repo_root):
    source = read_source(repo_root, "src/general_listener.c")
    scalar = extract_function(
        source, "peak_general_controller_detach_if_requested_unlocked"
    )
    batch = extract_function(
        source, "peak_general_controller_process_pending_batch_unlocked"
    )
    reset = extract_function(
        source, "peak_general_controller_reset_retry_unlocked"
    )

    require("peak_general_controller_clear_pending_request_context_unlocked" in reset,
            "reset_retry must clear pending request context")

    scalar_gum_detach = scalar.find("gum_interceptor_detach")
    scalar_finish = scalar.find(
        "peak_general_controller_finish_hook_mutation",
        scalar_gum_detach,
    )
    scalar_note = scalar.find(
        "peak_general_listener_note_detach_profile_seconds_unlocked",
        scalar_finish,
    )
    scalar_reset = scalar.find(
        "peak_general_controller_reset_retry_unlocked",
        scalar_note,
    )
    require(scalar_gum_detach != -1 and scalar_finish != -1 and
            scalar_note != -1 and scalar_reset != -1,
            "scalar detach success must finish, save profile seconds, then reset retry/context")
    require(scalar_finish < scalar_note < scalar_reset,
            "scalar detach profile accounting must run after finish and before reset/context clear")

    batch_finish = batch.find("peak_detach_controller_finish_hook_mutation_batch")
    batch_detach_arm = batch.find("PEAK_DETACH_OPERATION_DETACH", batch_finish)
    batch_note = batch.find(
        "peak_general_listener_note_detach_profile_seconds_unlocked",
        batch_detach_arm,
    )
    batch_reset = batch.find(
        "peak_general_controller_reset_retry_unlocked",
        batch_note,
    )
    require(batch_finish != -1 and batch_detach_arm != -1 and
            batch_note != -1 and batch_reset != -1,
            "batch detach success must finish, save profile seconds, then reset retry/context")
    require(batch_finish < batch_note < batch_reset,
            "batch detach profile accounting must run after finish and before reset/context clear")


def check_global_detach_overhead_selection(repo_root):
    source = read_source(repo_root, "src/general_listener.c")
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


def check_heartbeat_state_machine_boundary(repo_root):
    header = (repo_root / "include/general_listener.h").read_text(
        encoding="utf-8"
    )
    general = read_source(repo_root, "src/general_listener.c")
    controller = (repo_root / "src/detach_controller.c").read_text(
        encoding="utf-8"
    )
    enum_match = re.search(
        r"typedef\s+enum\s*\{(?P<body>.*?)\}\s*PeakHookState\s*;",
        header,
        re.S,
    )
    require(enum_match is not None, "missing PeakHookState enum")
    states = [
        re.sub(r"\s*=.*", "", item).strip()
        for item in enum_match.group("body").split(",")
        if item.strip()
    ]
    require(states == [
        "PEAK_HOOK_UNRESOLVED",
        "PEAK_HOOK_ATTACHED",
        "PEAK_HOOK_DETACH_REQUESTED",
        "PEAK_HOOK_DETACHING",
        "PEAK_HOOK_DETACHED",
        "PEAK_HOOK_REATTACH_REQUESTED",
        "PEAK_HOOK_REATTACHING",
        "PEAK_HOOK_SHUTDOWN",
    ], f"PeakHookState boundary changed: {states}")

    combined = general + "\n" + controller
    forbidden_tokens = (
        "PEAK_HOOK_PROBE",
        "PEAK_HOOK_CLOSEOUT",
        "probe_closeout",
        "reattach_probe",
        "closeout_state",
        "transition_reservation",
        "reattach_reservation",
        "reservation_state",
        "token_bucket",
        "controller_pacing",
        "pacing_budget",
    )
    for token in forbidden_tokens:
        require(token not in combined,
                f"heartbeat policy must not add lifecycle sidecar {token}")

    request_detach = extract_function(
        general, "peak_general_listener_request_detach_with_context_unlocked"
    )
    request_reattach = extract_function(
        general, "peak_general_listener_request_reattach_with_context_unlocked"
    )
    for label, body in (("detach request", request_detach),
                        ("reattach request", request_reattach)):
        require("peak_detach_controller_prepare_hook_mutation" not in body and
                "peak_detach_controller_finish_hook_mutation" not in body and
                "gum_interceptor_" not in body,
                f"{label} path must only mark request state")

    batch = extract_function(
        general, "peak_general_controller_process_pending_batch_unlocked"
    )
    prepare = batch.find("peak_detach_controller_prepare_hook_mutation_batch")
    finish = batch.find("peak_detach_controller_finish_hook_mutation_batch")
    detach_state = batch.find("PEAK_HOOK_DETACHING", prepare)
    reattach_state = batch.find("PEAK_HOOK_REATTACHING", prepare)
    detach_gum = batch.find("gum_interceptor_detach", prepare)
    reattach_gum = batch.find("peak_gum_interceptor_attach_target", prepare)
    require(prepare != -1 and finish != -1 and prepare < finish,
            "batch controller must preserve prepare-before-finish ordering")
    require(prepare < detach_state < finish and prepare < reattach_state < finish,
            "batch controller must keep transient states inside prepare/finish")
    require(prepare < detach_gum < finish and prepare < reattach_gum < finish,
            "Gum mutation must remain inside the prepared stop window")


def check_general_controller_dlopen_drain_order(repo_root):
    source = read_source(repo_root, "src/general_listener.c")
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
    source = read_source(repo_root, "src/general_listener.c")
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
    general = read_source(repo_root, "src/general_listener.c")
    controller = (repo_root / "src/detach_controller.c").read_text(
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
    controller = (repo_root / "src/detach_controller.c").read_text(
        encoding="utf-8"
    )
    signal_policy = (repo_root / "src/signal_policy.c").read_text(
        encoding="utf-8"
    )
    syscall_trampoline = (
        repo_root / "src/exec_raw_syscall_trampoline.S"
    ).read_text(encoding="utf-8")
    signal_public_header = (
        repo_root / "include/signal_policy.h"
    ).read_text(encoding="utf-8")
    signal_internal_header = (
        repo_root / "include/internal/signal_policy_internal.h"
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
    signal_clear_slots = extract_function(
        controller, "peak_detach_controller_signal_clear_slots"
    )
    signal_temp_breakpoint = extract_function(
        controller, "peak_detach_controller_signal_temp_breakpoint_out_of_range"
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
    require("FUTEX_WAIT | FUTEX_PRIVATE_FLAG" in controller and
            "FUTEX_WAKE | FUTEX_PRIVATE_FLAG" in controller and
            "INT_MAX" in controller and
            ".tv_nsec = 1000000L" in controller,
            "Linux signal waits must use a bounded private futex wait and wake-all")
    require("_Static_assert(sizeof(signal_release_epoch) == sizeof(int)" in controller and
            "_Static_assert(sizeof(int) * CHAR_BIT == 32" in controller and
            "_Static_assert(_Alignof(_Atomic int) == _Alignof(int)" in controller and
            "_Static_assert(ATOMIC_INT_LOCK_FREE == 2" in controller and
            "explicit supported Linux compiler ABI" in controller and
            "not a universal C11 representation proof" in controller and
            "peak_detach_controller_signal_release_futex_word()" in controller and
            "(int*)&signal_release_epoch" not in controller,
            "Linux futex waits must enforce the supported compiler ABI contract for an always-lock-free, aligned 32-bit word")
    require("expected_release_epoch" in signal_wait_for_release and
            "peak_detach_controller_signal_wait_release_epoch(expected_release_epoch)" in signal_wait_for_release and
            signal_wait_for_release.find("evacuate_epoch") <
            signal_wait_for_release.find("peak_detach_controller_signal_wait_release_epoch"),
            "signal wait must check evacuation before a futex wait using the current release word")
    require(signal_release.find("atomic_store_explicit(&signal_release_epoch, epoch") <
            signal_release.find("peak_detach_controller_signal_wake_release_waiters"),
            "release publication must wake signal waiters")
    require(signal_clear_slots.find("atomic_store_explicit(&signal_release_epoch, 0") <
            signal_clear_slots.find("peak_detach_controller_signal_wake_release_waiters"),
            "release reset must wake signal waiters")
    require(signal_temp_breakpoint.find("atomic_store_explicit(&slot->evacuate_epoch, epoch") <
            signal_temp_breakpoint.find("peak_detach_controller_signal_wake_release_waiters"),
            "evacuation publication must wake signal waiters")
    require(controller.count("peak_detach_controller_signal_wake_release_waiters()") == 3,
            "signal futex wake-all must occur only after release, reset, and evacuation publications")
    require(signal_wait_for_release.find("rewrite_epoch") <
            signal_wait_for_release.find("done_epoch"),
            "signal PC rewrite must remain ordered before done publication")
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
    require("peak_signal_policy_syscall_dispatch" in signal_policy and
            ".globl syscall" in syscall_trampoline and
            "peak_signal_policy_syscall_dispatch" in syscall_trampoline and
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
        "test_detach_hotloop_signal_wait_stress_strict",
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
    signal_wait_stress_begin = tests.find(
        "add_test(NAME test_detach_hotloop_signal_wait_stress_strict"
    )
    signal_wait_stress_end = tests.find(
        "add_test(NAME test_detach_hotloop_signal_thread_spawn_strict",
        signal_wait_stress_begin,
    )
    signal_wait_stress = tests[signal_wait_stress_begin:signal_wait_stress_end]
    require(signal_wait_stress_begin >= 0 and signal_wait_stress_end > signal_wait_stress_begin and
            "--require-redetach-after-reattach" in tests and
            "--spawn-transient-threads" in tests and
            "--fail-on-transition-skips" not in signal_wait_stress and
            "--max-reattach-classify-failed 0" in signal_wait_stress and
            "trace_has_redetach_after_reattach" in (repo_root / "test/detach_runtime/run_detach_hotloop_trace_check.py").read_text(encoding="utf-8"),
            "signal wait stress must prove transient-thread reattach/redetach while allowing safe churn-time prepare skips")
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
    require("${PROJECT_SOURCE_DIR}/src/signal_policy.c" in controller_tests_cmake and
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
    check_dlopen_detach_transaction(repo_root)
    check_dlopen_resolution_lock_order(repo_root)
    check_dlopen_fftw_scope_and_fork_guard(repo_root)
    check_safe_arm64_plt_reads_and_close_overlap_guard(repo_root)
    check_peak_init_heartbeat_order(repo_root)
    check_mpi_finalize_trampoline_default(repo_root)
    check_final_report_snapshot_order(repo_root)
    check_stop_window_accounting_sidecar(repo_root)
    check_mpi_startup_helper_warmup(repo_root)
    check_detach_profile_accounting_order(repo_root)
    check_global_detach_overhead_selection(repo_root)
    check_heartbeat_state_machine_boundary(repo_root)
    check_general_controller_dlopen_drain_order(repo_root)
    check_exclusive_time_nonnegative(repo_root)
    check_dlopen_test_hook_visibility(repo_root)
    check_shutdown_fail_closed_docs(repo_root)
    check_signal_backend_strict_invariants(repo_root)
    print("detach_lifecycle_invariants_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
