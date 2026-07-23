#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--preload-env", required=True)
    args = parser.parse_args()

    env = os.environ.copy()
    for name in ("PEAK_TARGET_FILE", "PEAK_TARGET_GROUP"):
        env.pop(name, None)

    existing_preload = env.get(args.preload_env, "")
    env[args.preload_env] = (
        f"{args.libpeak}:{existing_preload}"
        if existing_preload
        else args.libpeak
    )
    env.update({
        "OMP_NUM_THREADS": "4",
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_TARGET": "my_sleep_func",
        "PEAK_TEXT_OUTPUT": "1",
        "PEAK_VERBOSITY": "quiet",
    })

    with tempfile.TemporaryDirectory(prefix="peak-report-format-") as tmpdir:
        env["PEAK_STATSLOG_PATH"] = str(Path(tmpdir) / "stats.log")
        completed = subprocess.run(
            [args.exe],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    output = completed.stdout + completed.stderr
    require(completed.returncode == 0,
            f"fixture exited with {completed.returncode}\n{output}")

    required_sections = (
        "Application",
        "Overhead summary",
        "Controller accounting",
        "Detailed metrics (stable key=value)",
        "Function call statistics",
        "Function timing statistics",
    )
    output_lines = output.splitlines()
    for section in required_sections:
        require(output_lines.count(section) == 1,
                f"missing or duplicate report section: {section}")

    require(output.count("PEAK Library Performance Report") == 1,
            "report title must appear exactly once")
    require(re.search(r"^Time: [0-9.eE+-]+$", output, re.MULTILINE),
            "missing stable Time field")
    require(re.search(
                r"^Estimated overhead: [0-9.eE+-]+s per call and "
                r"[0-9.eE+-]+s total$",
                output,
                re.MULTILINE),
            "missing stable estimated-overhead field")
    require(re.search(r"^Report scope: local \(1 process\)$",
                      output, re.MULTILINE),
            "missing local report scope")
    require(re.search(r"^Instrumented targets: [1-9][0-9]*$",
                      output, re.MULTILINE),
            "missing instrumented-target count")
    require(re.search(r"^Profiled targets: [1-9][0-9]*$",
                      output, re.MULTILINE),
            "missing profiled-target count")
    require(re.search(r"^Recorded calls: [1-9][0-9]*$",
                      output, re.MULTILINE),
            "missing recorded-call count")
    require("[peak] local profile+control overhead:" in output,
            "missing stable profile/control metric")
    require("[peak] local final transition coverage:" in output,
            "missing stable transition-coverage metric")
    require(output.count("my_sleep_func") >= 2,
            "profiled function must appear in both tables")
    require("Markers: * ever detached; ** ever reattached after detachment."
            in output,
            "missing transition marker legend")

    rules = [line for line in output_lines
             if re.fullmatch(r"[=-]{80,}", line)]
    require(rules, "missing ASCII report rules")
    require({len(line) for line in rules} == {99},
            "report rules must use one consistent width")

    print("report_format_check_ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
