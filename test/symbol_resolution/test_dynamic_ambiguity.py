#!/usr/bin/env python3
import os
import subprocess
import sys


def main():
    program, module_a, module_b = sys.argv[1:]
    env = os.environ.copy()
    env.update({
        "PEAK_TARGET": f"{os.path.basename(module_a)}!peak_test::Widget::func(int,double)",
        "PEAK_TARGET_FILE": "",
        "PEAK_TARGET_GROUP": "",
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
        "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
        "PEAK_ENABLE_REATTACH": "0",
    })
    for mode in ("ambiguous", "ambiguous-sequential"):
        try:
            completed = subprocess.run([program, mode], env=env,
                                       capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired as error:
            raise AssertionError("timed out: dynamic ambiguity") from error
        if completed.returncode != 0:
            raise AssertionError(completed.stdout + completed.stderr)
        assert "dynamic_ambiguous_terminal_ok" in completed.stdout
        assert completed.stderr.count(
            "dynamic C++ selector requires path-qualified module!symbol scope") == 1
    print("dynamic_ambiguity_test_ok")


if __name__ == "__main__":
    main()
