#!/usr/bin/env python3
"""Observe real heartbeat counters without requesting a physical transition."""

import argparse
import csv
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

from run_reattach_diagnostics_check import parse_records


def run_bounded(command, env, timeout):
    process = subprocess.Popen(
        command,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=timeout)
        return process.returncode, output, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            output, _ = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, _ = process.communicate()
        return process.returncode, output, True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--timeout", type=int, default=15)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    with tempfile.TemporaryDirectory(prefix="peak-reattach-heartbeat-") as tmp:
        root = Path(tmp)
        trace = root / "heartbeat.csv"
        stats = root / "stats"
        sidecar = Path(str(trace) + ".reattach-diagnostics")
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("PEAK_") and not key.startswith("FAKE_DETACH_HELPER_")
        }
        env.update({
            "LD_PRELOAD": os.path.realpath(args.libpeak),
            "PEAK_DETACH_TRACE_PATH": str(trace),
            "PEAK_REATTACH_DIAGNOSTICS": "1",
            "PEAK_TARGET": "peak_detach_hot_target",
            "PEAK_MAX_NUM_THREADS": "16",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            # Reattach accounting must run so the test can validate budget
            # round/generation identity; zero cost and extreme gates keep the
            # target attached and avoid any Gum detach request.
            "PEAK_ENABLE_REATTACH": "1",
            "PEAK_COST": "0",
            # These values must fit the float-based environment schema.  An
            # out-of-range value falls back to the default threshold and can
            # request a real detach, invalidating this observation-only case.
            "PEAK_OVERHEAD_RATIO": "3e38",
            "PEAK_GLOBAL_OVERHEAD_RATIO": "3e38",
            "PEAK_HEARTBEAT_INTERVAL": "0.005",
            "PEAK_HIBERNATION_CYCLE": "1",
            "PEAK_DETACH_COUNT": "2147483647",
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": "strict",
            "PEAK_DETACH_BACKEND": "signal",
            "PEAK_STATSLOG_PATH": str(stats),
        })
        rc, output, timed_out = run_bounded(
            [args.exe, "--threads", "4", "--seconds", "1"], env, args.timeout)
        if timed_out:
            print(f"heartbeat workload timed out after {args.timeout}s", file=sys.stderr)
            if output:
                print(output, file=sys.stderr)
            return 1
        if rc != 0:
            print(f"heartbeat workload failed rc={rc}", file=sys.stderr)
            print(output, file=sys.stderr)
            return 1

        # The deliberately high threshold and zero estimated cost make this an
        # observation of heartbeat sampling, not a detach/reattach test.
        if trace.exists():
            with trace.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.reader(stream))
            if any(len(row) > 3 and row[3] in {"detach", "reattach"} for row in rows):
                print("heartbeat-only case unexpectedly recorded a physical transition", file=sys.stderr)
                return 1
        if not sidecar.is_file():
            print("heartbeat diagnostics sidecar missing", file=sys.stderr)
            return 1
        try:
            records = parse_records(sidecar)
        except (OSError, ValueError) as exc:
            print(f"invalid heartbeat diagnostics: {exc}", file=sys.stderr)
            return 1

        if records[0][1].get("capture") != "init-to-first-controller-stop":
            print("unexpected diagnostics capture boundary", file=sys.stderr)
            return 1
        end = records[-1][1]
        if end.get("complete") != "1" or any(
            end.get(key) != "0" for key in ("busy_drops", "overflow", "out_of_range", "concurrent")
        ):
            print("heartbeat diagnostics capture incomplete", file=sys.stderr)
            return 1

        hook_rows = [fields for kind, fields in records if kind == "RD_HOOK"]
        if not hook_rows:
            print("no per-hook heartbeat summary was emitted", file=sys.stderr)
            return 1
        try:
            active = [row for row in hook_rows if int(row.get("rounds", "0")) > 0]
            sampled = [row for row in active if int(row.get("samples", "0")) > 0]
        except ValueError:
            print("malformed heartbeat round/sample counters", file=sys.stderr)
            return 1
        if not sampled:
            print("no heartbeat hook had both rounds and samples", file=sys.stderr)
            return 1
        required = {
            "budget_profile", "control", "spent_ratio", "gate_ratio",
            "headroom", "predicted_stop", "ready", "reattach",
            "sample_round", "sample_gen", "budget_round", "budget_gen",
            "budget_sample_round", "budget_sample_gen",
        }
        if not required.issubset(sampled[0]):
            print(f"heartbeat summary missing budget fields: {sorted(required - set(sampled[0]))}", file=sys.stderr)
            return 1
        try:
            row = sampled[0]
            identity = {key: int(row[key]) for key in (
                "rounds", "samples", "gen", "sample_round", "sample_gen",
                "budget_round", "budget_gen", "budget_sample_round",
                "budget_sample_gen",
            )}
        except (KeyError, ValueError):
            print("malformed heartbeat round/generation identity", file=sys.stderr)
            return 1
        if not (
            identity["rounds"] >= identity["sample_round"] > 0
            and identity["samples"] > 0
            and identity["sample_gen"] <= identity["gen"]
            and identity["budget_round"] > 0
            and identity["budget_round"] <= identity["rounds"]
            and identity["budget_gen"] <= identity["gen"]
            and identity["budget_sample_round"] <= identity["budget_round"]
            and identity["budget_sample_gen"] <= identity["budget_gen"]
        ):
            print(f"inconsistent heartbeat sample/budget identity: {identity}", file=sys.stderr)
            return 1

    row = sampled[0]
    print(
        "reattach_heartbeat_diagnostics_ok "
        f"hook={row.get('hook')} rounds={row.get('rounds')} "
        f"samples={row.get('samples')} sample_round={row.get('sample_round')} "
        f"sample_gen={row.get('sample_gen')} budget_round={row.get('budget_round')} "
        f"budget_gen={row.get('budget_gen')} "
        f"budget_sample_round={row.get('budget_sample_round')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
