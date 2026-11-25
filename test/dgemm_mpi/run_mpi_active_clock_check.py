#!/usr/bin/env python3

import argparse
import os
import re
import shlex
import signal
import subprocess
import tempfile


APPLICATION = re.compile(
    r"mpi_active_clock_ok rank=(\d+) pid=(\d+) wait=(\S+) second_wait=(\S+) "
    r"dense=(\S+) gap=(\S+) recovery=(\S+)"
)
HEARTBEAT = re.compile(
    r"\[peak mpi active clock\] pid=(\d+) elapsed=(\S+) blocked=(\S+) "
    r"lifetime_target=(\S+) configured_target=(\S+)"
)
CLOCK_REJECTION = re.compile(
    r"\[peak mpi active clock unavailable\] pid=(\d+) reason=clock-qualification(?:\n|$)"
)


def run(args, enabled, dense=False):
    env = {name: value for name, value in os.environ.items()
           if not name.startswith("PEAK_")}
    env.update(
        PEAK_TARGET="peak_mpi_active_clock_test_target",
        PEAK_HEARTBEAT_INTERVAL="0.02",
        PEAK_HB_MAX_US="20000",
        PEAK_ENABLE_GLOBAL_HEARTBEAT="1",
        PEAK_VERBOSITY="debug",
    )
    if enabled is not None:
        env["PEAK_MPI_ACTIVE_CLOCK"] = enabled
    command = [
        args.mpiexec, args.nproc_flag, "2", *shlex.split(args.preflags),
        "env", f"LD_PRELOAD={args.libpeak}", args.exe, *shlex.split(args.postflags),
    ]
    if dense:
        command.append("--dense")
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

    application = {int(rank): (pid, *map(float, values))
                   for rank, pid, *values in APPLICATION.findall(output)}
    if set(application) != {0, 1}:
        raise AssertionError(f"missing application success markers\n{output}")
    if "Report scope: aggregate (2 MPI ranks)" not in output or "Recorded calls: 4" not in output:
        raise AssertionError(f"missing aggregate report or target samples\n{output}")
    samples = {pid: tuple(map(float, values)) for pid, *values
               in HEARTBEAT.findall(output)}
    rejections = set(CLOCK_REJECTION.findall(output))
    if enabled == "0":
        if "[peak mpi active clock" in output:
            raise AssertionError(f"disabled clock produced heartbeat diagnostics\n{output}")
        return

    pid, waited, second_wait, dense_time, gap, recovery = application[0]
    if waited < 0.1 or (dense and second_wait < 0.1):
        raise AssertionError(f"application did not exercise the long MPI waits\n{output}")
    rank_pids = {rank_pid for rank_pid, *_ in application.values()}
    if (len(rank_pids) != 2 or set(samples) & rejections
            or set(samples) | rejections != rank_pids):
        raise AssertionError(f"missing or conflicting per-rank clock diagnostics\n{output}")
    for rank_pid, (elapsed, blocked, target, configured) in samples.items():
        if not (0 <= blocked <= elapsed and 0 <= target <= configured):
            raise AssertionError(f"invalid heartbeat sample for PID {rank_pid}\n{output}")
    if rejections:
        print(f"MPI clock qualification failed on PID(s) {', '.join(sorted(rejections))}")
    if pid in rejections:
        return

    elapsed, blocked, target, configured = samples[pid]
    if not (waited / 2 <= blocked and target < configured):
        raise AssertionError(f"heartbeat did not discount the first Wait ({waited:.3g}s)\n{output}")
    if dense:
        combined_wait = waited + second_wait
        if blocked < 0.8 * combined_wait:
            raise AssertionError(f"heartbeat lost a long Wait after dense MPI\n{output}")
        # The final observation can precede application exit by one or more
        # ticks. Allow that lag and some uncertainty at the dense phase edges.
        if not (blocked <= combined_wait + dense_time + 0.06
                and elapsed - blocked >= recovery + gap - 0.1):
            raise AssertionError(f"active clock did not recover application time\n{output}")
    print(f"Wait {waited:.3g}s, second Wait {second_wait:.3g}s, heartbeat blocked "
          f"{blocked:.3g}s, target {target:.3g} (configured {configured:.3g})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpiexec", required=True)
    parser.add_argument("--nproc-flag", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--preflags", default="")
    parser.add_argument("--postflags", default="")
    args = parser.parse_args()
    run(args, None)
    run(args, "1")
    run(args, None, dense=True)
    run(args, "0")


if __name__ == "__main__":
    main()
