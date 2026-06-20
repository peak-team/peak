#!/usr/bin/env python3
"""Short strict physical-detach trace regression runner."""

import argparse
import csv
import os
import re
import subprocess
import sys


CALLS_PER_SEC_RE = re.compile(r"calls_per_sec=([0-9.]+)")
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


def make_env(args, sample):
    env = os.environ.copy()
    max_threads = max(args.peak_max_threads, args.threads + args.thread_slack)
    peak_target = TARGET_SYMBOL
    if args.paired_targets:
        peak_target = f"{TARGET_SYMBOL},{PAIRED_TARGET_SYMBOL}"
    env.update(
        {
            "LD_PRELOAD": args.libpeak,
            "PEAK_TARGET": peak_target,
            "PEAK_MAX_NUM_THREADS": str(max_threads),
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            "PEAK_ENABLE_REATTACH": "1" if args.enable_reattach else "0",
            "PEAK_COST": args.peak_cost,
            "PEAK_OVERHEAD_RATIO": args.overhead_ratio,
            "PEAK_HEARTBEAT_INTERVAL": args.heartbeat_interval,
            "PEAK_HIBERNATION_CYCLE": str(args.hibernation_cycle),
            "PEAK_HB_MIN_US": args.hb_min_us,
            "PEAK_HB_MAX_US": args.hb_max_us,
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": "strict",
            "PEAK_STATSLOG_PATH": f"{args.stats_prefix}-{sample}",
        }
    )
    if args.trace_prefix:
        env["PEAK_DETACH_TRACE_PATH"] = f"{args.trace_prefix}-{sample}.csv"
    return env


def trace_path(args, sample):
    if not args.trace_prefix:
        return None
    return f"{args.trace_prefix}-{sample}.csv"


def reset_trace(args, sample):
    path = trace_path(args, sample)
    if not path:
        return
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


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
                 trace_row_has_diagnostics(fields, "safe"))):
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
                 trace_row_has_diagnostics(fields, "safe"))):
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
    reset_trace(args, sample)
    completed = subprocess.run(
        cmd,
        env=make_env(args, sample),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=args.timeout,
        check=False,
    )
    output = completed.stdout
    calls_match = CALLS_PER_SEC_RE.search(output)
    required_marker = REATTACHED_MARKER_RE if args.require_reattach else DETACHED_MARKER_RE
    detached = DETACHED_MARKER_RE.search(output) is not None
    marker_ok = required_marker.search(output) is not None
    trace_detached = trace_has_success(args, sample, "detach")
    trace_reattached = trace_has_success(args, sample, "reattach")
    trace_detach_batched = trace_has_required_batch(
        args, sample, "detach", args.require_detach_batch_size)
    trace_reattach_batched = trace_has_required_batch(
        args, sample, "reattach", args.require_reattach_batch_size)
    bad_output = BAD_OUTPUT_RE.search(output)
    transition_skip = TRANSITION_SKIP_RE.search(output)
    calls_per_sec = float(calls_match.group(1)) if calls_match else 0.0

    if (completed.returncode != 0 or
            calls_match is None or
            calls_per_sec <= 0.0 or
            not marker_ok or
            not trace_detached or
            not trace_detach_batched or
            not trace_reattach_batched or
            (args.require_reattach and not trace_reattached) or
            bad_output or
            (args.fail_on_transition_skips and transition_skip)):
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
        if args.require_reattach and not trace_reattached:
            print("missing trace reattach success", file=sys.stderr)
        if bad_output:
            print(f"matched bad output: {bad_output.group(0)}", file=sys.stderr)
        if args.fail_on_transition_skips and transition_skip:
            print(f"matched transition skip: {transition_skip.group(0)}", file=sys.stderr)
        print(output, file=sys.stderr)
        raise SystemExit(1)

    print(
        f"trace_check sample={sample} threads={args.threads} "
        f"calls_per_sec={calls_per_sec:.3f} detached_marker={detached} "
        f"reattached_marker={REATTACHED_MARKER_RE.search(output) is not None}"
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
    parser.add_argument("--overhead-ratio", default="0.000001")
    parser.add_argument("--peak-cost", default="0.0000001")
    parser.add_argument("--hb-min-us", default="1000")
    parser.add_argument("--hb-max-us", default="1000")
    parser.add_argument("--peak-max-threads", type=int, default=0)
    parser.add_argument("--thread-slack", type=int, default=16)
    parser.add_argument("--require-reattach", action="store_true")
    parser.add_argument("--disable-reattach", action="store_false",
                        dest="enable_reattach")
    parser.add_argument("--paired-targets", action="store_true")
    parser.add_argument("--require-detach-batch-size", type=int, default=0)
    parser.add_argument("--require-reattach-batch-size", type=int, default=0)
    parser.add_argument("--fail-on-transition-skips", action="store_true")
    parser.add_argument("--require-trace-diagnostics", action="store_true")
    parser.add_argument("--trace-prefix", default="")
    parser.set_defaults(enable_reattach=True)
    args = parser.parse_args()

    if args.threads <= 0 or args.seconds <= 0 or args.samples <= 0:
        print("threads, seconds, and samples must be positive", file=sys.stderr)
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
