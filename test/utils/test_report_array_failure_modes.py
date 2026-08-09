#!/usr/bin/env python3
"""Exercise each final-report temporary-array allocation failure."""

import argparse
import os
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--preload", required=True)
    args = parser.parse_args()

    for successful_allocations in range(8):
        environment = os.environ.copy()
        environment.update({
            "LD_PRELOAD": args.preload,
            "PEAK_TARGET": "main,my_sleep_func",
            "PEAK_TEST_FAIL_REPORT_ARRAY_ALLOCATION_AFTER": str(
                successful_allocations),
        })
        completed = subprocess.run([args.executable], text=True,
                                   capture_output=True, env=environment)
        warning_count = completed.stderr.count(
            "disabling non-critical profiler subsystem")
        if (completed.returncode != 0 or warning_count != 1 or
                "final report allocation failed" not in completed.stderr or
                "Sleep is done" not in completed.stdout):
            sys.stdout.write(completed.stdout)
            sys.stderr.write(completed.stderr)
            return 1
    print("report_array_failure_modes_ok modes=8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
