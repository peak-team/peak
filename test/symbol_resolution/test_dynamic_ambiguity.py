#!/usr/bin/env python3
import os
import subprocess
import sys


def main():
    program, module_a, module_b = sys.argv[1:]
    env = os.environ.copy()
    env.pop("PEAK_VERBOSITY", None)
    env.update({
        "PEAK_TARGET": f"{os.path.basename(module_a)}!peak_test::Widget::func(int,double)",
        "PEAK_TARGET_FILE": "",
        "PEAK_TARGET_GROUP": "",
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
        "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
        "PEAK_ENABLE_REATTACH": "0",
    })
    diagnostic = "dynamic C++ selector requires path-qualified module!symbol scope"
    for mode, verbosity, diagnostic_count in (
        ("ambiguous", None, 0),
        ("ambiguous", "info", 1),
        ("ambiguous-sequential", "info", 1),
    ):
        run_env = env.copy()
        if verbosity is not None:
            run_env["PEAK_VERBOSITY"] = verbosity
        try:
            completed = subprocess.run([program, mode], env=run_env,
                                       capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired as error:
            raise AssertionError("timed out: dynamic ambiguity") from error
        if completed.returncode != 0:
            raise AssertionError(completed.stdout + completed.stderr)
        assert "dynamic_ambiguous_terminal_ok" in completed.stdout
        assert completed.stderr.count(diagnostic) == diagnostic_count, completed.stderr
    print("dynamic_ambiguity_test_ok")


if __name__ == "__main__":
    main()
