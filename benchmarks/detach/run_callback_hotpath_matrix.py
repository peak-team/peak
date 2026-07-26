#!/usr/bin/env python3
"""Measure steady-state callback cost across target durations and thread counts."""

import argparse
import os
import re
import statistics
import subprocess
import sys


RESULT_RE = re.compile(
    r"calls_per_sec=(?P<cps>[0-9.]+).*"
    r"ns_per_call=(?P<ns>[0-9.]+).*"
    r"thread_ns_per_call=(?P<thread_ns>[0-9.]+)"
)


def parse_csv_ints(value):
    result = [int(item) for item in value.split(",") if item]
    if not result or any(item <= 0 for item in result):
        raise argparse.ArgumentTypeError("expected comma-separated positive integers")
    return result


def parse_limit_map(value):
    result = {}
    for item in value.split(","):
        target, separator, limit = item.partition(":")
        if not separator:
            raise argparse.ArgumentTypeError(
                "expected comma-separated target-ns:limit pairs"
            )
        try:
            target_ns = int(target)
            numeric_limit = float(limit)
        except ValueError as error:
            raise argparse.ArgumentTypeError(str(error)) from error
        if target_ns <= 0 or numeric_limit <= 0.0:
            raise argparse.ArgumentTypeError("targets and limits must be positive")
        result[target_ns] = numeric_limit
    return result


def clean_env():
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    for name in list(env):
        if name.startswith("PEAK_"):
            del env[name]
    return env


def peak_env(libpeak, max_threads):
    env = clean_env()
    env.update(
        {
            "LD_PRELOAD": libpeak,
            "PEAK_TARGET": "peak_callback_hotpath_target",
            "PEAK_MAX_NUM_THREADS": str(max_threads + 8),
            "PEAK_COST": "0",
            "PEAK_HEARTBEAT_INTERVAL": "0.1",
            "PEAK_HIBERNATION_CYCLE": "50",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            "PEAK_ENABLE_REATTACH": "0",
        }
    )
    return env


def run_sample(args, env, threads, target_ns):
    completed = subprocess.run(
        [
            args.exe,
            "--threads",
            str(threads),
            "--target-ns",
            str(target_ns),
            "--seconds",
            str(args.seconds),
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        timeout=args.timeout,
        check=False,
    )
    match = RESULT_RE.search(completed.stdout)
    if completed.returncode != 0 or match is None:
        print(completed.stdout, file=sys.stderr)
        raise RuntimeError(
            f"benchmark failed: threads={threads} target_ns={target_ns} "
            f"rc={completed.returncode}"
        )
    return {
        "cps": float(match.group("cps")),
        "ns": float(match.group("ns")),
        "thread_ns": float(match.group("thread_ns")),
    }


def run_config(args, name, env, threads, target_ns):
    samples = [
        run_sample(args, env, threads, target_ns) for _ in range(args.samples)
    ]
    result = {
        key: statistics.median(sample[key] for sample in samples)
        for key in ("cps", "ns", "thread_ns")
    }
    print(
        f"HOTPATH_RESULT config={name} threads={threads} "
        f"target_ns={target_ns} ns_per_call={result['ns']:.3f} "
        f"thread_ns_per_call={result['thread_ns']:.3f} "
        f"calls_per_sec={result['cps']:.3f}"
    )
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--current-libpeak", required=True)
    parser.add_argument("--baseline-libpeak")
    parser.add_argument(
        "--threads", type=parse_csv_ints, default=parse_csv_ints("1,2,8,32,64")
    )
    parser.add_argument(
        "--target-ns", type=parse_csv_ints, default=parse_csv_ints("10,100,1000")
    )
    parser.add_argument("--seconds", type=float, default=0.5)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--max-slowdown",
        type=parse_limit_map,
        default=parse_limit_map("10:40,100:8,1000:2"),
    )
    parser.add_argument(
        "--max-ns-per-call",
        type=parse_limit_map,
        default=parse_limit_map("10:500,100:650,1000:1800"),
    )
    parser.add_argument("--min-scaling-efficiency", type=float, default=0.02)
    parser.add_argument("--min-cpus-for-gates", type=int, default=64)
    args = parser.parse_args()

    missing_slowdown = set(args.target_ns) - set(args.max_slowdown)
    missing_ns = set(args.target_ns) - set(args.max_ns_per_call)
    if missing_slowdown or missing_ns:
        parser.error(
            "every target-ns needs max-slowdown and max-ns-per-call limits"
        )

    max_threads = max(args.threads)
    configs = [("no_preload", clean_env())]
    if args.baseline_libpeak:
        configs.append(
            ("baseline_libpeak", peak_env(args.baseline_libpeak, max_threads))
        )
    configs.append(
        ("current_libpeak", peak_env(args.current_libpeak, max_threads))
    )

    results = {}
    for target_ns in args.target_ns:
        for threads in args.threads:
            for name, env in configs:
                results[(name, target_ns, threads)] = run_config(
                    args, name, env, threads, target_ns
                )

    failed = False
    for target_ns in args.target_ns:
        current_one = results[("current_libpeak", target_ns, 1)]["cps"]
        for threads in args.threads:
            baseline = results[("no_preload", target_ns, threads)]
            current = results[("current_libpeak", target_ns, threads)]
            slowdown = current["thread_ns"] / baseline["thread_ns"]
            scaling = current["cps"] / current_one
            efficiency = scaling / threads
            improvement = None
            if args.baseline_libpeak:
                before = results[("baseline_libpeak", target_ns, threads)]
                improvement = current["cps"] / before["cps"]
            before_text = (
                f" current_to_baseline_throughput={improvement:.6f}"
                if improvement is not None
                else ""
            )
            print(
                f"HOTPATH_GATE threads={threads} target_ns={target_ns} "
                f"slowdown={slowdown:.6f} scaling={scaling:.6f} "
                f"scaling_efficiency={efficiency:.6f}{before_text}"
            )
            # Gate serial slowdown separately from parallel scalability so
            # contention is not counted twice against the same sample.
            if threads == 1 and slowdown > args.max_slowdown[target_ns]:
                failed = True
            if current["ns"] > args.max_ns_per_call[target_ns]:
                failed = True
            if threads > 1 and efficiency < args.min_scaling_efficiency:
                failed = True

    available_cpus = (
        len(os.sched_getaffinity(0))
        if hasattr(os, "sched_getaffinity")
        else (os.cpu_count() or 1)
    )
    gates_active = available_cpus >= args.min_cpus_for_gates
    print(
        f"HOTPATH_SUMMARY cpus={available_cpus} gates_active={int(gates_active)} "
        f"max_slowdown={args.max_slowdown} "
        f"max_ns_per_call={args.max_ns_per_call} "
        f"min_scaling_efficiency={args.min_scaling_efficiency:.6f}"
    )
    if failed and gates_active:
        print("steady-state callback performance gate failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
