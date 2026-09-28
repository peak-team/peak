#!/usr/bin/env python3
"""Typed Gum test double: classifier regression, not full Gum hook validation."""
import argparse
import os
from pathlib import Path
import signal
import subprocess


def run(command):
    child = subprocess.Popen(command, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True)
    try:
        output, errors = child.communicate(timeout=30)
        print(output.decode(), end="")
        if child.returncode:
            raise RuntimeError(errors.decode() or str(child.returncode))
    finally:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--expect-old", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    source = (args.source or root / "cmake/peak-gum/peak_gum_low_gap.c").resolve()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    for name, flags in [("exec", ["-fno-pie", "-no-pie"]),
                        ("pie", ["-fPIE", "-pie"])]:
        binary = args.build_dir.resolve() / ("classifier-" + name)
        command = [args.cc, "-O2", "-fno-plt", "-Wall", "-Wextra", "-Werror", *flags,
                   "-I" + str(here), "-I" + str(root / "include"),
                   '-DOVERLAY_SOURCE="' + str(source) + '"',
                   str(here / "test_classifier.c"), "-ldl", "-o", str(binary)]
        if args.expect_old:
            command.append("-DEXPECT_OLD")
        run(command)
        run([str(binary)])


if __name__ == "__main__":
    def cancelled(signum, frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        raise KeyboardInterrupt("classifier regression cancelled")

    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    main()
