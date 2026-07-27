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
        and "atomic_compare_exchange_strong_explicit" in enter,
        "callback must publish the detach-count request through a CAS latch",
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
