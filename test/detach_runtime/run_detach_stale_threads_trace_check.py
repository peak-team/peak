#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys


FATAL_PATTERNS = [
    r"not proven safe",
    r"leaving listener state alive",
    r"fatal",
    r"tracked thread snapshot exceeded",
    r"thread id .* exceeds",
    r"detach helper shutdown failed",
    r"timed out draining pending target hook detach/reattach requests",
    r"Gum .* failed",
]


def trace_has_success(path, operation):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for raw_line in handle:
                fields = [field.strip() for field in raw_line.split(",")]
                if len(fields) >= 5 and fields[3] == operation and fields[4] == "success":
                    return True
    except FileNotFoundError:
        return False
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--trace-path", required=True)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--stale-threads", type=int, required=True)
    parser.add_argument("--stale-calls", type=int, required=True)
    parser.add_argument("program_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    program_args = args.program_args
    if program_args and program_args[0] == "--":
        program_args = program_args[1:]

    try:
        os.unlink(args.trace_path)
    except FileNotFoundError:
        pass

    env = os.environ.copy()
    env.update(
        {
            "LD_PRELOAD": args.libpeak,
            "PEAK_TARGET": "peak_detach_stale_target",
            "PEAK_MAX_NUM_THREADS": "192",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
            "PEAK_ENABLE_REATTACH": "1",
            "PEAK_COST": "0.0000001",
            "PEAK_OVERHEAD_RATIO": "0.01",
            "PEAK_HEARTBEAT_INTERVAL": "0.001",
            "PEAK_HIBERNATION_CYCLE": "1",
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": "strict",
            "PEAK_DETACH_TRACE_PATH": args.trace_path,
        }
    )

    proc = subprocess.run(
        [args.exe, *program_args],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.timeout,
    )
    output = proc.stdout
    sys.stdout.write(output)

    if proc.returncode != 0:
        raise AssertionError(f"stale-thread fixture exited with {proc.returncode}")

    for pattern in FATAL_PATTERNS:
        if re.search(pattern, output):
            raise AssertionError(f"fatal diagnostic matched: {pattern}")

    expected_stale_calls = args.stale_threads * args.stale_calls
    summary_re = (
        rf"stale_threads={args.stale_threads}\b.*"
        rf"stale_blocked={args.stale_threads}\b.*"
        rf"stale_calls={expected_stale_calls}\b"
    )
    if not re.search(summary_re, output):
        raise AssertionError(
            "stale-thread summary did not contain the expected blocked/call counts"
        )

    if not re.search(r"peak_detach_stale_target[*]+\|[ \t]+[1-9][0-9]*\|", output):
        raise AssertionError("missing profiled stale target row")

    if not trace_has_success(args.trace_path, "detach"):
        raise AssertionError("missing detach success in strict trace")
    if not trace_has_success(args.trace_path, "reattach"):
        raise AssertionError("missing reattach success in strict trace")

    print("stale_threads_trace_check_ok detach_success=1 reattach_success=1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.TimeoutExpired as exc:
        print(f"stale-thread fixture timed out after {exc.timeout}s", file=sys.stderr)
        raise SystemExit(1)
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
