#!/usr/bin/env python3

import argparse
import csv
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


FAIL_RE = re.compile(
    r"Caught signal|Segmentation fault|SIGSEGV|signal 11|"
    r"Fatal error|MPI_ABORT|MPI_Abort|MPII_finalize|ofi_cq_read|hwloc"
)
LAUNCHER_ABNORMAL_RE = re.compile(
    r"BAD TERMINATION|KILLED BY SIGNAL|exiting improperly|"
    r"mpiexec has exited due to process rank"
)
OPENMPI_IMPROPER_EXIT_BLOCK_RE = re.compile(
    r"-{10,}\n"
    r"mpiexec has exited due to process rank.*?"
    r"-{10,}\n",
    re.DOTALL,
)
EXPECTED_LAUNCHER_ABNORMAL_MODES = {
    "no-finalize",
    "no-finalize-nonzero",
    "subset-finalize-nonzero",
    "subset-finalize-clean",
    "subset-finalize-clean-collective",
    "subset-finalize-handoff",
}
EXPECTED_NONZERO_RETURN_MODES = {
    "finalize-nonzero",
    "finalize-return-nonzero",
}


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
            "finalize-clean-output-mpi-intel-default",
            "finalize-clean-output-mpi-intel-real-finalize",
            "finalize-clean-output-local",
            "finalize-clean-output-socket-bad-host",
            "finalize-clean-output-socket-bad-host-no-fallback",
            "finalize-clean-output-socket-release-fail",
            "finalize-clean-output-socket-token-mismatch",
            "finalize-clean-output-socket-token-mismatch-no-fallback",
            "finalize-socket-post-work",
            "finalize-defer-post-work",
            "finalize-nonzero",
            "subset-finalize-nonzero",
            "subset-finalize-clean",
            "subset-finalize-clean-collective",
            "subset-finalize-handoff",
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
    if (args.mode in {
            "finalize-clean-output-socket-bad-host",
            "finalize-clean-output-socket-bad-host-no-fallback",
            "finalize-clean-output-socket-release-fail",
            "finalize-clean-output-socket-token-mismatch",
            "finalize-clean-output-socket-token-mismatch-no-fallback",
        } and nprocs < 2):
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
    expected_max_peak_tables = None
    expected_stats_files = None
    expected_min_stats_files = None
    expected_min_target_count = None
    expected_extra = []
    done_file = None
    if args.mode == "no-finalize-nonzero":
        app_args.append("no-finalize-then-exit1")
        expected = "PMPI_Finalize was not observed on every rank"
        expected_peak_tables = 0
        expected_stats_files = None
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
    elif args.mode == "finalize-clean-output-mpi-intel-default":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env["PEAK_TEST_MPI_LIBRARY_VERSION"] = "Intel(R) MPI Library 2019"
        app_args.append("finalize-then-exit0")
        expected = "skipping real PMPI_Finalize for this MPI runtime"
        expected_extra.append("PMPI_Finalize was observed on every rank")
        expected_peak_tables = 0
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-mpi-intel-real-finalize":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env["PEAK_TEST_MPI_LIBRARY_VERSION"] = "Intel(R) MPI Library 2019"
        env["PEAK_MPI_REAL_FINALIZE"] = "1"
        app_args.append("finalize-then-exit0")
        expected = "PEAK output is complete; returning to real PMPI_Finalize"
        expected_extra.append("PMPI_Finalize was observed on every rank")
        expected_peak_tables = 0
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
        expected_stats_files = nprocs
    elif args.mode == "finalize-clean-output-socket-bad-host-no-fallback":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_HOST"] = "192.0.2.1"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        env["PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"] = "0"
        app_args.append("finalize-then-exit0")
        expected = "Socket aggregation"
        expected_peak_tables = 0
        expected_stats_files = 0
    elif args.mode == "finalize-clean-output-socket-release-fail":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        env["PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL"] = "1"
        app_args.append("finalize-then-exit0")
        expected = "Socket aggregation release failure requested by test hook"
        expected_peak_tables = 0
        expected_max_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "finalize-clean-output-socket-token-mismatch":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        app_args.append("finalize-token-mismatch-then-exit0")
        expected = "Socket aggregation received"
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "finalize-clean-output-socket-token-mismatch-no-fallback":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        env["PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"] = "0"
        app_args.append("finalize-token-mismatch-then-exit0")
        expected = "Socket aggregation received"
        expected_peak_tables = 0
        expected_stats_files = 0
    elif args.mode == "finalize-socket-post-work":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "5000"
        env["PEAK_MPI_EXIT_LOOPS"] = "16"
        env["PEAK_MPI_EXIT_POST_LOOPS"] = "32"
        app_args.append("finalize-post-work-then-exit0")
        expected = "Writing PEAK-owned socket-reduced output"
        expected_peak_tables = 1
        expected_stats_files = 1
        expected_min_target_count = nprocs * (16 + 32)
    elif args.mode == "finalize-defer-post-work":
        env["PEAK_MPI_FINALIZE_POLICY"] = "defer"
        env["PEAK_MPI_EXIT_LOOPS"] = "16"
        env["PEAK_MPI_EXIT_POST_LOOPS"] = "32"
        app_args.append("finalize-post-work-then-exit0")
        expected = "MPI runtime is not in an output-safe state"
        expected_peak_tables = 0
        expected_stats_files = nprocs
        expected_min_target_count = nprocs * (16 + 32)
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
        expected = None
        expected_peak_tables = 0
        expected_stats_files = None
    elif args.mode == "subset-finalize-clean-collective":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        app_args.append("subset-finalize-then-exit0")
        expected = None
        expected_peak_tables = 0
        expected_stats_files = None
    elif args.mode == "subset-finalize-handoff":
        app_args.append("subset-finalize-then-exit0-handoff")
        done_file = os.path.join(
            tempfile.gettempdir(),
            f"peak-subset-finalize-handoff-{os.getpid()}.txt",
        )
        try:
            os.unlink(done_file)
        except OSError:
            pass
        env["PEAK_MPI_SUBSET_FINALIZE_DONE_FILE"] = done_file
        env["PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS"] = "250"
        expected = "MPI finalize participation proof timed out"
        expected_extra.append("PEAK MPI collective proof failed or timed out")
        expected_peak_tables = 0
        expected_stats_files = None
        expected_min_stats_files = 1
    elif args.mode == "finalize-return-nonzero":
        app_args.append("finalize-then-return1")
        expected = "PMPI_Finalize was observed on every rank"
        expected_peak_tables = 1
        expected_stats_files = 1

    if args.require_detach_trace:
        # The strict-detach lifecycle variant intentionally uses an extremely
        # low detach threshold. A correct run may physically detach before any
        # sampled calls remain for the human-readable table, so the trace and
        # stats file are scheduler-dependent; the trace is the stable
        # mutation assertion for this mode.
        expected_peak_tables = 0
        expected_stats_files = None

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
        stats_rows = []
        for path in stats_files:
            with path.open(newline="", encoding="utf-8", errors="replace") as handle:
                stats_rows.extend(csv.DictReader(handle))
    output = proc.stdout
    sys.stdout.write(output)
    if done_file is not None:
        try:
            Path(done_file).unlink()
        except FileNotFoundError:
            pass
        except OSError:
            pass

    if expected is not None and expected not in output:
        raise AssertionError(f"missing expected PEAK diagnostic: {expected}")
    for extra in expected_extra:
        if extra not in output:
            raise AssertionError(f"missing expected PEAK diagnostic: {extra}")
    if args.require_pinned_finalize and "Leaving PEAK target hooks pinned" not in output:
        raise AssertionError("missing pinned-finalize diagnostic")
    for function_name in args.forbid_stats_function:
        if f'"{function_name}",' in stats_text:
            raise AssertionError(f"unexpected stats row for {function_name}")
    fatal_scan_output = output
    if args.mode in EXPECTED_LAUNCHER_ABNORMAL_MODES:
        fatal_scan_output = OPENMPI_IMPROPER_EXIT_BLOCK_RE.sub(
            "",
            fatal_scan_output,
        )
    if FAIL_RE.search(fatal_scan_output):
        raise AssertionError("MPI lifecycle run produced a crash/fatal MPI diagnostic")
    if (args.mode not in EXPECTED_LAUNCHER_ABNORMAL_MODES and
            LAUNCHER_ABNORMAL_RE.search(output)):
        raise AssertionError(
            "MPI lifecycle run produced an unexpected launcher-abnormal diagnostic"
        )
    if args.require_detach_trace:
        if not trace_path:
            raise AssertionError("PEAK_DETACH_TRACE_PATH is required for detach trace assertion")
        trace = Path(trace_path)
        if not trace.exists():
            raise AssertionError(f"detach trace was not written: {trace}")
        trace_text = trace.read_text(encoding="utf-8", errors="replace")
        if ",detach,success," not in trace_text:
            raise AssertionError("detach trace does not contain a successful detach")
    peak_table_count = output.count("PEAK Library")
    if peak_table_count < expected_peak_tables:
        raise AssertionError(
            f"expected at least {expected_peak_tables} PEAK output table(s)"
        )
    if (expected_max_peak_tables is not None and
            peak_table_count > expected_max_peak_tables):
        raise AssertionError(
            f"expected at most {expected_max_peak_tables} PEAK output table(s), "
            f"got {peak_table_count}"
        )
    if expected_stats_files is not None and len(stats_files) != expected_stats_files:
        raise AssertionError(
            f"expected {expected_stats_files} PEAK stats file(s), got {len(stats_files)}"
        )
    if (expected_min_stats_files is not None and
            len(stats_files) < expected_min_stats_files):
        raise AssertionError(
            f"expected at least {expected_min_stats_files} PEAK stats file(s), "
            f"got {len(stats_files)}"
        )
    if expected_min_target_count is not None:
        observed_count = sum(
            int(float(row.get("count", "0") or 0))
            for row in stats_rows
            if row.get("function") == "peak_mpi_exit_target"
        )
        if observed_count < expected_min_target_count:
            raise AssertionError(
                f"expected at least {expected_min_target_count} "
                f"post-finalize target calls, got {observed_count}"
            )
    if (args.mode not in EXPECTED_LAUNCHER_ABNORMAL_MODES and
            args.mode not in EXPECTED_NONZERO_RETURN_MODES and
            proc.returncode != 0):
        raise AssertionError(f"{args.mode} run returned {proc.returncode}")

    print(f"mpi_lifecycle_check_ok mode={args.mode} rc={proc.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
