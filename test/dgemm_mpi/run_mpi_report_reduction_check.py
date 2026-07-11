#!/usr/bin/env python3

import argparse
import math
import re
import shlex
import subprocess


PATTERNS = {
    "combined": re.compile(
        r"maximum profile\+control overhead: owner_rank=(?P<owner>\d+) "
        r"profile_seconds=(?P<profile>[0-9.]+) control_seconds=(?P<control>[0-9.]+) "
        r"elapsed_seconds=(?P<elapsed>[0-9.]+) ratio=(?P<ratio>[0-9.]+)"
    ),
    "profile": re.compile(
        r"maximum profile overhead: owner_rank=(?P<owner>\d+) "
        r"profile_seconds=(?P<profile>[0-9.]+) elapsed_seconds=(?P<elapsed>[0-9.]+) "
        r"ratio=(?P<ratio>[0-9.]+)"
    ),
    "control": re.compile(
        r"maximum control overhead: owner_rank=(?P<owner>\d+) "
        r"control_seconds=(?P<control>[0-9.]+) elapsed_seconds=(?P<elapsed>[0-9.]+) "
        r"ratio=(?P<ratio>[0-9.]+)"
    ),
    "management": re.compile(
        r"maximum heartbeat management overhead: owner_rank=(?P<owner>\d+) "
        r"cpu_seconds=(?P<management>[0-9.]+) elapsed_seconds=(?P<elapsed>[0-9.]+) "
        r"ratio=(?P<ratio>[0-9.]+)"
    ),
    "profile_risk": re.compile(
        r"maximum profile\+control risk overhead: owner_rank=(?P<owner>\d+) "
        r"profile_seconds=(?P<profile>[0-9.]+) raw_control_seconds=(?P<control>[0-9.]+) "
        r"local_ranks=(?P<local_ranks>\d+) control_risk_seconds=(?P<control_risk>[0-9.]+) "
        r"risk_seconds=(?P<risk>[0-9.]+) elapsed_seconds=(?P<elapsed>[0-9.]+) "
        r"ratio=(?P<ratio>[0-9.]+)"
    ),
    "control_risk": re.compile(
        r"maximum control risk overhead: owner_rank=(?P<owner>\d+) "
        r"raw_control_seconds=(?P<control>[0-9.]+) local_ranks=(?P<local_ranks>\d+) "
        r"control_risk_seconds=(?P<control_risk>[0-9.]+) elapsed_seconds=(?P<elapsed>[0-9.]+) "
        r"ratio=(?P<ratio>[0-9.]+)"
    ),
}


def close(label, actual, expected):
    if not math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-9):
        raise AssertionError(f"{label}: expected {expected}, got {actual}")


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpiexec", required=True)
    parser.add_argument("--nproc-flag", required=True)
    parser.add_argument("--nprocs", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--preflags", default="")
    parser.add_argument("--postflags", default="")
    return parser.parse_args()


def split_flags(value):
    return [flag for flag in shlex.split(value) if flag]


def main():
    args = parse_args()
    command = [
        args.mpiexec,
        args.nproc_flag,
        args.nprocs,
        *split_flags(args.preflags),
        args.exe,
        *split_flags(args.postflags),
    ]
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=15,
        check=False,
    )
    output = proc.stdout or ""
    print(output, end="")
    if proc.returncode != 0 or "mpi_report_reduction_ok" not in output:
        raise AssertionError(f"MPI report harness failed with rc={proc.returncode}")

    expected_owners = {
        "combined": 1,
        "profile": 1,
        "control": 0,
        "management": 1,
        "profile_risk": 1,
        "control_risk": 0,
    }
    expected_tuples = {
        0: {"profile": 1.0, "control": 1.0, "management": 0.5, "elapsed": 10.0},
        1: {"profile": 6.0, "control": 2.0, "management": 4.0, "elapsed": 20.0},
    }
    for name, pattern in PATTERNS.items():
        match = pattern.search(output)
        if match is None:
            raise AssertionError(f"missing production report line for {name}")
        values = match.groupdict()
        owner = int(values["owner"])
        if owner != expected_owners[name]:
            raise AssertionError(f"{name}: expected owner {expected_owners[name]}, got {owner}")
        expected = expected_tuples[owner]
        for field in ("profile", "control", "management", "elapsed"):
            if values.get(field) is not None:
                close(f"{name} {field}", float(values[field]), expected[field])

        elapsed = float(values["elapsed"])
        if name == "combined":
            numerator = float(values["profile"]) + float(values["control"])
        elif name == "profile":
            numerator = float(values["profile"])
        elif name == "control":
            numerator = float(values["control"])
        elif name == "management":
            numerator = float(values["management"])
        else:
            local_ranks = int(values["local_ranks"])
            control_risk = float(values["control_risk"])
            if local_ranks != 3:
                raise AssertionError(f"{name}: expected local_ranks=3, got {local_ranks}")
            close(
                f"{name} control-risk numerator",
                control_risk,
                float(values["control"]) * local_ranks,
            )
            if name == "profile_risk":
                numerator = float(values["risk"])
                close(
                    "profile-risk combined numerator",
                    numerator,
                    float(values["profile"]) + control_risk,
                )
            else:
                numerator = control_risk
        close(f"{name} ratio", float(values["ratio"]), numerator / elapsed)

    print("mpi_report_lines_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
