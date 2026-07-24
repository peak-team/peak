#!/usr/bin/env python3
"""Policy fixtures for launcher-specific MPI lifecycle acceptance."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


CHECKER_PATH = Path(__file__).with_name("run_mpi_lifecycle_check.py")
SPEC = importlib.util.spec_from_file_location("mpi_lifecycle_checker", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

MODE = "finalize-clean-output-mpi-reducer-fail"
NPROCS = 4
HYDRA_BLOCK = """\
===================================================================================
=   BAD TERMINATION OF ONE OF YOUR APPLICATION PROCESSES
=   RANK 3 PID 1234 RUNNING AT c001-001
=   KILLED BY SIGNAL: 9 (Killed)
===================================================================================
"""
REPORT_SEPARATOR = "-" * 99 + "\n"
COMPLETE_REPORT_TAIL = (
    "Function timing statistics\n"
    + REPORT_SEPARATOR
    + "Detailed function statistics (thread): aggregate timing in seconds\n"
    + "|                        function|   total (s)|   exclusive|"
    "     max (s)|     min (s)|   est. cost|\n"
    + REPORT_SEPARATOR
    + "|            peak_mpi_exit_target|       0.001|       0.001|"
    "       0.001|       0.001|   0.000e+00|\n"
    + REPORT_SEPARATOR
)


def fallback_output() -> str:
    forced = (
        "[peak] MPI reducer test hook forced failure for tuple; "
        "abandoning MPI reducer without touching MPI again\n"
    )
    fallback = (
        "[peak] MPI reducer failed; trying PEAK-owned socket aggregation "
        "fallback without further MPI calls\n"
    )
    return (
        (forced + fallback) * NPROCS
        + "PEAK done with: ./mpi-app\n"
        + f"Report scope: aggregate ({NPROCS} MPI ranks)\n"
        + COMPLETE_REPORT_TAIL
        + HYDRA_BLOCK
    )


class IntelReducerFallbackPolicyTest(unittest.TestCase):
    def allowed(self, output: str, **overrides: object) -> bool:
        arguments = {
            "mode": MODE,
            "returncode": 255,
            "output": output,
            "timed_out": False,
            "nprocs": NPROCS,
            "is_intel_mpi": True,
        }
        arguments.update(overrides)
        return CHECKER.intel_reducer_fallback_launcher_outcome_allowed(
            **arguments
        )

    def test_complete_intel_hydra_fallback_is_allowed(self) -> None:
        self.assertTrue(self.allowed(fallback_output()))

    def test_clean_return_is_not_the_hydra_exception(self) -> None:
        self.assertFalse(self.allowed(fallback_output(), returncode=0))

    def test_non_intel_launcher_is_not_allowed(self) -> None:
        self.assertFalse(self.allowed(fallback_output(), is_intel_mpi=False))

    def test_timeout_is_not_allowed(self) -> None:
        self.assertFalse(self.allowed(fallback_output(), timed_out=True))

    def test_every_rank_must_force_failure(self) -> None:
        output = fallback_output().replace(
            CHECKER.MPI_REDUCER_FAIL_FORCED_DIAGNOSTIC,
            "missing forced failure ",
            1,
        )
        self.assertFalse(self.allowed(output))

    def test_every_rank_must_enter_socket_fallback(self) -> None:
        output = fallback_output().replace(
            CHECKER.MPI_REDUCER_SOCKET_FALLBACK_DIAGNOSTIC,
            "missing socket fallback",
            1,
        )
        self.assertFalse(self.allowed(output))

    def test_complete_aggregate_report_is_required(self) -> None:
        output = fallback_output().replace(
            f"Report scope: aggregate ({NPROCS} MPI ranks)",
            "Report scope: local",
        )
        self.assertFalse(self.allowed(output))

    def test_report_must_precede_hydra_cleanup(self) -> None:
        output = fallback_output().replace(
            COMPLETE_REPORT_TAIL + HYDRA_BLOCK,
            HYDRA_BLOCK + COMPLETE_REPORT_TAIL,
        )
        self.assertFalse(self.allowed(output))

    def test_other_launcher_abnormality_is_rejected(self) -> None:
        output = fallback_output() + "mpiexec has exited due to process rank 1\n"
        self.assertFalse(self.allowed(output))


class ReportInterruptionPolicyTest(unittest.TestCase):
    def test_combined_gate_timeout_is_bounded_interruption_evidence(self) -> None:
        output = (
            "[peak] MPI combined finalize/report publication release "
            "timed out after 2500 ms; disabling later MPI teardown calls\n"
        )
        self.assertTrue(CHECKER.report_release_was_interrupted(output))

    def test_clean_release_is_not_interruption_evidence(self) -> None:
        output = (
            "[peak] All-rank report publication release completed: "
            "all_reports_succeeded=1 all_real_finalize_allowed=1\n"
        )
        self.assertFalse(CHECKER.report_release_was_interrupted(output))


if __name__ == "__main__":
    result = unittest.main(exit=False)
    if result.result.wasSuccessful():
        print("mpi_lifecycle_checker_policy_ok")
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
