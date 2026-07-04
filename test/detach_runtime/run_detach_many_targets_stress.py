#!/usr/bin/env python3
"""Manual many-target strict-detach stress runner.

This runner is intentionally opt-in.  It is designed for HPC/manual repro work
where we want many hook targets, a hot subset, many application threads, and
heartbeat-driven detach/reattach pressure without launching a large MPI
application.
"""

import argparse
import copy
import csv
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path


BAD_OUTPUT_RE = re.compile(
    r"fatal|not proven safe|leaving listener state alive|"
    r"detach helper shutdown failed|Gum .* failed|"
    r"tracked thread snapshot exceeded|thread id .* exceeds|"
    r"timed out draining pending target hook detach/reattach requests|"
    r"Segmentation fault|SIGSEGV|SIGABRT|SIGBUS|SIGILL",
    re.IGNORECASE,
)
OK_RE = re.compile(r"\bmany_targets_stress_ok\b")
CALLS_PER_SEC_RE = re.compile(r"\bcalls_per_sec=([0-9.]+)")
ELAPSED_RE = re.compile(r"\belapsed=([0-9.]+)")
TOTAL_CALLS_RE = re.compile(r"\btotal_calls=([0-9]+)")
QUOTA_COMPLETED_RE = re.compile(r"\bquota_completed=([01])")
PHASE_HOT_TARGET0_CALLS_RE = re.compile(r"\bphase_hot_target0_calls=([0-9]+)")
PHASE_COLD_TARGET0_CALLS_RE = re.compile(r"\bphase_cold_target0_calls=([0-9]+)")
ROTATING_HOT_CALLS_RE = re.compile(r"\brotating_hot_calls=([0-9]+)")
COLD_SWEEP_CALLS_RE = re.compile(r"\bcold_sweep_calls=([0-9]+)")
ALL_TARGET_CALLS_RE = re.compile(r"\ball_target_calls=([0-9]+)")
DISTINCT_CALLED_TARGETS_RE = re.compile(r"\bdistinct_called_targets=([0-9]+)")
DISTINCT_CALLED_HOT_TARGETS_RE = re.compile(
    r"\bdistinct_called_hot_targets=([0-9]+)"
)
DISTINCT_CALLED_COLD_TARGETS_RE = re.compile(
    r"\bdistinct_called_cold_targets=([0-9]+)"
)
FIXTURE_MAX_TARGETS = 1024
STRESS_SYMBOL_RE = re.compile(r"^peak_detach_stress_target_([0-9]+)$")
PEAK_TARGET0_COUNT_RE = re.compile(
    r"\|peak_detach_stress_target_0\*{0,2}\|\s*([0-9]+)\|"
)


def decode_timeout_output(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def target_name(index):
    return f"peak_detach_stress_target_{index}"


def format_counter(counter, limit=6):
    if not counter:
        return "none"
    parts = []
    for key, value in counter.most_common(limit):
        parts.append(f"{key}:{value}")
    return "|".join(parts)


def format_cycle_examples(cycles, limit=3):
    if not cycles:
        return "none"
    parts = []
    for cycle in cycles[:limit]:
        item = (
            "{hook}:d1={detach_time:.6f};r={reattach_time:.6f};"
            "d2={second_detach_time:.6f};closeout_span={closeout_span:.6f};"
            "call_delta={call_delta}".format(
                hook=cycle["symbol"],
                detach_time=cycle["detach_time"],
                reattach_time=cycle["reattach_time"],
                second_detach_time=cycle["second_detach_time"],
                closeout_span=cycle.get("closeout_span_s", 0.0),
                call_delta=cycle["call_delta"],
            )
        )
        if "sample_delta" in cycle:
            item += (
                ";sample={sample_start_time:.6f}->{sample_end_time:.6f}"
                ";sample_abs={sample_start_abs:.6f}->{sample_end_abs:.6f}"
                ";sample_delta={sample_delta}".format(
                sample_start_time=cycle.get("sample_start_time", 0.0),
                sample_end_time=cycle.get("sample_end_time", 0.0),
                sample_start_abs=cycle.get("sample_start_abs", 0.0),
                sample_end_abs=cycle.get("sample_end_abs", 0.0),
                sample_delta=cycle.get("sample_delta", 0),
            )
            )
        parts.append(item)
    return "|".join(parts)


def parse_float(value, fallback=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def parse_int(value, fallback=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def parse_trace_detail(detail):
    values = {}
    if not detail:
        return values
    for item in detail.split(";"):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        values[key] = value
    return values


def global_heartbeat_success_count(trace, operation):
    counts = trace["success_operation_source_counts"]
    return (
        counts.get(f"{operation}:global-heartbeat", 0)
        + counts.get(f"{operation}:global-overhead-recovery", 0)
    )


def global_heartbeat_counter_count(counter):
    return (
        counter.get("global-heartbeat", 0)
        + counter.get("global-overhead-recovery", 0)
    )


def is_global_heartbeat_source(source):
    return source in ("global-heartbeat", "global-overhead-recovery")


def write_target_file(path, target_count):
    with open(path, "w", encoding="utf-8") as handle:
        for index in range(target_count):
            handle.write(f"{target_name(index)}\n")


def make_env(args, work_dir):
    env = os.environ.copy()
    target_file = work_dir / "many-targets.txt"
    trace_path = work_dir / "detach-trace.csv"
    stats_prefix = work_dir / "peak-stats"
    max_threads = max(args.peak_max_threads, args.threads + args.thread_slack)

    write_target_file(target_file, args.targets)
    if args.disable_peak:
        return env

    env.update(
        {
            "LD_PRELOAD": str(args.libpeak),
            "PEAK_TARGET_FILE": str(target_file),
            "PEAK_MAX_NUM_THREADS": str(max_threads),
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": (
                "1" if args.per_target_heartbeat else "0"
            ),
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": (
                "1" if args.global_heartbeat else "0"
            ),
            "PEAK_ENABLE_REATTACH": "1" if args.enable_reattach else "0",
            "PEAK_COST": args.peak_cost,
            "PEAK_OVERHEAD_RATIO": args.overhead_ratio,
            "PEAK_GLOBAL_OVERHEAD_RATIO": args.global_overhead_ratio,
            "PEAK_GLOBAL_DETACH_FACTOR": args.global_detach_factor,
            "PEAK_GLOBAL_REATTACH_FACTOR": args.global_reattach_factor,
            "PEAK_HEARTBEAT_INTERVAL": args.heartbeat_interval,
            "PEAK_HIBERNATION_CYCLE": str(args.hibernation_cycle),
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": (
                "signal" if args.backend == "signal" else "strict"
            ),
            "PEAK_DETACH_BACKEND": args.backend,
            "PEAK_DETACH_TRACE_PATH": str(trace_path),
            "PEAK_STATSLOG_PATH": str(stats_prefix),
        }
    )
    if args.backend == "signal":
        env["PEAK_DETACH_HELPER"] = "/no/such/peak_detach_helper"
    elif args.helper:
        env["PEAK_DETACH_HELPER"] = str(args.helper)
    if args.detach_count is not None:
        env["PEAK_DETACH_COUNT"] = str(args.detach_count)
    for item in args.extra_env:
        key, value = item.split("=", 1)
        env[key] = value
    return env


def parse_trace(path, hot_targets, total_targets=None):
    result = {
        "rows": 0,
        "detach_success": 0,
        "reattach_success": 0,
        "classify_failed": 0,
        "prepare_failed": 0,
        "gum_failed": 0,
        "retry_abandoned": 0,
        "max_batch": 0,
        "max_stop_window_us": 0.0,
        "distinct_mutated_hooks": 0,
        "distinct_detached_hooks": 0,
        "distinct_reattached_hooks": 0,
        "distinct_batches": 0,
        "first_timestamp": 0.0,
        "last_timestamp": 0.0,
        "trace_duration_s": 0.0,
        "total_unique_stop_window_us": 0.0,
        "full_batches": 0,
        "same_hook_oscillations": 0,
        "global_same_hook_oscillations": 0,
        "distinct_oscillating_hooks": 0,
        "distinct_global_oscillating_hooks": 0,
        "max_hook_oscillations": 0,
        "detached_hot_hooks": 0,
        "detached_cold_hooks": 0,
        "reattached_hot_hooks": 0,
        "reattached_cold_hooks": 0,
        "source_counts": Counter(),
        "operation_source_counts": Counter(),
        "success_operation_source_counts": Counter(),
        "symbol_operation_counts": Counter(),
        "oscillation_by_hook": Counter(),
        "global_oscillation_by_hook": Counter(),
        "transition_by_hook": Counter(),
        "global_transition_by_hook": Counter(),
        "target0_max_detach_request_calls": 0,
        "target0_reattach_success": 0,
        "distinct_oscillating_hot_hooks": 0,
        "distinct_global_oscillating_hot_hooks": 0,
        "reattach_sampled_cycles": 0,
        "global_reattach_sampled_cycles": 0,
        "distinct_reattach_sampled_hooks": 0,
        "distinct_reattach_sampled_hot_hooks": 0,
        "distinct_global_reattach_sampled_hooks": 0,
        "distinct_global_reattach_sampled_hot_hooks": 0,
        "probe_detached_dwell_count": 0,
        "probe_detached_dwell_min_s": 0.0,
        "probe_detached_dwell_p50_s": 0.0,
        "probe_detached_dwell_p95_s": 0.0,
        "global_hot_probe_detached_dwell_count": 0,
        "global_hot_probe_detached_dwell_min_s": 0.0,
        "probe_dwell_update_count": 0,
        "probe_dwell_invalid_count": 0,
        "probe_dwell_hot_increase_count": 0,
        "probe_dwell_hot_increase_hot_hooks": 0,
        "probe_dwell_hot_increase_global_hot_hooks": 0,
        "probe_closeout_keep_attached": 0,
        "probe_closeout_hot": 0,
        "probe_closeout_invalid": 0,
        "probe_closeout_reserved": 0,
        "probe_closeout_reserved_wait": 0,
        "reattach_sampled_call_delta_total": 0,
        "reattach_sampled_call_delta_max": 0,
        "trace_ordered_reattach_cycles": 0,
        "trace_ordered_reattach_distinct_hooks": 0,
        "trace_ordered_reattach_hot_cycles": 0,
        "trace_ordered_reattach_distinct_hot_hooks": 0,
        "trace_ordered_global_hot_reattach_cycles": 0,
        "trace_ordered_global_hot_reattach_distinct_hooks": 0,
        "trace_ordered_reattach_call_delta_total": 0,
        "trace_ordered_reattach_call_delta_max": 0,
        "trace_ordered_reattach_cycle_examples": [],
        "strict_reattach_ordered_cycles": 0,
        "strict_reattach_ordered_distinct_hooks": 0,
        "strict_reattach_ordered_call_delta_total": 0,
        "strict_reattach_ordered_call_delta_max": 0,
        "strict_reattach_ordered_hot_cycles": 0,
        "strict_reattach_ordered_distinct_hot_hooks": 0,
        "strict_global_reattach_ordered_hot_cycles": 0,
        "strict_global_reattach_ordered_distinct_hot_hooks": 0,
        "strict_reattach_ordered_sample_delta_total": 0,
        "strict_reattach_ordered_sample_delta_max": 0,
        "strict_reattach_ordered_sample_span_s": 0.0,
        "strict_reattach_ordered_sample_bins": 0,
        "strict_reattach_ordered_cycle_examples": [],
        "reattach_sampled_span_s": 0.0,
        "heartbeat_liveness": [],
        "median_oscillations_per_oscillating_hook": 0.0,
        "top_hook_transition_share": 0.0,
        "top_global_hook_transition_share": 0.0,
        "shutdown_success": 0,
        "shutdown_distinct_hooks": 0,
        "shutdown_duplicate_hooks": 0,
        "shutdown_out_of_range_hooks": 0,
        "shutdown_missing_hooks": 0,
        "signal_rows": 0,
        "signal_bad": 0,
        "signal_timeout": 0,
        "signal_arrival_incomplete": 0,
        "signal_release_incomplete": 0,
        "backend_rows": 0,
        "backend_failed": 0,
        "backend_timeout": 0,
        "global_no_admission": 0,
        "global_warm_wait": 0,
        "pacing_blocked": 0,
        "first_global_detach_batch_id": 0,
        "first_global_detach_batch_size": 0,
        "first_global_detach_batch_time_s": 0.0,
        "first_global_detach_batch_hooks": 0,
        "first_global_detach_batch_hot_hooks": 0,
        "distinct_detached_hot_hooks_by_source": Counter(),
        "distinct_reattached_hot_hooks_by_source": Counter(),
        "distinct_oscillating_hot_hooks_by_source": Counter(),
        "signal_phase_counts": Counter(),
        "backend_phase_counts": Counter(),
        "events": [],
        "stop_windows": [],
    }
    result["hot_target_limit"] = hot_targets
    if not path.exists():
        return result

    mutated_hooks = set()
    detached_hooks = set()
    reattached_hooks = set()
    batch_stop_windows = {}
    batch_sizes = {}
    batch_first_times = {}
    batch_global_detach_hooks = defaultdict(set)
    batch_global_detach_hot_hooks = defaultdict(set)
    single_stop_window_total_us = 0.0
    hook_operations = defaultdict(list)
    hook_events = defaultdict(list)
    hook_symbols = {}
    shutdown_hooks = set()
    shutdown_duplicate_hooks = 0
    shutdown_out_of_range_hooks = 0
    detached_hot_by_source = defaultdict(set)
    reattached_hot_by_source = defaultdict(set)
    probe_dwell_hot_increase_hooks = set()
    probe_dwell_hot_increase_global_hooks = set()
    trace_ordered_hot_hooks = set()
    trace_ordered_global_hot_hooks = set()
    with open(path, "r", encoding="utf-8", newline="") as handle:
        for fields in csv.reader(handle):
            result["rows"] += 1
            if len(fields) >= 1:
                timestamp = parse_float(fields[0])
                if result["first_timestamp"] == 0.0:
                    result["first_timestamp"] = timestamp
                result["last_timestamp"] = timestamp
            hook_id = None
            if len(fields) >= 2:
                hook_id = parse_int(fields[1], None)
            symbol = fields[2] if len(fields) >= 3 else "<unknown>"
            if hook_id is not None:
                hook_symbols[hook_id] = symbol
            operation = fields[3] if len(fields) >= 4 else ""
            status = fields[4] if len(fields) >= 5 else ""
            safety = fields[6] if len(fields) >= 7 else ""
            detail = fields[13] if len(fields) >= 14 else ""
            if operation == "signal":
                result["signal_rows"] += 1
                result["signal_phase_counts"][status] += 1
                phase_bad = (
                    safety != "safe" or
                    status.endswith("-failed") or
                    "timeout" in status
                )
                if phase_bad:
                    result["signal_bad"] += 1
                if "timeout" in status:
                    result["signal_timeout"] += 1
                detail_values = parse_trace_detail(detail)
                active = parse_int(detail_values.get("active"), 0)
                arrived = parse_int(detail_values.get("arrived"), 0)
                done = parse_int(detail_values.get("done"), 0)
                if status == "arrivals-complete" and arrived < active:
                    result["signal_arrival_incomplete"] += 1
                    result["signal_bad"] += 1
                if status == "release-complete" and done < active:
                    result["signal_release_incomplete"] += 1
                    result["signal_bad"] += 1
                continue
            if operation == "backend":
                result["backend_rows"] += 1
                result["backend_phase_counts"][status] += 1
                if status.endswith("-failed") or "timeout" in status:
                    result["backend_failed"] += 1
                if "timeout" in status:
                    result["backend_timeout"] += 1
                continue
            if operation == "startup":
                continue
            batch_size = 0
            if len(fields) >= 10:
                batch_size = parse_int(fields[9])
                result["max_batch"] = max(result["max_batch"], batch_size)
            if len(fields) >= 11:
                stop_window_us = parse_float(fields[10])
                result["max_stop_window_us"] = max(
                    result["max_stop_window_us"], stop_window_us
                )
            else:
                stop_window_us = 0.0
            batch_id = 0
            if len(fields) >= 12:
                batch_id = parse_int(fields[11])
                if batch_id != 0:
                    batch_stop_windows[batch_id] = max(
                        batch_stop_windows.get(batch_id, 0.0),
                        stop_window_us,
                    )
                    batch_sizes[batch_id] = max(
                        batch_sizes.get(batch_id, 0),
                        batch_size,
                    )
                    batch_first_times[batch_id] = min(
                        batch_first_times.get(batch_id, timestamp),
                        timestamp,
                    )
            if stop_window_us > 0.0:
                result["stop_windows"].append(
                    {
                        "time": timestamp,
                        "batch_id": batch_id,
                        "stop_window_us": stop_window_us,
                    }
                )
            if len(fields) < 7:
                continue
            physical_patch = fields[5]
            source = fields[17] if len(fields) >= 18 else "unknown"
            request_calls = parse_int(fields[18], 0) if len(fields) >= 19 else 0
            request_total_time = parse_float(fields[21], 0.0) if len(fields) >= 22 else 0.0
            event_time = timestamp
            if operation == "global-detach":
                if status == "no-admission":
                    result["global_no_admission"] += 1
                elif status == "warm-wait":
                    result["global_warm_wait"] += 1
            elif operation == "liveness":
                if status == "pacing-blocked":
                    result["pacing_blocked"] += 1
                if (
                    symbol == "__peak_heartbeat__" and
                    status == "tick" and
                    len(fields) >= 23
                ):
                    result["heartbeat_liveness"].append(
                        {
                            "time": timestamp,
                            "attached_overhead": parse_float(fields[19], 0.0),
                            "effective_overhead": parse_float(fields[20], 0.0),
                            "elapsed": parse_float(fields[21], 0.0),
                            "transition_tokens": parse_float(fields[22], 0.0),
                        }
                    )
            elif operation == "probe-dwell":
                detail_values = parse_trace_detail(detail)
                sample_rate = parse_float(detail_values.get("sample_rate"), 0.0)
                target_ratio = parse_float(detail_values.get("target_ratio"), 0.0)
                old_dwell = parse_float(detail_values.get("old_dwell"), 0.0)
                new_dwell = parse_float(detail_values.get("new_dwell"), 0.0)
                result["probe_dwell_update_count"] += 1
                if status == "invalid":
                    result["probe_dwell_invalid_count"] += 1
                if (
                    status == "hot" and
                    sample_rate > target_ratio > 0.0 and
                    new_dwell > old_dwell and
                    hook_id is not None
                ):
                    symbol_for_hook = hook_symbols.get(hook_id, symbol)
                    match = STRESS_SYMBOL_RE.match(symbol_for_hook)
                    result["probe_dwell_hot_increase_count"] += 1
                    if match and int(match.group(1)) < hot_targets:
                        probe_dwell_hot_increase_hooks.add(hook_id)
                        if is_global_heartbeat_source(source):
                            probe_dwell_hot_increase_global_hooks.add(hook_id)
                continue
            elif operation == "probe-closeout":
                if status == "keep-attached":
                    result["probe_closeout_keep_attached"] += 1
                elif status == "hot-closeout":
                    result["probe_closeout_hot"] += 1
                elif status == "invalid-closeout":
                    result["probe_closeout_invalid"] += 1
                elif status == "reserved-closeout":
                    result["probe_closeout_reserved"] += 1
                elif status == "reserved-wait":
                    result["probe_closeout_reserved_wait"] += 1
                continue
            if batch_id == 0 and operation in ("detach", "reattach") and stop_window_us > 0.0:
                single_stop_window_total_us += stop_window_us
            result["source_counts"][source] += 1
            result["operation_source_counts"][f"{operation}:{source}"] += 1
            result["symbol_operation_counts"][f"{symbol}:{operation}"] += 1
            if status == "prepare-failed":
                result["prepare_failed"] += 1
            if status == "gum-failed":
                result["gum_failed"] += 1
            if status == "retry-abandoned":
                result["retry_abandoned"] += 1
            if safety == "classify-failed":
                result["classify_failed"] += 1
            if status == "success" and physical_patch == "1" and safety == "safe":
                result["success_operation_source_counts"][f"{operation}:{source}"] += 1
                if hook_id is not None:
                    mutated_hooks.add(hook_id)
                    hook_operations[hook_id].append((operation, source))
                if operation == "shutdown":
                    result["shutdown_success"] += 1
                if operation in ("detach", "reattach"):
                    # Column 0 is the authoritative absolute monotonic
                    # timestamp from PEAK.  Column 21 is the request's
                    # diagnostic total-time snapshot; it must not drive event
                    # ordering or sampling-window proof.
                    result["events"].append(
                        {
                            "time": event_time,
                            "trace_timestamp": timestamp,
                            "request_total_time": request_total_time,
                            "request_calls": request_calls,
                            "hook_id": hook_id,
                            "symbol": symbol,
                            "operation": operation,
                            "source": source,
                            "batch_id": batch_id,
                            "batch_size": batch_size,
                            "stop_window_us": stop_window_us,
                        }
                    )
                    if hook_id is not None:
                        hook_events[hook_id].append(
                            {
                                "time": event_time,
                                "request_calls": request_calls,
                                "request_total_time": request_total_time,
                                "symbol": symbol,
                                "operation": operation,
                                "source": source,
                                "batch_id": batch_id,
                                "batch_size": batch_size,
                                "stop_window_us": stop_window_us,
                            }
                        )
                    result["transition_by_hook"][symbol] += 1
                    if is_global_heartbeat_source(source):
                        result["global_transition_by_hook"][symbol] += 1
                if operation == "detach":
                    if hook_id is not None:
                        detached_hooks.add(hook_id)
                        if hook_id < hot_targets:
                            detached_hot_by_source[source].add(hook_id)
                            if (
                                is_global_heartbeat_source(source) and
                                batch_id != 0
                            ):
                                batch_global_detach_hooks[batch_id].add(hook_id)
                                batch_global_detach_hot_hooks[batch_id].add(
                                    hook_id
                                )
                    if symbol == target_name(0):
                        result["target0_max_detach_request_calls"] = max(
                            result["target0_max_detach_request_calls"],
                            request_calls,
                        )
                    result["detach_success"] += 1
                elif operation == "reattach":
                    if hook_id is not None:
                        reattached_hooks.add(hook_id)
                        if hook_id < hot_targets:
                            reattached_hot_by_source[source].add(hook_id)
                    if symbol == target_name(0):
                        result["target0_reattach_success"] += 1
                    result["reattach_success"] += 1
                elif operation == "shutdown":
                    if hook_id is not None:
                        if total_targets is not None and (
                            hook_id < 0 or hook_id >= total_targets
                        ):
                            shutdown_out_of_range_hooks += 1
                        elif hook_id in shutdown_hooks:
                            shutdown_duplicate_hooks += 1
                        else:
                            shutdown_hooks.add(hook_id)
    result["distinct_mutated_hooks"] = len(mutated_hooks)
    result["distinct_detached_hooks"] = len(detached_hooks)
    result["distinct_reattached_hooks"] = len(reattached_hooks)
    result["distinct_batches"] = len(batch_stop_windows)
    result["total_unique_stop_window_us"] = (
        sum(batch_stop_windows.values()) + single_stop_window_total_us
    )
    result["full_batches"] = sum(1 for size in batch_sizes.values() if size >= 64)
    if result["first_timestamp"] != 0.0 and result["last_timestamp"] > result["first_timestamp"]:
        result["trace_duration_s"] = result["last_timestamp"] - result["first_timestamp"]
    if batch_global_detach_hooks:
        first_batch_id = min(
            batch_global_detach_hooks,
            key=lambda item: batch_first_times.get(item, float("inf")),
        )
        first_batch_time = batch_first_times.get(first_batch_id, result["first_timestamp"])
        result["first_global_detach_batch_id"] = first_batch_id
        result["first_global_detach_batch_size"] = batch_sizes.get(first_batch_id, 0)
        result["first_global_detach_batch_time_s"] = (
            first_batch_time - result["first_timestamp"]
            if result["first_timestamp"] > 0.0 else 0.0
        )
        result["first_global_detach_batch_hooks"] = len(
            batch_global_detach_hooks[first_batch_id]
        )
        result["first_global_detach_batch_hot_hooks"] = len(
            batch_global_detach_hot_hooks[first_batch_id]
        )
    result["shutdown_distinct_hooks"] = len(shutdown_hooks)
    result["shutdown_duplicate_hooks"] = shutdown_duplicate_hooks
    result["shutdown_out_of_range_hooks"] = shutdown_out_of_range_hooks
    if total_targets is not None:
        result["shutdown_missing_hooks"] = max(
            0, total_targets - result["shutdown_distinct_hooks"]
        )
    oscillating_hot_by_source = defaultdict(set)

    for hook_id, operations in hook_operations.items():
        oscillations = 0
        global_oscillations = 0
        for index in range(2, len(operations)):
            if (
                operations[index - 2][0] == "detach"
                and operations[index - 1][0] == "reattach"
                and operations[index][0] == "detach"
            ):
                oscillations += 1
            if (
                operations[index - 2][0] == "detach"
                and is_global_heartbeat_source(operations[index - 2][1])
                and operations[index - 1][0] == "reattach"
                and is_global_heartbeat_source(operations[index - 1][1])
                and operations[index][0] == "detach"
                and is_global_heartbeat_source(operations[index][1])
            ):
                global_oscillations += 1
        if oscillations > 0:
            symbol = hook_symbols.get(hook_id, f"hook-{hook_id}")
            result["oscillation_by_hook"][symbol] = oscillations
            result["same_hook_oscillations"] += oscillations
            result["distinct_oscillating_hooks"] += 1
            match = STRESS_SYMBOL_RE.match(symbol)
            if match and int(match.group(1)) < hot_targets:
                result["distinct_oscillating_hot_hooks"] += 1
                for operation, source in operations:
                    if operation in ("detach", "reattach"):
                        oscillating_hot_by_source[source].add(hook_id)
            result["max_hook_oscillations"] = max(
                result["max_hook_oscillations"],
                oscillations,
            )
        if global_oscillations > 0:
            symbol = hook_symbols.get(hook_id, f"hook-{hook_id}")
            result["global_oscillation_by_hook"][symbol] = global_oscillations
            result["global_same_hook_oscillations"] += global_oscillations
            result["distinct_global_oscillating_hooks"] += 1
            match = STRESS_SYMBOL_RE.match(symbol)
            if match and int(match.group(1)) < hot_targets:
                result["distinct_global_oscillating_hot_hooks"] += 1
    sampled_hooks = set()
    sampled_hot_hooks = set()
    global_sampled_hooks = set()
    global_sampled_hot_hooks = set()
    sampled_times = []
    probe_detached_dwells = []
    global_hot_probe_detached_dwells = []
    trace_ordered_hooks = set()
    trace_ordered_examples = []
    for hook_id, events_for_hook in hook_events.items():
        last_heartbeat_detach = None
        for event in sorted(events_for_hook, key=lambda item: item["time"]):
            if (
                event["operation"] == "detach" and
                is_global_heartbeat_source(event.get("source"))
            ):
                last_heartbeat_detach = event
                continue
            if (
                event["operation"] == "reattach" and
                is_global_heartbeat_source(event.get("source")) and
                last_heartbeat_detach is not None
            ):
                dwell_s = event["time"] - last_heartbeat_detach["time"]
                if dwell_s >= 0.0:
                    probe_detached_dwells.append(dwell_s)
                    symbol = hook_symbols.get(hook_id, f"hook-{hook_id}")
                    match = STRESS_SYMBOL_RE.match(symbol)
                    if match and int(match.group(1)) < hot_targets:
                        global_hot_probe_detached_dwells.append(dwell_s)
                last_heartbeat_detach = None

        ordered_state = 0
        ordered_detach = None
        ordered_reattach = None
        for event in sorted(events_for_hook, key=lambda item: item["time"]):
            if event["operation"] == "reattach":
                ordered_reattach = event
                if ordered_state in {1, 2}:
                    ordered_state = 2
                continue
            if event["operation"] != "detach":
                continue

            if ordered_state == 0:
                ordered_detach = event
                ordered_state = 1
                continue
            if ordered_state == 1:
                ordered_detach = event
                continue

            # ordered_state == 2 => detach -> reattach -> later detach
            call_delta = event["request_calls"] - ordered_reattach["request_calls"]
            if ordered_reattach is not None and call_delta > 0:
                symbol = hook_symbols.get(hook_id, f"hook-{hook_id}")
                match = STRESS_SYMBOL_RE.match(symbol)
                result["trace_ordered_reattach_cycles"] += 1
                result["trace_ordered_reattach_call_delta_total"] += call_delta
                result["trace_ordered_reattach_call_delta_max"] = max(
                    result["trace_ordered_reattach_call_delta_max"],
                    call_delta,
                )
                trace_ordered_hooks.add(hook_id)
                if match and int(match.group(1)) < hot_targets:
                    result["trace_ordered_reattach_hot_cycles"] += 1
                    trace_ordered_hot_hooks.add(hook_id)
                    if (
                        is_global_heartbeat_source(
                            ordered_detach.get("source")
                        ) and
                        is_global_heartbeat_source(
                            ordered_reattach.get("source")
                        ) and
                        is_global_heartbeat_source(event.get("source"))
                    ):
                        result["trace_ordered_global_hot_reattach_cycles"] += 1
                        trace_ordered_global_hot_hooks.add(hook_id)
                if len(trace_ordered_examples) < 3:
                    trace_ordered_examples.append(
                        {
                            "hook_id": hook_id,
                            "symbol": hook_symbols.get(
                                hook_id, f"hook-{hook_id}"
                            ),
                            "detach_time": ordered_detach["time"],
                            "reattach_time": ordered_reattach["time"],
                            "second_detach_time": event["time"],
                            "call_delta": call_delta,
                        }
                    )

            # Start a new sequence with this detach as the anchor.
            ordered_detach = event
            ordered_state = 1

        pending_reattach = None
        for event in events_for_hook:
            if event["operation"] == "reattach":
                pending_reattach = event
                continue
            if event["operation"] != "detach" or pending_reattach is None:
                continue

            call_delta = event["request_calls"] - pending_reattach["request_calls"]
            if call_delta > 0:
                symbol = hook_symbols.get(hook_id, f"hook-{hook_id}")
                match = STRESS_SYMBOL_RE.match(symbol)
                result["reattach_sampled_cycles"] += 1
                result["reattach_sampled_call_delta_total"] += call_delta
                result["reattach_sampled_call_delta_max"] = max(
                    result["reattach_sampled_call_delta_max"],
                    call_delta,
                )
                sampled_hooks.add(hook_id)
                if match and int(match.group(1)) < hot_targets:
                    sampled_hot_hooks.add(hook_id)
                if (
                    is_global_heartbeat_source(pending_reattach["source"]) and
                    is_global_heartbeat_source(event["source"])
                ):
                    result["global_reattach_sampled_cycles"] += 1
                    global_sampled_hooks.add(hook_id)
                    if match and int(match.group(1)) < hot_targets:
                        global_sampled_hot_hooks.add(hook_id)
                sampled_times.append(event["time"])
            pending_reattach = None

    result["trace_ordered_reattach_distinct_hooks"] = len(trace_ordered_hooks)
    result["trace_ordered_reattach_distinct_hot_hooks"] = len(
        trace_ordered_hot_hooks
    )
    result["trace_ordered_global_hot_reattach_distinct_hooks"] = len(
        trace_ordered_global_hot_hooks
    )
    result["trace_ordered_reattach_cycle_examples"] = trace_ordered_examples
    result["distinct_reattach_sampled_hooks"] = len(sampled_hooks)
    result["distinct_reattach_sampled_hot_hooks"] = len(sampled_hot_hooks)
    result["distinct_global_reattach_sampled_hooks"] = len(global_sampled_hooks)
    result["distinct_global_reattach_sampled_hot_hooks"] = len(
        global_sampled_hot_hooks
    )
    result["probe_detached_dwell_count"] = len(probe_detached_dwells)
    if probe_detached_dwells:
        result["probe_detached_dwell_min_s"] = min(probe_detached_dwells)
        result["probe_detached_dwell_p50_s"] = percentile(
            probe_detached_dwells,
            0.50,
        )
        result["probe_detached_dwell_p95_s"] = percentile(
            probe_detached_dwells,
            0.95,
        )
    result["global_hot_probe_detached_dwell_count"] = len(
        global_hot_probe_detached_dwells
    )
    if global_hot_probe_detached_dwells:
        result["global_hot_probe_detached_dwell_min_s"] = min(
            global_hot_probe_detached_dwells
        )
    result["probe_dwell_hot_increase_hot_hooks"] = len(
        probe_dwell_hot_increase_hooks
    )
    result["probe_dwell_hot_increase_global_hot_hooks"] = len(
        probe_dwell_hot_increase_global_hooks
    )
    if len(sampled_times) >= 2:
        result["reattach_sampled_span_s"] = max(sampled_times) - min(sampled_times)
    if result["oscillation_by_hook"]:
        values = sorted(result["oscillation_by_hook"].values())
        middle = len(values) // 2
        if len(values) % 2:
            result["median_oscillations_per_oscillating_hook"] = float(
                values[middle]
            )
        else:
            result["median_oscillations_per_oscillating_hook"] = (
                values[middle - 1] + values[middle]
            ) / 2.0

    total_transitions = sum(result["transition_by_hook"].values())
    if total_transitions > 0:
        result["top_hook_transition_share"] = (
            result["transition_by_hook"].most_common(1)[0][1] /
            float(total_transitions)
        )
    total_global_transitions = sum(result["global_transition_by_hook"].values())
    if total_global_transitions > 0:
        result["top_global_hook_transition_share"] = (
            result["global_transition_by_hook"].most_common(1)[0][1] /
            float(total_global_transitions)
        )

    for hook_id in detached_hooks:
        symbol = hook_symbols.get(hook_id, "")
        match = STRESS_SYMBOL_RE.match(symbol)
        if match and int(match.group(1)) < hot_targets:
            result["detached_hot_hooks"] += 1
        elif match:
            result["detached_cold_hooks"] += 1
    for hook_id in reattached_hooks:
        symbol = hook_symbols.get(hook_id, "")
        match = STRESS_SYMBOL_RE.match(symbol)
        if match and int(match.group(1)) < hot_targets:
            result["reattached_hot_hooks"] += 1
        elif match:
            result["reattached_cold_hooks"] += 1
    result["distinct_detached_hot_hooks_by_source"] = Counter(
        {source: len(hooks) for source, hooks in detached_hot_by_source.items()}
    )
    result["distinct_reattached_hot_hooks_by_source"] = Counter(
        {source: len(hooks) for source, hooks in reattached_hot_by_source.items()}
    )
    result["distinct_oscillating_hot_hooks_by_source"] = Counter(
        {source: len(hooks) for source, hooks in oscillating_hot_by_source.items()}
    )
    return result


def build_command(args, work_dir):
    counts_path = work_dir / "target-counts.csv"
    sample_counts_path = work_dir / "target-count-samples.csv"
    cmd = [
        str(args.exe),
        "--threads",
        str(args.threads),
        "--seconds",
        str(args.seconds),
        "--targets",
        str(args.targets),
        "--hot-targets",
        str(args.hot_targets),
        "--burst",
        str(args.burst),
        "--all-target-period",
        str(args.all_target_period),
    ]
    if counts_path is not None:
        cmd.extend(["--counts-path", str(counts_path)])
    if args.sample_counts_interval_ms > 0:
        cmd.extend(
            [
                "--sample-counts-path",
                str(sample_counts_path),
                "--sample-counts-interval-ms",
                str(args.sample_counts_interval_ms),
            ]
        )
    if args.work_quota:
        cmd.extend(["--work-quota", str(args.work_quota)])
    if args.hot_phase_seconds:
        cmd.extend(["--hot-phase-seconds", str(args.hot_phase_seconds)])
    if args.phase_cold_target0_period:
        cmd.extend(
            [
                "--phase-cold-target0-period",
                str(args.phase_cold_target0_period),
            ]
        )
    if args.hot_rotation_period_us:
        cmd.extend(
            [
                "--hot-rotation-period-us",
                str(args.hot_rotation_period_us),
                "--active-hot-targets",
                str(args.active_hot_targets),
            ]
        )
    if args.cold_sweep_period:
        cmd.extend(["--cold-sweep-period", str(args.cold_sweep_period)])
    if args.hot_only_workload:
        cmd.append("--hot-only-workload")
    if args.call_cold_targets_once:
        cmd.append("--call-cold-targets-once")
    if args.controller_churn:
        cmd.extend(
            [
                "--controller-churn",
                "--churn-interval-us",
                str(args.churn_interval_us),
                "--churn-targets",
                str(args.churn_targets),
                "--churn-batch-size",
                str(args.churn_batch_size),
                "--churn-workers",
                str(args.churn_workers),
                "--churn-random-percent",
                str(args.churn_random_percent),
                "--churn-hot-bias-percent",
                str(args.churn_hot_bias_percent),
                "--churn-jitter-us",
                str(args.churn_jitter_us),
            ]
        )
    return cmd


def parse_target_counts(path):
    counts = {}

    if not path.exists():
        return counts
    with open(path, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                counts[int(row["index"])] = int(row["actual_calls"])
            except (KeyError, TypeError, ValueError):
                continue
    return counts


def parse_target_count_samples(path, hot_targets):
    snapshots = {}
    snapshot_times = {}
    by_index = defaultdict(list)

    if not path.exists():
        return [], by_index
    with open(path, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            elapsed = round(parse_float(row.get("elapsed_s")), 6)
            monotonic = parse_float(row.get("monotonic_s"), elapsed)
            index = parse_int(row.get("index"), -1)
            if index < 0:
                continue
            actual_calls = parse_int(row.get("actual_calls"), 0)
            if index < hot_targets:
                snapshots[elapsed] = snapshots.get(elapsed, 0) + actual_calls
                if elapsed not in snapshot_times:
                    snapshot_times[elapsed] = monotonic
                by_index[index].append(
                    {
                        "elapsed": elapsed,
                        "time": monotonic,
                        "actual_calls": actual_calls,
                    }
                )
    for samples in by_index.values():
        samples.sort(key=lambda item: item["elapsed"])
    return [
        {
            "elapsed": elapsed,
            "time": snapshot_times.get(elapsed, elapsed),
            "hot_calls": hot_calls,
        }
        for elapsed, hot_calls in sorted(snapshots.items())
    ], by_index


def first_sample_at_or_after(samples, target_time):
    for sample in samples:
        if sample.get("time", sample["elapsed"]) >= target_time:
            return sample
    return None


def last_sample_at_or_before(samples, target_time):
    candidate = None
    for sample in samples:
        if sample.get("time", sample["elapsed"]) > target_time:
            break
        candidate = sample
    return candidate


def compute_strict_hot_reattach_cycles(trace, samples_by_index, hot_targets, bin_s):
    result = {
        "cycles": 0,
        "distinct_hooks": 0,
        "global_cycles": 0,
        "global_distinct_hooks": 0,
        "call_delta_total": 0,
        "call_delta_max": 0,
        "sample_delta_total": 0,
        "sample_delta_max": 0,
        "closeout_span_max_s": 0.0,
        "sample_span_s": 0.0,
        "sample_bins": 0,
        "examples": [],
    }
    distinct_hooks = set()
    global_distinct_hooks = set()
    cycle_elapsed_times = []

    events_by_hook = defaultdict(list)
    for event in trace.get("events", []):
        hook_id = event.get("hook_id")
        if hook_id is None or hook_id < 0 or hook_id >= hot_targets:
            continue
        events_by_hook[hook_id].append(event)

    for hook_id, events_for_hook in events_by_hook.items():
        samples = samples_by_index.get(hook_id, [])
        if len(samples) < 2:
            continue

        ordered_state = 0
        ordered_detach = None
        ordered_reattach = None
        for event in sorted(events_for_hook, key=lambda item: item["time"]):
            if event["operation"] == "reattach":
                ordered_reattach = event
                if ordered_state in {1, 2}:
                    ordered_state = 2
                continue
            if event["operation"] != "detach":
                continue

            if ordered_state == 0:
                ordered_detach = event
                ordered_state = 1
                continue
            if ordered_state == 1:
                ordered_detach = event
                continue

            start_sample = first_sample_at_or_after(
                samples, ordered_reattach["trace_timestamp"]
            )
            end_sample = last_sample_at_or_before(
                samples,
                event["trace_timestamp"],
            )
            if (
                ordered_reattach is not None and
                start_sample is not None and
                end_sample is not None and
                end_sample.get("time", end_sample["elapsed"]) >
                    start_sample.get("time", start_sample["elapsed"])
            ):
                call_delta = (
                    event["request_calls"] -
                    ordered_reattach["request_calls"]
                )
                sample_delta = (
                    end_sample["actual_calls"] -
                    start_sample["actual_calls"]
                )
                if call_delta > 0 and sample_delta > 0:
                    start_elapsed = start_sample["elapsed"]
                    closeout_span_s = max(
                        0.0,
                        event["time"] - ordered_reattach["time"],
                    )
                    result["cycles"] += 1
                    result["call_delta_total"] += call_delta
                    result["call_delta_max"] = max(
                        result["call_delta_max"],
                        call_delta,
                    )
                    result["sample_delta_total"] += sample_delta
                    result["sample_delta_max"] = max(
                        result["sample_delta_max"],
                        sample_delta,
                    )
                    result["closeout_span_max_s"] = max(
                        result["closeout_span_max_s"],
                        closeout_span_s,
                    )
                    cycle_elapsed_times.append(start_elapsed)
                    distinct_hooks.add(hook_id)
                    if (
                        is_global_heartbeat_source(
                            ordered_detach.get("source")
                        ) and
                        is_global_heartbeat_source(
                            ordered_reattach.get("source")
                        ) and
                        is_global_heartbeat_source(event.get("source"))
                    ):
                        result["global_cycles"] += 1
                        global_distinct_hooks.add(hook_id)
                    if len(result["examples"]) < 3:
                        result["examples"].append(
                            {
                                "hook_id": hook_id,
                                "symbol": event.get(
                                    "symbol",
                                    target_name(hook_id),
                                ),
                                "detach_time": ordered_detach["time"],
                                "reattach_time": ordered_reattach["time"],
                                "second_detach_time": event["time"],
                                "closeout_span_s": closeout_span_s,
                                "call_delta": call_delta,
                                "sample_start_time": start_sample["elapsed"],
                                "sample_end_time": end_sample["elapsed"],
                                "sample_start_abs": start_sample.get(
                                    "time",
                                    start_sample["elapsed"],
                                ),
                                "sample_end_abs": end_sample.get(
                                    "time",
                                    end_sample["elapsed"],
                                ),
                                "sample_delta": sample_delta,
                            }
                        )

            ordered_detach = event
            ordered_state = 1

    result["distinct_hooks"] = len(distinct_hooks)
    result["global_distinct_hooks"] = len(global_distinct_hooks)
    if len(cycle_elapsed_times) >= 2:
        result["sample_span_s"] = (
            max(cycle_elapsed_times) - min(cycle_elapsed_times)
        )
    if cycle_elapsed_times and bin_s > 0.0:
        result["sample_bins"] = len(
            {int(time_value / bin_s) for time_value in cycle_elapsed_times}
        )
    return result


def compute_sampling_bins(trace, count_samples, hot_targets, duration_s, bin_s):
    empty = {
        "bins": 0,
        "bins_with_hot_progress": 0,
        "bins_with_detach": 0,
        "bins_with_reattach": 0,
        "post_detach_bins": 0,
        "post_detach_bins_with_attached_hot": 0,
        "attached_hot_min": 0,
        "attached_hot_p50": 0.0,
        "attached_hot_max": 0,
        "bins_with_hot_detach": 0,
        "bins_with_hot_reattach": 0,
        "bins_with_global_hot_detach": 0,
        "bins_with_global_hot_reattach": 0,
        "hot_reattached_p50": 0.0,
        "hot_reattached_max": 0,
        "global_hot_reattached_p50": 0.0,
        "global_hot_reattached_max": 0,
        "reattach_span_s": 0.0,
    }
    if duration_s <= 0.0 or bin_s <= 0.0:
        return empty

    events = sorted(
        (
            event for event in trace.get("events", [])
            if event["hook_id"] is not None and event["hook_id"] < hot_targets
        ),
        key=lambda event: event["time"],
    )
    absolute_samples = bool(count_samples and "time" in count_samples[0])
    if absolute_samples:
        candidates = [count_samples[0]["time"]]
        if events:
            candidates.append(events[0]["time"])
        base_time = min(candidates)
    else:
        base_time = 0.0
    attached = set(range(hot_targets))
    event_index = 0
    sample_index = 0
    previous_hot_calls = 0
    bins = []
    first_detach_time = None
    reattach_times = []

    for bin_index in range(int(math.ceil(duration_s / bin_s))):
        start_elapsed = bin_index * bin_s
        end_elapsed = min(duration_s, start_elapsed + bin_s)
        start = base_time + start_elapsed
        end = base_time + end_elapsed
        detach_count = 0
        reattach_count = 0
        hot_detached = set()
        hot_reattached = set()
        global_hot_detached = set()
        global_hot_reattached = set()
        had_attached_hot = len(attached) > 0
        max_attached_hot = len(attached)

        while event_index < len(events) and events[event_index]["time"] <= end:
            event = events[event_index]
            if event["operation"] == "detach":
                attached.discard(event["hook_id"])
                detach_count += 1
                hot_detached.add(event["hook_id"])
                if is_global_heartbeat_source(event.get("source")):
                    global_hot_detached.add(event["hook_id"])
                if first_detach_time is None:
                    first_detach_time = event["time"]
            elif event["operation"] == "reattach":
                attached.add(event["hook_id"])
                reattach_count += 1
                hot_reattached.add(event["hook_id"])
                if is_global_heartbeat_source(event.get("source")):
                    global_hot_reattached.add(event["hook_id"])
                reattach_times.append(event["time"])
            if attached:
                had_attached_hot = True
            max_attached_hot = max(max_attached_hot, len(attached))
            event_index += 1

        hot_calls = previous_hot_calls
        while (
            sample_index < len(count_samples) and
            count_samples[sample_index].get("time", count_samples[sample_index]["elapsed"]) <= end
        ):
            hot_calls = count_samples[sample_index]["hot_calls"]
            sample_index += 1
        hot_delta = max(0, hot_calls - previous_hot_calls)
        previous_hot_calls = hot_calls

        bins.append(
            {
                "hot_delta": hot_delta,
                "attached_hot": len(attached),
                "had_attached_hot": had_attached_hot,
                "max_attached_hot": max_attached_hot,
                "detach_count": detach_count,
                "reattach_count": reattach_count,
                "hot_detached": len(hot_detached),
                "hot_reattached": len(hot_reattached),
                "global_hot_detached": len(global_hot_detached),
                "global_hot_reattached": len(global_hot_reattached),
                "post_detach": (
                    first_detach_time is not None and end > first_detach_time
                ),
            }
        )

    attached_values = [item["attached_hot"] for item in bins]
    attached_sorted = sorted(attached_values)
    if attached_sorted:
        middle = len(attached_sorted) // 2
        if len(attached_sorted) % 2:
            attached_p50 = float(attached_sorted[middle])
        else:
            attached_p50 = (
                attached_sorted[middle - 1] + attached_sorted[middle]
            ) / 2.0
    else:
        attached_p50 = 0.0
    post_bins = [item for item in bins if item["post_detach"]]
    max_attached_values = [item["max_attached_hot"] for item in bins]
    hot_reattached_values = [item["hot_reattached"] for item in bins]
    global_hot_reattached_values = [
        item["global_hot_reattached"] for item in bins
    ]
    return {
        "bins": len(bins),
        "bins_with_hot_progress": sum(1 for item in bins if item["hot_delta"] > 0),
        "bins_with_detach": sum(1 for item in bins if item["detach_count"] > 0),
        "bins_with_reattach": sum(
            1 for item in bins if item["reattach_count"] > 0
        ),
        "post_detach_bins": len(post_bins),
        "post_detach_bins_with_attached_hot": sum(
            1 for item in post_bins if item["had_attached_hot"]
        ),
        "attached_hot_min": min(attached_values) if attached_values else 0,
        "attached_hot_p50": attached_p50,
        "attached_hot_max": max(attached_values) if attached_values else 0,
        "bins_with_hot_detach": sum(
            1 for item in bins if item["hot_detached"] > 0
        ),
        "bins_with_hot_reattach": sum(
            1 for item in bins if item["hot_reattached"] > 0
        ),
        "bins_with_global_hot_detach": sum(
            1 for item in bins if item["global_hot_detached"] > 0
        ),
        "bins_with_global_hot_reattach": sum(
            1 for item in bins if item["global_hot_reattached"] > 0
        ),
        "hot_reattached_p50": percentile(hot_reattached_values, 0.50),
        "hot_reattached_max": (
            max(hot_reattached_values) if hot_reattached_values else 0
        ),
        "global_hot_reattached_p50": percentile(
            global_hot_reattached_values,
            0.50,
        ),
        "global_hot_reattached_max": (
            max(global_hot_reattached_values)
            if global_hot_reattached_values else 0
        ),
        "attached_hot_any_bins": sum(
            1 for item in bins if item["had_attached_hot"]
        ),
        "attached_hot_max_in_bin": (
            max(max_attached_values) if max_attached_values else 0
        ),
        "reattach_span_s": (
            max(reattach_times) - min(reattach_times)
            if len(reattach_times) >= 2 else 0.0
        ),
    }


def compute_global_hot_reattach_progress(trace, hot_targets, duration_s, final_window_s):
    events = [
        event for event in trace.get("events", [])
        if event.get("operation") == "reattach"
        and event.get("hook_id") is not None
        and 0 <= event["hook_id"] < hot_targets
        and is_global_heartbeat_source(event.get("source"))
    ]
    if not events or duration_s <= 0.0:
        return {
            "count": 0,
            "distinct_hooks": 0,
            "final_window_count": 0,
            "final_window_distinct_hooks": 0,
            "max_gap_s": 0.0,
        }

    first_time = trace.get("first_timestamp", 0.0)
    if first_time <= 0.0:
        first_time = min(event["time"] for event in events)
    elapsed_events = sorted(
        (
            {
                "elapsed": max(0.0, event["time"] - first_time),
                "hook_id": event["hook_id"],
            }
            for event in events
        ),
        key=lambda item: item["elapsed"],
    )
    gaps = [
        elapsed_events[i]["elapsed"] - elapsed_events[i - 1]["elapsed"]
        for i in range(1, len(elapsed_events))
    ]
    final_start = max(0.0, duration_s - max(0.0, final_window_s))
    final_events = [
        event for event in elapsed_events
        if final_window_s > 0.0 and event["elapsed"] >= final_start
    ]
    return {
        "count": len(elapsed_events),
        "distinct_hooks": len({event["hook_id"] for event in elapsed_events}),
        "final_window_count": len(final_events),
        "final_window_distinct_hooks": len(
            {event["hook_id"] for event in final_events}
        ),
        "max_gap_s": max(gaps) if gaps else 0.0,
    }


def compute_liveness_overhead_window(trace, final_window_s):
    samples = list(trace.get("heartbeat_liveness", []))
    result = {
        "count": len(samples),
        "final_effective": 0.0,
        "final_attached": 0.0,
        "final_tokens": 0.0,
        "final_elapsed": 0.0,
        "window_s": max(0.0, final_window_s),
        "window_count": 0,
        "window_avg_effective": 0.0,
        "window_max_effective": 0.0,
        "window_avg_attached": 0.0,
        "window_max_attached": 0.0,
    }
    if not samples:
        return result

    samples.sort(key=lambda item: item["time"])
    last = samples[-1]
    result["final_effective"] = last["effective_overhead"]
    result["final_attached"] = last["attached_overhead"]
    result["final_tokens"] = last["transition_tokens"]
    result["final_elapsed"] = last["elapsed"]

    if final_window_s > 0.0:
        start_time = last["time"] - final_window_s
        window = [item for item in samples if item["time"] >= start_time]
    else:
        window = samples
    if not window:
        return result

    result["window_count"] = len(window)
    result["window_avg_effective"] = (
        sum(item["effective_overhead"] for item in window) / len(window)
    )
    result["window_max_effective"] = max(
        item["effective_overhead"] for item in window
    )
    result["window_avg_attached"] = (
        sum(item["attached_overhead"] for item in window) / len(window)
    )
    result["window_max_attached"] = max(
        item["attached_overhead"] for item in window
    )
    return result


def compute_transition_bins(trace, duration_s, bin_s, budget_per_sec):
    result = {
        "bins": 0,
        "max_stop_window_bin_per_sec": 0.0,
        "bins_over_budget": 0,
        "max_consecutive_over_budget_bins": 0,
    }
    stop_windows = trace.get("stop_windows", [])
    if not stop_windows:
        stop_windows = [
            {
                "time": event["time"],
                "batch_id": event.get("batch_id", 0),
                "stop_window_us": event.get("stop_window_us", 0.0),
            }
            for event in trace.get("events", [])
        ]
    if duration_s <= 0.0 or bin_s <= 0.0 or not stop_windows:
        return result

    base_time = min(item["time"] for item in stop_windows)
    bin_count = int(math.ceil(duration_s / bin_s))
    stop_window_s_by_bin = [0.0 for _ in range(bin_count)]
    seen_batches = set()

    for index, item in enumerate(stop_windows):
        stop_window_us = item.get("stop_window_us", 0.0)
        if stop_window_us <= 0.0:
            continue
        batch_id = item.get("batch_id", 0)
        if batch_id:
            key = ("batch", batch_id)
        else:
            key = ("single", index)
        if key in seen_batches:
            continue
        seen_batches.add(key)
        offset_s = item["time"] - base_time
        if offset_s < 0.0:
            offset_s = 0.0
        bin_index = int(offset_s / bin_s)
        if bin_index >= bin_count:
            bin_index = bin_count - 1
        stop_window_s_by_bin[bin_index] += stop_window_us / 1000000.0

    per_sec = [
        value / bin_s
        for value in stop_window_s_by_bin
    ]
    result["bins"] = bin_count
    result["max_stop_window_bin_per_sec"] = max(per_sec) if per_sec else 0.0
    if budget_per_sec is not None:
        consecutive = 0
        for value in per_sec:
            if value > budget_per_sec:
                result["bins_over_budget"] += 1
                consecutive += 1
                result["max_consecutive_over_budget_bins"] = max(
                    result["max_consecutive_over_budget_bins"],
                    consecutive,
                )
            else:
                consecutive = 0
    return result


def parse_peak_profile_stats(work_dir):
    counts = {}
    total_count = 0
    total_overhead_s = 0.0
    stats_file_count = 0
    malformed_rows = 0
    duplicate_target_rows = 0
    seen_targets = set()

    for path in sorted(work_dir.glob("peak-stats*.csv")):
        stats_file_count += 1
        with open(path, "r", encoding="utf-8", newline="") as handle:
            for row in csv.DictReader(handle):
                name = row.get("function", "")
                match = STRESS_SYMBOL_RE.match(name)
                if not match:
                    continue
                try:
                    index = int(match.group(1))
                    count = int(row.get("count", "0"))
                    overhead_s = float(row.get("overhead_s", "0"))
                except (TypeError, ValueError):
                    malformed_rows += 1
                    continue
                if index in seen_targets:
                    duplicate_target_rows += 1
                seen_targets.add(index)
                counts[index] = counts.get(index, 0) + count
                total_count += count
                if overhead_s > 0.0:
                    total_overhead_s += overhead_s
    overhead_per_call = total_overhead_s / total_count if total_count > 0 else 0.0
    return {
        "counts": counts,
        "total_count": total_count,
        "overhead_per_call": overhead_per_call,
        "estimated_profile_overhead_s": total_overhead_s,
        "stats_file_count": stats_file_count,
        "target_rows": len(seen_targets),
        "malformed_rows": malformed_rows,
        "duplicate_target_rows": duplicate_target_rows,
    }


def percentile(values, pct):
    if not values:
        return 0.0
    values = sorted(values)
    if len(values) == 1:
        return float(values[0])
    rank = (len(values) - 1) * pct
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    if low == high:
        return float(values[low])
    return float(values[low]) * (high - rank) + float(values[high]) * (rank - low)


def compute_hot_coverage(actual_counts, profiled_counts, hot_targets):
    ratios = []
    nonzero = 0
    actual_total = 0
    profiled_total = 0
    max_profiled = 0

    for index in range(hot_targets):
        actual = actual_counts.get(index, 0)
        profiled = profiled_counts.get(index, 0)

        if actual <= 0:
            continue
        ratios.append(profiled / float(actual))
        actual_total += actual
        profiled_total += profiled
        max_profiled = max(max_profiled, profiled)
        if profiled > 0:
            nonzero += 1

    return {
        "hot_actual_targets": len(ratios),
        "hot_profiled_targets": nonzero,
        "hot_profiled_target_ratio": (
            nonzero / float(len(ratios)) if ratios else 0.0
        ),
        "hot_coverage_p50": percentile(ratios, 0.50),
        "hot_coverage_p90": percentile(ratios, 0.90),
        "hot_coverage_min": min(ratios) if ratios else 0.0,
        "hot_coverage_max": max(ratios) if ratios else 0.0,
        "hot_total_coverage": (
            profiled_total / float(actual_total) if actual_total > 0 else 0.0
        ),
        "hot_top_profiled_share": (
            max_profiled / float(profiled_total) if profiled_total > 0 else 0.0
        ),
    }


def run_once(args):
    if args.work_dir is None:
        work_dir = Path(tempfile.mkdtemp(prefix="peak-many-target-stress-"))
        cleanup = args.cleanup
    else:
        work_dir = args.work_dir
        work_dir.mkdir(parents=True, exist_ok=True)
        cleanup = False

    trace_path = work_dir / "detach-trace.csv"
    for path in (
        trace_path,
        work_dir / "stdout.log",
        work_dir / "stderr.log",
        work_dir / "target-counts.csv",
        work_dir / "target-count-samples.csv",
    ):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
    for path in work_dir.glob("peak-stats*.csv"):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    env = make_env(args, work_dir)
    cmd = build_command(args, work_dir)
    print(
        "many_targets_stress_start "
        f"backend={args.backend} work_dir={work_dir} "
        f"threads={args.threads} targets={args.targets} "
        f"hot_targets={args.hot_targets} seconds={args.seconds}",
        flush=True,
    )
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            cwd=work_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = decode_timeout_output(exc.stdout)
        stderr = decode_timeout_output(exc.stderr)
        (work_dir / "stdout.log").write_text(stdout, encoding="utf-8")
        (work_dir / "stderr.log").write_text(stderr, encoding="utf-8")
        trace = parse_trace(trace_path, args.hot_targets, args.targets)
        print(
            f"many_targets_stress_timeout timeout={args.timeout} "
            f"work_dir={work_dir}",
            file=sys.stderr,
        )
        print(
            "many_targets_stress_timeout_summary "
            f"work_dir={work_dir} "
            f"trace_rows={trace['rows']} "
            f"detach_success={trace['detach_success']} "
            f"reattach_success={trace['reattach_success']} "
            f"classify_failed={trace['classify_failed']} "
            f"prepare_failed={trace['prepare_failed']} "
            f"gum_failed={trace['gum_failed']} "
            f"retry_abandoned={trace['retry_abandoned']} "
            f"signal_rows={trace['signal_rows']} "
            f"signal_bad={trace['signal_bad']} "
            f"signal_timeout={trace['signal_timeout']} "
            f"signal_arrival_incomplete={trace['signal_arrival_incomplete']} "
            f"signal_release_incomplete={trace['signal_release_incomplete']} "
            f"backend_rows={trace['backend_rows']} "
            f"backend_failed={trace['backend_failed']} "
            f"backend_timeout={trace['backend_timeout']} "
            f"global_no_admission={trace['global_no_admission']} "
            f"global_warm_wait={trace['global_warm_wait']} "
            f"pacing_blocked={trace['pacing_blocked']} "
            f"distinct_mutated_hooks={trace['distinct_mutated_hooks']} "
            f"distinct_detached_hooks={trace['distinct_detached_hooks']} "
            f"distinct_reattached_hooks={trace['distinct_reattached_hooks']} "
            f"distinct_batches={trace['distinct_batches']} "
            f"first_global_detach_batch_size={trace['first_global_detach_batch_size']} "
            f"first_global_detach_batch_hot_hooks={trace['first_global_detach_batch_hot_hooks']} "
            f"total_unique_stop_window_us={trace['total_unique_stop_window_us']:.3f} "
            f"same_hook_oscillations={trace['same_hook_oscillations']} "
            f"global_same_hook_oscillations={trace['global_same_hook_oscillations']} "
            f"distinct_oscillating_hooks={trace['distinct_oscillating_hooks']} "
            f"distinct_oscillating_hot_hooks={trace['distinct_oscillating_hot_hooks']} "
            f"distinct_global_oscillating_hooks={trace['distinct_global_oscillating_hooks']} "
            f"distinct_global_oscillating_hot_hooks={trace['distinct_global_oscillating_hot_hooks']} "
            f"probe_closeout_keep_attached={trace['probe_closeout_keep_attached']} "
            f"probe_closeout_hot={trace['probe_closeout_hot']} "
            f"probe_closeout_invalid={trace['probe_closeout_invalid']} "
            f"probe_closeout_reserved={trace['probe_closeout_reserved']} "
            f"probe_closeout_reserved_wait={trace['probe_closeout_reserved_wait']} "
            f"strict_reattach_ordered_cycles={trace['strict_reattach_ordered_cycles']} "
            f"strict_reattach_ordered_distinct_hooks={trace['strict_reattach_ordered_distinct_hooks']} "
            f"median_oscillations_per_oscillating_hook={trace['median_oscillations_per_oscillating_hook']:.3f} "
            f"top_hook_transition_share={trace['top_hook_transition_share']:.6f} "
            f"top_global_hook_transition_share={trace['top_global_hook_transition_share']:.6f} "
            f"detached_hot_hooks={trace['detached_hot_hooks']} "
            f"reattached_hot_hooks={trace['reattached_hot_hooks']} "
            f"strict_reattach_ordered_call_delta_total={trace['strict_reattach_ordered_call_delta_total']} "
            f"strict_reattach_ordered_call_delta_max={trace['strict_reattach_ordered_call_delta_max']} "
            f"source_counts={format_counter(trace['source_counts'])} "
            f"operation_source_counts={format_counter(trace['operation_source_counts'])} "
            f"success_operation_source_counts={format_counter(trace['success_operation_source_counts'])} "
            f"signal_phase_counts={format_counter(trace['signal_phase_counts'])} "
            f"backend_phase_counts={format_counter(trace['backend_phase_counts'])} "
            f"top_symbol_operations={format_counter(trace['symbol_operation_counts'])} "
            f"top_oscillating_hooks={format_counter(trace['oscillation_by_hook'])} "
            f"top_global_oscillating_hooks={format_counter(trace['global_oscillation_by_hook'])}",
            flush=True,
        )
        return 124

    (work_dir / "stdout.log").write_text(proc.stdout, encoding="utf-8")
    (work_dir / "stderr.log").write_text(proc.stderr, encoding="utf-8")
    combined = proc.stdout + "\n" + proc.stderr
    trace = parse_trace(trace_path, args.hot_targets, args.targets)
    calls_match = CALLS_PER_SEC_RE.search(proc.stdout)
    calls_per_sec = float(calls_match.group(1)) if calls_match else 0.0
    total_calls_match = TOTAL_CALLS_RE.search(proc.stdout)
    total_calls = int(total_calls_match.group(1)) if total_calls_match else 0
    quota_completed_match = QUOTA_COMPLETED_RE.search(proc.stdout)
    quota_completed = (
        quota_completed_match.group(1) == "1" if quota_completed_match else False
    )
    phase_hot_match = PHASE_HOT_TARGET0_CALLS_RE.search(proc.stdout)
    phase_cold_match = PHASE_COLD_TARGET0_CALLS_RE.search(proc.stdout)
    phase_hot_target0_calls = int(phase_hot_match.group(1)) if phase_hot_match else 0
    phase_cold_target0_calls = (
        int(phase_cold_match.group(1)) if phase_cold_match else 0
    )
    rotating_hot_match = ROTATING_HOT_CALLS_RE.search(proc.stdout)
    cold_sweep_match = COLD_SWEEP_CALLS_RE.search(proc.stdout)
    rotating_hot_calls = (
        int(rotating_hot_match.group(1)) if rotating_hot_match else 0
    )
    cold_sweep_calls = int(cold_sweep_match.group(1)) if cold_sweep_match else 0
    peak_target0_count = 0
    for match in PEAK_TARGET0_COUNT_RE.finditer(combined):
        peak_target0_count = max(peak_target0_count, int(match.group(1)))
    target0_post_detach_count = max(
        0, peak_target0_count - trace["target0_max_detach_request_calls"]
    )
    actual_counts = parse_target_counts(work_dir / "target-counts.csv")
    profile_stats = parse_peak_profile_stats(work_dir)
    profiled_counts = profile_stats["counts"]
    coverage = compute_hot_coverage(actual_counts,
                                    profiled_counts,
                                    args.hot_targets)
    elapsed_match = ELAPSED_RE.search(proc.stdout)
    elapsed_s = float(elapsed_match.group(1)) if elapsed_match else float(args.seconds)
    duration_s = elapsed_s if elapsed_s > 0.0 else float(args.seconds)
    count_samples, count_samples_by_index = parse_target_count_samples(
        work_dir / "target-count-samples.csv",
        args.hot_targets,
    )
    strict_hot_cycles = compute_strict_hot_reattach_cycles(
        trace,
        count_samples_by_index,
        args.hot_targets,
        args.sampling_bin_seconds,
    )
    trace["strict_reattach_ordered_cycles"] = strict_hot_cycles["cycles"]
    trace["strict_reattach_ordered_distinct_hooks"] = strict_hot_cycles[
        "distinct_hooks"
    ]
    trace["strict_reattach_ordered_call_delta_total"] = strict_hot_cycles[
        "call_delta_total"
    ]
    trace["strict_reattach_ordered_call_delta_max"] = strict_hot_cycles[
        "call_delta_max"
    ]
    trace["strict_reattach_ordered_hot_cycles"] = strict_hot_cycles["cycles"]
    trace["strict_reattach_ordered_distinct_hot_hooks"] = strict_hot_cycles[
        "distinct_hooks"
    ]
    trace["strict_global_reattach_ordered_hot_cycles"] = strict_hot_cycles[
        "global_cycles"
    ]
    trace["strict_global_reattach_ordered_distinct_hot_hooks"] = (
        strict_hot_cycles["global_distinct_hooks"]
    )
    trace["strict_reattach_ordered_sample_delta_total"] = strict_hot_cycles[
        "sample_delta_total"
    ]
    trace["strict_reattach_ordered_sample_delta_max"] = strict_hot_cycles[
        "sample_delta_max"
    ]
    trace["strict_reattach_ordered_closeout_span_max_s"] = strict_hot_cycles[
        "closeout_span_max_s"
    ]
    trace["strict_reattach_ordered_sample_span_s"] = strict_hot_cycles[
        "sample_span_s"
    ]
    trace["strict_reattach_ordered_sample_bins"] = strict_hot_cycles[
        "sample_bins"
    ]
    trace["strict_reattach_ordered_cycle_examples"] = strict_hot_cycles[
        "examples"
    ]
    sampling_bins = compute_sampling_bins(
        trace,
        count_samples,
        args.hot_targets,
        duration_s,
        args.sampling_bin_seconds,
    )
    global_hot_reattach_progress = compute_global_hot_reattach_progress(
        trace,
        args.hot_targets,
        duration_s,
        args.global_hot_reattach_final_window_s,
    )
    liveness_overhead = compute_liveness_overhead_window(
        trace,
        args.liveness_final_window_s,
    )
    transition_bins = compute_transition_bins(
        trace,
        duration_s,
        args.sampling_bin_seconds,
        args.max_stop_window_bin_per_sec,
    )
    trace_duration_s = (
        trace["trace_duration_s"] if trace["trace_duration_s"] > 0.0 else duration_s
    )
    detach_success_per_sec = trace["detach_success"] / duration_s
    reattach_success_per_sec = trace["reattach_success"] / duration_s
    batches_per_sec = trace["distinct_batches"] / duration_s
    stop_window_per_sec = (
        trace["total_unique_stop_window_us"] / 1000000.0 / duration_s
    )
    estimated_profile_overhead_per_sec = (
        profile_stats["estimated_profile_overhead_s"] / duration_s
        if duration_s > 0.0 else 0.0
    )
    estimated_effective_overhead_per_sec = (
        estimated_profile_overhead_per_sec + stop_window_per_sec
    )
    trace_rows_per_sec = trace["rows"] / trace_duration_s
    all_target_calls_match = ALL_TARGET_CALLS_RE.search(proc.stdout)
    all_target_calls = (
        int(all_target_calls_match.group(1)) if all_target_calls_match else 0
    )
    distinct_called_match = DISTINCT_CALLED_TARGETS_RE.search(proc.stdout)
    distinct_called_hot_match = DISTINCT_CALLED_HOT_TARGETS_RE.search(proc.stdout)
    distinct_called_cold_match = DISTINCT_CALLED_COLD_TARGETS_RE.search(proc.stdout)
    distinct_called_targets = (
        int(distinct_called_match.group(1)) if distinct_called_match else 0
    )
    distinct_called_hot_targets = (
        int(distinct_called_hot_match.group(1)) if distinct_called_hot_match else 0
    )
    distinct_called_cold_targets = (
        int(distinct_called_cold_match.group(1)) if distinct_called_cold_match else 0
    )

    print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    print(
        "many_targets_stress_summary "
        f"rc={proc.returncode} work_dir={work_dir} "
        f"calls_per_sec={calls_per_sec:.3f} total_calls={total_calls} "
        f"quota_completed={int(quota_completed)} "
        f"phase_hot_target0_calls={phase_hot_target0_calls} "
        f"phase_cold_target0_calls={phase_cold_target0_calls} "
        f"rotating_hot_calls={rotating_hot_calls} "
        f"cold_sweep_calls={cold_sweep_calls} "
        f"peak_target0_count={peak_target0_count} "
        f"target0_max_detach_request_calls={trace['target0_max_detach_request_calls']} "
        f"target0_post_detach_count={target0_post_detach_count} "
        f"target0_reattach_success={trace['target0_reattach_success']} "
        f"hot_profiled_targets={coverage['hot_profiled_targets']} "
        f"hot_profiled_target_ratio={coverage['hot_profiled_target_ratio']:.6f} "
        f"hot_total_coverage={coverage['hot_total_coverage']:.6f} "
        f"hot_coverage_p50={coverage['hot_coverage_p50']:.6f} "
        f"hot_coverage_p90={coverage['hot_coverage_p90']:.6f} "
        f"hot_coverage_min={coverage['hot_coverage_min']:.6f} "
        f"hot_coverage_max={coverage['hot_coverage_max']:.6f} "
        f"hot_top_profiled_share={coverage['hot_top_profiled_share']:.6f} "
        f"sampling_bins={sampling_bins['bins']} "
        f"sampling_bins_with_hot_progress={sampling_bins['bins_with_hot_progress']} "
        f"sampling_bins_with_detach={sampling_bins['bins_with_detach']} "
        f"sampling_bins_with_reattach={sampling_bins['bins_with_reattach']} "
        f"sampling_bins_with_hot_detach={sampling_bins['bins_with_hot_detach']} "
        f"sampling_bins_with_hot_reattach={sampling_bins['bins_with_hot_reattach']} "
        f"sampling_bins_with_global_hot_detach={sampling_bins['bins_with_global_hot_detach']} "
        f"sampling_bins_with_global_hot_reattach={sampling_bins['bins_with_global_hot_reattach']} "
        f"sampling_hot_reattached_p50={sampling_bins['hot_reattached_p50']:.3f} "
        f"sampling_hot_reattached_max={sampling_bins['hot_reattached_max']} "
        f"sampling_global_hot_reattached_p50={sampling_bins['global_hot_reattached_p50']:.3f} "
        f"sampling_global_hot_reattached_max={sampling_bins['global_hot_reattached_max']} "
        f"post_detach_bins={sampling_bins['post_detach_bins']} "
        f"post_detach_bins_with_attached_hot={sampling_bins['post_detach_bins_with_attached_hot']} "
        f"sampling_attached_hot_any_bins={sampling_bins['attached_hot_any_bins']} "
        f"sampling_attached_hot_min={sampling_bins['attached_hot_min']} "
        f"sampling_attached_hot_p50={sampling_bins['attached_hot_p50']:.3f} "
        f"sampling_attached_hot_max={sampling_bins['attached_hot_max']} "
        f"sampling_attached_hot_max_in_bin={sampling_bins['attached_hot_max_in_bin']} "
        f"sampling_reattach_span_s={sampling_bins['reattach_span_s']:.6f} "
        f"global_hot_reattach_count={global_hot_reattach_progress['count']} "
        f"global_hot_reattach_distinct_hooks={global_hot_reattach_progress['distinct_hooks']} "
        f"global_hot_reattach_final_window_s={args.global_hot_reattach_final_window_s:.6f} "
        f"global_hot_reattach_final_window_count={global_hot_reattach_progress['final_window_count']} "
        f"global_hot_reattach_final_window_distinct_hooks={global_hot_reattach_progress['final_window_distinct_hooks']} "
        f"global_hot_reattach_max_gap_s={global_hot_reattach_progress['max_gap_s']:.6f} "
        f"reattach_sampled_cycles={trace['reattach_sampled_cycles']} "
        f"global_reattach_sampled_cycles={trace['global_reattach_sampled_cycles']} "
        f"distinct_reattach_sampled_hooks={trace['distinct_reattach_sampled_hooks']} "
        f"distinct_reattach_sampled_hot_hooks={trace['distinct_reattach_sampled_hot_hooks']} "
        f"distinct_global_reattach_sampled_hooks={trace['distinct_global_reattach_sampled_hooks']} "
        f"distinct_global_reattach_sampled_hot_hooks={trace['distinct_global_reattach_sampled_hot_hooks']} "
        f"probe_detached_dwell_count={trace['probe_detached_dwell_count']} "
        f"probe_detached_dwell_min_s={trace['probe_detached_dwell_min_s']:.6f} "
        f"probe_detached_dwell_p50_s={trace['probe_detached_dwell_p50_s']:.6f} "
        f"probe_detached_dwell_p95_s={trace['probe_detached_dwell_p95_s']:.6f} "
        f"global_hot_probe_detached_dwell_count={trace['global_hot_probe_detached_dwell_count']} "
        f"global_hot_probe_detached_dwell_min_s={trace['global_hot_probe_detached_dwell_min_s']:.6f} "
        f"probe_dwell_update_count={trace['probe_dwell_update_count']} "
        f"probe_dwell_invalid_count={trace['probe_dwell_invalid_count']} "
        f"probe_dwell_hot_increase_count={trace['probe_dwell_hot_increase_count']} "
        f"probe_dwell_hot_increase_hot_hooks={trace['probe_dwell_hot_increase_hot_hooks']} "
        f"probe_dwell_hot_increase_global_hot_hooks={trace['probe_dwell_hot_increase_global_hot_hooks']} "
        f"probe_closeout_keep_attached={trace['probe_closeout_keep_attached']} "
        f"probe_closeout_hot={trace['probe_closeout_hot']} "
        f"probe_closeout_invalid={trace['probe_closeout_invalid']} "
        f"probe_closeout_reserved={trace['probe_closeout_reserved']} "
        f"probe_closeout_reserved_wait={trace['probe_closeout_reserved_wait']} "
        f"reattach_sampled_call_delta_total={trace['reattach_sampled_call_delta_total']} "
        f"reattach_sampled_call_delta_max={trace['reattach_sampled_call_delta_max']} "
        f"reattach_sampled_span_s={trace['reattach_sampled_span_s']:.6f} "
        f"trace_ordered_reattach_cycles={trace['trace_ordered_reattach_cycles']} "
        f"trace_ordered_reattach_distinct_hooks={trace['trace_ordered_reattach_distinct_hooks']} "
        f"trace_ordered_reattach_hot_cycles={trace['trace_ordered_reattach_hot_cycles']} "
        f"trace_ordered_reattach_distinct_hot_hooks={trace['trace_ordered_reattach_distinct_hot_hooks']} "
        f"trace_ordered_global_hot_reattach_cycles={trace['trace_ordered_global_hot_reattach_cycles']} "
        f"trace_ordered_global_hot_reattach_distinct_hooks={trace['trace_ordered_global_hot_reattach_distinct_hooks']} "
        f"trace_ordered_reattach_call_delta_total={trace['trace_ordered_reattach_call_delta_total']} "
        f"trace_ordered_reattach_call_delta_max={trace['trace_ordered_reattach_call_delta_max']} "
        f"trace_ordered_reattach_cycle_examples={format_cycle_examples(trace['trace_ordered_reattach_cycle_examples'])} "
        f"strict_reattach_ordered_cycles={trace['strict_reattach_ordered_cycles']} "
        f"strict_reattach_ordered_distinct_hooks={trace['strict_reattach_ordered_distinct_hooks']} "
        f"strict_reattach_ordered_call_delta_total={trace['strict_reattach_ordered_call_delta_total']} "
        f"strict_reattach_ordered_call_delta_max={trace['strict_reattach_ordered_call_delta_max']} "
        f"strict_reattach_ordered_hot_cycles={trace['strict_reattach_ordered_hot_cycles']} "
        f"strict_reattach_ordered_distinct_hot_hooks={trace['strict_reattach_ordered_distinct_hot_hooks']} "
        f"strict_global_reattach_ordered_hot_cycles={trace['strict_global_reattach_ordered_hot_cycles']} "
        f"strict_global_reattach_ordered_distinct_hot_hooks={trace['strict_global_reattach_ordered_distinct_hot_hooks']} "
        f"strict_reattach_ordered_sample_delta_total={trace['strict_reattach_ordered_sample_delta_total']} "
        f"strict_reattach_ordered_sample_delta_max={trace['strict_reattach_ordered_sample_delta_max']} "
        f"strict_reattach_ordered_closeout_span_max_s={trace['strict_reattach_ordered_closeout_span_max_s']:.6f} "
        f"strict_reattach_ordered_sample_span_s={trace['strict_reattach_ordered_sample_span_s']:.6f} "
        f"strict_reattach_ordered_sample_bins={trace['strict_reattach_ordered_sample_bins']} "
        f"strict_reattach_ordered_cycle_examples={format_cycle_examples(trace['strict_reattach_ordered_cycle_examples'])} "
        f"profiled_call_count={profile_stats['total_count']} "
        f"stats_file_count={profile_stats['stats_file_count']} "
        f"stats_target_rows={profile_stats['target_rows']} "
        f"stats_malformed_rows={profile_stats['malformed_rows']} "
        f"stats_duplicate_target_rows={profile_stats['duplicate_target_rows']} "
        f"calibrated_overhead_per_call={profile_stats['overhead_per_call']:.9e} "
        f"estimated_profile_overhead_per_sec={estimated_profile_overhead_per_sec:.6f} "
        f"estimated_effective_overhead_per_sec={estimated_effective_overhead_per_sec:.6f} "
        f"liveness_samples={liveness_overhead['count']} "
        f"liveness_final_window_s={liveness_overhead['window_s']:.6f} "
        f"liveness_final_effective_overhead={liveness_overhead['final_effective']:.6f} "
        f"liveness_final_attached_overhead={liveness_overhead['final_attached']:.6f} "
        f"liveness_final_transition_tokens={liveness_overhead['final_tokens']:.6f} "
        f"liveness_final_elapsed_s={liveness_overhead['final_elapsed']:.6f} "
        f"liveness_window_count={liveness_overhead['window_count']} "
        f"liveness_window_avg_effective_overhead={liveness_overhead['window_avg_effective']:.6f} "
        f"liveness_window_max_effective_overhead={liveness_overhead['window_max_effective']:.6f} "
        f"liveness_window_avg_attached_overhead={liveness_overhead['window_avg_attached']:.6f} "
        f"liveness_window_max_attached_overhead={liveness_overhead['window_max_attached']:.6f} "
        f"trace_rows={trace['rows']} "
        f"detach_success={trace['detach_success']} "
        f"reattach_success={trace['reattach_success']} "
        f"classify_failed={trace['classify_failed']} "
        f"prepare_failed={trace['prepare_failed']} "
        f"gum_failed={trace['gum_failed']} "
        f"retry_abandoned={trace['retry_abandoned']} "
        f"signal_rows={trace['signal_rows']} "
        f"signal_bad={trace['signal_bad']} "
        f"signal_timeout={trace['signal_timeout']} "
        f"signal_arrival_incomplete={trace['signal_arrival_incomplete']} "
        f"signal_release_incomplete={trace['signal_release_incomplete']} "
        f"backend_rows={trace['backend_rows']} "
        f"backend_failed={trace['backend_failed']} "
        f"backend_timeout={trace['backend_timeout']} "
        f"global_no_admission={trace['global_no_admission']} "
        f"global_warm_wait={trace['global_warm_wait']} "
        f"pacing_blocked={trace['pacing_blocked']} "
        f"all_target_calls={all_target_calls} "
        f"distinct_called_targets={distinct_called_targets} "
        f"distinct_called_hot_targets={distinct_called_hot_targets} "
        f"distinct_called_cold_targets={distinct_called_cold_targets} "
        f"distinct_mutated_hooks={trace['distinct_mutated_hooks']} "
        f"distinct_detached_hooks={trace['distinct_detached_hooks']} "
        f"distinct_reattached_hooks={trace['distinct_reattached_hooks']} "
        f"distinct_batches={trace['distinct_batches']} "
        f"full_batches={trace['full_batches']} "
        f"first_global_detach_batch_id={trace['first_global_detach_batch_id']} "
        f"first_global_detach_batch_size={trace['first_global_detach_batch_size']} "
        f"first_global_detach_batch_time_s={trace['first_global_detach_batch_time_s']:.6f} "
        f"first_global_detach_batch_hooks={trace['first_global_detach_batch_hooks']} "
        f"first_global_detach_batch_hot_hooks={trace['first_global_detach_batch_hot_hooks']} "
        f"detach_success_per_sec={detach_success_per_sec:.3f} "
        f"reattach_success_per_sec={reattach_success_per_sec:.3f} "
        f"batches_per_sec={batches_per_sec:.3f} "
        f"trace_rows_per_sec={trace_rows_per_sec:.3f} "
        f"total_unique_stop_window_us={trace['total_unique_stop_window_us']:.3f} "
        f"stop_window_per_sec={stop_window_per_sec:.6f} "
        f"max_stop_window_bin_per_sec={transition_bins['max_stop_window_bin_per_sec']:.6f} "
        f"stop_window_bins_over_budget={transition_bins['bins_over_budget']} "
        f"max_consecutive_stop_window_over_budget_bins={transition_bins['max_consecutive_over_budget_bins']} "
        f"max_batch={trace['max_batch']} "
        f"max_stop_window_us={trace['max_stop_window_us']:.3f} "
        f"same_hook_oscillations={trace['same_hook_oscillations']} "
        f"global_same_hook_oscillations={trace['global_same_hook_oscillations']} "
        f"distinct_oscillating_hooks={trace['distinct_oscillating_hooks']} "
        f"distinct_oscillating_hot_hooks={trace['distinct_oscillating_hot_hooks']} "
        f"distinct_global_oscillating_hooks={trace['distinct_global_oscillating_hooks']} "
        f"distinct_global_oscillating_hot_hooks={trace['distinct_global_oscillating_hot_hooks']} "
        f"median_oscillations_per_oscillating_hook={trace['median_oscillations_per_oscillating_hook']:.3f} "
        f"top_hook_transition_share={trace['top_hook_transition_share']:.6f} "
        f"top_global_hook_transition_share={trace['top_global_hook_transition_share']:.6f} "
        f"max_hook_oscillations={trace['max_hook_oscillations']} "
        f"detached_hot_hooks={trace['detached_hot_hooks']} "
        f"detached_cold_hooks={trace['detached_cold_hooks']} "
        f"reattached_hot_hooks={trace['reattached_hot_hooks']} "
        f"reattached_cold_hooks={trace['reattached_cold_hooks']} "
        f"shutdown_distinct_hooks={trace['shutdown_distinct_hooks']} "
        f"shutdown_duplicate_hooks={trace['shutdown_duplicate_hooks']} "
        f"shutdown_out_of_range_hooks={trace['shutdown_out_of_range_hooks']} "
        f"shutdown_missing_hooks={trace['shutdown_missing_hooks']} "
        f"distinct_detached_hot_hooks_by_source={format_counter(trace['distinct_detached_hot_hooks_by_source'])} "
        f"distinct_reattached_hot_hooks_by_source={format_counter(trace['distinct_reattached_hot_hooks_by_source'])} "
        f"distinct_oscillating_hot_hooks_by_source={format_counter(trace['distinct_oscillating_hot_hooks_by_source'])} "
        f"source_counts={format_counter(trace['source_counts'])} "
        f"operation_source_counts={format_counter(trace['operation_source_counts'])} "
        f"success_operation_source_counts={format_counter(trace['success_operation_source_counts'])} "
        f"signal_phase_counts={format_counter(trace['signal_phase_counts'])} "
        f"backend_phase_counts={format_counter(trace['backend_phase_counts'])} "
        f"top_symbol_operations={format_counter(trace['symbol_operation_counts'])} "
        f"top_oscillating_hooks={format_counter(trace['oscillation_by_hook'])} "
        f"top_global_oscillating_hooks={format_counter(trace['global_oscillation_by_hook'])}",
        flush=True,
    )

    failed = proc.returncode != 0
    if not OK_RE.search(proc.stdout):
        print("many-target stress fixture did not print success marker", file=sys.stderr)
        failed = True
    if BAD_OUTPUT_RE.search(combined):
        print("many-target stress output contains fatal diagnostic", file=sys.stderr)
        failed = True
    if args.require_peak_text_output and "PEAK done with:" not in proc.stderr:
        print("PEAK text summary was required but not found on stderr", file=sys.stderr)
        failed = True
    if args.require_csv_report and profile_stats["total_count"] == 0:
        print("PEAK CSV report was required but no profiled calls were found", file=sys.stderr)
        failed = True
    if (
        args.require_single_stats_file and
        profile_stats["stats_file_count"] != 1
    ):
        print(
            f"expected exactly one PEAK CSV stats file "
            f"({profile_stats['stats_file_count']} found)",
            file=sys.stderr,
        )
        failed = True
    if (
        args.fail_on_malformed_csv_report and
        (profile_stats["malformed_rows"] != 0 or
         profile_stats["duplicate_target_rows"] != 0)
    ):
        print(
            f"PEAK CSV report had malformed or duplicate target rows "
            f"(malformed={profile_stats['malformed_rows']} "
            f"duplicates={profile_stats['duplicate_target_rows']})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.require_shutdown_trace and
        trace["shutdown_success"] < args.targets
    ):
        print(
            f"shutdown trace rows below configured target count "
            f"({trace['shutdown_success']} < {args.targets})",
            file=sys.stderr,
        )
        failed = True
    if args.require_exact_shutdown_trace and (
        trace["shutdown_distinct_hooks"] != args.targets or
        trace["shutdown_duplicate_hooks"] != 0 or
        trace["shutdown_out_of_range_hooks"] != 0 or
        trace["shutdown_missing_hooks"] != 0
    ):
        print(
            f"shutdown trace did not exactly cover configured targets "
            f"(distinct={trace['shutdown_distinct_hooks']} "
            f"missing={trace['shutdown_missing_hooks']} "
            f"duplicate={trace['shutdown_duplicate_hooks']} "
            f"out_of_range={trace['shutdown_out_of_range_hooks']} "
            f"targets={args.targets})",
            file=sys.stderr,
        )
        failed = True
    if args.require_detach and trace["detach_success"] == 0:
        print("detach trace did not contain a successful detach", file=sys.stderr)
        failed = True
    if args.require_reattach and trace["reattach_success"] == 0:
        print("detach trace did not contain a successful reattach", file=sys.stderr)
        failed = True
    if args.fail_on_classify_failed and trace["classify_failed"] != 0:
        print("detach trace contains classify-failed rows", file=sys.stderr)
        failed = True
    if args.fail_on_prepare_failed and trace["prepare_failed"] != 0:
        print("detach trace contains prepare-failed rows", file=sys.stderr)
        failed = True
    if args.fail_on_gum_failed and trace["gum_failed"] != 0:
        print("detach trace contains gum-failed rows", file=sys.stderr)
        failed = True
    if args.fail_on_retry_abandoned and trace["retry_abandoned"] != 0:
        print("detach trace contains retry-abandoned rows", file=sys.stderr)
        failed = True
    if args.fail_on_signal_bad and trace["signal_bad"] != 0:
        print(
            "detach trace contains bad signal backend rows "
            f"(bad={trace['signal_bad']} timeout={trace['signal_timeout']} "
            f"arrival_incomplete={trace['signal_arrival_incomplete']} "
            f"release_incomplete={trace['signal_release_incomplete']})",
            file=sys.stderr,
        )
        failed = True
    if args.fail_on_backend_failed and trace["backend_failed"] != 0:
        print(
            "detach trace contains failed backend diagnostic rows "
            f"(failed={trace['backend_failed']} "
            f"timeout={trace['backend_timeout']})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_signal_bad is not None and
        trace["signal_bad"] > args.max_signal_bad
    ):
        print(
            f"bad signal backend rows exceeded maximum "
            f"({trace['signal_bad']} > {args.max_signal_bad})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_backend_failed is not None and
        trace["backend_failed"] > args.max_backend_failed
    ):
        print(
            f"failed backend diagnostic rows exceeded maximum "
            f"({trace['backend_failed']} > {args.max_backend_failed})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_pacing_blocked is not None and
        trace["pacing_blocked"] > args.max_pacing_blocked
    ):
        print(
            f"controller pacing-blocked rows exceeded maximum "
            f"({trace['pacing_blocked']} > {args.max_pacing_blocked})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_global_no_admission is not None and
        trace["global_no_admission"] > args.max_global_no_admission
    ):
        print(
            f"global detach no-admission rows exceeded maximum "
            f"({trace['global_no_admission']} > "
            f"{args.max_global_no_admission})",
            file=sys.stderr,
        )
        failed = True
    if trace["rows"] < args.min_trace_rows:
        print(
            f"detach trace rows below required minimum "
            f"({trace['rows']} < {args.min_trace_rows})",
            file=sys.stderr,
        )
        failed = True
    if trace["detach_success"] < args.min_detach_success:
        print(
            f"successful detach count below required minimum "
            f"({trace['detach_success']} < {args.min_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_detach_success is not None
        and trace["detach_success"] > args.max_detach_success
    ):
        print(
            f"successful detach count exceeded maximum "
            f"({trace['detach_success']} > {args.max_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if trace["reattach_success"] < args.min_reattach_success:
        print(
            f"successful reattach count below required minimum "
            f"({trace['reattach_success']} < {args.min_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_reattach_success is not None
        and trace["reattach_success"] > args.max_reattach_success
    ):
        print(
            f"successful reattach count exceeded maximum "
            f"({trace['reattach_success']} > {args.max_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if trace["distinct_mutated_hooks"] < args.min_distinct_mutated_hooks:
        print(
            f"distinct mutated hooks below required minimum "
            f"({trace['distinct_mutated_hooks']} < {args.min_distinct_mutated_hooks})",
            file=sys.stderr,
        )
        failed = True
    if trace["distinct_detached_hooks"] < args.min_distinct_detached_hooks:
        print(
            f"distinct detached hooks below required minimum "
            f"({trace['distinct_detached_hooks']} < "
            f"{args.min_distinct_detached_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_distinct_detached_hooks is not None
        and trace["distinct_detached_hooks"] > args.max_distinct_detached_hooks
    ):
        print(
            f"distinct detached hooks exceeded maximum "
            f"({trace['distinct_detached_hooks']} > "
            f"{args.max_distinct_detached_hooks})",
            file=sys.stderr,
        )
        failed = True
    if trace["distinct_reattached_hooks"] < args.min_distinct_reattached_hooks:
        print(
            f"distinct reattached hooks below required minimum "
            f"({trace['distinct_reattached_hooks']} < "
            f"{args.min_distinct_reattached_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_distinct_reattached_hooks is not None
        and trace["distinct_reattached_hooks"] > args.max_distinct_reattached_hooks
    ):
        print(
            f"distinct reattached hooks exceeded maximum "
            f"({trace['distinct_reattached_hooks']} > "
            f"{args.max_distinct_reattached_hooks})",
            file=sys.stderr,
        )
        failed = True
    if trace["distinct_batches"] < args.min_distinct_batches:
        print(
            f"distinct mutation batches below required minimum "
            f"({trace['distinct_batches']} < {args.min_distinct_batches})",
            file=sys.stderr,
        )
        failed = True
    if trace["first_global_detach_batch_size"] < args.min_first_global_detach_batch_size:
        print(
            f"first global detach batch size below required minimum "
            f"({trace['first_global_detach_batch_size']} < "
            f"{args.min_first_global_detach_batch_size})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["first_global_detach_batch_hot_hooks"] <
        args.min_first_global_detach_batch_hot_hooks
    ):
        print(
            f"first global detach batch hot hooks below required minimum "
            f"({trace['first_global_detach_batch_hot_hooks']} < "
            f"{args.min_first_global_detach_batch_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_first_global_detach_batch_time_s is not None and
        trace["first_global_detach_batch_time_s"] >
        args.max_first_global_detach_batch_time_s
    ):
        print(
            f"first global detach batch arrived too late "
            f"({trace['first_global_detach_batch_time_s']:.6f} > "
            f"{args.max_first_global_detach_batch_time_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_full_batches is not None
        and trace["full_batches"] > args.max_full_batches
    ):
        print(
            f"full mutation batches exceeded maximum "
            f"({trace['full_batches']} > {args.max_full_batches})",
            file=sys.stderr,
        )
        failed = True
    if args.max_batch is not None and trace["max_batch"] > args.max_batch:
        print(
            f"maximum mutation batch size exceeded maximum "
            f"({trace['max_batch']} > {args.max_batch})",
            file=sys.stderr,
        )
        failed = True
    if trace["total_unique_stop_window_us"] < args.min_total_stop_window_us:
        print(
            f"unique stop-window total below required minimum "
            f"({trace['total_unique_stop_window_us']:.3f} < "
            f"{args.min_total_stop_window_us:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_total_stop_window_us is not None
        and trace["total_unique_stop_window_us"] > args.max_total_stop_window_us
    ):
        print(
            f"unique stop-window total exceeded maximum "
            f"({trace['total_unique_stop_window_us']:.3f} > "
            f"{args.max_total_stop_window_us:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_stop_window_us is not None
        and trace["max_stop_window_us"] > args.max_stop_window_us
    ):
        print(
            f"maximum single stop-window exceeded maximum "
            f"({trace['max_stop_window_us']:.3f} > "
            f"{args.max_stop_window_us:.3f})",
            file=sys.stderr,
        )
        failed = True
    if stop_window_per_sec < args.min_stop_window_per_sec:
        print(
            f"stop-window seconds per elapsed second below required minimum "
            f"({stop_window_per_sec:.6f} < {args.min_stop_window_per_sec:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_stop_window_per_sec is not None
        and stop_window_per_sec > args.max_stop_window_per_sec
    ):
        print(
            f"stop-window seconds per elapsed second exceeded maximum "
            f"({stop_window_per_sec:.6f} > {args.max_stop_window_per_sec:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_stop_window_bin_per_sec is not None and
        transition_bins["max_stop_window_bin_per_sec"] >
        args.max_stop_window_bin_per_sec
    ):
        print(
            f"sliding stop-window seconds per elapsed second exceeded maximum "
            f"({transition_bins['max_stop_window_bin_per_sec']:.6f} > "
            f"{args.max_stop_window_bin_per_sec:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_stop_window_over_budget_bins is not None and
        transition_bins["bins_over_budget"] >
        args.max_stop_window_over_budget_bins
    ):
        print(
            f"stop-window over-budget bins exceeded maximum "
            f"({transition_bins['bins_over_budget']} > "
            f"{args.max_stop_window_over_budget_bins})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_consecutive_stop_window_over_budget_bins is not None and
        transition_bins["max_consecutive_over_budget_bins"] >
        args.max_consecutive_stop_window_over_budget_bins
    ):
        print(
            f"consecutive stop-window over-budget bins exceeded maximum "
            f"({transition_bins['max_consecutive_over_budget_bins']} > "
            f"{args.max_consecutive_stop_window_over_budget_bins})",
            file=sys.stderr,
        )
        failed = True
    if reattach_success_per_sec < args.min_reattach_success_per_sec:
        print(
            f"successful reattach rate below required minimum "
            f"({reattach_success_per_sec:.3f} < "
            f"{args.min_reattach_success_per_sec:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_reattach_success_per_sec is not None
        and reattach_success_per_sec > args.max_reattach_success_per_sec
    ):
        print(
            f"successful reattach rate exceeded maximum "
            f"({reattach_success_per_sec:.3f} > "
            f"{args.max_reattach_success_per_sec:.3f})",
            file=sys.stderr,
        )
        failed = True
    if batches_per_sec < args.min_batches_per_sec:
        print(
            f"mutation batch rate below required minimum "
            f"({batches_per_sec:.3f} < {args.min_batches_per_sec:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_batches_per_sec is not None
        and batches_per_sec > args.max_batches_per_sec
    ):
        print(
            f"mutation batch rate exceeded maximum "
            f"({batches_per_sec:.3f} > {args.max_batches_per_sec:.3f})",
            file=sys.stderr,
        )
        failed = True
    if trace["same_hook_oscillations"] < args.min_same_hook_oscillations:
        print(
            f"same-hook oscillations below required minimum "
            f"({trace['same_hook_oscillations']} < "
            f"{args.min_same_hook_oscillations})",
            file=sys.stderr,
        )
        failed = True
    if trace["distinct_oscillating_hooks"] < args.min_distinct_oscillating_hooks:
        print(
            f"distinct oscillating hooks below required minimum "
            f"({trace['distinct_oscillating_hooks']} < "
            f"{args.min_distinct_oscillating_hooks})",
            file=sys.stderr,
        )
        failed = True
    if trace["distinct_oscillating_hot_hooks"] < args.min_distinct_oscillating_hot_hooks:
        print(
            f"distinct oscillating hot hooks below required minimum "
            f"({trace['distinct_oscillating_hot_hooks']} < "
            f"{args.min_distinct_oscillating_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if trace["global_same_hook_oscillations"] < args.min_global_same_hook_oscillations:
        print(
            f"global same-hook oscillations below required minimum "
            f"({trace['global_same_hook_oscillations']} < "
            f"{args.min_global_same_hook_oscillations})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["distinct_global_oscillating_hooks"]
        < args.min_distinct_global_oscillating_hooks
    ):
        print(
            f"distinct global oscillating hooks below required minimum "
            f"({trace['distinct_global_oscillating_hooks']} < "
            f"{args.min_distinct_global_oscillating_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["distinct_global_oscillating_hot_hooks"]
        < args.min_distinct_global_oscillating_hot_hooks
    ):
        print(
            f"distinct global oscillating hot hooks below required minimum "
            f"({trace['distinct_global_oscillating_hot_hooks']} < "
            f"{args.min_distinct_global_oscillating_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["median_oscillations_per_oscillating_hook"] <
        args.min_median_oscillations_per_oscillating_hook
    ):
        print(
            f"median oscillations per oscillating hook below required minimum "
            f"({trace['median_oscillations_per_oscillating_hook']:.3f} < "
            f"{args.min_median_oscillations_per_oscillating_hook:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_top_hook_transition_share is not None and
        trace["top_hook_transition_share"] > args.max_top_hook_transition_share
    ):
        print(
            f"top-hook transition share exceeded maximum "
            f"({trace['top_hook_transition_share']:.6f} > "
            f"{args.max_top_hook_transition_share:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_top_global_hook_transition_share is not None and
        trace["top_global_hook_transition_share"] >
        args.max_top_global_hook_transition_share
    ):
        print(
            f"top global-hook transition share exceeded maximum "
            f"({trace['top_global_hook_transition_share']:.6f} > "
            f"{args.max_top_global_hook_transition_share:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_same_hook_oscillations is not None
        and trace["same_hook_oscillations"] > args.max_same_hook_oscillations
    ):
        print(
            f"same-hook oscillations exceeded maximum "
            f"({trace['same_hook_oscillations']} > "
            f"{args.max_same_hook_oscillations})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_hook_oscillations is not None
        and trace["max_hook_oscillations"] > args.max_hook_oscillations
    ):
        print(
            f"per-hook oscillation count exceeded maximum "
            f"({trace['max_hook_oscillations']} > "
            f"{args.max_hook_oscillations})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["success_operation_source_counts"].get("reattach:per-target-heartbeat", 0)
        < args.min_per_target_reattach_success
    ):
        print(
            "per-target heartbeat reattach successes below required minimum "
            f"({trace['success_operation_source_counts'].get('reattach:per-target-heartbeat', 0)} "
            f"< {args.min_per_target_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if global_heartbeat_success_count(trace, "reattach") < args.min_global_reattach_success:
        print(
            "global heartbeat reattach successes below required minimum "
            f"({global_heartbeat_success_count(trace, 'reattach')} "
            f"< {args.min_global_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["success_operation_source_counts"].get("detach:per-target-heartbeat", 0)
        < args.min_per_target_detach_success
    ):
        print(
            "per-target heartbeat detach successes below required minimum "
            f"({trace['success_operation_source_counts'].get('detach:per-target-heartbeat', 0)} "
            f"< {args.min_per_target_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if global_heartbeat_success_count(trace, "detach") < args.min_global_detach_success:
        print(
            "global heartbeat detach successes below required minimum "
            f"({global_heartbeat_success_count(trace, 'detach')} "
            f"< {args.min_global_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_per_target_reattach_success is not None
        and trace["success_operation_source_counts"].get("reattach:per-target-heartbeat", 0)
        > args.max_per_target_reattach_success
    ):
        print(
            "per-target heartbeat reattach successes exceeded maximum "
            f"({trace['success_operation_source_counts'].get('reattach:per-target-heartbeat', 0)} "
            f"> {args.max_per_target_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_global_reattach_success is not None
        and global_heartbeat_success_count(trace, "reattach")
        > args.max_global_reattach_success
    ):
        print(
            "global heartbeat reattach successes exceeded maximum "
            f"({global_heartbeat_success_count(trace, 'reattach')} "
            f"> {args.max_global_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_per_target_detach_success is not None
        and trace["success_operation_source_counts"].get("detach:per-target-heartbeat", 0)
        > args.max_per_target_detach_success
    ):
        print(
            "per-target heartbeat detach successes exceeded maximum "
            f"({trace['success_operation_source_counts'].get('detach:per-target-heartbeat', 0)} "
            f"> {args.max_per_target_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_global_detach_success is not None
        and global_heartbeat_success_count(trace, "detach")
        > args.max_global_detach_success
    ):
        print(
            "global heartbeat detach successes exceeded maximum "
            f"({global_heartbeat_success_count(trace, 'detach')} "
            f"> {args.max_global_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["success_operation_source_counts"].get(
            "detach:global-overhead-recovery", 0
        )
        < args.min_global_overhead_recovery_detach_success
    ):
        print(
            "global overhead recovery detach successes below required minimum "
            f"({trace['success_operation_source_counts'].get('detach:global-overhead-recovery', 0)} "
            f"< {args.min_global_overhead_recovery_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        global_heartbeat_counter_count(
            trace["distinct_detached_hot_hooks_by_source"]
        ) < args.min_global_detached_hot_hooks
    ):
        print(
            "global heartbeat distinct detached hot hooks below required minimum "
            f"({global_heartbeat_counter_count(trace['distinct_detached_hot_hooks_by_source'])} "
            f"< {args.min_global_detached_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        global_heartbeat_counter_count(
            trace["distinct_reattached_hot_hooks_by_source"]
        ) < args.min_global_reattached_hot_hooks
    ):
        print(
            "global heartbeat distinct reattached hot hooks below required minimum "
            f"({global_heartbeat_counter_count(trace['distinct_reattached_hot_hooks_by_source'])} "
            f"< {args.min_global_reattached_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        global_heartbeat_counter_count(
            trace["distinct_oscillating_hot_hooks_by_source"]
        ) < args.min_global_oscillating_hot_hooks
    ):
        print(
            "global heartbeat distinct oscillating hot hooks below required minimum "
            f"({global_heartbeat_counter_count(trace['distinct_oscillating_hot_hooks_by_source'])} "
            f"< {args.min_global_oscillating_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_global_overhead_recovery_detach_success is not None and
        trace["success_operation_source_counts"].get(
            "detach:global-overhead-recovery", 0
        ) > args.max_global_overhead_recovery_detach_success
    ):
        print(
            "global overhead recovery detach successes exceeded maximum "
            f"({trace['success_operation_source_counts'].get('detach:global-overhead-recovery', 0)} "
            f"> {args.max_global_overhead_recovery_detach_success})",
            file=sys.stderr,
        )
        failed = True
    if all_target_calls < args.min_all_target_calls:
        print(
            f"all-target call count below required minimum "
            f"({all_target_calls} < {args.min_all_target_calls})",
            file=sys.stderr,
        )
        failed = True
    if total_calls < args.min_total_calls:
        print(
            f"total call count below required minimum "
            f"({total_calls} < {args.min_total_calls})",
            file=sys.stderr,
        )
        failed = True
    if phase_cold_target0_calls < args.min_phase_cold_target0_calls:
        print(
            f"phase cold target0 calls below required minimum "
            f"({phase_cold_target0_calls} < {args.min_phase_cold_target0_calls})",
            file=sys.stderr,
        )
        failed = True
    if rotating_hot_calls < args.min_rotating_hot_calls:
        print(
            f"rotating hot calls below required minimum "
            f"({rotating_hot_calls} < {args.min_rotating_hot_calls})",
            file=sys.stderr,
        )
        failed = True
    if cold_sweep_calls < args.min_cold_sweep_calls:
        print(
            f"cold sweep calls below required minimum "
            f"({cold_sweep_calls} < {args.min_cold_sweep_calls})",
            file=sys.stderr,
        )
        failed = True
    if peak_target0_count < args.min_peak_target0_count:
        print(
            f"PEAK target0 count below required minimum "
            f"({peak_target0_count} < {args.min_peak_target0_count})",
            file=sys.stderr,
        )
        failed = True
    if target0_post_detach_count < args.min_target0_post_detach_count:
        print(
            f"PEAK target0 post-detach count below required minimum "
            f"({target0_post_detach_count} < "
            f"{args.min_target0_post_detach_count})",
            file=sys.stderr,
        )
        failed = True
    if trace["target0_reattach_success"] < args.min_target0_reattach_success:
        print(
            f"target0 reattach successes below required minimum "
            f"({trace['target0_reattach_success']} < "
            f"{args.min_target0_reattach_success})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_detached_cold_hooks is not None and
        trace["detached_cold_hooks"] > args.max_detached_cold_hooks
    ):
        print(
            f"detached cold hooks exceeded maximum "
            f"({trace['detached_cold_hooks']} > "
            f"{args.max_detached_cold_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_reattached_cold_hooks is not None and
        trace["reattached_cold_hooks"] > args.max_reattached_cold_hooks
    ):
        print(
            f"reattached cold hooks exceeded maximum "
            f"({trace['reattached_cold_hooks']} > "
            f"{args.max_reattached_cold_hooks})",
            file=sys.stderr,
        )
        failed = True
    if coverage["hot_profiled_targets"] < args.min_hot_profiled_targets:
        print(
            f"hot profiled targets below required minimum "
            f"({coverage['hot_profiled_targets']} < "
            f"{args.min_hot_profiled_targets})",
            file=sys.stderr,
        )
        failed = True
    if coverage["hot_profiled_target_ratio"] < args.min_hot_profiled_target_ratio:
        print(
            f"hot profiled target ratio below required minimum "
            f"({coverage['hot_profiled_target_ratio']:.6f} < "
            f"{args.min_hot_profiled_target_ratio:.6f})",
            file=sys.stderr,
        )
        failed = True
    if coverage["hot_total_coverage"] < args.min_hot_total_coverage:
        print(
            f"hot total coverage below required minimum "
            f"({coverage['hot_total_coverage']:.6f} < "
            f"{args.min_hot_total_coverage:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_hot_top_profiled_share is not None and
        coverage["hot_top_profiled_share"] > args.max_hot_top_profiled_share
    ):
        print(
            f"hot top profiled share exceeded maximum "
            f"({coverage['hot_top_profiled_share']:.6f} > "
            f"{args.max_hot_top_profiled_share:.6f})",
            file=sys.stderr,
        )
        failed = True
    if sampling_bins["bins_with_reattach"] < args.min_sampling_bins_with_reattach:
        print(
            f"sampling bins with reattach below required minimum "
            f"({sampling_bins['bins_with_reattach']} < "
            f"{args.min_sampling_bins_with_reattach})",
            file=sys.stderr,
        )
        failed = True
    if (
        sampling_bins["bins_with_hot_reattach"] <
        args.min_sampling_bins_with_hot_reattach
    ):
        print(
            f"sampling bins with hot reattach below required minimum "
            f"({sampling_bins['bins_with_hot_reattach']} < "
            f"{args.min_sampling_bins_with_hot_reattach})",
            file=sys.stderr,
        )
        failed = True
    if (
        sampling_bins["bins_with_global_hot_reattach"] <
        args.min_sampling_bins_with_global_hot_reattach
    ):
        print(
            f"sampling bins with global hot reattach below required minimum "
            f"({sampling_bins['bins_with_global_hot_reattach']} < "
            f"{args.min_sampling_bins_with_global_hot_reattach})",
            file=sys.stderr,
        )
        failed = True
    if (
        sampling_bins["hot_reattached_p50"] <
        args.min_sampling_hot_reattached_p50
    ):
        print(
            f"sampling hot-reattached p50 below required minimum "
            f"({sampling_bins['hot_reattached_p50']:.3f} < "
            f"{args.min_sampling_hot_reattached_p50:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        sampling_bins["global_hot_reattached_p50"] <
        args.min_sampling_global_hot_reattached_p50
    ):
        print(
            f"sampling global-hot-reattached p50 below required minimum "
            f"({sampling_bins['global_hot_reattached_p50']:.3f} < "
            f"{args.min_sampling_global_hot_reattached_p50:.3f})",
            file=sys.stderr,
        )
        failed = True
    if (
        sampling_bins["post_detach_bins_with_attached_hot"] <
        args.min_post_detach_bins_with_attached_hot
    ):
        print(
            f"post-detach bins with attached hot hooks below required minimum "
            f"({sampling_bins['post_detach_bins_with_attached_hot']} < "
            f"{args.min_post_detach_bins_with_attached_hot})",
            file=sys.stderr,
        )
        failed = True
    if (
        sampling_bins["attached_hot_p50"] <
        args.min_sampling_attached_hot_p50
    ):
        print(
            f"sampling attached-hot p50 below required minimum "
            f"({sampling_bins['attached_hot_p50']:.3f} < "
            f"{args.min_sampling_attached_hot_p50:.3f})",
            file=sys.stderr,
        )
        failed = True
    if sampling_bins["reattach_span_s"] < args.min_sampling_reattach_span_s:
        print(
            f"sampling reattach span below required minimum "
            f"({sampling_bins['reattach_span_s']:.6f} < "
            f"{args.min_sampling_reattach_span_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        global_hot_reattach_progress["final_window_count"] <
        args.min_global_hot_reattach_final_window
    ):
        print(
            "global hot reattach final-window count below required minimum "
            f"({global_hot_reattach_progress['final_window_count']} < "
            f"{args.min_global_hot_reattach_final_window})",
            file=sys.stderr,
        )
        failed = True
    if (
        global_hot_reattach_progress["final_window_distinct_hooks"] <
        args.min_global_hot_reattach_final_window_distinct_hooks
    ):
        print(
            "global hot reattach final-window distinct hooks below required "
            "minimum "
            f"({global_hot_reattach_progress['final_window_distinct_hooks']} < "
            f"{args.min_global_hot_reattach_final_window_distinct_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_global_hot_reattach_gap_s is not None and
        global_hot_reattach_progress["max_gap_s"] >
        args.max_global_hot_reattach_gap_s
    ):
        print(
            "global hot reattach max gap exceeded maximum "
            f"({global_hot_reattach_progress['max_gap_s']:.6f} > "
            f"{args.max_global_hot_reattach_gap_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if trace["reattach_sampled_cycles"] < args.min_reattach_sampled_cycles:
        print(
            f"post-reattach sampled cycles below required minimum "
            f"({trace['reattach_sampled_cycles']} < "
            f"{args.min_reattach_sampled_cycles})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["distinct_reattach_sampled_hot_hooks"] <
        args.min_distinct_reattach_sampled_hot_hooks
    ):
        print(
            f"distinct post-reattach sampled hot hooks below required minimum "
            f"({trace['distinct_reattach_sampled_hot_hooks']} < "
            f"{args.min_distinct_reattach_sampled_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["distinct_global_reattach_sampled_hot_hooks"] <
        args.min_distinct_global_reattach_sampled_hot_hooks
    ):
        print(
            f"distinct global post-reattach sampled hot hooks below required minimum "
            f"({trace['distinct_global_reattach_sampled_hot_hooks']} < "
            f"{args.min_distinct_global_reattach_sampled_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.min_probe_detached_dwell_s is not None and
        trace["probe_detached_dwell_count"] == 0
    ):
        print(
            "probe detached dwell gate requested but no dwell samples were observed",
            file=sys.stderr,
        )
        failed = True
    elif (
        args.min_probe_detached_dwell_s is not None and
        trace["probe_detached_dwell_min_s"] < args.min_probe_detached_dwell_s
    ):
        print(
            "probe detached dwell below required minimum "
            f"({trace['probe_detached_dwell_min_s']:.6f} < "
            f"{args.min_probe_detached_dwell_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.min_global_hot_probe_detached_dwell_count > 0 and
        trace["global_hot_probe_detached_dwell_count"] <
        args.min_global_hot_probe_detached_dwell_count
    ):
        print(
            "global hot probe detached dwell count below required minimum "
            f"({trace['global_hot_probe_detached_dwell_count']} < "
            f"{args.min_global_hot_probe_detached_dwell_count})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["probe_dwell_hot_increase_count"] <
        args.min_probe_dwell_hot_increases
    ):
        print(
            "probe dwell hot increases below required minimum "
            f"({trace['probe_dwell_hot_increase_count']} < "
            f"{args.min_probe_dwell_hot_increases})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_probe_dwell_invalid_count is not None and
        trace["probe_dwell_invalid_count"] >
        args.max_probe_dwell_invalid_count
    ):
        print(
            "probe dwell invalid count exceeded maximum "
            f"({trace['probe_dwell_invalid_count']} > "
            f"{args.max_probe_dwell_invalid_count})",
            file=sys.stderr,
        )
        failed = True
    if trace["probe_closeout_keep_attached"] < args.min_probe_closeout_keep_attached:
        print(
            "probe closeout keep-attached decisions below required minimum "
            f"({trace['probe_closeout_keep_attached']} < "
            f"{args.min_probe_closeout_keep_attached})",
            file=sys.stderr,
        )
        failed = True
    if trace["probe_closeout_hot"] < args.min_probe_closeout_hot:
        print(
            "probe closeout hot decisions below required minimum "
            f"({trace['probe_closeout_hot']} < "
            f"{args.min_probe_closeout_hot})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["trace_ordered_reattach_cycles"] <
        args.min_trace_ordered_reattach_cycles
    ):
        print(
            "trace ordered reattach cycles below required minimum "
            f"({trace['trace_ordered_reattach_cycles']} < "
            f"{args.min_trace_ordered_reattach_cycles})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["trace_ordered_reattach_distinct_hot_hooks"] <
        args.min_trace_ordered_reattach_distinct_hot_hooks
    ):
        print(
            "trace ordered reattach distinct hot hooks below required minimum "
            f"({trace['trace_ordered_reattach_distinct_hot_hooks']} < "
            f"{args.min_trace_ordered_reattach_distinct_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["trace_ordered_global_hot_reattach_cycles"] <
        args.min_trace_ordered_global_hot_reattach_cycles
    ):
        print(
            "trace ordered global hot reattach cycles below required minimum "
            f"({trace['trace_ordered_global_hot_reattach_cycles']} < "
            f"{args.min_trace_ordered_global_hot_reattach_cycles})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["trace_ordered_global_hot_reattach_distinct_hooks"] <
        args.min_trace_ordered_global_hot_reattach_distinct_hooks
    ):
        print(
            "trace ordered global hot reattach distinct hooks below required minimum "
            f"({trace['trace_ordered_global_hot_reattach_distinct_hooks']} < "
            f"{args.min_trace_ordered_global_hot_reattach_distinct_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["probe_dwell_hot_increase_hot_hooks"] <
        args.min_probe_dwell_hot_increase_hot_hooks
    ):
        print(
            "probe dwell hot-increase hot hooks below required minimum "
            f"({trace['probe_dwell_hot_increase_hot_hooks']} < "
            f"{args.min_probe_dwell_hot_increase_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["probe_dwell_hot_increase_global_hot_hooks"] <
        args.min_probe_dwell_hot_increase_global_hot_hooks
    ):
        print(
            "probe dwell global hot-increase hooks below required minimum "
            f"({trace['probe_dwell_hot_increase_global_hot_hooks']} < "
            f"{args.min_probe_dwell_hot_increase_global_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if trace["reattach_sampled_span_s"] < args.min_reattach_sampled_span_s:
        print(
            f"post-reattach sampled span below required minimum "
            f"({trace['reattach_sampled_span_s']:.6f} < "
            f"{args.min_reattach_sampled_span_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["reattach_sampled_call_delta_total"] <
        args.min_reattach_sampled_call_delta_total
    ):
        print(
            "reattach sampled total call delta below required minimum "
            f"({trace['reattach_sampled_call_delta_total']} < "
            f"{args.min_reattach_sampled_call_delta_total})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_cycles"] <
        args.min_strict_reattach_ordered_cycles
    ):
        print(
            "strict ordered reattach cycles below required minimum "
            f"({trace['strict_reattach_ordered_cycles']} < "
            f"{args.min_strict_reattach_ordered_cycles})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_distinct_hooks"] <
        args.min_strict_reattach_ordered_distinct_hooks
    ):
        print(
            "strict ordered reattach distinct hooks below required minimum "
            f"({trace['strict_reattach_ordered_distinct_hooks']} < "
            f"{args.min_strict_reattach_ordered_distinct_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_distinct_hot_hooks"] <
        args.min_strict_reattach_ordered_distinct_hot_hooks
    ):
        print(
            "strict ordered reattach distinct hot hooks below required minimum "
            f"({trace['strict_reattach_ordered_distinct_hot_hooks']} < "
            f"{args.min_strict_reattach_ordered_distinct_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_global_reattach_ordered_hot_cycles"] <
        args.min_strict_global_reattach_ordered_hot_cycles
    ):
        print(
            "strict global ordered reattach hot cycles below required minimum "
            f"({trace['strict_global_reattach_ordered_hot_cycles']} < "
            f"{args.min_strict_global_reattach_ordered_hot_cycles})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_global_reattach_ordered_distinct_hot_hooks"] <
        args.min_strict_global_reattach_ordered_distinct_hot_hooks
    ):
        print(
            "strict global ordered reattach distinct hot hooks below required minimum "
            f"({trace['strict_global_reattach_ordered_distinct_hot_hooks']} < "
            f"{args.min_strict_global_reattach_ordered_distinct_hot_hooks})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_call_delta_total"] <
        args.min_strict_reattach_ordered_call_delta_total
    ):
        print(
            "strict ordered reattach total call delta below required minimum "
            f"({trace['strict_reattach_ordered_call_delta_total']} < "
            f"{args.min_strict_reattach_ordered_call_delta_total})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_sample_delta_total"] <
        args.min_strict_reattach_ordered_sample_delta_total
    ):
        print(
            "strict ordered reattach sample delta below required minimum "
            f"({trace['strict_reattach_ordered_sample_delta_total']} < "
            f"{args.min_strict_reattach_ordered_sample_delta_total})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_strict_reattach_ordered_closeout_span_s is not None and
        trace["strict_reattach_ordered_closeout_span_max_s"] >
        args.max_strict_reattach_ordered_closeout_span_s
    ):
        print(
            "strict ordered reattach closeout span exceeded maximum "
            f"({trace['strict_reattach_ordered_closeout_span_max_s']:.6f} > "
            f"{args.max_strict_reattach_ordered_closeout_span_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_sample_span_s"] <
        args.min_strict_reattach_ordered_sample_span_s
    ):
        print(
            "strict ordered reattach sample span below required minimum "
            f"({trace['strict_reattach_ordered_sample_span_s']:.6f} < "
            f"{args.min_strict_reattach_ordered_sample_span_s:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        trace["strict_reattach_ordered_sample_bins"] <
        args.min_strict_reattach_ordered_sample_bins
    ):
        print(
            "strict ordered reattach sample bins below required minimum "
            f"({trace['strict_reattach_ordered_sample_bins']} < "
            f"{args.min_strict_reattach_ordered_sample_bins})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_estimated_profile_overhead_per_sec is not None and
        estimated_profile_overhead_per_sec >
        args.max_estimated_profile_overhead_per_sec
    ):
        print(
            f"estimated profile overhead/sec exceeded maximum "
            f"({estimated_profile_overhead_per_sec:.6f} > "
            f"{args.max_estimated_profile_overhead_per_sec:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_estimated_effective_overhead_per_sec is not None and
        estimated_effective_overhead_per_sec >
        args.max_estimated_effective_overhead_per_sec
    ):
        print(
            f"estimated effective overhead/sec exceeded maximum "
            f"({estimated_effective_overhead_per_sec:.6f} > "
            f"{args.max_estimated_effective_overhead_per_sec:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_liveness_final_effective_overhead is not None and
        liveness_overhead["final_effective"] >
        args.max_liveness_final_effective_overhead
    ):
        print(
            f"final heartbeat liveness effective overhead exceeded maximum "
            f"({liveness_overhead['final_effective']:.6f} > "
            f"{args.max_liveness_final_effective_overhead:.6f})",
            file=sys.stderr,
        )
        failed = True
    if liveness_overhead["count"] < args.min_liveness_samples:
        print(
            f"heartbeat liveness samples below required minimum "
            f"({liveness_overhead['count']} < {args.min_liveness_samples})",
            file=sys.stderr,
        )
        failed = True
    if liveness_overhead["window_count"] < args.min_liveness_window_count:
        print(
            f"heartbeat liveness final-window samples below required minimum "
            f"({liveness_overhead['window_count']} < "
            f"{args.min_liveness_window_count})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_liveness_window_avg_effective_overhead is not None and
        liveness_overhead["window_avg_effective"] >
        args.max_liveness_window_avg_effective_overhead
    ):
        print(
            f"heartbeat liveness final-window average effective overhead "
            f"exceeded maximum "
            f"({liveness_overhead['window_avg_effective']:.6f} > "
            f"{args.max_liveness_window_avg_effective_overhead:.6f})",
            file=sys.stderr,
        )
        failed = True
    if (
        args.max_liveness_window_max_effective_overhead is not None and
        liveness_overhead["window_max_effective"] >
        args.max_liveness_window_max_effective_overhead
    ):
        print(
            f"heartbeat liveness final-window max effective overhead "
            f"exceeded maximum "
            f"({liveness_overhead['window_max_effective']:.6f} > "
            f"{args.max_liveness_window_max_effective_overhead:.6f})",
            file=sys.stderr,
        )
        failed = True
    if calls_per_sec < args.min_calls_per_sec:
        print(
            f"calls/sec below required minimum "
            f"({calls_per_sec:.3f} < {args.min_calls_per_sec:.3f})",
            file=sys.stderr,
        )
        failed = True
    if args.max_elapsed is not None and elapsed_s > args.max_elapsed:
        print(
            f"elapsed time exceeded maximum "
            f"({elapsed_s:.6f} > {args.max_elapsed:.6f})",
            file=sys.stderr,
        )
        failed = True
    if args.require_quota_completed and not quota_completed:
        print("work quota was not completed", file=sys.stderr)
        failed = True
    if distinct_called_targets < args.min_distinct_called_targets:
        print(
            f"distinct called targets below required minimum "
            f"({distinct_called_targets} < {args.min_distinct_called_targets})",
            file=sys.stderr,
        )
        failed = True
    if distinct_called_hot_targets < args.min_distinct_called_hot_targets:
        print(
            f"distinct called hot targets below required minimum "
            f"({distinct_called_hot_targets} < "
            f"{args.min_distinct_called_hot_targets})",
            file=sys.stderr,
        )
        failed = True
    if distinct_called_cold_targets < args.min_distinct_called_cold_targets:
        print(
            f"distinct called cold targets below required minimum "
            f"({distinct_called_cold_targets} < "
            f"{args.min_distinct_called_cold_targets})",
            file=sys.stderr,
        )
        failed = True
    if args.require_max_batch and trace["max_batch"] < args.churn_batch_size:
        print(
            f"max batch below churn batch size "
            f"({trace['max_batch']} < {args.churn_batch_size})",
            file=sys.stderr,
        )
        failed = True

    if cleanup:
        shutil.rmtree(work_dir, ignore_errors=True)
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--libpeak", type=Path)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--backend", choices=("auto", "helper", "signal"), default="auto")
    parser.add_argument("--threads", type=int, default=64)
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--work-quota", type=int, default=0)
    parser.add_argument("--hot-phase-seconds", type=int, default=0)
    parser.add_argument("--phase-cold-target0-period", type=int, default=0)
    parser.add_argument("--hot-rotation-period-us", type=int, default=0)
    parser.add_argument("--active-hot-targets", type=int, default=64)
    parser.add_argument("--cold-sweep-period", type=int, default=0)
    parser.add_argument("--targets", type=int, default=832)
    parser.add_argument("--hot-targets", type=int, default=256)
    parser.add_argument("--burst", type=int, default=128)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--cleanup", action="store_true")
    parser.add_argument("--thread-slack", type=int, default=64)
    parser.add_argument("--peak-max-threads", type=int, default=512)
    parser.add_argument("--peak-cost", default="0.000000000001")
    parser.add_argument("--overhead-ratio", default="0.000001")
    parser.add_argument("--global-overhead-ratio", default="0.000001")
    parser.add_argument("--global-detach-factor", default="1.2")
    parser.add_argument("--global-reattach-factor", default="0.95")
    parser.add_argument("--heartbeat-interval", default="0.01")
    parser.add_argument("--hibernation-cycle", type=int, default=1)
    parser.add_argument("--detach-count", type=int)
    parser.add_argument("--disable-per-target-heartbeat", dest="per_target_heartbeat", action="store_false")
    parser.add_argument("--disable-global-heartbeat", dest="global_heartbeat", action="store_false")
    parser.add_argument("--disable-reattach", dest="enable_reattach", action="store_false")
    parser.add_argument("--disable-peak", action="store_true")
    parser.add_argument("--call-cold-targets-once", action="store_true")
    parser.add_argument("--hot-only-workload", action="store_true")
    parser.add_argument("--all-target-period", type=int, default=16)
    parser.add_argument("--controller-churn", action="store_true")
    parser.add_argument("--churn-interval-us", type=int, default=10000)
    parser.add_argument("--churn-targets", type=int, default=0)
    parser.add_argument("--churn-batch-size", type=int, default=64)
    parser.add_argument("--churn-workers", type=int, default=1)
    parser.add_argument("--churn-random-percent", type=int, default=0)
    parser.add_argument("--churn-hot-bias-percent", type=int, default=0)
    parser.add_argument("--churn-jitter-us", type=int, default=0)
    parser.add_argument("--min-trace-rows", type=int, default=0)
    parser.add_argument("--min-detach-success", type=int, default=1)
    parser.add_argument("--max-detach-success", type=int)
    parser.add_argument("--min-reattach-success", type=int, default=0)
    parser.add_argument("--max-reattach-success", type=int)
    parser.add_argument("--min-distinct-mutated-hooks", type=int, default=1)
    parser.add_argument("--min-distinct-detached-hooks", type=int, default=0)
    parser.add_argument("--max-distinct-detached-hooks", type=int)
    parser.add_argument("--min-distinct-reattached-hooks", type=int, default=0)
    parser.add_argument("--max-distinct-reattached-hooks", type=int)
    parser.add_argument("--min-distinct-batches", type=int, default=0)
    parser.add_argument("--max-full-batches", type=int)
    parser.add_argument("--max-batch", type=int)
    parser.add_argument("--min-total-stop-window-us", type=float, default=0.0)
    parser.add_argument("--max-total-stop-window-us", type=float)
    parser.add_argument("--max-stop-window-us", type=float)
    parser.add_argument("--min-stop-window-per-sec", type=float, default=0.0)
    parser.add_argument("--max-stop-window-per-sec", type=float)
    parser.add_argument("--min-reattach-success-per-sec", type=float, default=0.0)
    parser.add_argument("--max-reattach-success-per-sec", type=float)
    parser.add_argument("--min-batches-per-sec", type=float, default=0.0)
    parser.add_argument("--max-batches-per-sec", type=float)
    parser.add_argument("--min-same-hook-oscillations", type=int, default=0)
    parser.add_argument("--min-distinct-oscillating-hooks", type=int, default=0)
    parser.add_argument("--min-distinct-oscillating-hot-hooks", type=int, default=0)
    parser.add_argument("--min-global-same-hook-oscillations",
                        type=int,
                        default=0)
    parser.add_argument("--min-distinct-global-oscillating-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-distinct-global-oscillating-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-median-oscillations-per-oscillating-hook",
                        type=float,
                        default=0.0)
    parser.add_argument("--max-top-hook-transition-share", type=float)
    parser.add_argument("--max-top-global-hook-transition-share", type=float)
    parser.add_argument("--max-same-hook-oscillations", type=int)
    parser.add_argument("--max-hook-oscillations", type=int)
    parser.add_argument("--min-per-target-detach-success", type=int, default=0)
    parser.add_argument("--min-global-detach-success", type=int, default=0)
    parser.add_argument("--min-global-overhead-recovery-detach-success",
                        type=int,
                        default=0)
    parser.add_argument("--max-global-overhead-recovery-detach-success",
                        type=int)
    parser.add_argument("--min-per-target-reattach-success", type=int, default=0)
    parser.add_argument("--min-global-reattach-success", type=int, default=0)
    parser.add_argument("--max-per-target-detach-success", type=int)
    parser.add_argument("--max-global-detach-success", type=int)
    parser.add_argument("--max-per-target-reattach-success", type=int)
    parser.add_argument("--max-global-reattach-success", type=int)
    parser.add_argument("--min-all-target-calls", type=int, default=0)
    parser.add_argument("--min-total-calls", type=int, default=0)
    parser.add_argument("--min-phase-cold-target0-calls", type=int, default=0)
    parser.add_argument("--min-rotating-hot-calls", type=int, default=0)
    parser.add_argument("--min-cold-sweep-calls", type=int, default=0)
    parser.add_argument("--min-peak-target0-count", type=int, default=0)
    parser.add_argument("--min-target0-post-detach-count", type=int, default=0)
    parser.add_argument("--min-target0-reattach-success", type=int, default=0)
    parser.add_argument("--max-detached-cold-hooks", type=int)
    parser.add_argument("--max-reattached-cold-hooks", type=int)
    parser.add_argument("--min-hot-profiled-targets", type=int, default=0)
    parser.add_argument("--min-hot-profiled-target-ratio", type=float, default=0.0)
    parser.add_argument("--min-hot-total-coverage", type=float, default=0.0)
    parser.add_argument("--max-hot-top-profiled-share", type=float)
    parser.add_argument("--sample-counts-interval-ms", type=int, default=1000)
    parser.add_argument("--sampling-bin-seconds", type=float, default=1.0)
    parser.add_argument("--min-sampling-bins-with-reattach", type=int, default=0)
    parser.add_argument("--min-post-detach-bins-with-attached-hot",
                        type=int,
                        default=0)
    parser.add_argument("--min-sampling-attached-hot-p50", type=float, default=0.0)
    parser.add_argument("--min-sampling-reattach-span-s", type=float, default=0.0)
    parser.add_argument("--global-hot-reattach-final-window-s",
                        type=float,
                        default=0.0)
    parser.add_argument("--liveness-final-window-s", type=float, default=30.0)
    parser.add_argument("--min-global-hot-reattach-final-window",
                        type=int,
                        default=0)
    parser.add_argument(
        "--min-global-hot-reattach-final-window-distinct-hooks",
        type=int,
        default=0,
    )
    parser.add_argument("--max-global-hot-reattach-gap-s", type=float)
    parser.add_argument("--min-reattach-sampled-cycles", type=int, default=0)
    parser.add_argument("--min-distinct-reattach-sampled-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-distinct-global-reattach-sampled-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-probe-detached-dwell-s", type=float)
    parser.add_argument(
        "--min-global-hot-probe-detached-dwell-count",
        type=int,
        default=0,
    )
    parser.add_argument("--min-probe-dwell-hot-increases",
                        type=int,
                        default=0)
    parser.add_argument("--min-probe-dwell-hot-increase-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-probe-dwell-hot-increase-global-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--max-probe-dwell-invalid-count", type=int)
    parser.add_argument("--min-probe-closeout-keep-attached",
                        type=int,
                        default=0)
    parser.add_argument("--min-probe-closeout-hot", type=int, default=0)
    parser.add_argument("--min-trace-ordered-reattach-cycles",
                        type=int,
                        default=0)
    parser.add_argument("--min-trace-ordered-reattach-distinct-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-trace-ordered-global-hot-reattach-cycles",
                        type=int,
                        default=0)
    parser.add_argument(
        "--min-trace-ordered-global-hot-reattach-distinct-hooks",
        type=int,
        default=0,
    )
    parser.add_argument("--min-reattach-sampled-span-s", type=float, default=0.0)
    parser.add_argument(
        "--min-reattach-sampled-call-delta-total",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-cycles",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-distinct-hooks",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-distinct-hot-hooks",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-global-reattach-ordered-hot-cycles",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-global-reattach-ordered-distinct-hot-hooks",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-call-delta-total",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-sample-delta-total",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--max-strict-reattach-ordered-closeout-span-s",
        type=float,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-sample-span-s",
        type=float,
        default=0.0,
    )
    parser.add_argument(
        "--min-strict-reattach-ordered-sample-bins",
        type=int,
        default=0,
    )
    parser.add_argument("--require-peak-text-output", action="store_true")
    parser.add_argument("--require-csv-report", action="store_true")
    parser.add_argument("--require-shutdown-trace", action="store_true")
    parser.add_argument("--max-estimated-profile-overhead-per-sec", type=float)
    parser.add_argument("--max-estimated-effective-overhead-per-sec", type=float)
    parser.add_argument("--max-liveness-final-effective-overhead", type=float)
    parser.add_argument("--min-liveness-samples", type=int, default=0)
    parser.add_argument("--min-liveness-window-count", type=int, default=0)
    parser.add_argument("--max-liveness-window-avg-effective-overhead",
                        type=float)
    parser.add_argument("--max-liveness-window-max-effective-overhead",
                        type=float)
    parser.add_argument("--min-calls-per-sec", type=float, default=0.0)
    parser.add_argument("--max-elapsed", type=float)
    parser.add_argument("--require-quota-completed", action="store_true")
    parser.add_argument("--min-distinct-called-targets", type=int, default=0)
    parser.add_argument("--min-distinct-called-hot-targets", type=int, default=0)
    parser.add_argument("--min-distinct-called-cold-targets", type=int, default=0)
    parser.add_argument("--require-max-batch", action="store_true")
    parser.add_argument("--min-first-global-detach-batch-size",
                        type=int,
                        default=0)
    parser.add_argument("--min-first-global-detach-batch-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--max-first-global-detach-batch-time-s", type=float)
    parser.add_argument("--min-global-detached-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-global-reattached-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-global-oscillating-hot-hooks",
                        type=int,
                        default=0)
    parser.add_argument("--min-sampling-bins-with-hot-reattach",
                        type=int,
                        default=0)
    parser.add_argument("--min-sampling-bins-with-global-hot-reattach",
                        type=int,
                        default=0)
    parser.add_argument("--min-sampling-hot-reattached-p50",
                        type=float,
                        default=0.0)
    parser.add_argument("--min-sampling-global-hot-reattached-p50",
                        type=float,
                        default=0.0)
    parser.add_argument("--max-stop-window-bin-per-sec", type=float)
    parser.add_argument("--max-stop-window-over-budget-bins", type=int)
    parser.add_argument("--max-consecutive-stop-window-over-budget-bins",
                        type=int)
    parser.add_argument("--require-exact-shutdown-trace", action="store_true")
    parser.add_argument("--require-single-stats-file", action="store_true")
    parser.add_argument("--fail-on-malformed-csv-report", action="store_true")
    parser.add_argument("--no-require-detach", dest="require_detach", action="store_false")
    parser.add_argument("--no-require-reattach", dest="require_reattach", action="store_false")
    parser.add_argument("--allow-classify-failed", dest="fail_on_classify_failed", action="store_false")
    parser.add_argument("--allow-prepare-failed", dest="fail_on_prepare_failed", action="store_false")
    parser.add_argument("--allow-gum-failed", dest="fail_on_gum_failed", action="store_false")
    parser.add_argument(
        "--allow-retry-abandoned",
        dest="fail_on_retry_abandoned",
        action="store_false",
    )
    parser.add_argument("--fail-on-signal-bad", action="store_true")
    parser.add_argument("--fail-on-backend-failed", action="store_true")
    parser.add_argument("--max-signal-bad", type=int)
    parser.add_argument("--max-backend-failed", type=int)
    parser.add_argument("--max-pacing-blocked", type=int)
    parser.add_argument("--max-global-no-admission", type=int)
    parser.add_argument("--extra-env", action="append", default=[])
    parser.set_defaults(
        per_target_heartbeat=True,
        global_heartbeat=True,
        enable_reattach=True,
        require_detach=True,
        require_reattach=True,
        fail_on_classify_failed=True,
        fail_on_prepare_failed=True,
        fail_on_gum_failed=True,
        fail_on_retry_abandoned=True,
    )
    args = parser.parse_args()

    args.exe = args.exe.resolve()
    if args.libpeak is not None:
        args.libpeak = args.libpeak.resolve()
    if args.helper is not None:
        args.helper = args.helper.resolve()
    if args.work_dir is not None:
        args.work_dir = args.work_dir.resolve()

    if args.hot_targets > args.targets:
        parser.error("--hot-targets cannot exceed --targets")
    if args.hot_rotation_period_us < 0:
        parser.error("--hot-rotation-period-us cannot be negative")
    if args.hot_rotation_period_us > 0 and (
        args.active_hot_targets <= 0 or args.active_hot_targets > args.hot_targets
    ):
        parser.error("--active-hot-targets must be in [1, --hot-targets]")
    if args.cold_sweep_period < 0:
        parser.error("--cold-sweep-period cannot be negative")
    if args.churn_targets == 0:
        args.churn_targets = args.hot_targets
    if args.churn_targets < 0 or args.churn_targets > args.targets:
        parser.error("--churn-targets must be zero or between 1 and --targets")
    if args.churn_batch_size > 64:
        parser.error("--churn-batch-size cannot exceed controller batch limit 64")
    if args.controller_churn and args.churn_batch_size > args.churn_targets:
        parser.error("--churn-batch-size cannot exceed --churn-targets")
    if args.churn_workers <= 0:
        parser.error("--churn-workers must be positive")
    if args.churn_random_percent < 0 or args.churn_random_percent > 100:
        parser.error("--churn-random-percent must be between 0 and 100")
    if args.churn_hot_bias_percent < 0 or args.churn_hot_bias_percent > 100:
        parser.error("--churn-hot-bias-percent must be between 0 and 100")
    if args.churn_jitter_us < 0:
        parser.error("--churn-jitter-us cannot be negative")
    if args.targets > FIXTURE_MAX_TARGETS:
        parser.error(
            f"--targets cannot exceed the fixture's {FIXTURE_MAX_TARGETS} exported targets"
        )
    if args.samples <= 0:
        parser.error("--samples must be positive")
    if not args.disable_peak and args.libpeak is None:
        parser.error("--libpeak is required unless --disable-peak is set")
    if args.disable_peak:
        args.require_detach = False
        args.require_reattach = False
        args.min_detach_success = 0
        args.min_reattach_success = 0
        args.min_distinct_mutated_hooks = 0
        args.min_distinct_detached_hooks = 0
        args.min_distinct_reattached_hooks = 0
    if not args.enable_reattach and args.require_reattach:
        args.require_reattach = False
    if args.backend in {"auto", "helper"} and args.helper is None:
        print(
            "warning: no --helper supplied; auto/helper backends will use PEAK default helper discovery",
            file=sys.stderr,
        )
    for item in args.extra_env:
        if "=" not in item:
            parser.error("--extra-env entries must be KEY=VALUE")

    failures = 0
    for sample in range(args.samples):
        sample_args = copy.copy(args)
        if args.work_dir is not None and args.samples > 1:
            sample_args.work_dir = args.work_dir / f"sample-{sample}"
        print(f"many_targets_stress_sample_start sample={sample}", flush=True)
        rc = run_once(sample_args)
        print(f"many_targets_stress_sample_end sample={sample} rc={rc}", flush=True)
        if rc != 0:
            failures += 1

    if failures != 0:
        print(
            f"many_targets_stress_failed samples={args.samples} failures={failures}",
            file=sys.stderr,
        )
        return 1
    print(f"many_targets_stress_all_ok samples={args.samples}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
