#!/usr/bin/env python3
"""Policy fixtures for launcher-specific MPI lifecycle acceptance."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


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


class WriterFailureDiagnosticPolicyTest(unittest.TestCase):
    def test_dirfd_parent_failure_is_the_writer_failure_diagnostic(self) -> None:
        output = "[peak] failed to prepare stats csv destination: Not a directory\n"

        self.assertTrue(CHECKER.writer_destination_failure_observed(output))

    def test_temp_creation_diagnostic_is_not_the_dirfd_parent_failure(self) -> None:
        output = "[peak] failed to create temporary stats csv: Not a directory\n"

        self.assertFalse(CHECKER.writer_destination_failure_observed(output))


class SocketPortIsolationTest(unittest.TestCase):
    def test_busy_port_in_either_contiguous_pair_slot_retries(self) -> None:
        first_base = 21000
        second_base = 22000

        for busy_port in (first_base, first_base + 1):
            with self.subTest(busy_port=busy_port):
                created = []

                class FakeSocket:
                    def __init__(self, *_args):
                        self.closed = False
                        created.append(self)

                    def bind(self, address):
                        if address == ("0.0.0.0", busy_port):
                            raise OSError("simulated TIME_WAIT collision")

                    def listen(self, _backlog):
                        pass

                    def close(self):
                        self.closed = True

                CHECKER.ALLOCATED_SOCKET_TEST_PORT_BASES.clear()
                try:
                    with mock.patch.object(
                            CHECKER.secrets,
                            "randbelow",
                            side_effect=[
                                first_base - CHECKER.SOCKET_TEST_PORT_MIN,
                                second_base - CHECKER.SOCKET_TEST_PORT_MIN,
                            ]), mock.patch.object(
                                CHECKER.socket,
                                "socket",
                                side_effect=FakeSocket):
                        self.assertEqual(
                            CHECKER.reserve_socket_port_pair(), second_base
                        )
                finally:
                    CHECKER.ALLOCATED_SOCKET_TEST_PORT_BASES.clear()

                self.assertTrue(all(listener.closed for listener in created))

    def test_adjacent_invocations_do_not_reuse_a_preflighted_pair(self) -> None:
        first_base = 24000
        second_base = 25000
        created = []

        class FakeSocket:
            def __init__(self, *_args):
                created.append(self)

            def bind(self, _address):
                pass

            def listen(self, _backlog):
                pass

            def close(self):
                pass

        CHECKER.ALLOCATED_SOCKET_TEST_PORT_BASES.clear()
        try:
            with mock.patch.object(
                    CHECKER.secrets,
                    "randbelow",
                    side_effect=[
                        first_base - CHECKER.SOCKET_TEST_PORT_MIN,
                        first_base - CHECKER.SOCKET_TEST_PORT_MIN,
                        second_base - CHECKER.SOCKET_TEST_PORT_MIN,
                    ]), mock.patch.object(
                        CHECKER.socket,
                        "socket",
                        side_effect=FakeSocket):
                first = CHECKER.reserve_socket_port_pair()
                second = CHECKER.reserve_socket_port_pair()
        finally:
            CHECKER.ALLOCATED_SOCKET_TEST_PORT_BASES.clear()

        self.assertEqual((first, second), (first_base, second_base))
        self.assertEqual(len(created), 4)

    def test_all_socket_modes_and_reducer_labels_get_port_pairs(self) -> None:
        explicit_socket_modes = {
            "finalize-clean-output-socket-bad-host",
            "finalize-clean-output-socket-bad-host-no-fallback",
            "finalize-clean-output-socket-release-fail",
            "finalize-clean-output-socket-token-mismatch",
            "finalize-clean-output-socket-token-mismatch-no-fallback",
            "finalize-socket-post-work",
            "finalize-defer-socket-post-work",
        }
        reducer_labels = (
            "profile-control-ratio-tuple-doubles",
            "profile-control-ratio-owner",
            "profile-control-ratio-tuple-valid",
            "profile-control-ratio-tuple-local-ranks",
            "profile-control-ratio-tuple-counts",
        )
        self.assertSetEqual(
            CHECKER.SOCKET_TRANSPORT_MODES,
            explicit_socket_modes,
        )
        modes = list(explicit_socket_modes)
        modes.extend(
            "finalize-clean-output-mpi-reducer-fail"
            for _ in reducer_labels
        )
        environments = [{} for _ in modes]

        with mock.patch.object(
                CHECKER,
                "reserve_socket_port_pair",
                return_value=26000) as reserve:
            for mode, env in zip(modes, environments):
                self.assertEqual(
                    CHECKER.configure_lifecycle_socket_ports(mode, env),
                    26000,
                )

        self.assertEqual(reserve.call_count, len(modes))
        self.assertTrue(all(
            env.get("PEAK_OUTPUT_AGGREGATION_PORT") == "26000"
            for env in environments
        ))


class StatsArtifactNameTest(unittest.TestCase):
    def test_strict_rank_local_fallback_is_a_valid_identity_artifact(self) -> None:
        aggregate = (
            "peak-stats-j42-s7-hnode0-r0-p123-q0123456789abcdef.csv"
        )
        fallback = (
            "peak-stats-j42-s7-hnode0-r0-p123-q0123456789abcdef-"
            "ranklocal-hnode0.csv"
        )

        self.assertEqual(
            CHECKER.STATS_CSV_NAME_RE.fullmatch(aggregate)["rank"], "0"
        )
        self.assertEqual(
            CHECKER.STATS_CSV_NAME_RE.fullmatch(fallback)["rank"], "0"
        )
        self.assertIsNone(CHECKER.STATS_CSV_NAME_RE.fullmatch(
            "peak-stats-j42-s7-hnode0-r0-p123.csv"
        ))

    @staticmethod
    def release_name(rank: int, fallback: bool = False) -> str:
        suffix = "-ranklocal-hnode0" if fallback else ""
        return (
            f"peak-stats-j42-s7-hnode0-r{rank}-p{rank + 100}-"
            f"q{rank:016x}{suffix}.csv"
        )

    def test_socket_release_layout_requires_one_aggregate_and_every_rank(self) -> None:
        names = [self.release_name(0)] + [
            self.release_name(rank, fallback=True) for rank in range(4)
        ]

        CHECKER.require_socket_release_fallback_layout(names, 4)

    def test_socket_release_layout_rejects_extra_aggregate(self) -> None:
        names = [self.release_name(0), self.release_name(1), self.release_name(2)]
        names.extend(self.release_name(rank, fallback=True) for rank in range(4))

        with self.assertRaisesRegex(AssertionError, "exactly one aggregate"):
            CHECKER.require_socket_release_fallback_layout(names, 4)

    def test_socket_release_layout_rejects_missing_or_duplicate_rank(self) -> None:
        missing = [self.release_name(0)] + [
            self.release_name(rank, fallback=True) for rank in (0, 1, 3)
        ]
        duplicate = [self.release_name(0)] + [
            self.release_name(rank, fallback=True) for rank in (0, 1, 1, 3)
        ]

        with self.assertRaisesRegex(AssertionError, "exactly 4 rank-local"):
            CHECKER.require_socket_release_fallback_layout(missing, 4)
        with self.assertRaisesRegex(AssertionError, "each rank exactly once"):
            CHECKER.require_socket_release_fallback_layout(duplicate, 4)

    def test_compact_temporary_artifact_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(CHECKER.compact_temporary_stats_files(directory), [])
            artifact = Path(directory) / ".peak-tmp.p123.4"
            artifact.touch()
            artifacts = CHECKER.compact_temporary_stats_files(directory)

            self.assertEqual(artifacts, [artifact])
            with self.assertRaisesRegex(AssertionError, "compact CSV temporary"):
                CHECKER.reject_compact_temporary_stats_files(artifacts)


if __name__ == "__main__":
    result = unittest.main(exit=False)
    if result.result.wasSuccessful():
        print("mpi_lifecycle_checker_policy_ok")
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
