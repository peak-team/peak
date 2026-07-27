#!/usr/bin/env python3
"""Measure steady-state callback cost across target durations and thread counts."""

import argparse
import os
import re
import statistics
import subprocess
import sys


RESULT_RE = re.compile(
    r"calibrated_work_ns=(?P<calibrated_ns>[0-9.]+).*"
    r"work_iterations=(?P<work_iterations>[0-9]+).*"
    r"calls_per_sec=(?P<cps>[0-9.]+).*"
    r"ns_per_call=(?P<ns>[0-9.]+).*"
    r"thread_ns_per_call=(?P<thread_ns>[0-9.]+)"
)

CALIBRATION_RE = re.compile(
    r"target_ns=(?P<target_ns>[0-9]+).*"
    r"calibrated_work_ns=(?P<calibrated_ns>[0-9.]+).*"
    r"work_iterations=(?P<work_iterations>[0-9]+)"
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


def affinity_topology():
    allowed = sorted(
        os.sched_getaffinity(0)
        if hasattr(os, "sched_getaffinity")
        else range(os.cpu_count() or 1)
    )
    core_groups = {}
    topology_complete = True
    for cpu in allowed:
        try:
            with open(
                f"/sys/devices/system/cpu/cpu{cpu}/topology/physical_package_id",
                encoding="ascii",
            ) as handle:
                package = int(handle.read())
            with open(
                f"/sys/devices/system/cpu/cpu{cpu}/topology/core_id",
                encoding="ascii",
            ) as handle:
                core = int(handle.read())
        except (OSError, ValueError):
            topology_complete = False
            break
        core_groups.setdefault((package, core), []).append(cpu)

    if not topology_complete:
        return allowed, len(allowed)

    # Fill physical cores first, then SMT siblings. This makes the requested
    # thread counts comparable across schedulers and prevents a 2-thread run
    # from accidentally sharing one core.
    ordered = []
    sibling = 0
    while len(ordered) < len(allowed):
        added = False
        for cpus in core_groups.values():
            if sibling < len(cpus):
                ordered.append(cpus[sibling])
                added = True
        if not added:
            break
        sibling += 1
    return ordered, len(core_groups)


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


def calibrate_iterations(args, target_ns):
    completed = subprocess.run(
        [
            args.exe,
            "--target-ns",
            str(target_ns),
            "--calibrate-only",
        ],
        env=clean_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        timeout=args.timeout,
        check=False,
    )
    match = CALIBRATION_RE.search(completed.stdout)
    if completed.returncode != 0 or match is None:
        print(completed.stdout, file=sys.stderr)
        raise RuntimeError(
            f"benchmark calibration failed: target_ns={target_ns} "
            f"rc={completed.returncode}"
        )
    iterations = int(match.group("work_iterations"))
    if iterations <= 0:
        raise RuntimeError(
            f"benchmark calibration returned invalid iterations={iterations}"
        )
    print(
        f"HOTPATH_CALIBRATION target_ns={target_ns} "
        f"calibrated_work_ns={float(match.group('calibrated_ns')):.3f} "
        f"work_iterations={iterations}"
    )
    return iterations


def run_sample(args, env, threads, target_ns, work_iterations):
    completed = subprocess.run(
        [
            args.exe,
            "--threads",
            str(threads),
            "--target-ns",
            str(target_ns),
            "--work-iterations",
            str(work_iterations),
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
    measured_iterations = int(match.group("work_iterations"))
    if measured_iterations != work_iterations:
        raise RuntimeError(
            f"benchmark workload mismatch: threads={threads} "
            f"target_ns={target_ns} expected_iterations={work_iterations} "
            f"measured_iterations={measured_iterations}"
        )
    return {
        "cps": float(match.group("cps")),
        "ns": float(match.group("ns")),
        "thread_ns": float(match.group("thread_ns")),
        "calibrated_ns": float(match.group("calibrated_ns")),
        "work_iterations": measured_iterations,
    }


def run_config(args, name, env, threads, target_ns, work_iterations):
    samples = [
        run_sample(args, env, threads, target_ns, work_iterations)
        for _ in range(args.samples)
    ]
    result = {
        key: statistics.median(sample[key] for sample in samples)
        for key in ("cps", "ns", "thread_ns")
    }
    print(
        f"HOTPATH_RESULT config={name} threads={threads} "
        f"target_ns={target_ns} work_iterations={work_iterations} "
        f"ns_per_call={result['ns']:.3f} "
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
        default=parse_limit_map("10:30,100:5,1000:1.5"),
    )
    parser.add_argument(
        "--max-ns-per-call",
        type=parse_limit_map,
        default=parse_limit_map("10:350,100:500,1000:1600"),
    )
    parser.add_argument("--min-scaling-efficiency", type=float, default=0.65)
    parser.add_argument(
        "--min-relative-scaling-efficiency", type=float, default=0.75
    )
    args = parser.parse_args()

    missing_slowdown = set(args.target_ns) - set(args.max_slowdown)
    missing_ns = set(args.target_ns) - set(args.max_ns_per_call)
    if missing_slowdown or missing_ns:
        parser.error(
            "every target-ns needs max-slowdown and max-ns-per-call limits"
        )

    max_threads = max(args.threads)
    ordered_cpus, physical_cpus = affinity_topology()
    configs = [("no_preload", clean_env())]
    if args.baseline_libpeak:
        configs.append(
            ("baseline_libpeak", peak_env(args.baseline_libpeak, max_threads))
        )
    configs.append(
        ("current_libpeak", peak_env(args.current_libpeak, max_threads))
    )
    cpu_list = ",".join(str(cpu) for cpu in ordered_cpus)
    for _, env in configs:
        env["PEAK_BENCH_CPU_LIST"] = cpu_list

    results = {}
    for target_ns in args.target_ns:
        work_iterations = calibrate_iterations(args, target_ns)
        for threads in args.threads:
            for name, env in configs:
                results[(name, target_ns, threads)] = run_config(
                    args, name, env, threads, target_ns, work_iterations
                )

    failed = False
    for target_ns in args.target_ns:
        current_one = results[("current_libpeak", target_ns, 1)]["cps"]
        no_preload_one = results[("no_preload", target_ns, 1)]["cps"]
        physical_reference_threads = max(
            threads for threads in args.threads if threads <= physical_cpus
        )
        physical_reference_cps = results[
            ("current_libpeak", target_ns, physical_reference_threads)
        ]["cps"]
        for threads in args.threads:
            baseline = results[("no_preload", target_ns, threads)]
            current = results[("current_libpeak", target_ns, threads)]
            slowdown = current["thread_ns"] / baseline["thread_ns"]
            scaling = current["cps"] / current_one
            efficiency = scaling / threads
            if threads <= physical_cpus:
                scaling_capacity = float(threads)
                capacity_efficiency = efficiency
            else:
                scaling_capacity = (
                    float(physical_cpus) / physical_reference_threads
                )
                capacity_efficiency = (
                    current["cps"] / physical_reference_cps /
                    scaling_capacity
                )
            no_preload_efficiency = (
                baseline["cps"] / no_preload_one / threads
            )
            relative_efficiency = (
                efficiency / no_preload_efficiency
                if no_preload_efficiency > 0.0
                else 0.0
            )
            physical_gate_active = threads <= physical_cpus
            relative_gate_active = physical_gate_active
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
                f"scaling_efficiency={efficiency:.6f} "
                f"scaling_capacity={scaling_capacity:.6f} "
                f"capacity_scaling_efficiency={capacity_efficiency:.6f} "
                f"relative_scaling_efficiency={relative_efficiency:.6f} "
                f"physical_gate_active={int(physical_gate_active)} "
                f"relative_gate_active={int(relative_gate_active)}"
                f"{before_text}"
            )
            # Gate serial slowdown separately from parallel scalability so
            # contention is not counted twice against the same sample.
            # Normalize absolute scaling by available physical-core capacity so
            # every requested thread count remains gated on a smaller hosted
            # runner. Oversubscribed points use the largest measured physical
            # point as their reference, avoiding single-core turbo as an
            # unattainable all-core baseline. Relative scaling is comparable
            # only while each worker has a physical core: SMT and
            # oversubscription favor the tiny uninstrumented body according to
            # instruction mix, not merely callback serialization.
            if (physical_gate_active and threads == 1 and
                    slowdown > args.max_slowdown[target_ns]):
                failed = True
            if (physical_gate_active and threads == 1 and
                    current["thread_ns"] >
                    args.max_ns_per_call[target_ns]):
                failed = True
            if (threads > 1 and
                    capacity_efficiency < args.min_scaling_efficiency):
                failed = True
            if (relative_gate_active and threads > 1 and
                    relative_efficiency <
                    args.min_relative_scaling_efficiency):
                failed = True

    print(
        f"HOTPATH_SUMMARY logical_cpus={len(ordered_cpus)} "
        f"physical_cpus={physical_cpus} "
        f"max_slowdown={args.max_slowdown} "
        f"max_ns_per_call={args.max_ns_per_call} "
        f"min_scaling_efficiency={args.min_scaling_efficiency:.6f} "
        f"min_relative_scaling_efficiency="
        f"{args.min_relative_scaling_efficiency:.6f}"
    )
    if failed:
        print("steady-state callback performance gate failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
