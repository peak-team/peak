#!/usr/bin/env python3
"""Regression checks for the exact-exit test wrapper itself."""

import pathlib
import subprocess
import sys


RUNNER = pathlib.Path(__file__).with_name("run_expect_exit.py")


def invoke(*args: str) -> int:
    return subprocess.run([sys.executable, str(RUNNER), *args], check=False).returncode


def main() -> int:
    command = ["--", sys.executable, "-c", "import sys; sys.stderr.write('one\\n'); sys.exit(1)"]
    if invoke("--exit-code", "0", "--stderr-regex", "one", *command) == 0:
        return 1
    if invoke("--exit-code", "1", "--stderr-regex", "one",
              "--stderr-count-regex", "one", "--stderr-count", "2",
              *command) == 0:
        return 1
    if invoke("--exit-code", "1", "--stderr-regex", "one",
              "--stderr-count-regex", "one", "--stderr-count", "1",
              *command) != 0:
        return 1
    print("run_expect_exit_self_test_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
