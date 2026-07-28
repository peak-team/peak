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
    for name, body in (("enter", enter), ("leave", leave)):
        for token in forbidden:
            require(
                token not in body,
                f"{name} callback contains forbidden steady-state operation {token}",
            )

    require(
        "pthread_listener_lookup_thread" in initialize
        and "thread_data.initialized" in initialize
        and 'tls_model("initial-exec")' in source,
        "thread slot lookup must be cached by one-time TLS initialization",
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
        "_Atomic gboolean detach_count_request_pending" in header
        and "atomic_compare_exchange_strong_explicit" in generic_enter
        and "atomic_compare_exchange_strong_explicit" in enter
        and "peak_general_listener_controller_wake" in generic_enter
        and "peak_general_listener_controller_wake" in enter,
        "generic and fast threshold-crossing callbacks must publish the "
        "detach-count CAS latch and issue the lock-free controller wake",
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
        "atomic_exchange_explicit" in publish
        and "peak_general_listener_request_detach_with_context_unlocked" in publish,
        "controller must consume the CAS latch under its existing request path",
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
    require(
        "current_num_calls == peak_detach_count" in enter,
        "only the detach-threshold crossing call may publish a request",
    )

    print("general_listener_hotpath_ok")


if __name__ == "__main__":
    main()
