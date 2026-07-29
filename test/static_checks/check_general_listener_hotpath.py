#!/usr/bin/env python3
"""Guard the target callback against steady-state blocking operations."""

from pathlib import Path
import sys


def function_body(source, name):
    marker = f"\n{name}("
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing function {name}")
    brace = source.find("{", start)
    declaration_end = source.find(";", start)
    # Some hot-path helpers need a forward declaration.  Do not mistake the
    # next unrelated function body for the declaration's body.
    if declaration_end >= 0 and declaration_end < brace:
        start = source.find(marker, declaration_end)
        if start < 0:
            raise AssertionError(f"missing definition for {name}")
        brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body for {name}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body for {name}")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_general_listener_hotpath.py <source-root>")

    root = Path(sys.argv[1])
    source = (root / "src/general_listener.c").read_text()
    header = (root / "include/general_listener.h").read_text()
    generic_enter = function_body(source, "peak_general_listener_on_enter")
    enter = function_body(source, "peak_general_listener_fast_on_enter")
    leave = function_body(source, "peak_general_listener_fast_on_leave")
    initialize = function_body(
        source, "peak_general_listener_thread_state_initialize"
    )
    listener_init = function_body(source, "peak_general_listener_init")
    listener_ready = function_body(source, "peak_general_listener_is_ready")
    push = function_body(source, "peak_general_listener_push_invocation")
    increment = function_body(
        source, "peak_general_listener_num_calls_increment"
    )
    threshold_publish = function_body(
        source, "peak_general_listener_try_publish_detach_count_request"
    )
    threshold_publish_snapshot = function_body(
        source,
        "peak_general_listener_try_publish_detach_count_request_snapshot",
    )
    threshold_initial_snapshot = function_body(
        source,
        "peak_general_listener_try_publish_initial_detach_count_crossing_snapshot",
    )
    threshold_adjacent_generation = function_body(
        source,
        "peak_general_listener_first_attached_generation_is_adjacent",
    )
    threshold_take = function_body(
        source, "peak_general_listener_take_detach_count_request"
    )
    state_publish = function_body(
        source, "peak_general_controller_set_state_unlocked"
    )
    callback_state_publish = function_body(
        source, "peak_general_listener_publish_callback_hook_state"
    )
    threshold_configure = function_body(
        source, "peak_general_listener_configure_detach_threshold"
    )
    attach = function_body(source, "peak_general_listener_attach")
    controller_wake = function_body(
        source, "peak_general_listener_controller_wake"
    )
    controller_wait = function_body(
        source, "peak_general_listener_controller_wait"
    )
    controller_main = function_body(
        source, "peak_general_controller_thread_main"
    )
    controller_stop = function_body(
        source, "peak_general_listener_controller_stop"
    )
    publish = function_body(
        source, "peak_general_controller_publish_detach_count_requests_unlocked"
    )
    retire_slot = function_body(
        source, "peak_general_listener_retire_listener_slot"
    )
    total_calls = function_body(source, "peak_general_listener_total_calls")
    local_report = function_body(
        source, "peak_general_listener_print_with_mpi_job_policy"
    )
    test_call_count = function_body(
        source, "peak_general_listener_test_call_count"
    )

    forbidden = (
        "pthread_listener_lookup_thread",
        "pthread_mutex_lock",
        "pthread_pause_enable",
        "pthread_pause_disable",
        "pthread_sigmask",
        "gum_interceptor_ignore_current_thread",
        "gum_interceptor_unignore_current_thread",
        "g_object_is_floating",
        "g_mutex_lock",
        "g_new",
        "g_renew",
        "g_free",
    )
    for name, body in (
        ("enter", enter),
        ("leave", leave),
        ("threshold publish", threshold_publish),
        ("threshold snapshot publish", threshold_publish_snapshot),
        ("threshold initial snapshot", threshold_initial_snapshot),
    ):
        for token in forbidden:
            require(
                token not in body,
                f"{name} callback contains forbidden steady-state operation {token}",
            )

    require(
        "pthread_listener_current_thread_slot" in initialize
        and "pthread_listener_lookup_thread" not in initialize
        and "thread_data.initialized" in initialize
        and 'tls_model("initial-exec")' in source,
        "thread slot identity must be read from one-time initial-exec TLS initialization",
    )
    require(
        "entries_inline[16]" in source
        and "thread_data.entries_inline" in source
        and "peak_general_listener_grow_invocation_stack" in push,
        "child-time stack must use inline depth 16 and grow only on overflow",
    )
    require(
        "PEAK_GENERAL_LISTENER_CACHE_LINE_SIZE 64" in source
        and "guint8* fast_stats = (guint8*)self->fast_active" in source
        and "self->num_calls = (gulong*)(fast_stats + 8)" in source
        and "self->total_time = (gdouble*)(fast_stats + 16)" in source
        and "self->exclusive_time = (gdouble*)(fast_stats + 24)" in source
        and "self->max_time = (gfloat*)(fast_stats + 32)" in source
        and "self->min_time = (gfloat*)(fast_stats + 36)" in source
        and "peak_general_listener_num_calls_slot(self, index)" in enter,
        "per-thread accounting must remain isolated and coalesced by cache line",
    )
    require(
        "peak_general_listener_map_zeroed(" in listener_init
        and "MAP_PRIVATE | MAP_ANONYMOUS" in source
        and "atomic_init(peak_general_listener_fast_active_slot" in listener_init
        and "fast_stats_capacity" in listener_init
        and "fast_stats_mapping_size ==" in listener_ready
        and "self->num_calls ==" in listener_ready
        and "self->min_time ==" in listener_ready
        and "munmap(self->fast_active, self->fast_stats_mapping_size)" in source
        and "peak_general_listener_is_ready" in source
        and "g_aligned_alloc0(" not in listener_init,
        "packed listener statistics must use persistent anonymous mappings "
        "with guarded publication and matching teardown",
    )
    require(
        "__atomic_add_fetch" not in increment
        and "__atomic_fetch_add" not in increment
        and "__atomic_load_n" in increment
        and "__atomic_store_n" in increment,
        "single-writer call slots must not use a locked atomic RMW",
    )
    require(
        "_Atomic unsigned long long callback_hook_control" in header
        and "peak_general_listener_try_publish_detach_count_request"
        in generic_enter
        and "peak_general_listener_try_publish_detach_count_request" in enter
        and "gulong current_num_calls = peak_general_listener_num_calls_increment"
        in generic_enter
        and "gulong current_num_calls = peak_general_listener_num_calls_increment"
        in enter
        and "self,\n            current_num_calls" in generic_enter
        and "self,\n            current_num_calls" in enter
        and "detach_count_total" not in source
        and "peak_general_listener_controller_wake" in generic_enter
        and "peak_general_listener_controller_wake" in enter,
        "generic and fast threshold-crossing callbacks must publish their "
        "own slot's exact count through the detach-count CAS latch and issue "
        "the lock-free controller wake",
    )
    require(
        "current_num_calls != peak_detach_count" in threshold_publish
        and "callback_hook_control" in threshold_publish
        and "peak_general_listener_try_publish_detach_count_request_snapshot"
        in threshold_publish
        and "PEAK_HOOK_UNRESOLVED" in threshold_publish
        and "peak_general_listener_try_publish_initial_detach_count_crossing_snapshot"
        in threshold_publish
        and "unresolved_control" in threshold_publish
        and "peak_general_listener_first_attached_generation_is_adjacent"
        in threshold_publish
        and threshold_publish.count(
            "peak_general_listener_try_publish_detach_count_request_snapshot"
        ) == 2
        and "PEAK_HOOK_ATTACHED" in threshold_publish_snapshot
        and "PEAK_CALLBACK_DETACH_COUNT_REQUEST_BIT"
        in threshold_publish_snapshot
        and "atomic_compare_exchange_strong_explicit"
        in threshold_publish_snapshot
        and "PEAK_CALLBACK_INITIAL_DETACH_COUNT_CROSSING_BIT"
        in threshold_initial_snapshot
        and "atomic_compare_exchange_strong_explicit"
        in threshold_initial_snapshot,
        "the exact threshold crosser must publish through the packed "
        "ATTACHED request or first-activation crossing CAS",
    )
    require(
        "peak_general_listener_add_calls_saturating" not in source
        and "listener->retired_num_calls += calls" in retire_slot
        and "calls += peak_general_listener_num_calls_load" in total_calls
        and "total_num_calls += peak_general_listener_num_calls_load" in publish
        and "sum_num_calls[i] += calls" in local_report
        and "total += calls" in test_call_count,
        "call totals must preserve origin's unsigned modular aggregation in "
        "retirement, controller, heartbeat, report, and test snapshots",
    )
    require(
        "PEAK_HOOK_UNRESOLVED" in threshold_adjacent_generation
        and "PEAK_HOOK_ATTACHED" in threshold_adjacent_generation
        and "PEAK_CALLBACK_HOOK_CONTROL_LOW_MASK"
        in threshold_adjacent_generation
        and "PEAK_CALLBACK_HOOK_GENERATION_INCREMENT"
        in threshold_adjacent_generation,
        "a failed initial-crossing CAS may retry only against the immediately "
        "adjacent first ATTACHED generation",
    )
    require(
        "callback_hook_control" in threshold_take
        and "PEAK_HOOK_ATTACHED" in threshold_take
        and "PEAK_CALLBACK_DETACH_COUNT_REQUEST_BIT" in threshold_take
        and "atomic_compare_exchange_strong_explicit" in threshold_take
        and "peak_general_listener_publish_callback_hook_state"
        in state_publish
        and "PEAK_CALLBACK_HOOK_GENERATION_INCREMENT"
        in callback_state_publish
        and "PEAK_CALLBACK_HOOK_CONTROL_LOW_MASK"
        in callback_state_publish
        and "PEAK_CALLBACK_INITIAL_DETACH_COUNT_CROSSING_BIT"
        in callback_state_publish
        and "PEAK_HOOK_UNRESOLVED" in callback_state_publish
        and "PEAK_HOOK_ATTACHED" in callback_state_publish
        and "PEAK_CALLBACK_DETACH_COUNT_REQUEST_BIT"
        in callback_state_publish
        and "atomic_compare_exchange_weak_explicit"
        in callback_state_publish
        and "detach_count_request_published" in state_publish
        and "peak_general_listener_controller_wake" in state_publish
        and "_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2" in source
        and "callback_hook_state" not in header
        and "detach_count_request_pending" not in header,
        "callback state, initial crossing, and request must share an "
        "always-lock-free generation-tagged atomic word",
    )
    for token in (
        "pthread_mutex",
        "pthread_cond",
        "pthread_sigmask",
        "g_mutex",
        "g_new",
        "g_renew",
        "g_free",
        "malloc",
        "calloc",
    ):
        require(
            token not in controller_wake,
            f"threshold controller wake contains forbidden operation {token}",
        )
    require(
        "atomic_fetch_add_explicit" in controller_wake
        and "memory_order_release" in controller_wake
        and "peak_exec_raw_syscall6" in controller_wake
        and "FUTEX_WAKE | FUTEX_PRIVATE_FLAG" in controller_wake
        and "saved_errno = errno" in controller_wake
        and "errno = saved_errno" in controller_wake,
        "threshold crossing must use a release sequence and a private raw "
        "futex wake without changing callback errno",
    )
    require(
        "FUTEX_WAIT | FUTEX_PRIVATE_FLAG" in controller_wait
        and "expected_wake_sequence" in controller_wait
        and ".tv_nsec = 10000000L" in controller_wait,
        "controller wait must use the pre-scan sequence with a bounded "
        "private futex wait",
    )
    sequence_snapshot = controller_main.find(
        "atomic_load_explicit(&general_controller_wake_sequence"
    )
    process_pending = controller_main.find(
        "peak_general_controller_process_pending_unlocked"
    )
    wait_for_wake = controller_main.find(
        "peak_general_listener_controller_wait(expected_wake_sequence)"
    )
    require(
        sequence_snapshot != -1
        and process_pending != -1
        and wait_for_wake != -1
        and sequence_snapshot < process_pending < wait_for_wake,
        "controller must snapshot the wake sequence before scanning requests "
        "and wait on that exact older value",
    )
    stop_running = controller_stop.find(
        "general_controller_running = FALSE"
    )
    stop_unlock = controller_stop.find(
        "pthread_mutex_unlock(&general_controller_wake_mutex)",
        stop_running,
    )
    stop_wake = controller_stop.find(
        "peak_general_listener_controller_wake()",
        stop_unlock,
    )
    require(
        stop_running != -1
        and stop_unlock != -1
        and stop_wake != -1
        and stop_running < stop_unlock < stop_wake,
        "controller stop must publish running=false before its lock-free wake",
    )
    require(
        "peak_general_listener_take_detach_count_request" in publish
        and "peak_general_listener_request_detach_with_context_unlocked" in publish,
        "controller must consume the CAS latch under its existing request path",
    )
    configure_call = attach.find(
        "peak_general_listener_configure_detach_threshold()"
    )
    first_target_attach = attach.find(
        "for (size_t i = 0; i < peak_hook_address_count"
    )
    require(
        "peak_general_listener_parse_detach_count_override" in threshold_configure
        and "peak_general_overhead_bootstrapping" in threshold_configure
        and configure_call != -1
        and first_target_attach != -1
        and configure_call < first_target_attach,
        "detach threshold must be frozen before any target hook is activated",
    )
    require(
        "peak_general_listener_catch_up_initial_detach_count_unlocked"
        not in source
        and "callback_mark_physically_attached" not in source,
        "first-activation crossing must remain in the packed CAS domain "
        "without early ATTACHED publication or a racy counter scan",
    )
    require(
        "thread_data.self_mapped_id" in enter
        and "thread_data.self_mapped_id" in leave,
        "both callbacks must use the cached TLS slot",
    )
    require(
        "thread_data.self_mapped_valid" in enter
        and "return GUM_PEAK_FAST_ENTER_SKIP" in enter,
        "threads without a unique accounting slot must skip fast sampling",
    )
    require(
        "PEAK_LISTENER_FAST_DISPATCH_SECTION" in source
        and enter.find("peak_general_listener_fast_active_add") <
        enter.find("peak_general_listener_fast_reap_unwound")
        and leave.rfind("peak_general_listener_fast_active_remove") >
        leave.find("peak_general_listener_checkpoint_shadow_update"),
        "active lifetime must cover every helper outside the classified dispatch",
    )
    print("general_listener_hotpath_ok")


if __name__ == "__main__":
    main()
