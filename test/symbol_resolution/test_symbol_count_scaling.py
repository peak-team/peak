#!/usr/bin/env python3
import os
import subprocess
import sys


def collect(program, rich_kind):
    env = os.environ.copy()
    if rich_kind == "cxx":
        env["PEAK_TEST_LOAD_RICH"] = "1"
    elif rich_kind == "c":
        env["PEAK_TEST_LOAD_C_RICH"] = "1"
    completed = subprocess.run([program, "--symbol-count"], env=env,
                               capture_output=True, text=True, timeout=20)
    if completed.returncode != 0:
        raise AssertionError(completed.stdout + completed.stderr)
    fields = completed.stdout.strip().split()[1:]
    return {key: int(value) for key, value in
            (field.split("=", 1) for field in fields)}


def main():
    small = collect(sys.argv[1], None)
    cxx_rich = collect(sys.argv[1], "cxx")
    c_rich = collect(sys.argv[1], "c")
    assert small["module_passes"] == cxx_rich["module_passes"] == 1
    assert c_rich["module_passes"] == 1
    assert cxx_rich["module_enumerations"] == small["module_enumerations"] + 1
    assert c_rich["module_enumerations"] == small["module_enumerations"] + 1
    assert cxx_rich["symbol_visits"] >= small["symbol_visits"] + 128
    assert c_rich["symbol_visits"] >= small["symbol_visits"] + 128
    assert cxx_rich["demangles"] >= small["demangles"] + 128
    assert c_rich["demangles"] == small["demangles"]
    assert cxx_rich["candidate_matches"] == small["candidate_matches"] == 0
    assert c_rich["candidate_matches"] == 0
    print("symbol_count_scaling_test_ok")


if __name__ == "__main__":
    main()
