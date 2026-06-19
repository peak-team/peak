#!/usr/bin/env python3
"""Run PEAK detach hot-loop throughput comparisons.

This is intentionally a lightweight harness around test_detach_hotloop.  It is
not a CTest because it can compare against an independently built baseline
branch, but it makes the manual benchmark recipe reproducible.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys


CALLS_PER_SEC_RE = re.compile(r"calls_per_sec=([0-9.]+)")
DETACHED_MARKER_RE = re.compile(
    r"(?m)^\|\s*peak_detach_hot_target\*+\s*\|\s*[1-9][0-9]*\s*\|"
)


def run_one(exe, env, threads, seconds, timeout):
    completed = subprocess.run(
        [exe, "--threads", str(threads), "--seconds", str(seconds)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )
    match = CALLS_PER_SEC_RE.search(completed.stdout)
    calls_per_sec = float(match.group(1)) if match else None
    return completed.returncode, calls_per_sec, completed.stdout


def make_peak_env(libpeak, stats_prefix, mode, require_strict):
    env = os.environ.copy()
    env.update(
        {
            "LD_PRELOAD": libpeak,
            "PEAK_TARGET": "peak_detach_hot_target",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
            "PEAK_OVERHEAD_RATIO": "0.000001",
            "PEAK_HEARTBEAT_INTERVAL": "0.005",
            "PEAK_HIBERNATION_CYCLE": "1",
            "PEAK_STATSLOG_PATH": stats_prefix,
        }
    )
    if mode:
        env["PEAK_SAFE_DETACH_MODE"] = mode
    if require_strict:
        env["PEAK_REQUIRE_SAFE_DETACH"] = "1"
    return env


def summarize(name, values, detached_count):
    median = statistics.median(values)
    print(
        f"{name}: median={median:.3f} min={min(values):.3f} "
        f"max={max(values):.3f} detached_runs={detached_count}/{len(values)}"
    )
    return median


def run_config(args, name, env, require_detached_marker):
    values = []
    detached_count = 0
    for sample in range(args.samples):
        run_env = env.copy()
        if "PEAK_STATSLOG_PATH" in run_env:
            run_env["PEAK_STATSLOG_PATH"] = f"{run_env['PEAK_STATSLOG_PATH']}-{sample}"
        rc, calls_per_sec, output = run_one(
            args.exe, run_env, args.threads, args.seconds, args.timeout
        )
        detached = DETACHED_MARKER_RE.search(output) is not None
        if detached:
            detached_count += 1
        if rc != 0 or calls_per_sec is None:
            print(f"{name} sample {sample} failed rc={rc}", file=sys.stderr)
            print(output, file=sys.stderr)
            raise SystemExit(1)
        if require_detached_marker and not detached:
            print(
                f"{name} sample {sample} did not show physical detach marker",
                file=sys.stderr,
            )
            print(output, file=sys.stderr)
            raise SystemExit(1)
        print(
            f"{name} sample={sample} calls_per_sec={calls_per_sec:.3f} "
            f"detached_marker={detached}"
        )
        values.append(calls_per_sec)
    return summarize(name, values, detached_count)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, help="test_detach_hotloop executable")
    parser.add_argument("--current-libpeak", required=True, help="current libpeak.so")
    parser.add_argument("--baseline-libpeak", help="main/master libpeak.so")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--seconds", type=int, default=2)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument(
        "--min-strict-baseline-ratio",
        type=float,
        default=0.85,
        help="fail if strict physical median is below this fraction of no-preload",
    )
    args = parser.parse_args()

    medians = {}
    medians["baseline_no_preload"] = run_config(
        args, "baseline_no_preload", os.environ.copy(), False
    )

    if args.baseline_libpeak:
        medians["baseline_libpeak"] = run_config(
            args,
            "baseline_libpeak",
            make_peak_env(args.baseline_libpeak, "/tmp/peak-bench-baseline-lib", None, False),
            False,
        )

    medians["current_compatibility"] = run_config(
        args,
        "current_compatibility",
        make_peak_env(
            args.current_libpeak,
            "/tmp/peak-bench-current-compat",
            "compatibility",
            False,
        ),
        False,
    )
    medians["current_strict_physical"] = run_config(
        args,
        "current_strict_physical",
        make_peak_env(args.current_libpeak, "/tmp/peak-bench-current-strict", None, True),
        True,
    )

    ratio = medians["current_strict_physical"] / medians["baseline_no_preload"]
    print(f"strict_to_no_preload_ratio={ratio:.6f}")
    if ratio < args.min_strict_baseline_ratio:
        print(
            "strict physical detach throughput is below the requested baseline ratio",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
