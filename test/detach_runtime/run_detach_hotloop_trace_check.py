#!/usr/bin/env python3
"""Short strict physical-detach trace regression runner."""

import argparse
import csv
import os
import re
import subprocess
import sys


CALLS_PER_SEC_RE = re.compile(r"calls_per_sec=([0-9.]+)")
SPAWNED_THREADS_RE = re.compile(r"\bspawned_threads=([0-9]+)\b")
DETACHED_MARKER_RE = re.compile(
    r"(?m)^\|\s*peak_detach_hot_target\*+\s*\|\s*[1-9][0-9]*\s*\|"
)
REATTACHED_MARKER_RE = re.compile(
    r"(?m)^\|\s*peak_detach_hot_target\*\*\s*\|\s*[1-9][0-9]*\s*\|"
)
BAD_OUTPUT_RE = re.compile(
    r"fatal|not proven safe|leaving listener state alive|"
    r"detach helper shutdown failed|Gum .* failed|"
    r"tracked thread snapshot exceeded|thread id .* exceeds|"
    r"timed out draining pending target hook detach/reattach requests",
    re.IGNORECASE,
)
TRANSITION_SKIP_RE = re.compile(
    r"skipping Gum (detach|reattach)|classify-failed",
    re.IGNORECASE,
)
TARGET_SYMBOL = "peak_detach_hot_target"
PAIRED_TARGET_SYMBOL = "peak_detach_hot_target_two"
COLD_TARGET_SYMBOL = "peak_detach_cold_target"
TRACE_REQUEST_SOURCE_INDEX = 17
TRACE_REQUEST_CALLS_INDEX = 18
COLD_MARKED_RE = re.compile(
    r"(?m)^\|\s*peak_detach_cold_target\*+\s*\|\s*1\s*\|"
)
COLD_UNMARKED_RE = re.compile(
    r"(?m)^\|\s*peak_detach_cold_target\s*\|\s*1\s*\|"
)


def make_env(args, sample):
    env = os.environ.copy()
    max_threads = max(args.peak_max_threads, args.threads + args.thread_slack)
    peak_target = TARGET_SYMBOL
    if args.paired_targets:
        peak_target = f"{TARGET_SYMBOL},{PAIRED_TARGET_SYMBOL}"
    if args.cold_one_shot_target:
        peak_target = f"{peak_target},{COLD_TARGET_SYMBOL}"
    env.update(
        {
            "LD_PRELOAD": args.libpeak,
            "PEAK_TARGET": peak_target,
            "PEAK_MAX_NUM_THREADS": str(max_threads),
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": (
                "1" if args.enable_per_target_heartbeat else "0"
            ),
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": (
                "1" if args.enable_global_heartbeat else "0"
            ),
            "PEAK_ENABLE_REATTACH": "1" if args.enable_reattach else "0",
            "PEAK_COST": args.peak_cost,
            "PEAK_OVERHEAD_RATIO": args.overhead_ratio,
            "PEAK_GLOBAL_OVERHEAD_RATIO": args.global_overhead_ratio,
            "PEAK_GLOBAL_DETACH_FACTOR": args.global_detach_factor,
            "PEAK_GLOBAL_REATTACH_FACTOR": args.global_reattach_factor,
            "PEAK_HEARTBEAT_INTERVAL": args.heartbeat_interval,
            "PEAK_HIBERNATION_CYCLE": str(args.hibernation_cycle),
            "PEAK_HB_MIN_US": args.hb_min_us,
            "PEAK_HB_MAX_US": args.hb_max_us,
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": "strict",
            "PEAK_STATSLOG_PATH": f"{args.stats_prefix}-{sample}",
        }
    )
    if args.detach_count:
        env["PEAK_DETACH_COUNT"] = args.detach_count
    if args.reattach_cooldown_ms:
        env["PEAK_REATTACH_COOLDOWN_MS"] = args.reattach_cooldown_ms
    if args.detach_backend:
        env["PEAK_DETACH_BACKEND"] = args.detach_backend
        if args.detach_backend == "signal":
            env["PEAK_SAFE_DETACH_MODE"] = "signal"
            env.setdefault("PEAK_DETACH_HELPER", "/no/such/peak_detach_helper")
    if args.detach_helper:
        env["PEAK_DETACH_HELPER"] = args.detach_helper
    if args.helper_retry_log_prefix:
        env["G_TEST_PEAK_DETACH_HELPER_STOP_RETRY_ONCE"] = "1"
        env["G_TEST_PEAK_DETACH_HELPER_RETRY_LOG"] = helper_retry_log_path(args, sample)
    if args.trace_prefix:
        env["PEAK_DETACH_TRACE_PATH"] = f"{args.trace_prefix}-{sample}.csv"
    return env


def trace_path(args, sample):
    if not args.trace_prefix:
        return None
    return f"{args.trace_prefix}-{sample}.csv"


def helper_retry_log_path(args, sample):
    if not args.helper_retry_log_prefix:
        return None
    return f"{args.helper_retry_log_prefix}-{sample}.log"


def reset_trace(args, sample):
    for path in (trace_path(args, sample), helper_retry_log_path(args, sample)):
        if not path:
            continue
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass


def helper_retry_was_exercised(args, sample):
    path = helper_retry_log_path(args, sample)
    if not path:
        return True
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return "stop_snapshot_retry" in handle.read()
    except OSError:
        return False


def trace_row_has_diagnostics(fields, expected_last_retry_status=None):
    if len(fields) < 12:
        return False
    if expected_last_retry_status is not None and len(fields) < 13:
        return False
    try:
        retry_count = int(fields[7])
        pending_age_s = float(fields[8])
        batch_size = int(fields[9])
        stop_window_us = float(fields[10])
        batch_id = int(fields[11])
    except ValueError:
        return False
    if (expected_last_retry_status is not None and
            fields[12] != expected_last_retry_status):
        return False
    return (retry_count >= 0 and pending_age_s >= 0.0 and
            batch_size >= 1 and stop_window_us > 0.0 and batch_id != 0)


def trace_has_success(args, sample, operation):
    path = trace_path(args, sample)
    if not path:
        return True

    try:
        with open(path, "r", encoding="utf-8") as handle:
            rows = list(csv.reader(handle))
    except OSError:
        return False

    for fields in rows:
        if (len(fields) >= 7 and
                fields[2] == TARGET_SYMBOL and
                fields[3] == operation and
                fields[4] == "success" and
                fields[5] == "1" and
                fields[6] == "safe" and
                (not args.require_trace_diagnostics or
                 trace_row_has_diagnostics(fields))):
            return True
    return False


def read_trace_rows(args, sample):
    path = trace_path(args, sample)
    if not path:
        return None

    try:
        with open(path, "r", encoding="utf-8") as handle:
            return list(csv.reader(handle))
    except OSError:
        return None


def trace_has_required_batch(args, sample, operation, required_size):
    if required_size <= 0:
        return True

    rows = read_trace_rows(args, sample)
    if rows is None:
        return False

    required_symbols = {TARGET_SYMBOL}
    if args.paired_targets:
        required_symbols.add(PAIRED_TARGET_SYMBOL)
    batch_groups = {}

    for fields in rows:
        if len(fields) < 7 or fields[2] not in required_symbols:
            continue
        batch_size = 0
        if len(fields) >= 10:
            try:
                batch_size = int(fields[9])
            except ValueError:
                batch_size = 0
        if fields[3] == operation and (
                fields[4] in {"prepare-failed", "gum-failed"} or
                fields[6] == "classify-failed"):
            if args.fail_on_transition_skips:
                print(f"unexpected transition row for {fields[2]}: {fields}",
                      file=sys.stderr)
                return False
            continue
        if (len(fields) >= 12 and
                fields[3] == operation and
                fields[4] == "success" and
                fields[5] == "1" and
                fields[6] == "safe" and
                fields[11] != "0" and
                batch_size >= required_size and
                (not args.require_trace_diagnostics or
                 trace_row_has_diagnostics(fields))):
            batch_key = (fields[3], fields[11])
            batch_groups.setdefault(batch_key, set()).add(fields[2])

    for symbols in batch_groups.values():
        if required_symbols.issubset(symbols):
            return True

    print(f"missing same-batch {operation} trace rows for "
          f"{sorted(required_symbols)}",
          file=sys.stderr)
    for key, symbols in sorted(batch_groups.items()):
        missing = required_symbols - symbols
        if missing:
            print(f"batch key {key} missing {sorted(missing)}",
              file=sys.stderr)
    return False


def trace_has_required_request_source(args, sample):
    if not args.require_detach_source:
        return True

    rows = read_trace_rows(args, sample)
    if rows is None:
        return False

    tracked_symbols = {TARGET_SYMBOL}
    if args.paired_targets:
        tracked_symbols.add(PAIRED_TARGET_SYMBOL)

    for fields in rows:
        if (len(fields) <= TRACE_REQUEST_CALLS_INDEX or
                fields[2] not in tracked_symbols or
                fields[3] != "detach" or
                fields[4] != "success" or
                fields[5] != "1" or
                fields[6] != "safe"):
            continue
        if fields[TRACE_REQUEST_SOURCE_INDEX] != args.require_detach_source:
            continue
        try:
            request_calls = int(fields[TRACE_REQUEST_CALLS_INDEX])
        except ValueError:
            continue
        if request_calls >= args.min_detach_request_calls:
            return True

    print(
        f"missing detach trace source={args.require_detach_source} "
        f"min_request_calls={args.min_detach_request_calls}",
        file=sys.stderr,
    )
    return False


def trace_transition_counts(args, sample):
    rows = read_trace_rows(args, sample)
    counts = {
        "detach": {"classify_failed": 0, "prepare_failed": 0, "gum_failed": 0, "success": 0},
        "reattach": {"classify_failed": 0, "prepare_failed": 0, "gum_failed": 0, "success": 0},
    }
    if rows is None:
        return counts

    tracked_symbols = {TARGET_SYMBOL}
    if args.paired_targets:
        tracked_symbols.add(PAIRED_TARGET_SYMBOL)

    for fields in rows:
        if len(fields) < 7 or fields[2] not in tracked_symbols:
            continue
        operation = fields[3]
        if operation not in counts:
            continue
        if fields[6] == "classify-failed" or (
                len(fields) >= 13 and fields[12] == "classify-failed"):
            counts[operation]["classify_failed"] += 1
        if fields[4] == "prepare-failed":
            counts[operation]["prepare_failed"] += 1
        if fields[4] == "gum-failed":
            counts[operation]["gum_failed"] += 1
        if fields[4] == "success":
            counts[operation]["success"] += 1

    return counts


def trace_transition_count(counts, operation, name):
    if operation == "all":
        return sum(values[name] for values in counts.values())
    return counts[operation][name]


def trace_has_helper_unavailable(args, sample):
    rows = read_trace_rows(args, sample)
    if rows is None:
        return False

    for fields in rows:
        if len(fields) < 14 or fields[2] != TARGET_SYMBOL:
            continue
        if (fields[4] == "prepare-failed" and
                fields[6] in {"permission-denied", "unsupported"} and
                fields[13] == "helper-stop-status"):
            return True
    return False


def trace_transition_limits_ok(args, sample):
    counts = trace_transition_counts(args, sample)
    limits = (
        ("all", "classify_failed", args.max_classify_failed),
        ("detach", "classify_failed", args.max_detach_classify_failed),
        ("reattach", "classify_failed", args.max_reattach_classify_failed),
        ("reattach", "success", args.max_reattach_success),
    )
    ok = True

    if args.fail_on_transition_skips:
        limits = limits + (
            ("all", "classify_failed", 0),
            ("all", "prepare_failed", 0),
            ("all", "gum_failed", 0),
        )

    for operation, name, limit in limits:
        if limit < 0:
            continue
        value = trace_transition_count(counts, operation, name)
        if value > limit:
            print(
                f"trace transition limit exceeded sample={sample} "
                f"operation={operation} {name}={value} limit={limit}",
                file=sys.stderr,
            )
            ok = False

    return ok


def run_sample(args, sample):
    cmd = [
        args.exe,
        "--threads",
        str(args.threads),
        "--seconds",
        str(args.seconds),
    ]
    if args.paired_targets:
        cmd.append("--paired-targets")
    if args.cold_one_shot_target:
        cmd.append("--cold-one-shot-target")
    if args.spawn_transient_threads:
        cmd.extend([
            "--spawn-transient-threads",
            "--spawner-threads",
            str(args.spawner_threads),
        ])
    if args.request_detach_after_start:
        cmd.append("--request-detach-after-start")
    if args.wait_for_hook_before_start:
        cmd.append("--wait-for-hook-before-start")
    reset_trace(args, sample)
    completed = subprocess.run(
        cmd,
        env=make_env(args, sample),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        timeout=args.timeout,
        check=False,
    )
    output = completed.stdout
    calls_match = CALLS_PER_SEC_RE.search(output)
    spawned_match = SPAWNED_THREADS_RE.search(output)
    required_marker = REATTACHED_MARKER_RE if args.require_reattach else DETACHED_MARKER_RE
    detached = DETACHED_MARKER_RE.search(output) is not None
    marker_ok = required_marker.search(output) is not None
    trace_detached = trace_has_success(args, sample, "detach")
    trace_reattached = trace_has_success(args, sample, "reattach")
    trace_detach_batched = trace_has_required_batch(
        args, sample, "detach", args.require_detach_batch_size)
    trace_reattach_batched = trace_has_required_batch(
        args, sample, "reattach", args.require_reattach_batch_size)
    trace_source_ok = trace_has_required_request_source(args, sample)
    trace_transition_ok = trace_transition_limits_ok(args, sample)
    helper_retry_ok = helper_retry_was_exercised(args, sample)
    bad_output = BAD_OUTPUT_RE.search(output)
    transition_skip = TRANSITION_SKIP_RE.search(output)
    cold_marked = COLD_MARKED_RE.search(output) is not None
    cold_unmarked = COLD_UNMARKED_RE.search(output) is not None
    cold_trace_transition = False
    if args.cold_one_shot_target:
        rows = read_trace_rows(args, sample)
        if rows is not None:
            cold_trace_transition = any(
                len(fields) >= 4 and
                fields[2] == COLD_TARGET_SYMBOL and
                fields[3] in {"detach", "reattach"}
                for fields in rows
            )
    calls_per_sec = float(calls_match.group(1)) if calls_match else 0.0
    spawned_threads = int(spawned_match.group(1)) if spawned_match else 0

    failed = (
        completed.returncode != 0 or
        calls_match is None or
        calls_per_sec <= 0.0 or
        not marker_ok or
        not trace_detached or
        not trace_detach_batched or
        not trace_reattach_batched or
        not trace_source_ok or
        not trace_transition_ok or
        not helper_retry_ok or
        (args.require_reattach and not trace_reattached) or
        (args.spawn_transient_threads and spawned_threads <= 0) or
        (args.cold_one_shot_target and
         (cold_marked or not cold_unmarked or cold_trace_transition)) or
        bad_output or
        (args.fail_on_transition_skips and transition_skip)
    )
    if failed:
        if args.skip_helper_unavailable and trace_has_helper_unavailable(args, sample):
            print("helper backend unavailable for this platform/policy; skipping")
            raise SystemExit(77)
        print(f"trace check sample {sample} failed rc={completed.returncode}",
              file=sys.stderr)
        if calls_match is None:
            print("missing calls_per_sec output", file=sys.stderr)
        elif calls_per_sec <= 0.0:
            print("calls_per_sec must be positive", file=sys.stderr)
        if not marker_ok:
            if args.require_reattach:
                print("missing strict physical detach+reattach marker", file=sys.stderr)
            else:
                print("missing strict physical detach marker", file=sys.stderr)
        if not trace_detached:
            print("missing trace detach success", file=sys.stderr)
        if not trace_detach_batched:
            print("missing required batched detach trace evidence", file=sys.stderr)
        if not trace_reattach_batched:
            print("missing required batched reattach trace evidence", file=sys.stderr)
        if not trace_source_ok:
            print("missing required detach request source evidence", file=sys.stderr)
        if not trace_transition_ok:
            print("trace transition limits failed", file=sys.stderr)
        if not helper_retry_ok:
            print("helper STOP snapshot retry was not exercised", file=sys.stderr)
        if args.require_reattach and not trace_reattached:
            print("missing trace reattach success", file=sys.stderr)
        if args.spawn_transient_threads and spawned_threads <= 0:
            print("transient thread spawners did not create workers", file=sys.stderr)
        if args.cold_one_shot_target and cold_marked:
            print("cold one-shot target was marked detached/reattached in output", file=sys.stderr)
        if args.cold_one_shot_target and not cold_unmarked:
            print("cold one-shot target was not reported as exactly one unmarked call", file=sys.stderr)
        if args.cold_one_shot_target and cold_trace_transition:
            print("cold one-shot target has a physical detach/reattach trace row", file=sys.stderr)
        if bad_output:
            print(f"matched bad output: {bad_output.group(0)}", file=sys.stderr)
        if args.fail_on_transition_skips and transition_skip:
            print(f"matched transition skip: {transition_skip.group(0)}", file=sys.stderr)
        print(output, file=sys.stderr)
        raise SystemExit(1)

    print(
        f"trace_check sample={sample} threads={args.threads} "
        f"calls_per_sec={calls_per_sec:.3f} detached_marker={detached} "
        f"reattached_marker={REATTACHED_MARKER_RE.search(output) is not None} "
        f"spawned_threads={spawned_threads}"
    )
    return calls_per_sec


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--threads", type=int, default=32)
    parser.add_argument("--seconds", type=int, default=1)
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--stats-prefix", default="/tmp/peak-hotloop-trace-check")
    parser.add_argument("--heartbeat-interval", default="0.001")
    parser.add_argument("--hibernation-cycle", type=int, default=1)
    parser.add_argument("--reattach-cooldown-ms", default="")
    parser.add_argument("--overhead-ratio", default="0.000001")
    parser.add_argument("--peak-cost", default="0.000000000001")
    parser.add_argument("--detach-count", default="")
    parser.add_argument("--hb-min-us", default="1000")
    parser.add_argument("--hb-max-us", default="1000")
    parser.add_argument("--peak-max-threads", type=int, default=0)
    parser.add_argument("--thread-slack", type=int, default=16)
    parser.add_argument("--require-reattach", action="store_true")
    parser.add_argument("--request-detach-after-start", action="store_true")
    parser.add_argument("--wait-for-hook-before-start", action="store_true")
    parser.add_argument("--disable-reattach", action="store_false",
                        dest="enable_reattach")
    parser.add_argument("--disable-per-target-heartbeat", action="store_false",
                        dest="enable_per_target_heartbeat")
    parser.add_argument("--enable-global-heartbeat", action="store_true")
    parser.add_argument("--global-overhead-ratio", default="0.000001")
    parser.add_argument("--global-detach-factor", default="1.0")
    parser.add_argument("--global-reattach-factor", default="1.0")
    parser.add_argument("--paired-targets", action="store_true")
    parser.add_argument("--cold-one-shot-target", action="store_true")
    parser.add_argument("--spawn-transient-threads", action="store_true")
    parser.add_argument("--spawner-threads", type=int, default=2)
    parser.add_argument("--require-detach-batch-size", type=int, default=0)
    parser.add_argument("--require-reattach-batch-size", type=int, default=0)
    parser.add_argument("--fail-on-transition-skips", action="store_true")
    parser.add_argument("--max-classify-failed", type=int, default=-1)
    parser.add_argument("--max-detach-classify-failed", type=int, default=-1)
    parser.add_argument("--max-reattach-classify-failed", type=int, default=-1)
    parser.add_argument("--max-reattach-success", type=int, default=-1)
    parser.add_argument("--require-trace-diagnostics", action="store_true")
    parser.add_argument("--require-detach-source", default="")
    parser.add_argument("--min-detach-request-calls", type=int, default=1)
    parser.add_argument("--detach-backend", choices=("helper", "signal"),
                        default="")
    parser.add_argument("--detach-helper", default="")
    parser.add_argument("--helper-retry-log-prefix", default="")
    parser.add_argument("--skip-helper-unavailable", action="store_true")
    parser.add_argument("--trace-prefix", default="")
    parser.set_defaults(enable_reattach=True, enable_per_target_heartbeat=True)
    args = parser.parse_args()

    if (args.threads <= 0 or args.seconds <= 0 or args.samples <= 0 or
            args.spawner_threads <= 0):
        print(
            "threads, seconds, samples, and spawner-threads must be positive",
            file=sys.stderr,
        )
        return 2
    if args.require_reattach_batch_size > 0:
        args.require_reattach = True

    values = [run_sample(args, sample) for sample in range(args.samples)]
    print(
        f"hotloop_trace_check_ok samples={args.samples} threads={args.threads} "
        f"min_calls_per_sec={min(values):.3f} max_calls_per_sec={max(values):.3f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
