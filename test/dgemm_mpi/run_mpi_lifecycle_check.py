#!/usr/bin/env python3

import argparse
import csv
import os
import re
import shlex
import signal
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
INTEL_MPI_SKIP_HYDRA_SIGKILL_RE = re.compile(
    r"={10,}\r?\n"
    r"=\s+BAD TERMINATION OF ONE OF YOUR APPLICATION PROCESSES\r?\n"
    r"=\s+RANK \d+ PID \d+ RUNNING AT [^\r\n]+\r?\n"
    r"=\s+KILLED BY SIGNAL:\s*9 \(Killed\)\r?\n"
    r"={10,}\r?\n"
)
OPENMPI_IMPROPER_EXIT_BLOCK_RE = re.compile(
    r"-{10,}\n"
    r"(?:mpiexec|prterun) has exited due to process rank.*?"
    r"-{10,}\n",
    re.DOTALL,
)
NUMBER_RE = r"[0-9.eE+-]+"
EXPECTED_LAUNCHER_ABNORMAL_MODES = {
    "no-finalize",
    "no-finalize-nonzero",
    "subset-finalize-nonzero",
    "subset-finalize-clean",
    "subset-finalize-clean-collective",
    "subset-finalize-handoff",
    "finalize-clean-output-mpi-reducer-fail",
}
EXPECTED_TIMEOUT_MODES = EXPECTED_LAUNCHER_ABNORMAL_MODES - {
    "finalize-clean-output-mpi-reducer-fail",
}
EXPECTED_NONZERO_RETURN_MODES = {
    "finalize-nonzero",
    "finalize-return-nonzero",
}
INTEL_MPI_ONLY_MODES = {
    "finalize-clean-output-mpi-intel-default",
    "finalize-clean-output-mpi-intel-real-finalize",
}
NON_INTEL_MPI_ONLY_MODES = {
    "finalize-clean-output-mpi-real-finalize-default",
    "finalize-clean-output-mpi-writer-fail",
}
STATS_FIELDS = [
    "function",
    "count",
    "per_thread",
    "per_rank",
    "call_max_s",
    "call_min_s",
    "total_s",
    "exclusive_s",
    "thread_max_s",
    "thread_min_s",
    "overhead_s",
]
REPORT_RELEASE_REAL_DIAGNOSTIC = (
    "All-rank report publication release completed: "
    "all_reports_succeeded=1 all_real_finalize_allowed=1"
)
REPORT_RELEASE_SKIP_DIAGNOSTIC = (
    "All-rank report publication release completed: "
    "all_reports_succeeded=1 all_real_finalize_allowed=0"
)
REPORT_REAL_FINALIZE_DIAGNOSTIC = (
    "PEAK output is complete; returning to real PMPI_Finalize"
)
REPORT_INTEL_SKIP_DIAGNOSTIC = (
    "PEAK output release is complete; skipping real PMPI_Finalize "
    "for Intel MPI 2019 compatibility"
)
REPORT_COMPLETION_DIAGNOSTICS = (
    "All-rank report publication release completed:",
    REPORT_REAL_FINALIZE_DIAGNOSTIC,
    "PEAK output release is complete; skipping real PMPI_Finalize",
)
REPORT_RELEASE_INTERRUPTION_DIAGNOSTICS = (
    "MPI_Iallreduce for report publication release failed; "
    "disabling later MPI teardown calls",
    "MPI_Test for report publication release failed; "
    "disabling later MPI teardown calls",
    "MPI report publication release timed out after 2500 ms; "
    "disabling later MPI teardown calls",
    "All-rank report publication release failed; "
    "skipping real PMPI_Finalize and avoiding later MPI teardown calls",
)


def split_flags(value):
    return [flag for flag in shlex.split(value) if flag]


def launcher_version_text(mpiexec):
    try:
        proc = subprocess.run(
            [mpiexec, "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=5.0,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return proc.stdout or ""


def launcher_looks_like_intel_mpi(mpiexec):
    if any(
        os.environ.get(name)
        for name in ("I_MPI_ROOT", "I_MPI_FABRICS", "I_MPI_HYDRA_BOOTSTRAP")
    ):
        return True
    text = f"{mpiexec}\n{launcher_version_text(mpiexec)}"
    return (
        "Intel(R) MPI" in text or
        "Intel MPI" in text or
        "I_MPI" in text
    )


def intel_default_launcher_outcome_allowed(mode, returncode, output, timed_out):
    if mode != "finalize-clean-output-mpi-intel-default" or timed_out:
        return False

    blocks = list(INTEL_MPI_SKIP_HYDRA_SIGKILL_RE.finditer(output))
    if returncode == 0:
        return not blocks and LAUNCHER_ABNORMAL_RE.search(output) is None
    if len(blocks) != 1:
        return False

    block = blocks[0]
    remaining_output = output[:block.start()] + output[block.end():]
    return LAUNCHER_ABNORMAL_RE.search(remaining_output) is None


def require_complete_stats_evidence(name, evidence):
    if evidence["size"] <= 0:
        raise AssertionError(f"PEAK final CSV was empty: {name}")
    if evidence["fields"] != STATS_FIELDS:
        raise AssertionError(
            f"PEAK final CSV had an incomplete or unexpected header: "
            f"{name}: {evidence['fields']!r}"
        )
    for row in evidence["rows"]:
        if None in row or any(value is None for value in row.values()):
            raise AssertionError(f"PEAK final CSV contained a partial row: {name}")
    target_rows = [
        row for row in evidence["rows"]
        if row.get("function") == "peak_mpi_exit_target"
    ]
    if len(target_rows) != 1:
        raise AssertionError(
            "PEAK final CSV did not contain exactly one target row: " + name
        )
    if int(float(target_rows[0].get("count", "0") or 0)) <= 0:
        raise AssertionError(f"PEAK final CSV target count was not positive: {name}")


def report_clean_completion_policy(output):
    continued_real = (
        REPORT_RELEASE_REAL_DIAGNOSTIC in output and
        REPORT_REAL_FINALIZE_DIAGNOSTIC in output and
        REPORT_RELEASE_SKIP_DIAGNOSTIC not in output and
        REPORT_INTEL_SKIP_DIAGNOSTIC not in output
    )
    continued_intel_skip = (
        REPORT_RELEASE_SKIP_DIAGNOSTIC in output and
        REPORT_INTEL_SKIP_DIAGNOSTIC in output and
        REPORT_RELEASE_REAL_DIAGNOSTIC not in output and
        REPORT_REAL_FINALIZE_DIAGNOSTIC not in output
    )
    if continued_real == continued_intel_skip:
        return None
    return "real-finalize" if continued_real else "intel-2019-skip"


def terminate_process_group(proc, sig):
    try:
        os.killpg(proc.pid, sig)
    except ProcessLookupError:
        pass


def run_mpi_command(command, env, timeout):
    proc = subprocess.Popen(
        command,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = proc.communicate(timeout=timeout)
        return proc.returncode, output or "", False
    except subprocess.TimeoutExpired:
        terminate_process_group(proc, signal.SIGTERM)
        try:
            output, _ = proc.communicate(timeout=2.0)
        except subprocess.TimeoutExpired:
            terminate_process_group(proc, signal.SIGKILL)
            output, _ = proc.communicate()
        return proc.returncode, output or "", True


def require_socket_maximum_tuples(output, nprocs):
    lines = [
        line for line in output.splitlines()
        if "[peak] per-rank maximum " in line
    ]
    if len(lines) != 6:
        raise AssertionError(
            f"expected 6 socket maximum tuples, got {len(lines)}"
        )
    for line in lines:
        owner_match = re.search(r"owner_rank=(\d+)", line)
        elapsed_match = re.search(
            rf"elapsed_seconds=({NUMBER_RE})", line
        )
        if owner_match is None or elapsed_match is None:
            raise AssertionError(f"incomplete socket maximum tuple: {line}")
        owner = int(owner_match.group(1))
        elapsed = float(elapsed_match.group(1))
        if owner < 0 or owner >= nprocs:
            raise AssertionError(f"socket maximum owner out of range: {line}")
        if elapsed <= 0.0:
            raise AssertionError(f"socket maximum lost owner elapsed data: {line}")
        if "risk overhead" in line:
            local_ranks = re.search(r"local_ranks=(\d+)", line)
            if local_ranks is None or int(local_ranks.group(1)) <= 0:
                raise AssertionError(
                    f"socket maximum lost owner local-rank data: {line}"
                )


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
            "finalize-clean-output-mpi-publish-before-root-return",
            "finalize-clean-output-mpi-reducer-fail",
            "finalize-clean-output-mpi-writer-fail",
            "finalize-clean-output-mpi-intel-default",
            "finalize-clean-output-mpi-intel-real-finalize",
            "finalize-clean-output-mpi-real-finalize-default",
            "finalize-clean-output-mpi-real-finalize-explicit",
            "finalize-clean-output-local",
            "finalize-clean-output-socket-bad-host",
            "finalize-clean-output-socket-bad-host-no-fallback",
            "finalize-clean-output-socket-release-fail",
            "finalize-clean-output-socket-token-mismatch",
            "finalize-clean-output-socket-token-mismatch-no-fallback",
            "finalize-socket-post-work",
            "finalize-defer-post-work",
            "finalize-defer-socket-post-work",
            "finalize-nonzero",
            "subset-finalize-nonzero",
            "subset-finalize-clean",
            "subset-finalize-clean-collective",
            "subset-finalize-handoff",
            "finalize-return-nonzero",
        ],
        required=True,
    )
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--preflags", default="")
    parser.add_argument("--postflags", default="")
    parser.add_argument("--require-pinned-finalize", action="store_true")
    parser.add_argument("--require-detach-trace", action="store_true")
    parser.add_argument("--include-finalize-target", action="store_true")
    parser.add_argument("--forbid-stats-function", action="append", default=[])
    parser.add_argument("--reducer-fail-label")
    parser.add_argument(
        "--report-signal-phase",
        choices=["before-publish", "after-publish"],
    )
    parser.add_argument(
        "--report-signal",
        choices=["TERM", "KILL"],
    )
    return parser.parse_args()


def main():
    args = parse_args()
    nprocs = int(args.nprocs)
    report_signal_requested = (
        args.report_signal_phase is not None or
        args.report_signal is not None
    )
    if ((args.report_signal_phase is None) !=
            (args.report_signal is None)):
        raise AssertionError(
            "--report-signal-phase and --report-signal must be used together"
        )
    if report_signal_requested and args.mode != "finalize-clean-output-local":
        raise AssertionError(
            "report publication signals require finalize-clean-output-local"
        )
    if args.mode in INTEL_MPI_ONLY_MODES:
        if not launcher_looks_like_intel_mpi(args.mpiexec):
            print(
                f"mpi_lifecycle_check_ok mode={args.mode} "
                "skipped=needs-intel-mpi"
            )
            return 0
    elif args.mode in NON_INTEL_MPI_ONLY_MODES:
        if launcher_looks_like_intel_mpi(args.mpiexec):
            print(
                f"mpi_lifecycle_check_ok mode={args.mode} "
                "skipped=needs-non-intel-mpi"
            )
            return 0
    if args.mode.startswith("subset-") and nprocs < 2:
        print(f"mpi_lifecycle_check_ok mode={args.mode} skipped=needs-2-ranks")
        return 0
    if (args.mode == "finalize-clean-output-mpi-publish-before-root-return" and
            nprocs < 2):
        print(f"mpi_lifecycle_check_ok mode={args.mode} skipped=needs-2-ranks")
        return 0
    if (args.mode in {
            "finalize-clean-output-socket-bad-host",
            "finalize-clean-output-socket-bad-host-no-fallback",
            "finalize-clean-output-socket-release-fail",
            "finalize-clean-output-socket-token-mismatch",
            "finalize-clean-output-socket-token-mismatch-no-fallback",
            "finalize-defer-socket-post-work",
        } and nprocs < 2):
        print(f"mpi_lifecycle_check_ok mode={args.mode} skipped=needs-2-ranks")
        return 0

    env = os.environ.copy()
    env.setdefault("PEAK_VERBOSITY", "info")
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
    expected_target_count = None
    expected_per_thread = None
    expected_positive_minima = False
    verify_socket_maxima = False
    expected_extra = []
    forbidden_output = []
    done_file = None
    verify_finalize_return_publication = False
    force_stats_path_failure = False
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
        if args.require_detach_trace:
            app_args.append("finalize-then-exit0")
        else:
            env["PEAK_MPI_EXIT_TARGET_DELAY_US"] = "100"
            app_args.append("finalize-uneven-then-exit0")
            expected_target_count = int(env.get("PEAK_MPI_EXIT_LOOPS", "16"))
            expected_per_thread = expected_target_count
            expected_positive_minima = True
        expected = "PMPI_Finalize was observed on every rank"
        expected_extra.append(
            "per-rank maximum profile+control overhead: owner_rank="
        )
        expected_extra.append(
            "owner/local control stop-window overhead: owner_rank="
        )
        expected_peak_tables = 1
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-mpi-publish-before-root-return":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env.pop("PEAK_MPI_REAL_FINALIZE", None)
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        env["PEAK_TEST_REPORT_ROOT_WRITE_DELAY_MS"] = "750"
        app_args.append("finalize-publish-before-root-return")
        expected = "PMPI_Finalize was observed on every rank"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=1"
        )
        expected_extra.append(
            "Test hook delaying aggregate CSV publication by 750 ms"
        )
        expected_peak_tables = 1
        expected_stats_files = 1
        expected_per_thread = int(env.get("PEAK_MPI_EXIT_LOOPS", "16"))
        expected_target_count = nprocs * expected_per_thread
        verify_finalize_return_publication = True
    elif args.mode == "finalize-clean-output-mpi-reducer-fail":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env.setdefault("PEAK_TARGET", "peak_mpi_exit_target")
        env["PEAK_MPI_REAL_FINALIZE"] = "1"
        env["PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "5000"
        reducer_fail_label = (
            args.reducer_fail_label or
            "profile-control-ratio-tuple-doubles"
        )
        env["PEAK_TEST_MPI_REDUCER_FAIL_LABEL"] = reducer_fail_label
        app_args.append("finalize-then-exit0")
        expected = f"MPI reducer test hook forced failure for {reducer_fail_label}"
        expected_extra.append("MPI reducer failed; trying PEAK-owned socket aggregation fallback")
        expected_extra.append("MPI output reducer failed or timed out")
        expected_extra.append("PEAK output reducer did not complete cleanly")
        forbidden_output.append("PEAK output is complete; returning to real PMPI_Finalize")
        expected_peak_tables = 1
        expected_stats_files = 1
        verify_socket_maxima = True
    elif args.mode == "finalize-clean-output-mpi-writer-fail":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env.pop("PEAK_MPI_REAL_FINALIZE", None)
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        env.pop("PEAK_TEST_MPI_LIBRARY_VERSION", None)
        app_args.append("finalize-then-exit0")
        expected = "failed to create temporary stats csv"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=0"
        )
        expected_extra.append(
            "PEAK report publication failed on at least one rank; "
            "returning to real PMPI_Finalize for clean MPI teardown"
        )
        forbidden_output.append(
            "All-rank report publication release failed"
        )
        forbidden_output.append(
            "skipping real PMPI_Finalize for Intel MPI 2019 compatibility"
        )
        expected_peak_tables = 1
        expected_stats_files = 0
        force_stats_path_failure = True
    elif args.mode == "finalize-clean-output-mpi-intel-default":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env["PEAK_TEST_MPI_LIBRARY_VERSION"] = "Intel(R) MPI Library 2019"
        env.pop("PEAK_MPI_REAL_FINALIZE", None)
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        app_args.append("finalize-then-exit0")
        expected = (
            "skipping real PMPI_Finalize for Intel MPI 2019 compatibility"
        )
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=1"
        )
        expected_extra.append("PMPI_Finalize was observed on every rank")
        forbidden_output.append(
            "PEAK output is complete; returning to real PMPI_Finalize"
        )
        expected_peak_tables = 0
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-mpi-intel-real-finalize":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env["PEAK_TEST_MPI_LIBRARY_VERSION"] = "Intel(R) MPI Library 2019"
        env["PEAK_MPI_REAL_FINALIZE"] = "1"
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        app_args.append("finalize-then-exit0")
        expected = "PEAK output is complete; returning to real PMPI_Finalize"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=1"
        )
        expected_extra.append("PMPI_Finalize was observed on every rank")
        expected_peak_tables = 0
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-mpi-real-finalize-default":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env.pop("PEAK_MPI_REAL_FINALIZE", None)
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        app_args.append("finalize-then-exit0")
        expected = "PEAK output is complete; returning to real PMPI_Finalize"
        expected_extra.append("PMPI_Finalize was observed on every rank")
        expected_peak_tables = 0
        expected_stats_files = 1
    elif args.mode == "finalize-clean-output-mpi-real-finalize-explicit":
        env["PEAK_OUTPUT_AGGREGATION"] = "mpi"
        env["PEAK_MPI_REAL_FINALIZE"] = "1"
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
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
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=1"
        )
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "finalize-clean-output-socket-bad-host-no-fallback":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_HOST"] = "192.0.2.1"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        env["PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"] = "0"
        app_args.append("finalize-then-exit0")
        expected = "Socket aggregation"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=0"
        )
        expected_peak_tables = 0
        expected_stats_files = 0
    elif args.mode == "finalize-clean-output-socket-release-fail":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        env["PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL"] = "1"
        app_args.append("finalize-then-exit0")
        expected = "Socket aggregation release failure requested by test hook"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=1"
        )
        # Root has already atomically published and printed the complete
        # aggregate before the injected release failure. Preserve that report;
        # every rank then emits a uniquely named local fallback.
        expected_peak_tables = 1
        expected_max_peak_tables = 1
        expected_stats_files = nprocs + 1
    elif args.mode == "finalize-clean-output-socket-token-mismatch":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        app_args.append("finalize-token-mismatch-then-exit0")
        expected = "Socket aggregation received"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=1"
        )
        expected_peak_tables = 0
        expected_stats_files = nprocs
    elif args.mode == "finalize-clean-output-socket-token-mismatch-no-fallback":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "500"
        env["PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"] = "0"
        app_args.append("finalize-token-mismatch-then-exit0")
        expected = "Socket aggregation received"
        expected_extra.append(
            "All-rank report publication release completed: "
            "all_reports_succeeded=0"
        )
        expected_peak_tables = 0
        expected_stats_files = 0
    elif args.mode == "finalize-socket-post-work":
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        # Socket selects only the report transport. With no explicit defer
        # policy, output must be committed from the MPI finalizer path, before
        # the application performs its post-finalize target calls.
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "5000"
        env["PEAK_MPI_EXIT_LOOPS"] = "16"
        env["PEAK_MPI_EXIT_POST_LOOPS"] = "32"
        env["PEAK_MPI_EXIT_TARGET_DELAY_US"] = "100"
        app_args.append("finalize-post-work-uneven-then-exit0")
        expected = "Writing PEAK-owned socket-reduced output"
        expected_peak_tables = 1
        expected_stats_files = 1
        expected_target_count = 16
        expected_per_thread = 16
        expected_positive_minima = True
        verify_socket_maxima = True
    elif args.mode == "finalize-defer-post-work":
        env["PEAK_MPI_FINALIZE_POLICY"] = "defer"
        env["PEAK_OUTPUT_AGGREGATION"] = "rank-local"
        env["PEAK_MPI_EXIT_LOOPS"] = "16"
        env["PEAK_MPI_EXIT_POST_LOOPS"] = "32"
        app_args.append("finalize-post-work-then-exit0")
        expected = "Aggregate output is disabled for strict teardown"
        expected_extra.append(
            "Leaving PEAK target hooks pinned after application PMPI_Finalize"
        )
        expected_peak_tables = 0
        expected_stats_files = nprocs
        expected_min_target_count = nprocs * (16 + 32)
    elif args.mode == "finalize-defer-socket-post-work":
        env["PEAK_MPI_FINALIZE_POLICY"] = "defer"
        env["PEAK_OUTPUT_AGGREGATION"] = "socket"
        env["PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"] = "5000"
        env["PEAK_MPI_EXIT_LOOPS"] = "16"
        env["PEAK_MPI_EXIT_POST_LOOPS"] = "32"
        app_args.append("finalize-post-work-uneven-then-exit0")
        expected = "Writing PEAK-owned socket-reduced output"
        expected_peak_tables = 1
        expected_stats_files = 1
        expected_target_count = 16 + 32
        expected_per_thread = expected_target_count
        expected_positive_minima = True
        verify_socket_maxima = True
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

    if report_signal_requested:
        signal_number = 15 if args.report_signal == "TERM" else 9
        env.pop("PEAK_MPI_REAL_FINALIZE", None)
        env.pop("PEAK_MPI_FINALIZE_POLICY", None)
        env.pop("PEAK_TEST_MPI_LIBRARY_VERSION", None)
        env["PEAK_TEST_REPORT_SIGNAL_PHASE"] = args.report_signal_phase
        env["PEAK_TEST_REPORT_SIGNAL"] = args.report_signal
        env["PEAK_TEST_REPORT_SIGNAL_RANK"] = "0"
        # If the selected rank terminates, give surviving ranks time to fail
        # closed at the report-release gate before the outer watchdog fires.
        # A runtime that consumes SIGTERM can still complete this small test
        # well within the same bound.
        env["PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS"] = "2500"
        expected_extra.append(
            f"Test hook delivering signal {signal_number} on rank 0 at "
            f"report phase {args.report_signal_phase}"
        )
        # SIGKILL always terminates the writer. MPI runtimes may either
        # terminate on SIGTERM or consume it and let the writer continue, so
        # the validation below distinguishes those outcomes explicitly.
        expected_peak_tables = 0
        expected_stats_files = None

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
        stats_prefix_path = Path(stats_dir) / "peak-stats"
        if force_stats_path_failure:
            blocked_component = Path(stats_dir) / "not-a-directory"
            blocked_component.write_text("regular file\n", encoding="utf-8")
            stats_prefix_path = blocked_component / "peak-stats"
        stats_prefix = str(stats_prefix_path)
        env["PEAK_STATSLOG_PATH"] = stats_prefix
        if verify_finalize_return_publication:
            env["PEAK_MPI_FINALIZE_ENTER_MARKER_PREFIX"] = str(
                Path(stats_dir) / "finalize-enter"
            )
            env["PEAK_MPI_FINALIZE_RETURN_MARKER_PREFIX"] = str(
                Path(stats_dir) / "finalize-return"
            )
        returncode, output, timed_out = run_mpi_command(
            command,
            env=env,
            timeout=args.timeout,
        )
        stats_files = sorted(Path(stats_dir).glob("peak-stats-p*.csv"))
        temporary_stats_files = sorted(
            Path(stats_dir).glob("peak-stats-p*.csv.tmp.*")
        )
        finalize_enter_markers = sorted(
            Path(stats_dir).glob("finalize-enter-r*.txt")
        )
        finalize_enter_marker_text = {
            path.name: path.read_text(encoding="utf-8", errors="replace")
            for path in finalize_enter_markers
        }
        finalize_return_markers = sorted(
            Path(stats_dir).glob("finalize-return-r*.txt")
        )
        finalize_return_marker_text = {
            path.name: path.read_text(encoding="utf-8", errors="replace")
            for path in finalize_return_markers
        }
        stats_text = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for path in stats_files
        )
        stats_rows = []
        stats_file_evidence = {}
        for path in stats_files:
            with path.open(newline="", encoding="utf-8", errors="strict") as handle:
                reader = csv.DictReader(handle, strict=True)
                rows = list(reader)
                stats_file_evidence[path.name] = {
                    "size": path.stat().st_size,
                    "fields": reader.fieldnames,
                    "rows": rows,
                }
                stats_rows.extend(rows)
        selected_stats_pattern = (
            "peak-stats-p*-r0.csv" if nprocs > 1 else "peak-stats-p*.csv"
        )
        selected_temporary_stats_pattern = (
            "peak-stats-p*-r0.csv.tmp.*" if nprocs > 1 else
            "peak-stats-p*.csv.tmp.*"
        )
        selected_stats_files = sorted(
            Path(stats_dir).glob(selected_stats_pattern)
        )
        selected_temporary_stats_files = sorted(
            Path(stats_dir).glob(selected_temporary_stats_pattern)
        )
        selected_stats_rows = []
        selected_stats_fields = None
        if len(selected_stats_files) == 1:
            selected_stats_evidence = stats_file_evidence[
                selected_stats_files[0].name
            ]
            selected_stats_size = selected_stats_evidence["size"]
            selected_stats_fields = selected_stats_evidence["fields"]
            selected_stats_rows = selected_stats_evidence["rows"]
    sys.stdout.write(output)
    if done_file is not None:
        try:
            Path(done_file).unlink()
        except FileNotFoundError:
            pass
        except OSError:
            pass

    intel_default_launcher_allowed = intel_default_launcher_outcome_allowed(
        args.mode,
        returncode,
        output,
        timed_out,
    )
    report_completion_policy = report_clean_completion_policy(output)
    report_release_interrupted = any(
        diagnostic in output
        for diagnostic in REPORT_RELEASE_INTERRUPTION_DIAGNOSTICS
    )
    launcher_abnormal = LAUNCHER_ABNORMAL_RE.search(output) is not None
    report_interruption_evidence = (
        returncode != 0 or
        launcher_abnormal or
        report_release_interrupted
    )
    report_signal_continued = (
        report_signal_requested and
        args.report_signal == "TERM" and
        returncode == 0 and
        not timed_out and
        report_completion_policy is not None and
        not report_interruption_evidence
    )
    report_signal_interrupted = (
        report_signal_requested and
        args.report_signal == "TERM" and
        not timed_out and
        report_completion_policy is None and
        report_interruption_evidence
    )

    if expected is not None and expected not in output:
        raise AssertionError(f"missing expected PEAK diagnostic: {expected}")
    for extra in expected_extra:
        if extra not in output:
            raise AssertionError(f"missing expected PEAK diagnostic: {extra}")
    for forbidden in forbidden_output:
        if forbidden in output:
            raise AssertionError(f"unexpected PEAK diagnostic: {forbidden}")
    if verify_socket_maxima:
        require_socket_maximum_tuples(output, nprocs)
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
    if report_signal_requested and FAIL_RE.search(fatal_scan_output):
        raise AssertionError(
            "report signal run produced an unintended crash/fatal MPI diagnostic"
        )
    if not report_signal_requested and FAIL_RE.search(fatal_scan_output):
        raise AssertionError("MPI lifecycle run produced a crash/fatal MPI diagnostic")
    if (not report_signal_requested and
            args.mode not in EXPECTED_LAUNCHER_ABNORMAL_MODES and
            not intel_default_launcher_allowed and
            LAUNCHER_ABNORMAL_RE.search(output)):
        raise AssertionError(
            "MPI lifecycle run produced an unexpected launcher-abnormal diagnostic"
        )
    if (report_signal_requested and timed_out):
        raise AssertionError(
            f"report signal run did not terminate within {args.timeout:g}s"
        )
    if (not report_signal_requested and timed_out and
            args.mode not in EXPECTED_TIMEOUT_MODES):
        raise AssertionError(f"{args.mode} run timed out after {args.timeout:g}s")
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
    if temporary_stats_files and not report_signal_requested:
        raise AssertionError(
            "PEAK CSV temporary file remained after launcher return: "
            + ", ".join(path.name for path in temporary_stats_files)
        )
    if report_signal_requested:
        if args.report_signal == "KILL" and returncode == 0:
            raise AssertionError(
                "SIGKILL report hook did not terminate the MPI launcher"
            )
        for name, evidence in stats_file_evidence.items():
            require_complete_stats_evidence(name, evidence)

        if report_signal_continued:
            if len(stats_files) != nprocs:
                raise AssertionError(
                    "continued SIGTERM did not publish one complete CSV per rank: "
                    f"expected {nprocs}, got {len(stats_files)}"
                )
            if temporary_stats_files:
                raise AssertionError(
                    "continued SIGTERM left a temporary CSV: "
                    + ", ".join(path.name for path in temporary_stats_files)
                )
        else:
            if args.report_signal == "TERM" and not report_signal_interrupted:
                raise AssertionError(
                    "SIGTERM neither completed cleanly nor produced explicit "
                    "bounded-interruption evidence"
                )
            for diagnostic in REPORT_COMPLETION_DIAGNOSTICS:
                if diagnostic in output:
                    raise AssertionError(
                        "interrupted report-signal run contained clean-completion "
                        "evidence: " + diagnostic
                    )
            if args.report_signal_phase == "before-publish":
                if selected_stats_files:
                    raise AssertionError(
                        "rank 0 final CSV existed after the before-publish signal"
                    )
            else:
                if len(selected_stats_files) != 1:
                    raise AssertionError(
                        "expected exactly one rank 0 final CSV after publication, "
                        f"got {len(selected_stats_files)}"
                    )
                if selected_temporary_stats_files:
                    raise AssertionError(
                        "rank 0 temporary CSV remained after atomic publication"
                    )

        if (report_signal_continued or
                args.report_signal_phase == "after-publish"):
            if len(selected_stats_files) != 1:
                raise AssertionError(
                    "expected exactly one complete rank 0 final CSV, "
                    f"got {len(selected_stats_files)}"
                )
            if selected_stats_size <= 0:
                raise AssertionError("rank 0 final CSV was empty")
            if selected_stats_fields != STATS_FIELDS:
                raise AssertionError(
                    "rank 0 final CSV had an incomplete or unexpected header: "
                    f"{selected_stats_fields!r}"
                )
            target_rows_after_signal = [
                row for row in selected_stats_rows
                if row.get("function") == "peak_mpi_exit_target"
            ]
            if len(target_rows_after_signal) != 1:
                raise AssertionError(
                    "rank 0 final CSV did not contain exactly one target row"
                )
            row = target_rows_after_signal[0]
            if None in row or any(value is None for value in row.values()):
                raise AssertionError("rank 0 final CSV contained a partial row")
            if int(float(row.get("count", "0") or 0)) <= 0:
                raise AssertionError(
                    "rank 0 final CSV target count was not positive"
                )
    if verify_finalize_return_publication:
        if returncode != 0 or timed_out:
            raise AssertionError(
                "MPI launcher did not complete cleanly after report "
                f"publication: rc={returncode} timed_out={int(timed_out)}"
            )
        if len(finalize_enter_markers) != nprocs:
            present_markers = ", ".join(
                path.name for path in finalize_enter_markers
            ) or "none"
            raise AssertionError(
                f"expected {nprocs} MPI_Finalize entry markers, "
                f"got {len(finalize_enter_markers)}; "
                f"present={present_markers}; launcher_rc={returncode}; "
                f"timed_out={int(timed_out)}"
            )
        for rank in range(nprocs):
            entry_name = f"finalize-enter-r{rank}.txt"
            entry_text = finalize_enter_marker_text.get(entry_name, "")
            if f"rank={rank} entered=1" not in entry_text:
                raise AssertionError(
                    "rank did not enter intercepted MPI_Finalize: "
                    f"{entry_name}={entry_text!r}"
                )

        if len(finalize_return_markers) != nprocs:
            raise AssertionError(
                f"expected {nprocs} MPI_Finalize return markers, "
                f"got {len(finalize_return_markers)}; "
                f"present={sorted(finalize_return_marker_text)}"
            )
        for rank in range(nprocs):
            marker_name = f"finalize-return-r{rank}.txt"
            if marker_name not in finalize_return_marker_text:
                raise AssertionError(
                    "rank did not return from intercepted MPI_Finalize: "
                    f"missing {marker_name}"
                )
            marker_text = finalize_return_marker_text.get(marker_name, "")
            marker_prefix = f"rank={rank} finalize_rc=0 final_csv_published="
            if marker_prefix + "1" not in marker_text:
                raise AssertionError(
                    "rank returned from intercepted MPI_Finalize "
                    "before publishing the complete CSV: "
                    f"{marker_name}={marker_text!r}"
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
    target_rows = [
        row for row in stats_rows
        if row.get("function") == "peak_mpi_exit_target"
    ]
    if expected_target_count is not None:
        observed_count = sum(
            int(float(row.get("count", "0") or 0))
            for row in target_rows
        )
        if observed_count != expected_target_count:
            raise AssertionError(
                f"expected exactly {expected_target_count} target calls, "
                f"got {observed_count}"
            )
    if expected_per_thread is not None:
        if len(target_rows) != 1:
            raise AssertionError(
                f"expected one aggregate target row, got {len(target_rows)}"
            )
        observed_per_thread = int(
            float(target_rows[0].get("per_thread", "0") or 0)
        )
        if observed_per_thread != expected_per_thread:
            raise AssertionError(
                f"expected per_thread={expected_per_thread}, "
                f"got {observed_per_thread}"
            )
    if expected_positive_minima:
        row = target_rows[0]
        for field in ("call_min_s", "thread_min_s"):
            value = float(row.get(field, "0") or 0)
            if value <= 0.0:
                raise AssertionError(
                    f"expected positive {field} from active ranks, got {value}"
                )
    if (not report_signal_requested and
            args.mode not in EXPECTED_LAUNCHER_ABNORMAL_MODES and
            args.mode not in EXPECTED_NONZERO_RETURN_MODES and
            not intel_default_launcher_allowed and
            returncode != 0):
        raise AssertionError(f"{args.mode} run returned {returncode}")

    if report_signal_requested:
        if report_signal_continued:
            outcome = "continued"
        elif report_signal_interrupted:
            outcome = "interrupted"
        else:
            outcome = "terminated"
        print(
            "mpi_report_signal_atomicity_ok "
            f"phase={args.report_signal_phase} "
            f"signal={args.report_signal} outcome={outcome} rc={returncode}"
        )
        return 0

    timeout_text = " timeout=1" if timed_out else ""
    print(f"mpi_lifecycle_check_ok mode={args.mode} rc={returncode}{timeout_text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
