#!/usr/bin/env python3

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


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
            "no-finalize-nonzero",
            "no-finalize-collective-disabled",
            "finalize-clean",
            "finalize-clean-output-mpi",
            "finalize-clean-output-local",
            "finalize-clean-output-socket-bad-host",
            "finalize-clean-output-socket-token-mismatch",
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
    parser.add_argument("--require-pinned-finalize", action="store_true")
    parser.add_argument("--require-detach-trace", action="store_true")
    parser.add_argument("--include-finalize-target", action="store_true")
    parser.add_argument("--forbid-stats-function", action="append", default=[])
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
    if args.include_finalize_target:
        current_target = env.get("PEAK_TARGET", "")
        env["PEAK_TARGET"] = (
            f"{current_target},MPI_Finalize" if current_target else "MPI_Finalize"
        )
    trace_path = env.get("PEAK_DETACH_TRACE_PATH")
    if trace_path:
        try:
            Path(trace_path).unlink()
        except FileNotFoundError:
            pass

    app_args = []
    expected = "PMPI_Finalize was not observed on every rank"
    expected_peak_tables = 0
    expected_stats_files = nprocs
    if args.mode == "no-finalize-nonzero":
        app_args.append("no-finalize-then-exit1")
        expected = "PMPI_Finalize was not observed on every rank"
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "no-finalize-collective-disabled":
        env["PEAK_MPI_COLLECTIVE_OUTPUT"] = "0"
        expected = "Aggregate output is disabled for strict teardown"
        expected_peak_tables = 0
        expected_stats_files = nprocs
    if args.mode == "finalize-clean":
        app_args.append("finalize-then-exit0")
        expected = "PMPI_Finalize was observed on every rank"
        expected_peak_tables = 1
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-mpi":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        app_args.append("finalize-then-exit0")
        expected = "PMPI_Finalize was observed on every rank"
        expected_peak_tables = 1
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-local":
        env["PEAK_OUTPUT_AGGREGATION"] = "rank-local"
        app_args.append("finalize-then-exit0")
        expected = "Aggregate output is disabled for strict teardown"
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "finalize-clean-output-socket-bad-host":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_HOST"] = "192.0.2.1"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        app_args.append("finalize-then-exit0")
        expected = "Socket aggregation"
        expected_peak_tables = 0
        expected_stats_files = 0
    elif args.mode == "finalize-clean-output-socket-token-mismatch":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        app_args.append("finalize-then-exit0")
        expected = "Socket aggregation received"
        expected_peak_tables = 0
        expected_stats_files = 0
    elif args.mode == "finalize-nonzero":
        app_args.append("finalize-then-exit1")
        expected = "PMPI_Finalize was observed on every rank"
        expected_peak_tables = 1
        expected_stats_files = 1
    elif args.mode == "subset-finalize-nonzero":
        app_args.append("subset-finalize-then-exit1")
        expected = None
        expected_peak_tables = 0
        expected_stats_files = None
    elif args.mode == "subset-finalize-clean":
        app_args.append("subset-finalize-then-exit0")
        expected = "PMPI_Finalize was not observed on every rank"
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "subset-finalize-clean-collective":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        app_args.append("subset-finalize-then-exit0")
        expected = "PMPI_Finalize was not observed on every rank"
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "finalize-return-nonzero":
        app_args.append("finalize-then-return1")
        expected = "PMPI_Finalize was observed on every rank"
        expected_peak_tables = 1
        expected_stats_files = 1

    if args.mode == "finalize-clean-output-socket-token-mismatch":
        rank_wrapper = (
            "rank=${PMI_RANK:-${PMIX_RANK:-${OMPI_COMM_WORLD_RANK:-${SLURM_PROCID:-0}}}}; "
            "if [ \"$rank\" = \"1\" ]; then "
            "export PEAK_OUTPUT_AGGREGATION_TOKEN=peak-token-mismatch; "
            "else export PEAK_OUTPUT_AGGREGATION_TOKEN=peak-token-match; fi; "
            "exec \"$@\""
        )
        executable = ["/bin/bash", "-lc", rank_wrapper, "peak-token-wrapper", args.exe]
    else:
        executable = [args.exe]

    command = [
        args.mpiexec,
        args.nproc_flag,
        str(args.nprocs),
        *split_flags(args.preflags),
        *executable,
        *split_flags(args.postflags),
        *app_args,
    ]

    with tempfile.TemporaryDirectory(
        prefix=f"peak-mpi-lifecycle-{args.mode}-",
        dir=os.getcwd(),
    ) as stats_dir:
        stats_prefix = str(Path(stats_dir) / "peak-stats")
        env["PEAK_STATSLOG_PATH"] = stats_prefix
        proc = subprocess.run(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.timeout,
            check=False,
        )
        stats_files = sorted(Path(stats_dir).glob("peak-stats-p*.csv"))
        stats_text = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for path in stats_files
        )
    output = proc.stdout
    sys.stdout.write(output)

    if expected is not None and expected not in output:
        raise AssertionError(f"missing expected PEAK diagnostic: {expected}")
    if args.require_pinned_finalize and "Leaving PEAK target hooks pinned" not in output:
        raise AssertionError("missing pinned-finalize diagnostic")
    for function_name in args.forbid_stats_function:
        if f'"{function_name}",' in stats_text:
            raise AssertionError(f"unexpected stats row for {function_name}")
    if FAIL_RE.search(output):
        raise AssertionError("MPI lifecycle run produced a crash/fatal MPI diagnostic")
    if args.require_detach_trace:
        if not trace_path:
            raise AssertionError("PEAK_DETACH_TRACE_PATH is required for detach trace assertion")
        trace = Path(trace_path)
        if not trace.exists():
            raise AssertionError(f"detach trace was not written: {trace}")
        trace_text = trace.read_text(encoding="utf-8", errors="replace")
        if ",detach,success," not in trace_text:
            raise AssertionError("detach trace does not contain a successful detach")
    if output.count("PEAK Library") < expected_peak_tables:
        raise AssertionError(
            f"expected at least {expected_peak_tables} PEAK output table(s)"
        )
    if expected_stats_files is not None and len(stats_files) != expected_stats_files:
        raise AssertionError(
            f"expected {expected_stats_files} PEAK stats file(s), got {len(stats_files)}"
        )
    if args.mode in (
        "no-finalize",
        "finalize-clean",
        "finalize-clean-output-local",
        "finalize-clean-output-socket-bad-host",
        "finalize-clean-output-socket-token-mismatch",
    ) and proc.returncode != 0:
        raise AssertionError(f"{args.mode} run returned {proc.returncode}")

    print(f"mpi_lifecycle_check_ok mode={args.mode} rc={proc.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
