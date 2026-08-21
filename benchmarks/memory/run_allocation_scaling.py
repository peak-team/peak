#!/usr/bin/env python3
import argparse
import os
import random
import re
import statistics
import subprocess
import tempfile

from validate_memlog_accounting import validate

RESULT_RE = re.compile(r"ns_per_call=([0-9.]+)")
MEMLOG_RE = re.compile(r"memlog CSV written: .* \(events=([0-9]+) dropped=([0-9]+)\)")


def parse_csv(value, cast):
    parsed = [cast(item) for item in value.split(",") if item]
    if not parsed:
        raise argparse.ArgumentTypeError("list must not be empty")
    return parsed


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--current-libpeak", required=True)
    parser.add_argument("--baseline-libpeak")
    parser.add_argument("--threads", default="1,2,4,8")
    parser.add_argument("--modes", default="pair,realloc,mixed")
    parser.add_argument("--iterations", type=int, default=100000)
    parser.add_argument("--min-iterations-per-thread", type=int, default=0)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--max-profiled-ns-per-call", type=float, default=750.0)
    parser.add_argument("--max-incremental-ns-per-call", type=float, default=700.0)
    parser.add_argument("--min-throughput-retention", type=float, default=0.90)
    parser.add_argument("--raw-retention-min-threads", type=int, default=4)
    parser.add_argument("--verify-accounting", action="store_true")
    return parser.parse_args()


def run_once(args, library, threads, mode):
    iterations = max(1, args.iterations // threads,
                     args.min_iterations_per_thread)
    minimum_events_per_iteration = {
        "pair": 2,
        "realloc": 2,
        "mixed": 4,
        "aligned": 2,
        "posix": 2,
    }[mode]
    minimum_events = minimum_events_per_iteration * iterations * threads
    with tempfile.TemporaryDirectory(prefix="peak-memory-benchmark.") as output_dir:
        stats_path = os.path.join(output_dir, "stats.csv")
        memlog_path = os.path.join(output_dir, "mem.csv")
        environment = os.environ.copy()
        for name in (
            "LD_PRELOAD",
            "PEAK_TARGET",
            "PEAK_MEMORY_PROFILE",
            "PEAK_MEMORY_TRACK_ALL",
            "PEAK_MEMLOG_CHUNK_EVENTS",
            "PEAK_STATSLOG_TEMPLATE",
            "PEAK_MEMLOG_TEMPLATE",
        ):
            environment.pop(name, None)
        if library is not None:
            environment.update({
                "LD_PRELOAD": library,
                "PEAK_TARGET": "run_allocation_benchmark",
                "PEAK_VERBOSITY": "debug",
                "PEAK_MEMORY_PROFILE": "TRUE",
                "PEAK_MEMORY_TRACK_ALL": "TRUE",
                "PEAK_MEMLOG_CHUNK_EVENTS": str(iterations * threads * 4 + 1024),
                "PEAK_STATSLOG_TEMPLATE": stats_path,
                "PEAK_MEMLOG_TEMPLATE": memlog_path,
            })
        result = subprocess.run(
            [args.exe, str(threads), str(iterations), mode],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
            timeout=60,
        )
        combined_output = result.stdout + result.stderr
        match = RESULT_RE.search(combined_output)
        if match is None:
            raise RuntimeError(combined_output)
        if library is not None:
            memlog_match = MEMLOG_RE.search(combined_output)
            failures = []
            if memlog_match is None:
                failures.append("missing memlog completion")
            else:
                event_count = int(memlog_match.group(1))
                if event_count < minimum_events:
                    failures.append(
                        f"events={event_count} below expected {minimum_events}"
                    )
                if int(memlog_match.group(2)) != 0:
                    failures.append(f"dropped={memlog_match.group(2)}")
            if not os.path.isfile(stats_path):
                failures.append("missing stats CSV")
            if not os.path.isfile(memlog_path):
                failures.append("missing memlog CSV")
            if failures:
                raise RuntimeError("profile artifact validation failed: " +
                                   ", ".join(failures) + "\n" +
                                   combined_output)
            with open(memlog_path, "rb") as memlog:
                if memlog.readline() != b"ts_ns,delta,current,tid,op\n":
                    raise RuntimeError("invalid memlog header")
            if args.verify_accounting:
                validate(memlog_path)
        return float(match.group(1))


def main():
    args = parse_args()
    threads = parse_csv(args.threads, int)
    modes = parse_csv(args.modes, str)
    libraries = {"unprofiled": None, "current": args.current_libpeak}
    if args.baseline_libpeak:
        libraries["baseline"] = args.baseline_libpeak
    results = {
        (label, mode, count): []
        for label in libraries
        for mode in modes
        for count in threads
    }
    cases = [
        (label, mode, count)
        for _ in range(args.samples)
        for mode in modes
        for count in threads
        for label in libraries
    ]
    random.Random(82).shuffle(cases)
    for label, mode, count in cases:
        results[(label, mode, count)].append(
            run_once(args, libraries[label], count, mode)
        )

    failures = []
    print("label mode threads median_ns_per_call min_ns_per_call "
          "max_ns_per_call throughput_calls_per_second")
    for mode in modes:
        best_lower_current = None
        best_lower_unprofiled = None
        for count in threads:
            medians = {}
            for label in libraries:
                samples = results[(label, mode, count)]
                value = statistics.median(samples)
                medians[label] = value
                print(label, mode, count, f"{value:.3f}",
                      f"{min(samples):.3f}", f"{max(samples):.3f}",
                      f"{1e9 / value:.3f}")
            current = medians["current"]
            unprofiled = medians["unprofiled"]
            raw_retention = (1.0 if best_lower_current is None else
                             best_lower_current / current)
            unprofiled_retention = (
                1.0 if best_lower_unprofiled is None else
                best_lower_unprofiled / unprofiled
            )
            relative_retention = (
                raw_retention / min(1.0, unprofiled_retention)
            )
            incremental = current - unprofiled
            print(
                "gate",
                mode,
                count,
                f"raw_retention_vs_best_lower={raw_retention:.6f}",
                f"unprofiled_retention_vs_best_lower={unprofiled_retention:.6f}",
                f"relative_retention={relative_retention:.6f}",
                f"incremental_ns={incremental:.3f}",
                *([] if "baseline" not in medians else [
                    f"current_over_baseline={current / medians['baseline']:.6f}"
                ]),
            )
            if current > args.max_profiled_ns_per_call:
                failures.append(f"{mode}/{count}: profiled {current:.3f} ns")
            if incremental > args.max_incremental_ns_per_call:
                failures.append(f"{mode}/{count}: incremental {incremental:.3f} ns")
            if (count >= args.raw_retention_min_threads and
                    raw_retention < args.min_throughput_retention):
                failures.append(
                    f"{mode}/{count}: raw retention {raw_retention:.6f}"
                )
            if relative_retention < args.min_throughput_retention:
                failures.append(
                    f"{mode}/{count}: relative retention "
                    f"{relative_retention:.6f}"
                )
            if "baseline" in medians and current > medians["baseline"] * 1.05:
                failures.append(
                    f"{mode}/{count}: current {current:.3f} ns exceeds baseline "
                    f"{medians['baseline']:.3f} ns by more than 5%"
                )
            if best_lower_current is None or current < best_lower_current:
                best_lower_current = current
            if (best_lower_unprofiled is None or
                    unprofiled < best_lower_unprofiled):
                best_lower_unprofiled = unprofiled
    if failures:
        for failure in failures:
            print(f"allocation scaling gate failed: {failure}")
        return 1
    print("allocation_scaling_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
