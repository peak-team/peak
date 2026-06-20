#!/usr/bin/env python3
"""Repeated strict-detach stress for stale caller threads."""

import argparse
import csv
import os
import re
import subprocess
import sys


CALLS_PER_SEC_RE = re.compile(r"calls_per_sec=([0-9.]+)")
INT_VALUE_RE = re.compile(r"\b([a-z_]+)=([0-9]+)\b")
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
TARGET_SYMBOL = "peak_detach_stale_target"


def marker_re(require_reattach):
    suffix = r"\*\*" if require_reattach else r"\*+"
    return re.compile(
        rf"(?m)^\|\s*peak_detach_stale_target{suffix}\s*\|\s*[1-9][0-9]*\s*\|"
    )


def make_env(args, sample):
    env = os.environ.copy()
    max_threads = max(
        args.peak_max_threads,
        args.active_threads + args.stale_threads + args.thread_slack,
    )
    env.update(
        {
            "LD_PRELOAD": args.libpeak,
            "PEAK_TARGET": "peak_detach_stale_target",
            "PEAK_MAX_NUM_THREADS": str(max_threads),
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
            "PEAK_ENABLE_REATTACH": "1" if args.require_reattach else "0",
            "PEAK_COST": args.peak_cost,
            "PEAK_OVERHEAD_RATIO": args.overhead_ratio,
            "PEAK_HEARTBEAT_INTERVAL": args.heartbeat_interval,
            "PEAK_HIBERNATION_CYCLE": str(args.hibernation_cycle),
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": "strict",
            "PEAK_STATSLOG_PATH": f"{args.stats_prefix}-{sample}",
        }
    )
    if args.detach_backend:
        env["PEAK_DETACH_BACKEND"] = args.detach_backend
        if args.detach_backend == "signal":
            env["PEAK_SAFE_DETACH_MODE"] = "signal"
            env.setdefault("PEAK_DETACH_HELPER", "/no/such/peak_detach_helper")
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


def trace_has_success(args, sample, operation):
    rows = read_trace_rows(args, sample)
    if rows is None:
        if not trace_path(args, sample):
            return True
        return False

    for fields in rows:
        if (len(fields) >= 7 and
                fields[2] == TARGET_SYMBOL and
                fields[3] == operation and
                fields[4] == "success" and
                fields[5] == "1" and
                fields[6] == "safe"):
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


def trace_transition_counts(args, sample):
    rows = read_trace_rows(args, sample)
    counts = {
        "detach": {"classify_failed": 0, "prepare_failed": 0, "gum_failed": 0},
        "reattach": {"classify_failed": 0, "prepare_failed": 0, "gum_failed": 0},
    }
    if rows is None:
        return counts

    for fields in rows:
        if len(fields) < 7 or fields[2] != TARGET_SYMBOL:
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

    return counts


def trace_transition_count(counts, operation, name):
    if operation == "all":
        return sum(values[name] for values in counts.values())
    return counts[operation][name]


def print_trace_transition_counts(sample, counts):
    for operation in ("detach", "reattach"):
        values = counts[operation]
        print(
            f"trace transition counts sample={sample} operation={operation} "
            f"classify_failed={values['classify_failed']} "
            f"prepare_failed={values['prepare_failed']} "
            f"gum_failed={values['gum_failed']}",
            file=sys.stderr,
        )


def trace_transition_limits_ok(args, sample):
    counts = trace_transition_counts(args, sample)
    limits = (
        ("all", "classify_failed", args.max_classify_failed),
        ("all", "prepare_failed", args.max_prepare_failed),
        ("all", "gum_failed", args.max_gum_failed),
        ("detach", "classify_failed", args.max_detach_classify_failed),
        ("detach", "prepare_failed", args.max_detach_prepare_failed),
        ("detach", "gum_failed", args.max_detach_gum_failed),
        ("reattach", "classify_failed", args.max_reattach_classify_failed),
        ("reattach", "prepare_failed", args.max_reattach_prepare_failed),
        ("reattach", "gum_failed", args.max_reattach_gum_failed),
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

    if not ok:
        print_trace_transition_counts(sample, counts)

    return ok


def int_values(output):
    return {match.group(1): int(match.group(2))
            for match in INT_VALUE_RE.finditer(output)}


def run_sample(args, sample):
    cmd = [
        args.exe,
        "--active-threads",
        str(args.active_threads),
        "--stale-threads",
        str(args.stale_threads),
        "--stale-calls",
        str(args.stale_calls),
        "--seconds",
        str(args.seconds),
        "--stale-mode",
        args.stale_mode,
        "--stale-ready-timeout",
        str(args.stale_ready_timeout),
    ]
    if args.active_after_stale:
        cmd.append("--active-after-stale")
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
    values = int_values(output)
    stale_blocked = f"stale_blocked={args.stale_threads}" in output
    active_calls = values.get("active_calls", 0)
    observed_stale_calls = values.get("stale_calls", -1)
    side_effect = values.get("side_effect", 0)
    marker_ok = marker_re(args.require_reattach).search(output) is not None
    trace_detached = trace_has_success(args, sample, "detach")
    trace_reattached = trace_has_success(args, sample, "reattach")
    trace_transition_ok = trace_transition_limits_ok(args, sample)
    bad_output = BAD_OUTPUT_RE.search(output)
    transition_skip = TRANSITION_SKIP_RE.search(output)
    calls_per_sec = float(calls_match.group(1)) if calls_match else 0.0
    expected_stale_calls = args.stale_threads * args.stale_calls

    if (completed.returncode != 0 or
            calls_match is None or
            calls_per_sec <= 0.0 or
            not stale_blocked or
            active_calls <= 0 or
            observed_stale_calls != expected_stale_calls or
            side_effect == 0 or
            not marker_ok or
            not trace_detached or
            not trace_transition_ok or
            (args.require_reattach and not trace_reattached) or
            bad_output or
            (args.fail_on_transition_skips and transition_skip)):
        print(f"stale stress sample {sample} failed rc={completed.returncode}",
              file=sys.stderr)
        if calls_match is None:
            print("missing calls_per_sec output", file=sys.stderr)
        elif calls_per_sec <= 0.0:
            print("calls_per_sec must be positive", file=sys.stderr)
        if not stale_blocked:
            print("not all stale callers reached blocked state", file=sys.stderr)
        if active_calls <= 0:
            print("active calls did not run", file=sys.stderr)
        if observed_stale_calls != expected_stale_calls:
            print(
                f"stale calls mismatch: observed={observed_stale_calls} "
                f"expected={expected_stale_calls}",
                file=sys.stderr,
            )
        if side_effect == 0:
            print("side_effect did not change", file=sys.stderr)
        if not marker_ok:
            print("missing required strict detach marker", file=sys.stderr)
        if not trace_detached:
            print("missing trace detach success", file=sys.stderr)
        if not trace_transition_ok:
            print("trace transition limits failed", file=sys.stderr)
        if args.require_reattach and not trace_reattached:
            print("missing trace reattach success", file=sys.stderr)
        if bad_output:
            print(f"matched bad output: {bad_output.group(0)}", file=sys.stderr)
        if args.fail_on_transition_skips and transition_skip:
            print(f"matched transition skip: {transition_skip.group(0)}",
                  file=sys.stderr)
        print(output, file=sys.stderr)
        raise SystemExit(1)

    print(
        f"stale sample={sample} active_threads={args.active_threads} "
        f"stale_threads={args.stale_threads} stale_mode={args.stale_mode} "
        f"calls_per_sec={calls_per_sec:.3f}"
    )
    return calls_per_sec


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--active-threads", type=int, default=16)
    parser.add_argument("--stale-threads", type=int, default=48)
    parser.add_argument("--stale-calls", type=int, default=2048)
    parser.add_argument("--seconds", type=int, default=2)
    parser.add_argument("--stale-ready-timeout", type=int, default=5)
    parser.add_argument("--samples", type=int, default=6)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--stats-prefix", default="/tmp/peak-stale-thread-stress")
    parser.add_argument("--trace-prefix", default="")
    parser.add_argument("--heartbeat-interval", default="0.001")
    parser.add_argument("--hibernation-cycle", type=int, default=1)
    parser.add_argument("--overhead-ratio", default="0.000001")
    parser.add_argument("--peak-cost", default="0.0000001")
    parser.add_argument("--peak-max-threads", type=int, default=0)
    parser.add_argument("--thread-slack", type=int, default=32)
    parser.add_argument("--require-reattach", action="store_true")
    parser.add_argument("--active-after-stale", action="store_true")
    parser.add_argument("--stale-mode", default="parked",
                        choices=["parked", "unrelated-spin", "unrelated-sleep"])
    parser.add_argument("--fail-on-transition-skips", action="store_true")
    parser.add_argument("--max-classify-failed", type=int, default=-1)
    parser.add_argument("--max-prepare-failed", type=int, default=-1)
    parser.add_argument("--max-gum-failed", type=int, default=-1)
    parser.add_argument("--max-detach-classify-failed", type=int, default=-1)
    parser.add_argument("--max-detach-prepare-failed", type=int, default=-1)
    parser.add_argument("--max-detach-gum-failed", type=int, default=-1)
    parser.add_argument("--max-reattach-classify-failed", type=int, default=-1)
    parser.add_argument("--max-reattach-prepare-failed", type=int, default=-1)
    parser.add_argument("--max-reattach-gum-failed", type=int, default=-1)
    parser.add_argument("--detach-backend", choices=("helper", "signal"),
                        default="")
    args = parser.parse_args()

    if (args.active_threads <= 0 or
            args.stale_threads <= 0 or
            args.seconds <= 0 or
            args.stale_ready_timeout <= 0 or
            args.samples <= 0):
        print(
              "active-threads, stale-threads, seconds, stale-ready-timeout, "
              "and samples must be positive",
              file=sys.stderr)
        return 2

    values = [run_sample(args, sample) for sample in range(args.samples)]
    print(
        f"stale_thread_stress_ok samples={args.samples} "
        f"active_threads={args.active_threads} stale_threads={args.stale_threads} "
        f"min_calls_per_sec={min(values):.3f} max_calls_per_sec={max(values):.3f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
