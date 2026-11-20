#!/usr/bin/env python3

import argparse
import os
import re
import shlex
import signal
import subprocess
import tempfile


APPLICATION = re.compile(r"mpi_active_clock_ok rank=(\d+) pid=(\d+) wait=(\S+)")
HEARTBEAT = re.compile(
    r"\[peak mpi active clock\] pid=(\d+) elapsed=(\S+) blocked=(\S+) "
    r"lifetime_target=(\S+) configured_target=(\S+)"
)


def run(args, enabled):
    env = {name: value for name, value in os.environ.items()
           if not name.startswith("PEAK_")}
    env.update(
        PEAK_MPI_ACTIVE_CLOCK=enabled,
        PEAK_TARGET="peak_mpi_active_clock_test_target",
        PEAK_HEARTBEAT_INTERVAL="0.02",
        PEAK_HB_MAX_US="20000",
        PEAK_ENABLE_GLOBAL_HEARTBEAT="1",
        PEAK_VERBOSITY="debug",
    )
    command = [
        args.mpiexec, args.nproc_flag, "2", *shlex.split(args.preflags),
        "env", f"LD_PRELOAD={args.libpeak}", args.exe, *shlex.split(args.postflags),
    ]
    with tempfile.TemporaryDirectory(prefix="peak-mpi-clock-") as directory:
        with subprocess.Popen(
            command, cwd=directory, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, start_new_session=True,
        ) as process:
            try:
                output, _ = process.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, _ = process.communicate()
                raise AssertionError(f"MPI active-clock test timed out\n{output}")
    if process.returncode != 0:
        raise AssertionError(f"MPI active-clock test failed with {process.returncode}\n{output}")

    application = {int(rank): (pid, float(wait)) for rank, pid, wait
                   in APPLICATION.findall(output)}
    if set(application) != {0, 1}:
        raise AssertionError(f"missing application success markers\n{output}")
    if "Report scope: aggregate (2 MPI ranks)" not in output or "Recorded calls: 4" not in output:
        raise AssertionError(f"missing aggregate report or target samples\n{output}")
    samples = {pid: tuple(map(float, values)) for pid, *values
               in HEARTBEAT.findall(output)}
    if enabled == "0":
        if samples:
            raise AssertionError(f"disabled clock produced heartbeat samples\n{output}")
        return

    pid, waited = application[0]
    if set(samples) != {pid for pid, _ in application.values()}:
        raise AssertionError(f"missing per-rank heartbeat samples\n{output}")
    elapsed, blocked, target, configured = samples[pid]
    if not (waited >= 0.1 and waited / 2 <= blocked <= elapsed
            and 0 <= target < configured):
        raise AssertionError(f"heartbeat did not discount the first Wait ({waited:.3g}s)\n{output}")
    print(f"first Wait {waited:.3g}s, heartbeat blocked {blocked:.3g}s, "
          f"target {target:.3g} (configured {configured:.3g})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpiexec", required=True)
    parser.add_argument("--nproc-flag", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--preflags", default="")
    parser.add_argument("--postflags", default="")
    args = parser.parse_args()
    run(args, "1")
    run(args, "0")


if __name__ == "__main__":
    main()
