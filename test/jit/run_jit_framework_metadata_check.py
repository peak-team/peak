#!/usr/bin/env python3
"""Smoke checks for runtime-provided JIT metadata formats."""

import argparse
import csv
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile


SKIP_RETURN_CODE = 77
V8_TARGET_NAME = "peakJitV8Target"
V8_OPTIMIZED_TARGET_RE = re.compile(
    rf"(?:^|\s)(?:LazyCompile|JS):\*{re.escape(V8_TARGET_NAME)}(?:\s|$)"
)


def v8_row_is_optimized_target(row):
    return V8_OPTIMIZED_TARGET_RE.search(row) is not None


def merge_preload(libpeak):
    existing = os.environ.get("LD_PRELOAD")
    if existing:
        return f"{libpeak}:{existing}"
    return libpeak


def find_stats_csv(stats_prefix, pid):
    expected = f"{stats_prefix}-p{pid}.csv"
    if os.path.exists(expected):
        return expected

    candidates = sorted(glob.glob(f"{stats_prefix}-p*.csv"))
    if len(candidates) == 1:
        return candidates[0]
    return None


def symbol_count(rows, symbol):
    total = 0
    for row in rows:
        if row.get("function") != symbol:
            continue
        total += int(row.get("count", "0"))
    return total


def run_v8_name_patterns():
    positive_rows = [
        "3679993bcce0 9b LazyCompile:*peakJitV8Target /tmp/v8_peak_jit.js:1:25",
        "7fdadafc6580 a4 JS:*peakJitV8Target /tmp/v8_peak_jit.js:1:25",
    ]
    negative_rows = [
        "3be1ff9c1f08 6 JS:~peakJitV8Target /tmp/v8_peak_jit.js:1:25",
        "7fdadafc6500 38 JS:^peakJitV8Target /tmp/v8_peak_jit.js:1:25",
        "3679993bcce0 9b LazyCompile:~peakJitV8Target /tmp/v8_peak_jit.js:1:25",
        "7fdadafc6580 a4 JS:*peakJitV8TargetExtra /tmp/v8_peak_jit.js:1:25",
        "7fdadafc6580 a4 JS:*otherTarget /tmp/v8_peak_jit.js:1:25",
    ]

    missed = [row for row in positive_rows if not v8_row_is_optimized_target(row)]
    false_hits = [row for row in negative_rows if v8_row_is_optimized_target(row)]
    if missed or false_hits:
        raise AssertionError(
            f"unexpected V8 optimized-name matches missed={missed} false_hits={false_hits}"
        )

    print("v8_name_patterns_ok")
    return 0


def node_v8_script():
    return "\n".join(
        [
            "function peakJitV8Target(x) { return x + 1; }",
            "let sum = 0;",
            "for (let i = 0; i < 3000000; i++) sum += peakJitV8Target(i);",
            "console.log('node_v8_metadata_ok pid=' + process.pid + ' sum=' + sum);",
        ]
    )


def run_node_v8(timeout, libpeak=None):
    node = shutil.which("node")
    if node is None:
        print("node_v8_metadata_skip reason=missing-node")
        return SKIP_RETURN_CODE

    script = node_v8_script()

    with tempfile.TemporaryDirectory(prefix="peak-node-v8-jit-") as tmpdir:
        script_path = os.path.join(tmpdir, "v8_peak_jit.js")
        stats_prefix = os.path.join(tmpdir, "peak-node-v8-stats")
        trace_path = os.path.join(tmpdir, "peak-node-v8-jit-trace.csv")
        with open(script_path, "w", encoding="utf-8") as handle:
            handle.write(script)

        command = [node, "--perf-basic-prof", script_path]
        env = os.environ.copy()
        if libpeak is not None:
            env.update(
                {
                    "LD_PRELOAD": merge_preload(libpeak),
                    "PEAK_TARGET": V8_TARGET_NAME,
                    "PEAK_STATSLOG_PATH": stats_prefix,
                    "PEAK_JIT_ENABLE": "1",
                    "PEAK_JIT_PROVIDER": "perfmap",
                    "PEAK_JIT_TRACE_PATH": trace_path,
                    "PEAK_SIGNAL_RESERVE_EARLY": "never",
                    "PEAK_HEARTBEAT_INTERVAL": "0",
                    "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
                    "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
                    "PEAK_ENABLE_REATTACH": "0",
                    "PEAK_COST": "0",
                }
            )

        completed = subprocess.run(
            command,
            cwd=tmpdir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        output = completed.stdout + completed.stderr

        if completed.returncode != 0:
            if libpeak is not None:
                raise AssertionError(
                    f"Node failed while preloaded with PEAK rc={completed.returncode}\n"
                    f"{output}"
                )
            print(output, end="")
            print(f"node_v8_metadata_skip reason=node-failed rc={completed.returncode}")
            return SKIP_RETURN_CODE

        match = re.search(r"\bpid=([0-9]+)\b", output)
        if match is None:
            raise AssertionError(f"Node output did not include pid=\n{output}")

        pid = int(match.group(1))
        perf_map = f"/tmp/perf-{pid}.map"
        try:
            with open(perf_map, encoding="utf-8") as handle:
                rows = handle.read().splitlines()
        finally:
            try:
                os.unlink(perf_map)
            except FileNotFoundError:
                pass

        stats_csv = find_stats_csv(stats_prefix, pid) if libpeak is not None else None
        stats_rows = []
        if stats_csv is not None:
            with open(stats_csv, newline="", encoding="utf-8") as handle:
                stats_rows = list(csv.DictReader(handle))
        trace = ""
        if os.path.exists(trace_path):
            with open(trace_path, encoding="utf-8") as handle:
                trace = handle.read()

    target_rows = [row for row in rows if V8_TARGET_NAME in row]
    optimized_rows = [row for row in target_rows if v8_row_is_optimized_target(row)]
    if not target_rows:
        raise AssertionError(f"V8 perf map did not include {V8_TARGET_NAME}")
    if not optimized_rows:
        print(
            f"node_v8_metadata_skip reason=missing-optimized-row rows={target_rows}"
        )
        return SKIP_RETURN_CODE

    if libpeak is not None:
        count = symbol_count(stats_rows, V8_TARGET_NAME)
        if count <= 0:
            raise AssertionError(
                f"PEAK did not record Node/V8 JIT calls for {V8_TARGET_NAME}; "
                f"stats_csv={stats_csv} rows={stats_rows} trace={trace} "
                f"target_rows={target_rows}"
            )
        attached_records = [
            line
            for line in trace.splitlines()
            if ",perfmap-record,perfmap," in line and line.endswith(",attached")
        ]
        if not attached_records:
            raise AssertionError(
                f"PEAK stats recorded {count} calls but JIT trace had no attached "
                f"record; trace={trace}"
            )
        print(
            "node_v8_peak_profile_ok "
            f"target_rows={len(target_rows)} optimized_rows={len(optimized_rows)} "
            f"count={count}"
        )
        return 0

    print(
        "node_v8_metadata_ok "
        f"target_rows={len(target_rows)} optimized_rows={len(optimized_rows)}"
    )
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--framework",
        choices=("node-v8", "node-v8-peak", "v8-name-patterns"),
        required=True,
    )
    parser.add_argument("--libpeak")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    if args.timeout <= 0:
        print("--timeout must be positive", file=sys.stderr)
        return 2

    if args.framework == "v8-name-patterns":
        return run_v8_name_patterns()
    if args.framework == "node-v8-peak":
        if args.libpeak is None:
            print("--libpeak is required for node-v8-peak", file=sys.stderr)
            return 2
        return run_node_v8(args.timeout, os.path.abspath(args.libpeak))
    if args.framework == "node-v8":
        return run_node_v8(args.timeout)

    raise AssertionError(f"unhandled framework: {args.framework}")


if __name__ == "__main__":
    raise SystemExit(main())
