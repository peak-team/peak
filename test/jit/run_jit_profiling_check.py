#!/usr/bin/env python3
"""Run the mmap JIT fixture under PEAK and validate statslog output."""

import argparse
import csv
import glob
import os
import re
import subprocess
import sys
import tempfile


SKIP_RETURN_CODE = 77
JIT_SYMBOL = "peak_jit_hot"
PID_RE = re.compile(r"\bpid=([0-9]+)\b")


def merge_preload(libpeak):
    existing = os.environ.get("LD_PRELOAD")
    if existing:
        return f"{libpeak}:{existing}"
    return libpeak


def make_env(libpeak, stats_prefix, map_path, trace_path):
    env = os.environ.copy()
    env.update(
        {
            "LD_PRELOAD": merge_preload(libpeak),
            "PEAK_TARGET": JIT_SYMBOL,
            "PEAK_STATSLOG_PATH": stats_prefix,
            "PEAK_JIT_ENABLE": "1",
            "PEAK_JIT_PROVIDER": "perfmap",
            "PEAK_JIT_MAP_PATH": map_path,
            "PEAK_JIT_TRACE_PATH": trace_path,
            "PEAK_HEARTBEAT_INTERVAL": "0",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            "PEAK_ENABLE_REATTACH": "0",
            "PEAK_COST": "0",
        }
    )
    return env


def mode_flag(mode):
    if mode in ("positive", "final-drain"):
        return "--with-perf-map"
    if mode == "negative":
        return "--without-metadata"
    raise ValueError(f"unknown mode: {mode}")


def find_stats_csv(stats_prefix, pid):
    expected = f"{stats_prefix}-p{pid}.csv"
    if os.path.exists(expected):
        return expected

    candidates = sorted(glob.glob(f"{stats_prefix}-p*.csv"))
    if len(candidates) == 1:
        return candidates[0]
    return None


def parse_stats_csv(path):
    if path is None:
        return []
    with open(path, newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def symbol_count(rows, symbol):
    total = 0
    for row in rows:
        if row.get("function") != symbol:
            continue
        try:
            total += int(row.get("count", "0"))
        except ValueError:
            raise AssertionError(f"invalid count in stats row: {row}") from None
    return total


def read_text(path):
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def cleanup_perf_map(pid):
    path = f"/tmp/perf-{pid}.map"
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    except OSError as exc:
        print(f"warning: failed to remove {path}: {exc}", file=sys.stderr)


def run_one(args, tmpdir, mode):
    stats_prefix = os.path.join(tmpdir, f"peak-jit-{mode}")
    map_path = os.path.join(tmpdir, f"perf-{mode}.map")
    trace_path = os.path.join(tmpdir, f"jit-{mode}-trace.csv")
    try:
        os.unlink(map_path)
    except FileNotFoundError:
        pass
    command = [
        args.exe,
        mode_flag(mode),
        "--iterations",
        str(args.iterations),
        "--metadata-sleep-us",
        "0" if mode == "final-drain" else str(args.metadata_sleep_us),
    ]
    completed = subprocess.run(
        command,
        env=make_env(args.libpeak, stats_prefix, map_path, trace_path),
        cwd=tmpdir,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=args.timeout,
        check=False,
    )

    output = completed.stdout + completed.stderr
    pid_match = PID_RE.search(output)
    pid = int(pid_match.group(1)) if pid_match else None

    if completed.returncode == SKIP_RETURN_CODE:
        print(output, end="")
        return {
            "mode": mode,
            "skipped": True,
            "count": 0,
            "stats_csv": None,
            "pid": pid,
        }

    if completed.returncode != 0:
        raise AssertionError(
            f"{mode} fixture failed with rc={completed.returncode}\n{output}"
        )

    if pid is None:
        raise AssertionError(f"{mode} fixture output did not include pid=\n{output}")

    stats_csv = find_stats_csv(stats_prefix, pid)
    rows = parse_stats_csv(stats_csv)
    count = symbol_count(rows, JIT_SYMBOL)
    cleanup_perf_map(pid)
    try:
        os.unlink(map_path)
    except FileNotFoundError:
        pass

    if mode in ("positive", "final-drain"):
        trace = read_text(trace_path)
        attached_records = [
            line
            for line in trace.splitlines()
            if ",perfmap-record,perfmap," in line and line.endswith(",attached")
        ]
        if len(attached_records) != 1:
            raise AssertionError(
                f"expected exactly one attached perf-map record, got "
                f"{len(attached_records)}\ntrace={trace}\n{output}"
            )
    if mode == "positive":
        if stats_csv is None:
            raise AssertionError(
                f"positive JIT run did not create stats csv for pid {pid}\n"
                f"expected prefix: {stats_prefix}\n{output}"
            )
        if count <= 0:
            trace = read_text(trace_path)
            raise AssertionError(
                f"positive JIT run did not record {JIT_SYMBOL}; "
                f"stats_csv={stats_csv}\nrows={rows}\ntrace={trace}\n{output}"
            )
    elif mode == "negative":
        if count != 0:
            raise AssertionError(
                f"negative JIT run unexpectedly recorded {JIT_SYMBOL}; "
                f"stats_csv={stats_csv} count={count}\nrows={rows}\n{output}"
            )

    print(
        f"jit_profiling_{mode}_ok pid={pid} "
        f"stats_csv={stats_csv or '<none>'} {JIT_SYMBOL}_count={count}"
    )
    return {
        "mode": mode,
        "skipped": False,
        "count": count,
        "stats_csv": stats_csv,
        "pid": pid,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument(
        "--mode",
        choices=("positive", "negative", "both", "final-drain"),
        default="both",
    )
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--metadata-sleep-us", type=int, default=50000)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    args.exe = os.path.abspath(args.exe)
    args.libpeak = os.path.abspath(args.libpeak)

    if args.iterations <= 0:
        print("--iterations must be positive", file=sys.stderr)
        return 2
    if args.metadata_sleep_us < 0:
        print("--metadata-sleep-us must be non-negative", file=sys.stderr)
        return 2
    if not os.path.exists(args.exe):
        print(f"missing fixture executable: {args.exe}", file=sys.stderr)
        return 2
    if not os.path.exists(args.libpeak):
        print(f"missing libpeak: {args.libpeak}", file=sys.stderr)
        return 2

    modes = ["positive", "negative"] if args.mode == "both" else [args.mode]
    with tempfile.TemporaryDirectory(prefix="peak-jit-profile-") as tmpdir:
        results = [run_one(args, tmpdir, mode) for mode in modes]

    if any(result["skipped"] for result in results):
        return SKIP_RETURN_CODE

    counts = {result["mode"]: result["count"] for result in results}
    print(
        "jit_profiling_check_ok "
        + " ".join(f"{mode}_count={count}" for mode, count in counts.items())
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
