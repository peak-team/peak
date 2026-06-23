#!/usr/bin/env python3

import argparse
import os
import re
import shlex
import subprocess
import sys


FAIL_RE = re.compile(
    r"Caught signal|Segmentation fault|SIGSEGV|signal 11|BAD TERMINATION|"
    r"Fatal error|MPI_ABORT|MPI_Abort|MPII_finalize|ofi_cq_read|hwloc"
)


def split_flags(value):
    return [flag for flag in shlex.split(value) if flag]


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpiexec", required=True)
    parser.add_argument("--nproc-flag", required=True)
    parser.add_argument("--nprocs", required=True)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument(
        "--mode",
        choices=[
            "no-finalize",
            "finalize-nonzero",
            "subset-finalize-nonzero",
            "subset-finalize-clean",
            "subset-finalize-clean-collective",
            "finalize-return-nonzero",
        ],
        required=True,
    )
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--preflags", default="")
    parser.add_argument("--postflags", default="")
    return parser.parse_args()


def main():
    args = parse_args()
    nprocs = int(args.nprocs)
    if args.mode.startswith("subset-") and nprocs < 2:
        print(f"mpi_lifecycle_check_ok mode={args.mode} skipped=needs-2-ranks")
        return 0

    env = os.environ.copy()
    old_preload = env.get("LD_PRELOAD")
    env["LD_PRELOAD"] = args.libpeak if not old_preload else args.libpeak + ":" + old_preload

    app_args = []
    expected = "MPI collective output is disabled for strict teardown"
    if args.mode == "finalize-nonzero":
        app_args.append("finalize-then-exit1")
        expected = "PMPI_Finalize was requested before nonzero exit status 1"
    elif args.mode == "subset-finalize-nonzero":
        app_args.append("subset-finalize-then-exit1")
        expected = "PMPI_Finalize was requested before nonzero exit status 1"
    elif args.mode == "subset-finalize-clean":
        app_args.append("subset-finalize-then-exit0")
        expected = "MPI collective output is disabled for strict teardown"
    elif args.mode == "subset-finalize-clean-collective":
        app_args.append("subset-finalize-then-exit0")
        expected = "PMPI_Finalize was not observed on every rank"
    elif args.mode == "finalize-return-nonzero":
        app_args.append("finalize-then-return1")
        expected = "PMPI_Finalize was requested before nonzero exit status 1"

    command = [
        args.mpiexec,
        args.nproc_flag,
        str(args.nprocs),
        *split_flags(args.preflags),
        args.exe,
        *split_flags(args.postflags),
        *app_args,
    ]

    proc = subprocess.run(
        command,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=args.timeout,
        check=False,
    )
    output = proc.stdout
    sys.stdout.write(output)

    if expected not in output:
        raise AssertionError(f"missing expected PEAK diagnostic: {expected}")
    if FAIL_RE.search(output):
        raise AssertionError("MPI lifecycle run produced a crash/fatal MPI diagnostic")
    if output.count("PEAK Library") < nprocs:
        raise AssertionError("expected rank-local PEAK output from every rank")
    if args.mode == "no-finalize" and proc.returncode != 0:
        raise AssertionError(f"no-finalize run returned {proc.returncode}")

    print(f"mpi_lifecycle_check_ok mode={args.mode} rc={proc.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
