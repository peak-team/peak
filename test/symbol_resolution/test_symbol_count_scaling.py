#!/usr/bin/env python3
import os
import subprocess
import sys


def collect(program, rich):
    env = os.environ.copy()
    if rich:
        env["PEAK_TEST_LOAD_RICH"] = "1"
    completed = subprocess.run([program, "--symbol-count"], env=env,
                               capture_output=True, text=True, timeout=20)
    if completed.returncode != 0:
        raise AssertionError(completed.stdout + completed.stderr)
    fields = completed.stdout.strip().split()[1:]
    return {key: int(value) for key, value in
            (field.split("=", 1) for field in fields)}


def main():
    small = collect(sys.argv[1], False)
    large = collect(sys.argv[1], True)
    assert small["module_passes"] == large["module_passes"] == 1
    assert large["module_enumerations"] == small["module_enumerations"] + 1
    assert large["symbol_visits"] >= small["symbol_visits"] + 128
    assert large["demangles"] >= small["demangles"] + 128
    assert large["candidate_matches"] == small["candidate_matches"] == 0
    print("symbol_count_scaling_test_ok")


if __name__ == "__main__":
    main()
