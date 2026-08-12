#!/usr/bin/env python3

import pathlib
import re
import sys


def require(condition, message):
    if not condition:
        print(message, file=sys.stderr)
        raise SystemExit(1)


def read_source(repo_root, rel):
    return (repo_root / rel).read_text(encoding="utf-8")


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


def check_darwin_strict_lifecycle(repo_root):
    controller = read_source(repo_root, "src/detach_controller_darwin.c")
    gum_bridge = read_source(repo_root, "src/gum_peak_darwin_patch_api.c")
    general = read_source(repo_root, "src/general_listener.c")
    peak = read_source(repo_root, "src/peak.c")
    readme = read_source(repo_root, "README.md")
    controller_doc = read_source(
        repo_root, "docs/physical-detach-controller.md"
    )
    workflow = read_source(repo_root, ".github/workflows/cmake.yml")
    concurrent_test = read_source(
        repo_root, "test/macos/detach_concurrent_main.c"
    )
    threaded_bootstrap_test = read_source(
        repo_root, "test/macos/threaded_bootstrap_main.c"
    )
    branch_target = read_source(
        repo_root, "test/macos/smoke_branch_target.S"
    )

    activation = extract_function(peak, "peak_activate_runtime")
    bootstrap_proof = activation.find(
        "peak_general_listener_startup_attach_can_skip_stop"
    )
    gum_init = activation.find("gum_init_embedded()")
    pthread_attach = activation.find("pthread_listener_attach()")
    require(0 <= bootstrap_proof < gum_init < pthread_attach and
            "PEAK_RUNTIME_ACTIVATION_REJECTED" in
            activation[bootstrap_proof:gum_init] and
            "no Gum hooks were installed" in
            activation[bootstrap_proof:gum_init],
            "Darwin activation must prove single-threaded bootstrap before "
            "Gum initialization or pthread hook mutation")

    cancel = extract_function(controller, "peak_darwin_cancel_stop_window")
    resume = cancel.find("if (!peak_darwin_resume_threads")
    abort = cancel.find("peak_detach_controller_abort_after_failed_finish")
    dispose = cancel.find("peak_darwin_dispose_writable_pages")
    require(0 <= resume < abort < dispose,
            "Darwin STOP cancellation must fail-stop before clearing held state when resume fails")

    patch_plan = extract_function(controller, "peak_darwin_build_patch_plan")
    identity_lookup = patch_plan.find(
        "peak_gum_darwin_get_canonical_address_exact"
    )
    identity_check = patch_plan.find(
        "current_function_address != record->function_address"
    )
    saved_address_use = patch_plan.find(
        "plan->function_address = record->function_address"
    )
    require(0 <= identity_lookup < identity_check < saved_address_use and
            "PEAK_DETACH_STATUS_CLASSIFY_FAILED" in
            patch_plan[identity_lookup:saved_address_use],
            "Darwin reattach must verify current canonical patch identity before using saved bytes")
    require(re.search(
                r"peak_gum_darwin_get_canonical_address_exact\s*\(\s*"
                r"request->interceptor,\s*record->function_address,\s*"
                r"request->listener,",
                patch_plan,
            ) is not None,
            "Darwin reattach exact lookup must use the saved Gum canonical "
            "address, not the original PEAK request address")

    exact_lookup = extract_function(
        gum_bridge, "peak_gum_darwin_get_canonical_address_exact"
    )
    exact_context = extract_function(
        gum_bridge, "peak_gum_darwin_find_context_exact"
    )
    require("peak_gum_darwin_find_context_exact" in exact_lookup and
            "peak_gum_darwin_find_context(" not in exact_lookup and
            "g_hash_table_lookup" in exact_context and
            "g_hash_table_iter_init" not in exact_context,
            "Darwin reattach identity lookup must not fall back to listener-only discovery")

    prepare = extract_function(
        controller, "peak_detach_controller_prepare_hook_mutation"
    )
    shutdown_reject = prepare.find(
        "request->operation == PEAK_DETACH_OPERATION_SHUTDOWN"
    )
    nonphysical_reject = prepare.find(
        "request->operation != PEAK_DETACH_OPERATION_DETACH"
    )
    require(0 <= shutdown_reject < nonphysical_reject and
            "PEAK_DETACH_STATUS_UNSUPPORTED" in
            prepare[shutdown_reject:nonphysical_reject] and
            "PEAK_DETACH_STATUS_UNSUPPORTED" in
            prepare[nonphysical_reject:] and
            "return FALSE;" in prepare[nonphysical_reject:] and
            "return TRUE;" not in prepare[nonphysical_reject:],
            "Darwin non-physical Gum mutations must fail closed without a held window")
    finish = extract_function(
        controller, "peak_detach_controller_finish_hook_mutation"
    )
    require("request->operation != PEAK_DETACH_OPERATION_DETACH" in finish and
            "request->operation != PEAK_DETACH_OPERATION_REATTACH" in finish and
            "status = PEAK_DETACH_STATUS_UNSUPPORTED" in finish,
            "Darwin non-physical finish must remain fail-closed without a held window")

    final_detach = extract_function(general, "peak_general_listener_dettach")
    require("Darwin process-exit teardown leaves Gum target listener state alive"
            in final_detach and
            final_detach.find("return FALSE;") <
            final_detach.find("pthread_t controller_tid"),
            "Darwin process exit must retain Gum target listener state")
    finalizer = extract_function(peak, "peak_fini_impl")
    loader_state = finalizer.find("gboolean dlopen_shutdown_flushed")
    darwin_loader_guard = finalizer.find("#if defined(__APPLE__)", loader_state)
    darwin_loader_else = finalizer.find("#else", darwin_loader_guard)
    require(0 <= loader_state < darwin_loader_guard < darwin_loader_else and
            "dlopen_interceptor_" not in
            finalizer[darwin_loader_guard:darwin_loader_else] and
            "no loader listener" in
            finalizer[darwin_loader_guard:darwin_loader_else] and
            "Darwin process-exit teardown leaves Gum support hook state alive"
            in finalizer,
            "Darwin must neither install nor shut down dynamic-loader attach machinery")

    init = extract_function(peak, "peak_init")
    memory_parse = init.find(
        "peak_memory_profile = parse_env_to_bool(PEAK_MEMORY_PROFILE)"
    )
    memory_reject = init.find("rejecting PEAK_MEMORY_PROFILE on macOS")
    requested_work = init.find("gboolean has_requested_work")
    require(0 <= memory_parse < memory_reject < requested_work and
            "peak_memory_profile = false;" in
            init[memory_reject:requested_work] and
            "peak_memory_track_all = false;" in
            init[memory_reject:requested_work],
            "macOS must reject memory profiling before deciding whether any "
            "supported work was requested")

    dynamic_attach = activation.find("dlopen_interceptor_attach()")
    dynamic_attach_guard = activation.rfind(
        "#if !defined(__APPLE__)", 0, dynamic_attach
    )
    dynamic_attach_end = activation.find("#endif", dynamic_attach)
    dynamic_enable = activation.find(
        "dlopen_interceptor_enable_dynamic_attach()"
    )
    dynamic_enable_guard = activation.rfind(
        "#if !defined(__APPLE__)", 0, dynamic_enable
    )
    dynamic_enable_end = activation.find("#endif", dynamic_enable)
    require(0 <= dynamic_attach_guard < dynamic_attach < dynamic_attach_end and
            0 <= dynamic_enable_guard < dynamic_enable < dynamic_enable_end,
            "macOS activation must not install or enable dlopen/dlclose "
            "dynamic-attach machinery")

    require("detach_concurrent_main.c" in workflow and
            "timeout-minutes: 2" in workflow and
            "PEAK_MAX_NUM_THREADS=32" in workflow and
            "! grep -F 'Accounting diagnostics:'" in workflow and
            "! grep -F '\"PEAK_ACCOUNTING_DIAGNOSTICS\"'" in workflow and
            "PEAK_MACOS_WORKER_COUNT 4u" in concurrent_test and
            "pthread_create" in concurrent_test and
            "pthread_join" in concurrent_test,
            "macOS CI must exercise bounded concurrent detach/reattach and pthread creation")
    require("threaded_bootstrap_main.c" in workflow and
            "refusing macOS runtime activation" in workflow and
            "pthread_create" in threaded_bootstrap_test and
            "dlopen(argv[1]" in threaded_bootstrap_test,
            "macOS CI must exercise fail-closed activation after peers exist")
    require("smoke_branch_target.S" in workflow and
            "peak_macos_branch_target" in workflow and
            "b _peak_macos_smoke_target" in branch_target,
            "macOS CI must exercise reattach through a Gum-followed Arm64 branch")
    require("rejecting PEAK_MEMORY_PROFILE on macOS" in workflow,
            "macOS CI must exercise explicit memory-profile rejection")
    require("Runtime `ATTACH`, `REPLACE`, and" in readme and
            "`REVERT` mutations fail closed" in readme and
            "does not install" in readme and
            "`PEAK_MEMORY_PROFILE` is also rejected explicitly" in readme and
            "does not claim to cover threads" in readme and
            "created directly through Mach APIs" in readme and
            "Memory profiling is rejected" in controller_doc and
            "arbitrary Mach thread creation" in controller_doc and
            "not claimed by this backend" in controller_doc,
            "Darwin docs must bound v1 safety to startup attach and pthread-based workloads")


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
    listener_attach = extract_function(source, "dlopen_interceptor_attach")
    detach = extract_function(source, "dlopen_interceptor_dettach")
    unresolved_counts = extract_function(
        source, "dlopen_interceptor_unresolved_counts"
    )
    retain_handle = extract_function(
        source, "dlopen_interceptor_retain_dynamic_handle"
    )
    enqueue = extract_function(
        source, "dlopen_interceptor_enqueue_dynamic_attach_request"
    )
    pin_handle = extract_function(
        source, "dlopen_interceptor_pin_dynamic_handle_reference"
    )
    ownership_thread = extract_function(
        source, "dlopen_interceptor_ownership_thread_main"
    )
    guarded_dlclose = extract_function(
        source, "dlopen_interceptor_guarded_dlclose"
    )
    revert_dlclose_guard = extract_function(
        source, "dlopen_interceptor_revert_dlclose_guard"
    )
    loaded_identity = extract_function(
        source, "dlopen_interceptor_loaded_module_identity"
    )
    requeue = extract_function(
        source, "dlopen_interceptor_requeue_dynamic_attach_request"
    )
    begin_drain = extract_function(
        source, "dlopen_interceptor_begin_dynamic_attach_drain"
    )
    begin_callback = extract_function(
        source, "dlopen_interceptor_begin_callback"
    )
    end_drain = extract_function(
        source, "dlopen_interceptor_end_dynamic_attach_drain"
    )
    drain = extract_function(
        source, "dlopen_interceptor_drain_dynamic_attach_queue_with_budget"
    )

    require("source_target_array_FFTW[i]" in membership and
            "strcmp(name, source_target_array_FFTW[i]) == 0" in membership and
            "strncmp(" not in membership,
            "FFTW scope must use exact built-in group membership")
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
    require(on_leave.count("dlopen_interceptor_enqueue_dynamic_attach_request(") == 1,
            "one dlopen callback must have only one asynchronous enqueue site")
    require("dlopen_interceptor_unresolved_counts();" in on_leave and
            "if (dlopen_sync_fftw_enabled)" not in on_leave,
            "all dlopen callbacks must use atomic unresolved hints without a target scan")
    end_callback_body = extract_function(
        source, "dlopen_interceptor_end_callback"
    )
    callback_reachable = (
        admission +
        begin_callback_body +
        end_callback_body +
        unresolved_counts +
        on_enter +
        on_leave +
        enqueue
    )
    for loader_api in (
        "dlopen",
        "dlmopen",
        "dlinfo",
        "dlsym",
        "dlvsym",
        "dlclose",
        "dlerror",
        "dladdr",
        "dl_iterate_phdr",
    ):
        require(re.search(rf"\b{loader_api}\s*\(", callback_reachable) is None,
                f"dlopen callback-reachable code must not call loader API {loader_api}")
    require("dlopen_interceptor_pin_dynamic_handle_reference(" not in
            callback_reachable and
            "dlopen_interceptor_wait_for_owned_request(" not in
            callback_reachable and
            "dlopen_interceptor_fftw_module_scan_completed(" not in
            callback_reachable and
            "g_printerr(" not in on_leave and
            "peak_log_" not in on_leave,
            "dlopen callback must only publish borrowed work without pinning, waiting, cache scans, or logging")
    require("dlerror(" not in attach,
            "controller symbol misses must remain conservative without dlerror/gettext")
    require(unresolved_counts.count("atomic_load_explicit(") == 2 and
            "peak_general_listener_controller_lock" not in unresolved_counts and
            "for (" not in unresolved_counts and
            attach.count("dlopen_interceptor_refresh_unresolved_non_fftw_unlocked()") == 1 and
            "atomic_store_explicit(&dlopen_may_have_unresolved_non_fftw" in attach,
            "dlopen callbacks must read only atomic unresolved hints while controller work refreshes mixed-target state")
    require("resolved_count > 1" in attach and
            "max_batch_capacity > 1" in attach and
            "resolved_count < max_batch_capacity" in attach,
            "single-symbol attaches must stay scalar and batch workspaces must be capped by resolved targets")
    require("peak_general_listener_controller_wake();" not in enqueue and
            "peak_general_listener_controller_wake();" in on_leave and
            "peak_general_listener_controller_wake();" not in requeue and
            "pthread_cond_signal" not in enqueue and
            "pthread_cond_signal" not in requeue,
            "callback work must wake after admission closes while retry requeues must not self-wake")
    require("active_dlopen_callback_count != 0" in begin_drain,
            "controller drain must not overlap any admitted dlopen callback")
    require("while (" in begin_callback and
            "dynamic_attach_drain_active" in begin_callback and
            "pthread_cond_wait(&dynamic_attach_gate_cond" in begin_callback and
            "pthread_cond_broadcast(&dynamic_attach_gate_cond)" in end_drain,
            "callback admission and controller drain must form a two-way barrier")
    require("application_handle" in enqueue and
            "PEAK_DLOPEN_REQUEST_BORROWED" in enqueue and
            "dynamic_attach_pending_ownership_count" in enqueue and
            "dlopen_interceptor_pin_dynamic_handle_reference(" in
            ownership_thread and
            "dlmopen((Lmid_t)namespace_id" in pin_handle and
            "RTLD_DI_LMID" in loaded_identity and
            "retained_token != module_token" in pin_handle and
            re.search(r"\b(dlopen|dlmopen)\s*\(", drain) is None,
            "callback enqueue must publish borrowed work, the broker must pin the exact loader namespace, and the controller drain must never reopen by filename")
    require("atomic_load_explicit(&dlclose_guard_owner_pid" in
            guarded_dlclose and
            "atomic_load_explicit(&dlclose_guard_install_pid" in
            guarded_dlclose and
            "getpid() != install_pid" in guarded_dlclose and
            "PEAK_DLCLOSE_GUARD_REVERTING" in guarded_dlclose and
            "PEAK_DLCLOSE_GUARD_REVERTED" in guarded_dlclose and
            "dynamic_attach_pending_ownership_count" in guarded_dlclose and
            "request->reference_transferred = TRUE" in guarded_dlclose and
            "request->handle_owned = TRUE" in guarded_dlclose and
            guarded_dlclose.count("result = close_function(handle);") >= 2 and
            "atomic_fetch_add_explicit(&active_dlclose_guard_count" in
            guarded_dlclose and
            guarded_dlclose.count(
                "atomic_fetch_sub_explicit(&active_dlclose_guard_count"
            ) >= 3 and
            guarded_dlclose.count("memory_order_seq_cst") >= 5 and
            re.search(r"\bdlclose\s*\(", guarded_dlclose) is None,
            "dlclose guard must fail open in fork children, use an SC route/readers gate, and transfer pending references without loader calls")
    reverting_publish = revert_dlclose_guard.find(
        "PEAK_DLCLOSE_GUARD_REVERTING"
    )
    active_wait = revert_dlclose_guard.find(
        "dlopen_interceptor_wait_for_dlclose_guard_idle()"
    )
    gum_revert = revert_dlclose_guard.find(
        "gum_interceptor_revert(dlopen_interceptor, dlclose_hook_address)"
    )
    reverted_publish = revert_dlclose_guard.find(
        "PEAK_DLCLOSE_GUARD_REVERTED"
    )
    require(reverting_publish != -1 and active_wait != -1 and
            gum_revert != -1 and reverted_publish != -1 and
            reverting_publish < active_wait < gum_revert < reverted_publish and
            revert_dlclose_guard.count("memory_order_seq_cst") >= 4,
            "dlclose revert must publish an SC closing gate, drain admitted trampoline readers, revert, then publish the restored route")
    replace_position = listener_attach.find("gum_interceptor_replace_fast(")
    broker_start_position = listener_attach.find(
        "dlopen_interceptor_start_ownership_thread()"
    )
    require(replace_position != -1 and broker_start_position != -1 and
            replace_position < broker_start_position,
            "ownership broker must start only after startup dlopen/dlclose Gum mutations preserve their single-thread proof")
    drain_release = drain.find(
        "dlopen_interceptor_release_dynamic_attach_request_metadata("
    )
    drain_end = drain.find("dlopen_interceptor_end_dynamic_attach_drain();")
    drain_close = drain.find(
        "dlopen_interceptor_internal_dlclose(handles_to_close[i]);"
    )
    require(drain_release != -1 and drain_end != -1 and drain_close != -1 and
            drain_release < drain_end < drain_close,
            "queue-owned handles must be closed only after the controller reopens callback admission")

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
    require("request->scope == PEAK_DLOPEN_ATTACH_FFTW_ONLY" in attach and
            "request->scope == PEAK_DLOPEN_ATTACH_ALL" in attach and
            "resolved_fftw_from_handle" in attach and
            "peak_hook_address_count == target_count" in attach and
            "resolved_targets[i].address != NULL" in attach and
            "dlopen_interceptor_target_is_unresolved_unlocked(i)" in attach and
            "completed_fftw_scan && request->module_token != NULL" in retain_handle and
            "g_hash_table_add(dlopen_completed_fftw_modules" in retain_handle,
            "FFTW module cache publication must require a complete scan and a retained exact module")
    require("atomic_load_explicit(&dlopen_listener_owner_pid" in admission and
            "getpid() == owner" in admission and
            "pthread_mutex" not in admission and "g_" not in admission,
            "fork-child admission predicate must be PID-only and lock-free")
    require("atomic_store_explicit(&dlopen_listener_owner_pid" in enable and
            "getpid()" in enable,
            "dlopen callback admission must open only with dynamic attach")
    require("dlopen_listener_attached" in enable and
            "dlclose_guard_replaced" in enable and
            "original_dlclose != NULL" in enable,
            "dynamic admission must require both installed loader hooks and a published dlclose trampoline")
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
            "callback count admission must revalidate owner and OPEN under the gate mutex")
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
    pending_idle = shutdown.find(
        "dlopen_interceptor_wait_for_pending_ownership_idle()"
    )
    guard_close = shutdown.find(
        "atomic_store_explicit(&dlclose_guard_owner_pid"
    )
    broker_stop = shutdown.find(
        "dlopen_interceptor_stop_ownership_thread()"
    )
    queue_discard = shutdown.find(
        "dlopen_interceptor_discard_dynamic_attach_queue()"
    )
    require(pending_idle != -1 and guard_close != -1 and
            broker_stop != -1 and queue_discard != -1 and
            pending_idle < guard_close < broker_stop < queue_discard,
            "shutdown must resolve handoffs before disabling the guard, stopping the broker, and discarding owned work")
    detach_shutdown = detach.find(
        "dlopen_interceptor_shutdown_dynamic_attach()"
    )
    guard_revert = detach.find(
        "dlopen_interceptor_revert_dlclose_guard("
    )
    teardown_flush = detach.find("dlopen_interceptor_flush_teardown()")
    require(detach_shutdown != -1 and guard_revert != -1 and
            teardown_flush != -1 and
            detach_shutdown < guard_revert < teardown_flush,
            "teardown must resolve ownership, safely revert dlclose, and only then flush")

    peak_source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    fini = extract_function(peak_source, "peak_fini_impl")
    pinned_shutdown = fini.find(
        "dlopen_interceptor_shutdown_dynamic_attach()"
    )
    shutdown_failure = fini.find(
        "if (!dlopen_shutdown_flushed)", pinned_shutdown
    )
    early_report = fini.find(
        "peak_general_listener_print_with_mpi_job_policy(",
        pinned_shutdown,
    )
    regular_report = fini.find(
        "peak_general_listener_print(",
        pinned_shutdown,
    )
    report_positions = [
        position for position in (early_report, regular_report)
        if position != -1
    ]
    report = min(report_positions) if report_positions else -1
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
            ("peak_gum_interceptor_attach_target(" in normalized or
             "peak_general_listener_gum_attach_target(" in normalized) and
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
    require("peak_general_listener_gum_attach_target(" in dlopen_scalar_attach and
            "&candidate->attach_plan" in dlopen_scalar_attach and
            "peak_general_listener_gum_attach_target(" in dlopen_batch_attach and
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


def check_x86_patched_gum_requires_exact_attach(repo_root):
    cmake = (repo_root / "cmake/frida-gum.cmake").read_text(encoding="utf-8")
    match = re.search(
        r"function\(_peak_validate_frida_gum_peak_api\).*?endfunction\(\)",
        cmake,
        flags=re.DOTALL,
    )
    require(match is not None, "missing patched Gum API validation function")
    validate = match.group(0)
    exact_probe = validate.find("PEAK_GUM_HAS_PEAK_EXACT_ATTACH_API")
    x86_guard = validate.find('MATCHES "^(x86_64|amd64)$"', exact_probe)
    fatal = validate.find("message(FATAL_ERROR", x86_guard)
    require(exact_probe != -1 and x86_guard != -1 and fatal != -1 and
            exact_probe < x86_guard < fatal and
            "NOT PEAK_GUM_HAS_PEAK_EXACT_ATTACH_API" in
            validate[x86_guard:fatal],
            "Linux x86 PEAK-patched Gum must fail configuration without exact-entry attach")


def check_fast_listener_unwind_abi(repo_root):
    cmake = (repo_root / "cmake/frida-gum.cmake").read_text(
        encoding="utf-8"
    )
    api = (repo_root / "cmake/peak-gum/frida-gum-peak-api.h").read_text(
        encoding="utf-8"
    )
    overlay = (repo_root / "cmake/peak-gum/gum_peak_pc_api.c").read_text(
        encoding="utf-8"
    )
    patcher = (
        repo_root / "cmake/peak-gum/patch_frida_gum_elf_module.py"
    ).read_text(encoding="utf-8")
    listener = read_source(repo_root, "src/general_listener.c")
    unwind_test = (
        repo_root / "test/detach_runtime/test_fastpath_nonlocal_unwind.c"
    ).read_text(encoding="utf-8")

    require("GUM_PEAK_FAST_LISTENER_VERSION 8u" in api and
            "GUM_PEAK_FAST_LISTENER_VERSION != 8" in cmake,
            "patched Gum configuration must reject an older fast-listener callback ABI")
    require("peak_gum_get_interceptor_thread_context" in patcher and
            '"--globalize-symbol"' in patcher and
            'tls_model("initial-exec")' in overlay and
            "peak_gum_cached_invocation_stack" in overlay and
            "peak_gum_invocation_stack_reap_unwound(stack_address)"
            in overlay and
            "gum_interceptor_peak_invocation_stack_entry_matches" in overlay and
            "peak_gum_invocation_stack_depth()" in overlay and
            "peak_gum_invocation_stack_reap_to_depth(gum_stack_depth)"
            in overlay,
            "direct dispatch must cache, snapshot, and restore Gum's generic "
            "invocation-stack depth without a steady-state Gum TLS lookup")
    require("entry->gum_stack_depth = gum_stack_depth" in listener and
            "*gum_stack_depth_out = entry.gum_stack_depth" in listener,
            "the PEAK direct invocation entry must carry its Gum stack boundary")
    require("peak_general_listener_invocation_stack_address(ic)" in listener and
            "gum_invocation_context_get_depth(ic)" in listener and
            "gum_interceptor_peak_invocation_stack_entry_matches("
            in listener and
            "peak_general_listener_fast_reap_unwound(stack_address)"
            in listener,
            "the next direct entry must independently reconcile escaped PEAK "
            "frames after Gum synchronizes its own live stack boundary")
    require("install_mixed_listener()" in unwind_test and
            "(gpointer)peak_fastpath_unwind_inner" in unwind_test and
            "mixed_listener_leaves" in unwind_test and
            "escape_all_from_deeper_frame()" in unwind_test and
            "escape_generic_from_deeper_frame()" in unwind_test and
            "escape-all recovery direct call failed" in unwind_test and
            "peak_fastpath_unwind_unrelated_bridge()" in unwind_test and
            "unrelated listener count mismatch" in unwind_test and
            "peak_general_listener_test_current_invocation_level"
            in unwind_test and
            "PEAK invocation stack was not fully reaped" in unwind_test,
            "non-local-unwind regression must cover surviving and fully "
            "escaped mixed frames plus a generic-only escape followed by a "
            "live unrelated Gum invocation")


def check_peak_init_heartbeat_order(repo_root):
    source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    init = extract_function(source, "peak_init")
    body = extract_function(source, "peak_activate_runtime")
    fini = extract_function(source, "peak_fini_impl")

    group_load_position = init.find("load_symbols_from_array(PEAK_TARGET_GROUP_ENV")
    deduplicate_position = init.find("peak_deduplicate_target_names(")
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
    gum_init_position = body.find("gum_init_embedded()")
    general_attach_position = body.find("peak_general_listener_attach()")
    syscall_attach_position = body.find("syscall_interceptor_attach()")
    dlopen_attach_position = body.find("dlopen_interceptor_attach()")
    malloc_attach_position = body.find("malloc_interceptor_attach()")
    controller_start_position = body.find(
        "peak_general_listener_controller_start()"
    )
    dynamic_enable_position = body.find("dlopen_interceptor_enable_dynamic_attach()")
    require(-1 not in (gum_init_position, general_attach_position,
                       syscall_attach_position,
                       dlopen_attach_position, malloc_attach_position,
                       controller_start_position, dynamic_enable_position) and
            gum_init_position < general_attach_position <
            syscall_attach_position <
            dlopen_attach_position < malloc_attach_position <
            controller_start_position < dynamic_enable_position <
            heartbeat_position,
            "startup Gum hooks must finish before controller and dlopen admission")
    for forbidden in (
        "gum_init_embedded()",
        "pthread_listener_attach()",
        "mpi_interceptor_attach()",
        "peak_general_listener_attach()",
        "syscall_interceptor_attach()",
        "dlopen_interceptor_attach()",
        "malloc_interceptor_attach()",
        "peak_general_listener_controller_start()",
        "pthread_create(&heartbeat_thread",
    ):
        require(forbidden not in init,
                f"MPI pre-init configuration must not perform {forbidden}")
    main_wrapper = extract_function(source, "main_wrapper")
    require("peak_init();" in main_wrapper,
            "main_wrapper must configure PEAK before application main")
    require("exit_interceptor_attach" not in source and
            "exit_interceptor_detach" not in source,
            "ELF exit handling must not use a post-MPI Gum replacement")
    exit_interposer = extract_function(source, "exit")
    exit_handler = extract_function(source, "peak_exit")
    require("peak_exit(status);" in exit_interposer and
            "peak_runtime_close_activation_for_teardown();" in exit_handler and
            "peak_fini();" in exit_handler and
            "original_exit(status);" in exit_handler,
            "the ordinary ELF exit interposer must remain inert before "
            "activation and finalize PEAK afterward")
    unavailable_position = exit_handler.find(
        "if (atomic_load_explicit(&peak_exit_interposer_unavailable,"
    )
    activation_position = exit_handler.find(
        "peak_runtime_close_activation_for_teardown();"
    )
    require(unavailable_position != -1 and
            unavailable_position < activation_position and
            "original_exit(status);" in exit_handler[unavailable_position:activation_position],
            "an unavailable explicit-exit interposer must delegate to libc "
            "exit before PEAK teardown")
    exit_resolver = extract_function(source, "peak_resolve_real_exit")
    primary_exit_lookup = exit_resolver.find('dlsym(RTLD_NEXT, "exit")')
    libc_exit_lookup = exit_resolver.find('dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL)')
    forced_disable = exit_resolver.find("if (force_libc_fallback)")
    require("force_libc_fallback" in exit_resolver and
            "original_exit == NULL && !force_libc_fallback" in exit_resolver and
            primary_exit_lookup != -1 and libc_exit_lookup > primary_exit_lookup and
            forced_disable > libc_exit_lookup,
            "the exit-interposer failure hook must exercise the libc exit "
            "fallback before disabling PEAK teardown")
    activation_close = extract_function(
        source, "peak_runtime_close_activation_for_teardown"
    )
    require("PEAK_RUNTIME_ACTIVATION_CANCELED" in source and
            "PEAK_RUNTIME_ACTIVATION_READY" in activation_close and
            "PEAK_RUNTIME_ACTIVATION_CANCELED" in activation_close and
            "atomic_compare_exchange_weak_explicit(" in activation_close,
            "inactive teardown must atomically claim READY -> CANCELED")
    require("PEAK_RUNTIME_ACTIVATION_IN_PROGRESS" in activation_close and
            "sched_yield();" in activation_close,
            "teardown must wait if activation wins the READY transition")
    require("peak_runtime_close_activation_for_teardown();" in
            extract_function(source, "peak_fini"),
            "main-return finalization must close deferred activation")
    libc_start_main_position = source.find("int __libc_start_main(")
    resolve_exit_position = source.find(
        "peak_resolve_real_exit();",
        libc_start_main_position,
    )
    require(libc_start_main_position != -1 and
            resolve_exit_position != -1,
            "the real exit symbol must be resolved without a Gum patch "
            "before application main")
    policy = extract_function(
        source, "peak_mpi_activation_policy_post_init"
    )
    require('g_ascii_strcasecmp(value, "post-init")' in policy and
            'g_ascii_strcasecmp(value, "immediate")' in policy and
            "return FALSE;" in policy,
            "post-MPI activation must be explicit and default fail-open to "
            "immediate activation")
    pending_policy_position = init.find("if (activate_post_mpi_init)")
    pending_position = init.find(
        "query_result == MPI_SUCCESS && !initialized",
        pending_policy_position,
    )
    pending_return = init.find("return;", pending_position)
    fallback_activation = init.rfind("peak_activate_runtime();")
    require(pending_policy_position != -1 and pending_position != -1 and
            pending_return != -1 and
            fallback_activation != -1 and
            pending_policy_position < pending_position < pending_return <
            fallback_activation,
            "only the explicit post-init policy may defer runtime activation "
            "until an init-return interposer reports completion")

    mpi = read_source(repo_root, "src/mpi_interceptor.c")
    init_call = extract_function(mpi, "mpi_interceptor_call_init")
    init_thread_call = extract_function(
        mpi, "mpi_interceptor_call_init_thread"
    )
    for label, interposer in (("MPI_Init", init_call),
                              ("MPI_Init_thread", init_thread_call)):
        real_call = interposer.find("result = init(")
        completion = interposer.find("peak_mpi_init_completed(result);")
        outermost = interposer.find("peak_mpi_init_wrapper_depth == 0")
        require(real_call != -1 and outermost != -1 and completion != -1 and
                real_call < outermost < completion,
                f"{label} must activate PEAK only after the outermost real "
                "initializer returns")
    require("gum_" not in init_call and "gum_" not in init_thread_call,
            "MPI initialization interposers must remain independent of Gum")
    for symbol in ("MPI_Init", "PMPI_Init",
                   "MPI_Init_thread", "PMPI_Init_thread"):
        require(f'dlsym(RTLD_NEXT, "{symbol}")' in mpi,
                f"{symbol} must preserve the next same-name wrapper chain")
    require(mpi.count("pthread_once(") >= 4,
            "MPI initialization symbol resolution must be thread-safe")
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
    guard = extract_function(peak_source, "peak_mpi_real_finalize_config_allowed")
    require("PEAK_MPI_REAL_FINALIZE_ENV" in guard and
            "peak_env_value_truthy(value)" in guard,
            "real MPI finalizer policy must preserve explicit env override")
    require("peak_mpi_runtime_is_intel_2019()" in guard and
            "return FALSE;" in guard,
            "Intel MPI 2019 must skip its unsafe real finalizer by default")
    vendor = extract_function(peak_source, "peak_mpi_runtime_is_intel_2019")
    require("MPI_Get_library_version" in vendor and
            "Intel(R) MPI" in vendor and
            "Intel MPI" in vendor and
            'strstr(text, "2019")' in vendor,
            "Intel MPI 2019 containment must inspect the MPI library version")
    fini = extract_function(peak_source, "peak_fini_impl")
    completion = (
        'peak_log_report("[peak] PEAK output is complete; '
        'report publication and release succeeded\\n")'
    )
    require(completion in fini and
            "report_release_protocol_completed &&\n"
            "                all_reports_succeeded" in fini,
            "successful all-rank report release must emit one "
            "default-visible completion marker")
    require(fini.count("PEAK output is complete;") == 1,
            "MPI finalization policy diagnostics must not duplicate the "
            "canonical PEAK completion marker")


def check_final_report_snapshot_order(repo_root):
    peak_source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    general = read_source(repo_root, "src/general_listener.c")
    formatter = read_source(
        repo_root, "src/general_listener/report_formatter.c"
    )
    mpi_transport = read_source(
        repo_root, "src/general_listener/mpi_report_transport.c"
    )
    socket_transport = read_source(
        repo_root, "src/general_listener/socket_report_transport.c"
    )
    runtime_config = read_source(
        repo_root, "src/general_listener/runtime_config.c"
    )
    fini = extract_function(peak_source, "peak_fini_impl")
    local_report = extract_function(
        general, "peak_general_listener_local_report_overhead"
    )
    freeze_report = extract_function(
        general, "peak_general_listener_freeze_final_report_snapshot"
    )
    write_report = extract_function(
        general, "peak_general_listener_write_report"
    )
    print_text = extract_function(
        formatter, "peak_report_formatter_write_text"
    )
    print_mpi_maxima = extract_function(
        formatter, "peak_report_formatter_write_rank_maxima"
    )
    print_entry = extract_function(
        general,
        "peak_general_listener_print_with_mpi_job_policy",
    )
    local_ranks = extract_function(
        runtime_config, "peak_general_listener_local_mpi_ranks"
    )
    detect_local_ranks = extract_function(
        runtime_config, "peak_general_listener_detect_local_mpi_ranks"
    )
    configure_runtime = extract_function(
        runtime_config, "peak_general_listener_runtime_configure"
    )
    listener_attach = extract_function(
        general, "peak_general_listener_attach"
    )
    parse_positive_uint = extract_function(
        runtime_config, "peak_general_listener_parse_positive_uint_text"
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
            "parsed > UINT_MAX" in parse_positive_uint and
            "MPI_LOCALNRANKS" in detect_local_ranks and
            "OMPI_COMM_WORLD_LOCAL_SIZE" in detect_local_ranks and
            "MV2_COMM_WORLD_LOCAL_SIZE" in detect_local_ranks and
            "PMI_LOCAL_SIZE" in detect_local_ranks and
            "return parsed;" in detect_local_ranks and
            "return 1U;" in detect_local_ranks,
            "local MPI rank discovery must reject invalid values and fall back to one")
    require("configured_local_mpi_ranks" in local_ranks and
            "getenv" not in local_ranks and
            "peak_general_listener_detect_local_mpi_ranks()" in
            configure_runtime and
            "peak_general_listener_runtime_configure();" in listener_attach,
            "heartbeat local-rank policy must be snapshotted before the controller starts")
    require("peak_general_listener_multiply_nonnegative_finite" in control_risk and
            "peak_general_listener_local_mpi_ranks()" in control_risk and
            "return DBL_MAX;" in control_risk,
            "control risk must be local ranks times raw control and fail closed")

    require("PeakReportSnapshot* snapshot" in write_report and
            "peak_report_formatter_write_rank_local_csv(snapshot)" in
                write_report and
            "peak_report_formatter_write_rank_local_csv_host_disambiguated(" in
                write_report and
            "peak_report_formatter_write_csv(snapshot)" in write_report and
            "peak_report_formatter_write_text(snapshot, &options)" in
                write_report,
            "final reporting wrapper must forward the immutable report snapshot with explicit rank-local naming")
    require("const PeakReportOverhead* overhead" in print_text and
            "overhead = &snapshot->overhead" in print_text and
            "overhead->valid" in print_text and
            "overhead->profile_seconds" in print_text and
            "overhead->control_seconds" in print_text and
            "overhead->profile_ratio" in print_text and
            "overhead->control_ratio" in print_text and
            "overhead->profile_control_risk_ratio" in print_text and
            "overhead->control_risk_ratio" in print_text and
            "overhead->management_ratio" in print_text,
            "text output must consume explicit raw and risk report fields")
    require("profile_ratio=%.9f control_ratio=%.9f ratio=%.9f" in print_text,
            "local text output must include explicit profile/control ratio fields")
    require("[peak] local profile+local-rank-control risk: profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u risk_control_seconds=%.9f ratio=%.9f" in print_text and
            "[peak] per-rank maximum profile+control risk overhead: owner_rank=%d profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f" in print_mpi_maxima and
            "[peak] per-rank maximum control risk overhead: owner_rank=%d raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f" in print_mpi_maxima and
            "[peak] per-rank maximum profile+control overhead: owner_rank=%d profile_seconds=%.9f control_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f" in print_mpi_maxima and
            "peak_report_formatter_write_rank_maxima" in print_text,
            "text output must keep strict, separate, and owner-consistent raw/risk ratio contracts")
    require("[peak] %s final transition coverage: detached_targets=%zu reattached_targets=%zu revisited_targets=%zu" in print_text and
            "rank_count > 1 ? \"aggregate\" : \"local\"" in print_text,
            "final output must expose exact aggregate ever-revisited coverage")
    require("[peak] per-rank elapsed range: min_seconds=%.9f max_seconds=%.9f" in print_text and
            "overhead->elapsed_min_seconds" in print_text and
            "overhead->elapsed_max_seconds" in print_text,
            "final output must expose the exact per-rank elapsed range contract")
    require("PeakReportOverhead local_report =" in print_entry and
            "peak_general_listener_local_report_overhead(sum_num_calls)" in print_entry and
            "peak_general_listener_build_report_snapshot(" in print_entry and
            "&local_report" in print_entry and
            "local_snapshot, TRUE, TRUE, active_mpi_job)" in print_entry,
            "local final output must consume the frozen report snapshot")

    early_report_position = fini.find(
        "peak_general_listener_print_with_mpi_job_policy("
    )
    proof_position = fini.find(
        "peak_mpi_teardown_all_ranks_requested_finalize("
    )
    proof_guard_position = fini.rfind(
        "if (need_mpi_finalize_proof", early_report_position, proof_position
    )
    regular_report_position = fini.find(
        "peak_general_listener_print_with_mpi_job_policy(",
        early_report_position + 1,
    )
    report_position = early_report_position
    cuda_position = fini.find(
        "cuda_interceptor_print_with_mpi_job_policy(", report_position)
    finalize_permission_position = fini.find(
        "mpi_interceptor_set_real_finalize_allowed(", report_position
    )
    report_release_position = fini.find(
        "peak_mpi_teardown_complete_report_release(", report_position
    )
    combined_release_position = fini.find(
        "peak_mpi_teardown_complete_post_publication_release(",
        report_position,
    )
    require(early_report_position != -1 and proof_position != -1 and
            proof_guard_position != -1 and
            regular_report_position != -1 and cuda_position != -1 and
            combined_release_position != -1 and
            report_release_position != -1 and
            finalize_permission_position != -1 and
            early_report_position < proof_position <
                regular_report_position < cuda_position <
                combined_release_position < report_release_position <
                finalize_permission_position,
            "local/socket output must precede MPI's proof-first output while CUDA output and both release gates remain ordered before the real-finalize decision")
    require(
        "!publish_before_finalize_proof" in
            fini[proof_guard_position:proof_position] and
        "if (publish_before_finalize_proof)" in
            fini[cuda_position:combined_release_position] and
        "(publish_before_finalize_proof ||" in fini and
        "all_ranks_requested_mpi_finalize" in
            fini[combined_release_position:finalize_permission_position],
        "post-publication local/socket output must combine finalize participation with its long release gate while MPI aggregation retains the separate proof-first gate")
    require(
        "output_mode == PEAK_OUTPUT_AGGREGATION_SOCKET" in
            fini[cuda_position:combined_release_position] and
        "socket_combined_release_minimum_ms" in
            fini[cuda_position:combined_release_position] and
        "publication_timeout_minimum_ms," in
            fini[combined_release_position:report_release_position],
        "the combined gate must use the stable attempted socket mode, including socket-to-local fallback, to select the R+2T timeout floor")
    require("fflush(stderr)" in fini and
            fini.find("fflush(stderr)", cuda_position) <
                combined_release_position,
            "CUDA text output must be flushed before the all-rank report release")
    require("report_release_protocol_completed &&" in fini and
            "all_real_mpi_finalize_config_allowed" in fini and
            "real_mpi_finalize_config_allowed" in fini and
            fini.find("report_release_protocol_completed &&", report_position) <
                finalize_permission_position,
            "real PMPI_Finalize must require a completed release protocol and an all-rank policy decision")

    reduce_result = extract_function(
        mpi_transport, "peak_mpi_report_transport_reduce"
    )
    tuple_reduce = extract_function(
        mpi_transport, "peak_mpi_reduce_report_rank_tuples"
    )
    tuple_bcast = extract_function(
        mpi_transport, "peak_mpi_bcast_report_rank_tuple"
    )
    set_mpi_overhead = extract_function(
        mpi_transport, "peak_mpi_report_transport_set_overhead"
    )
    require("peak_report_overhead_rank_tuple(&local->overhead)" in reduce_result and
            "peak_mpi_reduce_report_rank_tuples" in reduce_result and
            "peak_mpi_report_transport_set_overhead" in reduce_result and
            "maximum_reports" in reduce_result and
            "maximum_owner_ranks" in reduce_result and
            "peak_report_maxima_load" in set_mpi_overhead and
            "&overhead->per_rank_maxima" in set_mpi_overhead and
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
    require("MPI_BYTE" not in tuple_bcast and
            "MPI_INT" in tuple_bcast and
            "MPI_UNSIGNED" in tuple_bcast and
            "PEAK_MPI_UINT64_DATATYPE" in tuple_bcast and
            "MPI_DOUBLE" in tuple_bcast,
            "MPI report tuples must use field-wise typed broadcasts")
    require("_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64" in
                mpi_transport and
            "MPI_UINT64_T" in mpi_transport and
            "UINT64_MAX == ULONG_MAX" in mpi_transport and
            "UINT64_MAX == ULLONG_MAX" in mpi_transport,
            "MPI reporting must select an exact uint64 datatype with a compile-time fallback")
    require("local_elapsed_valid" in reduce_result and
            "\"elapsed-valid\"" in reduce_result and
            "MPI_MIN" in reduce_result and
            "\"elapsed-min\"" in reduce_result and
            "MPI_MAX" in reduce_result and
            "\"elapsed-max\"" in reduce_result and
            "overhead->elapsed_min_seconds = min_elapsed_seconds" in
                set_mpi_overhead and
            "overhead->elapsed_max_seconds = max_elapsed_seconds" in
                set_mpi_overhead and
            "mpi_min_elapsed_seconds" in reduce_result and
            "mpi_max_elapsed_seconds" in reduce_result,
            "MPI final reporting must validate and reduce exact elapsed endpoints")
    require("\"accounting-valid\"" in reduce_result and
            "MPI_MIN" in reduce_result and
            "\"failed-stop-window-max\"" in reduce_result and
            "(UINT64_MAX - 1) / (uint64_t)size" in reduce_result and
            "\"failed-stop-window-count\"" in reduce_result and
            "MPI_SUM" in reduce_result and
            "all_accounting_valid != 0" in reduce_result and
            "mpi_failed_stop_window_count" in reduce_result and
            "overhead->accounting_valid = all_accounting_valid" in
                set_mpi_overhead and
            "overhead->failed_stop_window_count = failed_stop_window_count" in
                set_mpi_overhead,
            "MPI final reporting must carry all-rank accounting validity and failed-window evidence")

    socket_result = extract_function(
        socket_transport, "peak_socket_report_transport_begin"
    )
    socket_validate_header = extract_function(
        socket_transport, "peak_socket_gather_validate_header"
    )
    socket_prepare_receipt = extract_function(
        socket_transport, "peak_socket_gather_prepare_receipt"
    )
    socket_root_gather = extract_function(
        socket_transport, "peak_socket_reduce_root_gather"
    )
    socket_read_ready = extract_function(
        socket_transport, "peak_socket_gather_read_ready"
    )
    socket_write_ready = extract_function(
        socket_transport, "peak_socket_gather_write_ready"
    )
    socket_peer_begin = extract_function(
        socket_transport, "peak_socket_report_peer_begin"
    )
    set_socket_overhead = extract_function(
        socket_transport, "peak_socket_report_set_aggregate_overhead"
    )
    dropped_calls_are_saturated = re.search(
        r"\*aggregate->dropped_calls\s*=\s*"
        r"peak_socket_add_uint64_saturated\s*\(\s*"
        r"\*aggregate->dropped_calls\s*,\s*"
        r"connection->header\.dropped_calls\s*\)",
        socket_prepare_receipt,
        re.DOTALL,
    ) is not None
    dropped_threads_are_saturated = re.search(
        r"\*aggregate->dropped_threads\s*=\s*"
        r"peak_socket_add_uint64_saturated\s*\(\s*"
        r"\*aggregate->dropped_threads\s*,\s*"
        r"connection->header\.dropped_threads\s*\)",
        socket_prepare_receipt,
        re.DOTALL,
    ) is not None
    require("#define PEAK_SOCKET_REDUCE_VERSION 13U" in socket_transport and
            "peak_socket_reduce_header_set_report_tuple" in socket_result and
            "peak_socket_reduce_header_report_tuple" in
                socket_validate_header and
            "peak_report_rank_tuple_is_valid" in socket_validate_header and
            "peak_report_maxima_initialize" in socket_result and
            "peak_report_maxima_consider" in socket_prepare_receipt and
            "peak_socket_report_set_aggregate_overhead" in socket_result and
            "report.per_rank_maxima = *maxima" in set_socket_overhead and
            "combined_maximum->stop_window_count" in set_socket_overhead and
            "combined_maximum->elapsed_seconds" in set_socket_overhead and
            "report.accounting_valid = accounting_valid" in
                set_socket_overhead and
            dropped_calls_are_saturated and
            dropped_threads_are_saturated,
            "socket reducer must carry complete owner tuples and accounting health")
    require("PEAK_SOCKET_GATHER_ACTIVE_MAX" in socket_transport and
            "peak_socket_reduce_gather_active_limit" in
                socket_root_gather and
            "active < active_limit" in socket_root_gather and
            "poll(descriptors" in socket_root_gather and
            "progress_timeout_ms" in socket_root_gather and
            "hard_deadline_us" in socket_root_gather and
            "peak_socket_reduce_refresh_progress_deadline_us" in
                socket_root_gather and
            "peak_socket_reduce_remaining_ms(progress_deadline_us)" in
                socket_root_gather and
            "connection->record_index > record_index_before" in
                socket_root_gather and
            "completed > completed_before" in socket_root_gather and
            "PEAK_SOCKET_GATHER_READING_HEADER" in socket_read_ready and
            "PEAK_SOCKET_GATHER_READING_PAYLOAD" in socket_read_ready,
            "socket gather must combine bounded no-progress and absolute deadlines")
    release_bind = socket_result.find(
        "peak_socket_reduce_bind_listener(release_port)"
    )
    gather_listen = socket_result.find(
        "peak_socket_reduce_create_listener(port, size - 1)"
    )
    require(release_bind != -1 and gather_listen != -1 and
            release_bind < gather_listen and
            "int release_listener;" in socket_transport and
            "session->release_listener = -1" in socket_transport,
            "socket root must reserve one release fd before gather and consume it exactly once")
    require("PEAK_SOCKET_REDUCE_GATHER_RECEIPT" in
                socket_prepare_receipt and
            "PEAK_SOCKET_REDUCE_GATHER_REGISTERED" in
                socket_prepare_receipt and
            "sizeof(connection->receipt)" in socket_write_ready and
            "PEAK_SOCKET_GATHER_READING_CONFIRM" in socket_root_gather and
            "PEAK_SOCKET_REDUCE_GATHER_RECEIPT_CONFIRM" in
                socket_read_ready and
            "peak_socket_reduce_recv_all" in socket_peer_begin and
            "peak_socket_reduce_send_gather_confirmation" in
                socket_peer_begin and
            "receipt_received" in socket_peer_begin and
            "release_targets[rank] = true" in socket_root_gather,
            "wire-v11 gather must receipt and confirm each peer before completion")
    require("min_total_time[i] = DBL_MAX" in print_entry and
            "sum_min_time[i] = FLT_MAX" in print_entry and
            "peak_report_calls_per_active_thread" in formatter and
            "thread_count[i] = 1" not in print_entry,
            "inactive ranks must be neutral for thread counts and minima")


def check_stop_window_accounting_sidecar(repo_root):
    source = (repo_root / "src/detach_controller.c").read_text(
        encoding="utf-8"
    )
    general = read_source(repo_root, "src/general_listener.c")
    attach_policy = read_source(
        repo_root, "src/general_listener/attach_policy.c"
    )
    formatter = read_source(
        repo_root, "src/general_listener/report_formatter.c"
    )
    mpi_transport = read_source(
        repo_root, "src/general_listener/mpi_report_transport.c"
    )
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
        attach_policy, "peak_general_listener_attach_target_is_supported"
    )
    general_listener_attach = extract_function(
        general, "peak_general_listener_attach"
    )
    overhead_bootstrap = extract_function(
        general, "peak_general_overhead_bootstrapping"
    )
    startup_skip = extract_function(
        attach_policy, "peak_general_listener_startup_attach_can_skip_stop"
    )
    general_attach_policy_init = extract_function(
        attach_policy, "peak_general_listener_init_attach_policy_once"
    )
    heartbeat = extract_function(general, "peak_heartbeat_monitor")
    note_runtime_start = extract_function(
        general, "peak_general_listener_note_runtime_start"
    )
    print_text = extract_function(
        formatter, "peak_report_formatter_write_text"
    )
    summarize_report = extract_function(
        formatter, "peak_report_formatter_summarize"
    )
    local_report = extract_function(
        general, "peak_general_listener_local_report_overhead"
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
        mpi_transport,
        "peak_mpi_report_transport_reduce",
    )
    begin_marker_swap = extract_function(
        general,
        "peak_general_listener_begin_report_marker_swap",
    )
    print_entry = extract_function(
        general,
        "peak_general_listener_print_with_mpi_job_policy",
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
    require("PeakDetachAccountingSnapshot accounting" in local_report and
            "peak_general_listener_runtime_accounting_snapshot(&accounting)" in
                local_report and
            "peak_general_listener_control_wall_ns_since_heartbeat(" in
                local_report and
            "overhead.control_seconds" in local_report and
            "summary.stop_window_seconds = overhead->control_seconds" in
                summarize_report and
            "summary.elapsed_seconds = overhead->elapsed_seconds" in
                summarize_report and
            "summary.stop_window_seconds / summary.elapsed_seconds" in
                summarize_report and
            "control stop-window overhead:" in print_text,
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
        heartbeat.find("/* 1) Per-target detach. */"):
        heartbeat.find("/* 2) Global detach. */")
    ]
    global_detach_for_budget = heartbeat[
        heartbeat.find("/* 2) Global detach. */"):
        heartbeat.find("/* 3) Reattach. */")
    ]
    adaptive_sleep_start = heartbeat.find(
        "/* Adapt the next heartbeat sleep interval. */")
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
    reattach_section = heartbeat[
        heartbeat.find("/* 3) Reattach. */"):]
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
    revisit_reduce = re.search(
        r"peak_mpi_reduce_checked\(\s*local->revisited,\s*"
        r"aggregate->revisited,\s*hook_count,\s*MPI_INT,\s*MPI_MAX,\s*0,\s*"
        r"rank\s*==\s*0,\s*"
        r"\"revisited-marker\"\s*\)",
        reduce_result,
    )
    mpi_root_branch = print_entry.find(
        "result == PEAK_MPI_REPORT_TRANSPORT_ROOT_READY"
    )
    marker_swap_call = print_entry.find(
        "peak_general_listener_begin_report_marker_swap(aggregate)",
        mpi_root_branch,
    )
    marker_swap_commit = print_entry.find(
        "peak_general_listener_commit_report_marker_swap(&marker_swap)",
        marker_swap_call,
    )
    require(revisit_reduce is not None and
            "swap.installed_revisited[i] = source->revisited[i] != 0" in
                begin_marker_swap and
            "array_listener_revisited = swap.installed_revisited" in
                begin_marker_swap and
            mpi_root_branch != -1 and marker_swap_call != -1 and
            marker_swap_commit != -1 and
            mpi_root_branch < marker_swap_call < marker_swap_commit,
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
    require(general_attach_supported.count("peak_log_info(") == 2 and
            "g_printerr(" not in general_attach_supported and
            "peak_log_warn(" not in general_attach_supported,
            "expected target safety-policy skips must remain INFO diagnostics "
            "instead of producing one WARN per MPI rank")
    require("peak_general_listener_init_attach_policy();" in general,
            "general listener attach must initialize cached attach policy")
    require('opendir("/proc/self/task")' in startup_skip and
            "errno = 0;" in startup_skip and
            "read_errno = errno;" in startup_skip and
            "task_count > 1" in startup_skip and
            "return read_errno == 0 && task_count == 1;" in startup_skip,
            "startup attach stop-skip must fail closed on /proc read errors and be proven by a single-thread task count")
    require("startup_attach_can_skip_stop" in general_listener_attach and
            "!startup_attach_can_skip_stop &&" in general_listener_attach,
            "initial attach must skip the stop backend only after a single-thread proof")
    require("startup_attach_can_skip_stop" in overhead_bootstrap and
            overhead_bootstrap.count(
                "peak_general_listener_startup_attach_can_skip_stop()"
            ) >= 2 and
            overhead_bootstrap.count("!startup_attach_can_skip_stop &&") >= 4,
            "startup overhead calibration attach/detach must independently prove single-thread safety before skipping the stop backend")

    support_attach_supported = extract_function(
        attach_policy,
        "peak_general_listener_support_attach_target_is_supported",
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
    syscall_attach = extract_function(syscall, "syscall_interceptor_attach")
    require(
        "skipping close support hook:" in syscall_attach and
        "peak_log_info(" in syscall_attach and
        "g_printerr(" not in syscall_attach,
        "the expected close/__close_nocancel overlap fallback must remain an "
        "INFO diagnostic instead of producing one WARN per MPI rank",
    )
    require('peak_general_listener_attach_target_is_supported("dlopen"' in
            dlopen_attach,
            "dlopen listener must use normal target prologue policy so dynamic attach is not disabled by support-only early-return guards")
    require('peak_general_listener_support_attach_target_is_supported(' in
            dlopen_attach and
            '"dlclose",' in dlopen_attach,
            "dlclose ownership guard must use the support-hook prologue policy")
    require("startup_attach_can_skip_stop" in dlopen_attach and
            dlopen_attach.count("!startup_attach_can_skip_stop &&") >= 2,
            "startup dlopen listener attach must not start the stop backend after a single-thread proof")
    require("peak_general_listener_attach_target_is_supported" in dlopen_dynamic,
            "dynamic dlopen user targets must use normal target prologue policy")
    require("peak_general_listener_support_attach_target_is_supported" not in dlopen_dynamic,
            "dynamic dlopen user targets must not use support prologue policy")


def check_mpi_startup_helper_warmup(repo_root):
    source = (repo_root / "src/peak.c").read_text(encoding="utf-8")
    controller = (repo_root / "src/detach_controller.c").read_text(encoding="utf-8")
    controller_tests = (
        repo_root / "test/detach_controller/test_detach_controller.c"
    ).read_text(encoding="utf-8")
    controller_tests_cmake = (
        repo_root / "test/detach_controller/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    body = extract_function(source, "peak_init")
    activation = extract_function(source, "peak_activate_runtime")

    check_mpi_position = body.find("found_MPI = check_MPI();")
    configure_position = body.find(
        "peak_detach_controller_configure_mpi_process(found_MPI != 0);"
    )
    pending_return_position = body.find(
        "query_result == MPI_SUCCESS && !initialized"
    )
    pthread_attach_position = activation.find("pthread_listener_attach();")
    warmup_position = activation.find(
        "peak_detach_controller_warmup_backend();"
    )
    require(check_mpi_position != -1 and
            configure_position != -1 and
            pthread_attach_position != -1 and
            warmup_position != -1 and
            pending_return_position != -1,
            "peak_init must keep explicit MPI detection, backend configuration, and helper warmup")
    require(check_mpi_position < configure_position <
            pending_return_position,
            "MPI auto-backend containment must be configured before pending "
            "for MPI_Init completion")
    require(pthread_attach_position < warmup_position,
            "runtime activation must install pthread containment before "
            "non-MPI helper warmup")
    non_mpi_configure_position = body.find(
        "peak_detach_controller_configure_mpi_process(FALSE);"
    )
    require(non_mpi_configure_position != -1 and
            non_mpi_configure_position != -1,
            "non-MPI builds must freeze controller configuration explicitly")
    warmup_context = activation[
        max(0, warmup_position - 180):warmup_position + 80
    ]
    require("!found_MPI" in warmup_context,
            "helper warmup must be suppressed for MPI-linked programs before PMPI_Init")
    require("peak_detach_mpi_process" in controller and
            "auto safe detach using signal backend because MPI runtime is present" in controller,
            "auto mode must select the in-process signal backend for MPI applications")
    require("test_detach_controller_auto_mpi_uses_signal" in controller_tests_cmake and
            "fake-helper-auto-mpi" in controller_tests and
            "MPI auto backend never creates helper log" in controller_tests,
            "MPI auto-backend containment must prove that an available helper is never started")


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

    global_detach_marker = "/* 2) Global detach. */"
    reattach_marker = "/* 3) Reattach. */"
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
    reattach_gum = batch.find("peak_general_listener_gum_attach_target", prepare)
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
    report_model = read_source(
        repo_root, "src/general_listener/report_model.c"
    )
    report_snapshot = read_source(
        repo_root, "src/general_listener/report_snapshot.c"
    )
    helper = extract_function(
        source, "peak_general_listener_exclusive_duration"
    )
    pop = extract_function(source, "peak_general_listener_pop_invocation")
    enter = extract_function(source, "peak_general_listener_on_enter")
    leave = extract_function(source, "peak_general_listener_on_leave")
    output = extract_function(source, "peak_general_listener_write_report")
    sanitize = extract_function(
        report_model, "peak_report_sanitize_times"
    )
    prepare_for_render = extract_function(
        report_snapshot, "peak_report_snapshot_prepare_for_render"
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
    require("exclusive_time[i] < 0.0" in sanitize and
            "exclusive_time[i] = 0.0" in sanitize,
            "output must clamp negative exclusive times after aggregation")
    require("exclusive_time[i] > total_time[i]" in sanitize,
            "output must clamp exclusive time to total time after aggregation")
    require("peak_report_sanitize_times" in prepare_for_render and
            "snapshot->total_time" in prepare_for_render and
            "snapshot->exclusive_time" in prepare_for_render and
            "peak_report_snapshot_prepare_for_render(snapshot)" in output and
            output.find("peak_report_snapshot_prepare_for_render(snapshot)") <
            output.find("peak_report_formatter_write_csv(snapshot)") and
            output.find("peak_report_snapshot_prepare_for_render(snapshot)") <
            output.find("peak_report_formatter_write_text(snapshot, &options)"),
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


def check_runtime_configuration_freeze(repo_root):
    logging = read_source(repo_root, "src/logging.c")
    signal_policy = read_source(repo_root, "src/signal_policy.c")
    controller = read_source(repo_root, "src/detach_controller.c")
    peak = read_source(repo_root, "src/peak.c")
    general = read_source(repo_root, "src/general_listener.c")
    runtime_config = read_source(
        repo_root, "src/general_listener/runtime_config.c"
    )
    dlopen = read_source(repo_root, "src/dlopen_interceptor.c")
    jit = read_source(repo_root, "src/jit_provider.c")
    product_cmake = read_source(repo_root, "src/CMakeLists.txt")
    test_cmake = read_source(repo_root, "test/CMakeLists.txt")
    hotloop_test = read_source(
        repo_root, "test/detach_runtime/test_detach_hotloop.c"
    )

    log_configure_once = extract_function(logging, "peak_log_configure_once")
    log_configure = extract_function(logging, "peak_log_configure")
    log_verbosity = extract_function(logging, "peak_log_verbosity")
    peak_init = extract_function(peak, "peak_init")
    controller_configure = extract_function(
        controller, "peak_detach_controller_configure_mpi_process"
    )
    require("getenv(PEAK_VERBOSITY_ENV)" in log_configure_once and
            "pthread_once(&peak_log_configuration_once" in log_configure and
            "getenv" not in log_verbosity and
            "peak_log_configure();" in peak_init and
            "peak_log_configure();" in controller_configure,
            "logging verbosity must be frozen before controller threads and "
            "later log sites must only consume the immutable cache")

    signal_configure_once = extract_function(
        signal_policy, "peak_signal_policy_configure_once"
    )
    signal_configure = extract_function(
        signal_policy, "peak_signal_policy_configure"
    )
    signal_choose = extract_function(
        signal_policy, "peak_signal_policy_choose_reserved_signal"
    )
    signal_forced = extract_function(
        signal_policy, "peak_signal_policy_env_forces_signal"
    )
    signal_constructor = extract_function(
        signal_policy, "peak_signal_policy_constructor"
    )
    require('getenv("PEAK_DETACH_SIGNAL")' in signal_configure_once and
            "pthread_once(&signal_configuration_once" in signal_configure and
            "getenv" not in signal_choose and
            "getenv" not in signal_forced and
            "peak_signal_policy_configure();" in signal_constructor and
            "peak_signal_policy_configure();" in controller_configure,
            "reserved-signal selection must snapshot its environment before "
            "controller use")

    general_attach = extract_function(general, "peak_general_listener_attach")
    reattach_ready = extract_function(
        general, "peak_general_listener_reattach_cooldown_ready_unlocked"
    )
    retry_exceeded = extract_function(
        general, "peak_general_controller_retry_budget_exceeded_unlocked"
    )
    shutdown_drain = extract_function(
        general, "peak_general_controller_shutdown_drain_ms"
    )
    trace_enabled = extract_function(
        general, "peak_general_controller_trace_enabled"
    )
    general_initializers = (
        "peak_general_listener_runtime_configure();",
        "peak_general_listener_init_reattach_policy();",
        "peak_general_controller_init_retry_limits();",
        "peak_general_controller_init_shutdown_policy();",
        "peak_general_controller_init_trace_config();",
    )
    require(all(initializer in general_attach
                for initializer in general_initializers),
            "general-listener policy must be frozen during attach before the "
            "controller starts")
    for label, body in (
        ("reattach cooldown", reattach_ready),
        ("retry budget", retry_exceeded),
        ("shutdown drain", shutdown_drain),
        ("controller trace gate", trace_enabled),
    ):
        require("getenv" not in body and "g_getenv" not in body,
                f"{label} runtime decision must only read cached configuration")

    runtime_configure = extract_function(
        runtime_config, "peak_general_listener_runtime_configure"
    )
    local_ranks = extract_function(
        runtime_config, "peak_general_listener_local_mpi_ranks"
    )
    require("peak_general_listener_detect_local_mpi_ranks()" in
            runtime_configure and
            "getenv" not in local_ranks and
            "configured_local_mpi_ranks" in local_ranks,
            "heartbeat local-rank policy must be snapshotted before thread "
            "startup")

    dlopen_config_once = extract_function(
        dlopen, "dlopen_interceptor_init_runtime_config_once"
    )
    dlopen_config = extract_function(
        dlopen, "dlopen_interceptor_init_runtime_config"
    )
    dlopen_debug = extract_function(
        dlopen, "dlopen_interceptor_debug_enabled"
    )
    dlopen_selector = extract_function(
        dlopen, "dlopen_interceptor_target_uses_selector_resolution"
    )
    startup_selector = extract_function(
        general, "peak_symbol_should_use_cpp_map"
    )
    dlopen_dynamic_attach = extract_function(
        dlopen, "dlopen_interceptor_attach_from_request"
    )
    dlopen_trace = extract_function(
        dlopen, "dlopen_interceptor_trace_counters"
    )
    dlopen_attach = extract_function(dlopen, "dlopen_interceptor_attach")
    require("PEAK_DLOPEN_TRACE_PATH" in dlopen_config_once and
            "PEAK_DLOPEN_DEBUG" in dlopen_config_once and
            "PEAK_ENABLE_CXX_SYMBOL_SCAN" in dlopen_config_once and
            "configured_cxx_symbol_scan_enabled" in dlopen_config_once and
            "g_once_init_enter(&dlopen_runtime_config_initialized)" in
            dlopen_config and
            "getenv" not in dlopen_debug and
            "g_getenv" not in dlopen_debug and
            "configured_cxx_symbol_scan_enabled" not in dlopen_selector and
            "peak_target_selector_has_top_level_offset" in dlopen_selector and
            'strstr(target, "+0x")' not in dlopen_selector and
            "peak_target_selector_has_top_level_offset" in startup_selector and
            'strstr(symbol, "+0x")' not in startup_selector and
            "getenv" not in dlopen_selector and
            "g_getenv" not in dlopen_selector and
            "dlsym(request->handle" in dlopen_dynamic_attach and
            dlopen_dynamic_attach.index("dlsym(request->handle") <
            dlopen_dynamic_attach.index(
                "selector_resolutions = g_try_new0") and
            "if (needs_selector_resolution)" in dlopen_dynamic_attach and
            "if (selector_resolutions == NULL)" not in
            dlopen_dynamic_attach and
            "peak_target_resolver_module_matches(selector_module" in
            dlopen_dynamic_attach and
            "request->filename" in dlopen_dynamic_attach and
            "configured_cxx_symbol_scan_enabled" in dlopen_dynamic_attach and
            "getenv" not in dlopen_dynamic_attach and
            "g_getenv" not in dlopen_dynamic_attach and
            "getenv" not in dlopen_trace and
            "g_getenv" not in dlopen_trace and
            "dlopen_interceptor_init_runtime_config();" in dlopen_attach,
            "dlopen diagnostics must be frozen before loader/controller "
            "callbacks can consume them")

    jit_config_once = extract_function(
        jit, "peak_jit_init_runtime_config_once"
    )
    jit_config = extract_function(jit, "peak_jit_init_runtime_config")
    jit_trace_path = extract_function(jit, "peak_jit_trace_path")
    jit_retry_timeout = extract_function(
        jit, "peak_jit_not_exec_retry_timeout_ms"
    )
    jit_drain_budget = extract_function(jit, "peak_jit_drain_record_budget")
    jit_provider_enable = extract_function(jit, "peak_jit_provider_enable")
    require("g_getenv" in jit_config_once and
            "g_once_init_enter(&peak_jit_runtime_config_initialized)" in
            jit_config and
            "g_getenv" not in jit_trace_path and
            "g_getenv" not in jit_retry_timeout and
            "g_getenv" not in jit_drain_budget and
            "peak_jit_init_runtime_config();" in jit_provider_enable,
            "JIT controller policy must be snapshotted before provider work")

    require("PEAK_RUNTIME_CONFIG_TEST_MUTABLE_ENV" in test_cmake and
            "PEAK_RUNTIME_CONFIG_TEST_MUTABLE_ENV" not in product_cmake and
            "PEAK_DETACH_CONTROLLER_TEST_REFRESH_HELPER_ENV" not in
            product_cmake,
            "mutable environment refresh hooks must remain isolated from "
            "libpeak product targets")
    replace_helper_env = extract_function(
        controller, "peak_detach_controller_test_replace_helper_env"
    )
    no_trace_pending_age = extract_function(
        hotloop_test, "run_controller_no_trace_pending_age_check"
    )
    require("getenv" not in replace_helper_env and
            "g_getenv" not in replace_helper_env and
            "helper_fd >= 0" in replace_helper_env and
            "helper_pid > 0" in replace_helper_env and
            "held_mutation.active" in replace_helper_env and
            "peak_detach_controller_test_replace_helper_env" in
            no_trace_pending_age and
            'setenv("FAKE_DETACH_HELPER_SCENARIO"' not in
            no_trace_pending_age,
            "the no-trace failure test must update only an explicit stopped-"
            "helper test snapshot, never reread live process environment")


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
    signal_wait_event = extract_function(
        controller, "peak_detach_controller_signal_wait_event"
    )
    signal_wake_waiters = extract_function(
        controller, "peak_detach_controller_signal_wake_release_waiters"
    )
    signal_trap_handler = extract_function(
        controller, "peak_detach_controller_signal_trap_handler"
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
    backend_configuration = extract_function(
        controller, "peak_detach_controller_init_backend_configuration_once"
    )
    helper_configuration = extract_function(
        controller, "peak_detach_controller_init_helper_configuration_once"
    )
    helper_environment = extract_function(
        controller, "peak_detach_controller_snapshot_helper_env"
    )
    helper_path = extract_function(
        controller, "peak_detach_controller_helper_path"
    )
    helper_env = extract_function(
        controller, "peak_detach_controller_helper_env"
    )
    requested_backend = extract_function(
        controller, "peak_detach_controller_requested_backend"
    )
    auto_backend = extract_function(
        controller, "peak_detach_controller_auto_should_use_signal_backend"
    )
    configure_mpi = extract_function(
        controller, "peak_detach_controller_configure_mpi_process"
    )
    warmup_backend = extract_function(
        controller, "peak_detach_controller_warmup_backend"
    )
    prepare_mutation = extract_function(
        controller, "peak_detach_controller_prepare_hook_mutation"
    )
    prepare_batch = extract_function(
        controller, "peak_detach_controller_prepare_hook_mutation_batch"
    )
    wait_for_mutation = extract_function(
        controller, "peak_detach_controller_wait_for_mutation_window"
    )
    test_gate_delay = extract_function(
        controller, "peak_detach_controller_test_delay_after_gate_begin"
    )

    require("PEAK_SAFE_DETACH_MODE_STRICT" in controller_mode and
            "getenv" not in controller_mode and
            '"compatibility"' not in controller_mode and
            '"legacy"' not in controller_mode,
            "strict-auto must be the only runtime-selected detach mode and must not reread its environment")
    require('g_getenv("PEAK_DETACH_BACKEND")' in backend_configuration and
            'g_getenv("PEAK_SAFE_DETACH_MODE")' in backend_configuration and
            'g_getenv("PEAK_TEST_PTRACE_SCOPE")' in backend_configuration and
            'fopen("/proc/sys/kernel/yama/ptrace_scope", "r")'
            in backend_configuration and
            "strtol(scope_value, &end, 10)" in backend_configuration,
            "detach backend and ptrace policy must be parsed by the initialization cache")
    require("getenv" not in test_gate_delay and
            "strtoul" not in test_gate_delay and
            "configured_test_gate_delay_us" in test_gate_delay,
            "test-only mutation delay must consume initialization-time configuration without rereading the environment")
    for name, body in (
        ("requested backend", requested_backend),
        ("auto backend", auto_backend),
        ("helper path", helper_path),
        ("helper environment", helper_env),
    ):
        require("getenv" not in body and
                "fopen" not in body and
                "strtol" not in body,
                f"{name} mutation decision must only read cached configuration")
    require('g_getenv("PEAK_DETACH_HELPER")' in helper_configuration and
            "peak_detach_controller_snapshot_helper_env()" in
            helper_configuration and
            "while (environ[count] != NULL)" in helper_environment,
            "helper path and sanitized environment must be snapshotted during initialization")
    require("peak_detach_controller_cache_backend_configuration();"
            in configure_mpi and
            "peak_detach_controller_cache_backend_configuration();"
            in warmup_backend and
            "peak_detach_controller_cache_helper_configuration();"
            in configure_mpi and
            "peak_detach_controller_cache_helper_configuration();"
            in warmup_backend and
            "peak_detach_controller_init_strict_gate_wait_timeout();"
            in configure_mpi and
            "peak_detach_controller_init_strict_gate_wait_timeout();"
            in warmup_backend,
            "PEAK initialization entry points must cache all environment-derived controller policy before runtime mutations")
    require("pthread_once(&backend_configuration_initialized" in controller and
            "pthread_once(&helper_configuration_initialized" in controller and
            "peak_detach_controller_cache_backend_configuration" not in prepare_mutation and
            "peak_detach_controller_cache_backend_configuration" not in prepare_batch and
            "peak_detach_controller_cache_helper_configuration" not in prepare_mutation and
            "peak_detach_controller_cache_helper_configuration" not in prepare_batch and
            "peak_detach_controller_init_strict_gate_wait_timeout" not in
            wait_for_mutation,
            "controller mutations must consume the one-time initialization cache without lazy configuration parsing")
    controller_tests_cmake = read_source(
        repo_root, "test/detach_controller/CMakeLists.txt"
    )
    product_cmake = read_source(repo_root, "src/CMakeLists.txt")
    require("PEAK_DETACH_CONTROLLER_TEST_REFRESH_HELPER_ENV" in
            controller_tests_cmake and
            "PEAK_DETACH_CONTROLLER_TEST_REFRESH_HELPER_ENV" not in
            product_cmake and
            "#ifdef PEAK_DETACH_CONTROLLER_TEST_REFRESH_HELPER_ENV" in
            controller and
            "#ifdef PEAK_ENABLE_TEST_HOOKS\nstatic void\npeak_detach_controller_free_helper_env_snapshot"
            not in controller,
            "helper-environment refresh must remain isolated to standalone controller test executables, never libpeak test builds")
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
            ".tv_sec = 1" in signal_wait_event and
            ".tv_nsec = 0" in signal_wait_event and
            "peak_exec_raw_syscall6" in signal_wait_event and
            "syscall(" not in signal_wait_event and
            "peak_exec_raw_syscall6" in signal_wake_waiters and
            "syscall(" not in signal_wake_waiters,
            "Linux signal waits must use raw, bounded private event futex waits and wake-all")
    require("_Static_assert(sizeof(signal_wait_sequence) == sizeof(int)" in controller and
            "_Static_assert(sizeof(int) * CHAR_BIT == 32" in controller and
            "_Static_assert(_Alignof(_Atomic int) == _Alignof(int)" in controller and
            "_Static_assert(ATOMIC_INT_LOCK_FREE == 2" in controller and
            "explicit supported Linux compiler ABI" in controller and
            "not a universal C11 representation proof" in controller and
            "peak_detach_controller_signal_wait_futex_word()" in controller and
            "(int*)&signal_wait_sequence" not in controller,
            "Linux futex waits must enforce the supported compiler ABI contract for an always-lock-free, aligned 32-bit word")
    require("expected_sequence" in signal_wait_for_release and
            "expected_release_epoch" in signal_wait_for_release and
            "peak_detach_controller_signal_wait_event(expected_sequence)" in
            signal_wait_for_release and
            signal_wait_for_release.find("expected_sequence") <
            signal_wait_for_release.find("expected_release_epoch") and
            signal_wait_for_release.find("evacuate_epoch") <
            signal_wait_for_release.find(
                "peak_detach_controller_signal_wait_event"),
            "signal wait must snapshot the event sequence before checking release and evacuation predicates")
    require(signal_wake_waiters.find(
                "atomic_fetch_add_explicit(&signal_wait_sequence") <
            signal_wake_waiters.find("FUTEX_WAKE | FUTEX_PRIVATE_FLAG"),
            "signal wake must advance the futex event sequence before wake-all")
    require("peak_exec_raw_syscall6(SYS_gettid" in signal_handler and
            "syscall(" not in signal_handler and
            "peak_exec_raw_syscall6(SYS_gettid" in signal_trap_handler and
            "syscall(" not in signal_trap_handler,
            "signal handlers must bypass the interposed libc syscall symbol")
    require("test_detach_controller_signal_event_futex" in
            controller_tests_cmake and
            "signal-event-futex" in controller_tests and
            "signal event waiter remains parked without polling" in
            controller_tests and
            "signal event waiter performs one event-driven wait" in
            controller_tests and
            "peak_detach_controller_test_signal_wait_count()" in
            controller_tests and
            "peak_detach_controller_test_wake_signal_waiters();" in
            controller_tests and
            "peak_detach_controller_test_wait_for_signal_event(stale_sequence)"
            in controller_tests,
            "signal event futex must test both event-driven parking and wake-before-wait")
    require("--min-overhead-ratio 0.04" not in tests and
            tests.count("--min-final-detached-targets 832") >= 2 and
            tests.count("--min-final-reattached-targets 832") >= 2 and
            tests.count("--min-final-revisited-targets 832") >= 2 and
            tests.count("--min-weighted-call-coverage 0.001") >= 2 and
            tests.count("--min-phase-target-breadth 1.0") >= 2,
            "long acceptance must cap slowdown while proving explicit target coverage instead of requiring overhead")
    require(signal_release.find("atomic_store_explicit(&signal_release_epoch, epoch") <
            signal_release.find("peak_detach_controller_signal_wake_release_waiters"),
            "release publication must wake signal waiters")
    require(signal_clear_slots.find("atomic_store_explicit(&signal_release_epoch, 0") <
            signal_clear_slots.find("peak_detach_controller_signal_wake_release_waiters"),
            "release reset must wake signal waiters")
    require(signal_temp_breakpoint.find("atomic_store_explicit(&slot->evacuate_epoch, epoch") <
            signal_temp_breakpoint.find("peak_detach_controller_signal_wake_release_waiters"),
            "evacuation publication must wake signal waiters")
    require(controller.count("peak_detach_controller_signal_wake_release_waiters()") == 4 and
            "peak_detach_controller_test_wake_signal_waiters" in controller,
            "production signal futex wake-all must occur only after release, reset, and evacuation publications")
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
    check_darwin_strict_lifecycle(repo_root)
    check_safe_pc_alignment(repo_root)
    check_support_hook_lifetimes(repo_root)
    check_dlopen_detach_transaction(repo_root)
    check_dlopen_resolution_lock_order(repo_root)
    check_dlopen_fftw_scope_and_fork_guard(repo_root)
    check_safe_arm64_plt_reads_and_close_overlap_guard(repo_root)
    check_x86_patched_gum_requires_exact_attach(repo_root)
    check_fast_listener_unwind_abi(repo_root)
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
    check_runtime_configuration_freeze(repo_root)
    check_shutdown_fail_closed_docs(repo_root)
    check_signal_backend_strict_invariants(repo_root)
    print("detach_lifecycle_invariants_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
