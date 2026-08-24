#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import signal
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


SKIP = 77


@dataclass
class RunResult:
    returncode: int
    stdout: str
    stderr: str

    @property
    def output(self) -> str:
        return self.stdout + "\n" + self.stderr


def clean_environment() -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("PEAK_")}
    env.pop("LD_PRELOAD", None)
    env.pop("DYLD_INSERT_LIBRARIES", None)
    return env


def profiled_environment(peak: Path, **overrides: str) -> dict[str, str]:
    env = clean_environment()
    env.update({
        "LD_PRELOAD": str(peak),
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_OUTPUT_AGGREGATION": "local",
        "PEAK_VERBOSITY": "warn",
    })
    env.update(overrides)
    return env


def run_process(command: list[str], env: dict[str, str], timeout: float,
                cwd: Path) -> RunResult:
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        raise RuntimeError(
            f"command timed out after {timeout:.1f}s: {' '.join(command)}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}")
    return RunResult(process.returncode, stdout, stderr)


def print_result(label: str, result: RunResult) -> None:
    print(f"[{label}] stdout:")
    print(result.stdout.rstrip())
    if result.stderr:
        print(f"[{label}] stderr:")
        print(result.stderr.rstrip())


def require_success(label: str, result: RunResult,
                    allow_skip: bool = True) -> None:
    if result.returncode == SKIP:
        print_result(label, result)
        if allow_skip:
            raise SystemExit(SKIP)
        raise RuntimeError(
            f"{label} reported an unavailable GPU after its baseline passed")
    if result.returncode != 0:
        print_result(label, result)
        raise RuntimeError(f"{label} exited with status {result.returncode}")


def diagnostic(output: str, name: str, required: bool = True,
               allow_negative: bool = False) -> int:
    value_pattern = r"-?[0-9]+" if allow_negative else r"[0-9]+"
    values = [int(value) for value in
              re.findall(rf"(?<![A-Za-z0-9_]){re.escape(name)}=({value_pattern})",
                         output)]
    if not values:
        if required:
            raise RuntimeError(f"missing CUDA diagnostic: {name}=")
        return 0
    return max(values)


def require_contains(output: str, token: str, label: str) -> None:
    if token not in output:
        raise RuntimeError(f"{label} is missing required token: {token}")


def run_sampling(args: argparse.Namespace, directory: Path) -> None:
    baseline = run_process([str(args.exe)], clean_environment(), 15, directory)
    require_success("sampling-baseline", baseline)
    profile = run_process(
        [str(args.exe)],
        profiled_environment(
            args.peak,
            PEAK_GPU_MONITOR_ALL="TRUE",
            PEAK_CUDA_EVENT_POOL_CAPACITY="4",
        ),
        20,
        directory,
    )
    require_success("sampling-profile", profile, allow_skip=False)
    print_result("sampling-profile", profile)
    require_contains(profile.output, "cuda_event_recycling_ok",
                     "sampling application output")
    require_contains(profile.output, "peak_cuda_recycling_late_marker_kernel",
                     "sampling CUDA report")
    observed = diagnostic(profile.output, "observed")
    accepted = diagnostic(profile.output, "accepted")
    completed = diagnostic(profile.output, "completed")
    high_water = diagnostic(profile.output, "pool_high_water")
    if observed < 49:
        raise RuntimeError(f"observed={observed}, expected at least 49")
    if accepted <= 4 or completed <= 4:
        raise RuntimeError(
            "event slots were not demonstrably recycled: "
            f"accepted={accepted} completed={completed}")
    if not completed <= accepted <= observed:
        raise RuntimeError(
            "CUDA sampling counters are inconsistent: "
            f"completed={completed} accepted={accepted} observed={observed}")
    if high_water > 4:
        raise RuntimeError(
            f"pool_high_water={high_water}, fixed capacity is 4")
    print("cuda_sampling_regression_ok "
          f"observed={observed} accepted={accepted} completed={completed} "
          f"pool_high_water={high_water}")

    disabled = run_process(
        [str(args.exe)],
        profiled_environment(
            args.peak,
            PEAK_GPU_MONITOR_ALL="TRUE",
            PEAK_CUDA_EVENT_POOL_CAPACITY="4",
            PEAK_TEST_FAIL_CUDA_HARVESTER_CAPTURE_MODE="1",
        ),
        20,
        directory,
    )
    require_success("sampling-capture-mode-fail-open", disabled,
                    allow_skip=False)
    print_result("sampling-capture-mode-fail-open", disabled)
    require_contains(disabled.output, "cuda_event_recycling_ok",
                     "capture-mode initialization fail-open output")
    require_contains(
        disabled.output,
        "could not enter relaxed stream-capture mode",
        "capture-mode initialization fail-open warning",
    )
    disabled_observed = diagnostic(disabled.output, "observed")
    disabled_accepted = diagnostic(disabled.output, "accepted")
    disabled_completed = diagnostic(disabled.output, "completed")
    disabled_high_water = diagnostic(disabled.output, "pool_high_water")
    disabled_unavailable = diagnostic(
        disabled.output, "harvester_unavailable")
    if disabled_observed != 49 or disabled_accepted != 0 or \
            disabled_completed != 0 or disabled_high_water != 0 or \
            disabled_unavailable != 49:
        raise RuntimeError(
            "failed harvester initialization did not disable CUDA timing "
            "without changing the application: "
            f"observed={disabled_observed} accepted={disabled_accepted} "
            f"completed={disabled_completed} "
            f"pool_high_water={disabled_high_water} "
            f"harvester_unavailable={disabled_unavailable}")
    for counter in (
            "pool_full", "identity_full", "event_create_failed",
            "timing_error", "stream_capture_skipped",
            "capture_query_failed", "capture_query_unsupported",
            "unsupported_multi_device", "event_query_failed",
            "elapsed_failed", "context_query_failed",
            "context_switch_failed", "context_restore_failed",
            "finalization_timeout", "finalization_incomplete",
            "dimension_overflow"):
        value = diagnostic(disabled.output, counter)
        if value != 0:
            raise RuntimeError(
                "failed harvester initialization reported an unexpected "
                f"CUDA profiler failure: {counter}={value}")


def run_capture(args: argparse.Namespace, directory: Path) -> None:
    baseline = run_process([str(args.exe)], clean_environment(), 15, directory)
    require_success("capture-baseline", baseline)
    profile = run_process(
        [str(args.exe)],
        profiled_environment(
            args.peak,
            PEAK_GPU_MONITOR_ALL="TRUE",
            PEAK_CUDA_EVENT_POOL_CAPACITY="8",
        ),
        20,
        directory,
    )
    require_success("capture-profile", profile, allow_skip=False)
    print_result("capture-profile", profile)
    require_contains(profile.output, "cuda_stream_capture_ok",
                     "capture application output")
    require_contains(profile.output, "runtime_helper_quiesced=1",
                     "Runtime capture quiescence evidence")
    require_contains(profile.output, "runtime_helper_recycled=1",
                     "Runtime post-capture recycling evidence")
    require_contains(profile.output, "driver_helper_quiesced=1",
                     "Driver capture quiescence evidence")
    require_contains(profile.output, "driver_helper_recycled=1",
                     "Driver post-capture recycling evidence")
    skipped = diagnostic(profile.output, "stream_capture_skipped")
    observed = diagnostic(profile.output, "observed")
    accepted = diagnostic(profile.output, "accepted")
    completed = diagnostic(profile.output, "completed")
    high_water = diagnostic(profile.output, "pool_high_water")
    pool_full = diagnostic(profile.output, "pool_full")
    unavailable = diagnostic(profile.output, "harvester_unavailable")
    identity_full = diagnostic(profile.output, "identity_full")
    warmup_match = re.search(r"initialization_warmups=([0-9]+)",
                             profile.output)
    if warmup_match is None:
        raise RuntimeError("capture fixture did not report initialization "
                           "warmup attempts")
    warmups = int(warmup_match.group(1))
    if skipped < 80 or skipped % 40 != 0:
        raise RuntimeError(
            "captured target launches were not consistently classified as "
            f"pass-through: stream_capture_skipped={skipped}")
    minimum_sampled_or_full = 83
    if warmups < 1 or unavailable < 1 or unavailable > 2 * warmups or \
            observed != accepted + pool_full + unavailable + skipped or \
            accepted + pool_full < minimum_sampled_or_full or \
            completed != accepted:
        raise RuntimeError(
            "capture accounting did not include every captured launch, "
            "graph launch, helper marker, and initialization pass-through: "
            f"observed={observed} accepted={accepted} "
            f"completed={completed} pool_full={pool_full} "
            f"harvester_unavailable={unavailable} "
            f"initialization_warmups={warmups} "
            f"minimum_sampled_or_full={minimum_sampled_or_full}")
    if high_water == 0 or high_water > 8:
        raise RuntimeError(
            f"pool_high_water={high_water}, expected in [1, 8]")
    if identity_full < 1:
        raise RuntimeError(
            "graph-identity churn did not exercise the fixed aggregate "
            f"bound: identity_full={identity_full}")
    for counter in (
            "event_create_failed", "timing_error",
            "capture_query_failed", "capture_query_unsupported",
            "unsupported_multi_device", "event_query_failed",
            "elapsed_failed", "context_query_failed",
            "context_switch_failed", "context_restore_failed",
            "finalization_timeout", "finalization_incomplete",
            "dimension_overflow"):
        value = diagnostic(profile.output, counter)
        if value != 0:
            raise RuntimeError(f"{counter}={value}, expected 0")
    print("cuda_capture_regression_ok "
          f"stream_capture_skipped={skipped} observed={observed} "
          f"accepted={accepted} completed={completed} "
          f"harvester_unavailable={unavailable} "
          f"pool_full={pool_full} pool_high_water={high_water} "
          f"identity_full={identity_full}")

    races = run_process(
        [str(args.exe), "--capture-races"],
        profiled_environment(
            args.peak,
            PEAK_GPU_MONITOR_ALL="TRUE",
            PEAK_CUDA_EVENT_POOL_CAPACITY="16",
        ),
        30,
        directory,
    )
    require_success("capture-races", races, allow_skip=False)
    print_result("capture-races", races)
    require_contains(races.output, "cuda_capture_races_ok",
                     "capture race application output")
    require_contains(races.output, "begin_first_nodes=1",
                     "begin-first graph topology")
    require_contains(races.output, "begin_first_results=1/1",
                     "begin-first application results")
    require_contains(races.output, "reader_first_nodes=1",
                     "reader-first graph topology")
    require_contains(races.output, "reader_first_results=1/1",
                     "reader-first application results")
    require_contains(races.output, "reader_pending_quiesced=1",
                     "reader-first harvester quiescence")
    require_contains(races.output, "reader_pending_recycled=1",
                     "reader-first post-capture recycling")
    require_contains(races.output, "global_cycles=32",
                     "global cross-stream capture cycles")
    require_contains(races.output, "global_nodes_per_cycle=1",
                     "global cross-stream graph topology")
    require_contains(races.output, "global_results=32/32",
                     "global cross-stream application results")

    race_warmup_match = re.search(r"initialization_warmups=([0-9]+)",
                                  races.output)
    if race_warmup_match is None:
        raise RuntimeError("capture race fixture did not report harvester "
                           "warmup attempts")
    race_warmups = int(race_warmup_match.group(1))
    race_observed = diagnostic(races.output, "observed")
    race_accepted = diagnostic(races.output, "accepted")
    race_completed = diagnostic(races.output, "completed")
    race_unavailable = diagnostic(races.output, "harvester_unavailable")
    race_skipped = diagnostic(races.output, "stream_capture_skipped")
    race_high_water = diagnostic(races.output, "pool_high_water")
    race_pool_full = diagnostic(races.output, "pool_full")
    minimum_race_sampled_or_full = 36
    if race_warmups < 1 or race_unavailable < 1 or \
            race_unavailable > 2 * race_warmups or \
            race_skipped < 67 or \
            race_observed != race_accepted + race_pool_full + \
            race_unavailable + race_skipped or \
            race_accepted + race_pool_full < \
            minimum_race_sampled_or_full or \
            race_completed != race_accepted or race_high_water == 0 or \
            race_high_water > 16:
        raise RuntimeError(
            "capture race accounting did not preserve the admitted-reader "
            "launch and every post-capture graph launch: "
            f"observed={race_observed} accepted={race_accepted} "
            f"completed={race_completed} skipped={race_skipped} "
            f"harvester_unavailable={race_unavailable} "
            f"initialization_warmups={race_warmups} "
            f"pool_full={race_pool_full} "
            f"minimum_sampled_or_full={minimum_race_sampled_or_full} "
            f"pool_high_water={race_high_water}")
    for counter in (
            "identity_full", "event_create_failed",
            "timing_error", "capture_query_failed",
            "capture_query_unsupported", "unsupported_multi_device",
            "event_query_failed", "elapsed_failed",
            "context_query_failed", "context_switch_failed",
            "context_restore_failed", "finalization_timeout",
            "finalization_incomplete", "dimension_overflow"):
        value = diagnostic(races.output, counter)
        if value != 0:
            raise RuntimeError(
                f"capture race reported {counter}={value}, expected 0")
    print("cuda_capture_race_regression_ok "
          f"observed={race_observed} accepted={race_accepted} "
          f"completed={race_completed} "
          f"stream_capture_skipped={race_skipped} "
          f"pool_full={race_pool_full}")

    lifecycle_environment = profiled_environment(
        args.peak,
        PEAK_GPU_MONITOR_ALL="TRUE",
        PEAK_CUDA_EVENT_POOL_CAPACITY="8",
        PEAK_CUDA_FINALIZATION_TIMEOUT_MS="500",
    )
    lifecycle_cases = (
        (
            "reader-wins",
            "--lifecycle-reader-wins",
            "cuda_lifecycle_reader_wins_ok",
            (
                "admitted_before_close=1",
                "closer_waited=1",
                "capture_waited_for_reader=1",
                "fail_closed_transition=1",
                "nodes=1",
                "result=2",
            ),
        ),
        (
            "closer-wins",
            "--lifecycle-closer-wins",
            "cuda_lifecycle_closer_wins_ok",
            (
                "close_before_increment=1",
                "finalize_before_release=1",
                "listener_rejected=1",
                "fail_closed_transition=1",
                "nodes=1",
                "result=2",
            ),
        ),
    )
    for label, option, marker, evidence in lifecycle_cases:
        lifecycle = run_process(
            [str(args.exe), option], lifecycle_environment, 15, directory)
        require_success(f"capture-lifecycle-{label}", lifecycle,
                        allow_skip=False)
        print_result(f"capture-lifecycle-{label}", lifecycle)
        require_contains(lifecycle.output, marker,
                         f"CUDA lifecycle {label} marker")
        for token in evidence:
            require_contains(lifecycle.output, token,
                             f"CUDA lifecycle {label} evidence")
        for counter in (
                "pool_full", "identity_full", "event_create_failed",
                "timing_error", "stream_capture_skipped",
                "capture_query_failed", "capture_query_unsupported",
                "unsupported_multi_device", "event_query_failed",
                "elapsed_failed", "context_query_failed",
                "context_switch_failed", "context_restore_failed",
                "finalization_timeout", "finalization_incomplete",
                "dimension_overflow"):
            value = diagnostic(lifecycle.output, counter)
            if value != 0:
                raise RuntimeError(
                    f"CUDA lifecycle {label} reported {counter}={value}, "
                    "expected 0")
    print("cuda_lifecycle_gate_regression_ok orderings=2 "
          "listener_rejection=1")


def run_context(args: argparse.Namespace, directory: Path) -> None:
    baseline = run_process([str(args.exe)], clean_environment(), 25, directory)
    require_success("context-baseline", baseline)
    profile = run_process(
        [str(args.exe)],
        profiled_environment(
            args.peak,
            PEAK_GPU_MONITOR_ALL="TRUE",
            PEAK_CUDA_EVENT_POOL_CAPACITY="2",
        ),
        30,
        directory,
    )
    require_success("context-profile", profile, allow_skip=False)
    print_result("context-profile", profile)
    require_contains(profile.output, "cuda_multi_context_ok",
                     "multi-context application output")
    require_contains(profile.output, "context_restore_checks=24",
                     "Driver multi-context application output")
    require_contains(profile.output, "results=12/12",
                     "Driver multi-context application output")
    require_contains(profile.output, "query_error_injected=1",
                     "Driver harvest-error application output")
    names_match = re.search(r"driver_names_available=([01])", profile.output)
    if names_match is None:
        raise RuntimeError("multi-context fixture did not report Driver name "
                           "capability")
    if int(names_match.group(1)) != 0:
        require_contains(profile.output, "peak_cuda_context_driver_zero",
                         "multi-context CUDA report")
        require_contains(profile.output, "peak_cuda_context_driver_one",
                         "multi-context CUDA report")

    for counter in ("elapsed_failed", "context_query_failed",
                    "context_switch_failed",
                    "context_restore_failed"):
        value = diagnostic(profile.output, counter)
        if value != 0:
            raise RuntimeError(f"{counter}={value}, expected 0")
    event_query_failed = diagnostic(profile.output, "event_query_failed")
    if event_query_failed != 1:
        raise RuntimeError(
            f"event_query_failed={event_query_failed}, expected exactly 1")
    timing_error = diagnostic(profile.output, "timing_error")
    if timing_error != event_query_failed:
        raise RuntimeError(
            "the injected event-query failure was not reflected exactly in "
            f"the aggregate timing counter: timing_error={timing_error} "
            f"event_query_failed={event_query_failed}")

    called_match = re.search(r"multi_device_called=([01])", baseline.output)
    if called_match is None:
        raise RuntimeError("baseline did not report multi_device_called")
    multi_device_called = int(called_match.group(1))
    unsupported = diagnostic(profile.output, "unsupported_multi_device")
    if multi_device_called and unsupported < 1:
        raise RuntimeError(
            "the cooperative multi-device call was not explicitly skipped")

    accepted = diagnostic(profile.output, "accepted")
    completed = diagnostic(profile.output, "completed")
    observed = diagnostic(profile.output, "observed")
    unavailable = diagnostic(profile.output, "harvester_unavailable")
    high_water = diagnostic(profile.output, "pool_high_water")
    pool_full = diagnostic(profile.output, "pool_full")
    warmup_match = re.search(r"initialization_warmups=([0-9]+)",
                             profile.output)
    if warmup_match is None:
        raise RuntimeError("multi-context fixture did not report harvester "
                           "warmup attempts")
    warmups = int(warmup_match.group(1))
    expected_observed = warmups + 25 + unsupported
    expected_accepted = expected_observed - unavailable - unsupported
    if warmups < 1 or unavailable < 1 or unavailable > warmups or \
            observed != expected_observed or \
            accepted != expected_accepted or \
            completed != accepted - 1 or pool_full != 0 or \
            high_water == 0 or high_water > 2:
        raise RuntimeError(
            "context-owned slots were not recycled after the injected "
            "harvest failure: "
            f"observed={observed} accepted={accepted} "
            f"completed={completed} pool_full={pool_full} "
            f"pool_high_water={high_water} "
            f"harvester_unavailable={unavailable} "
            f"initialization_warmups={warmups} "
            f"unsupported_multi_device={unsupported}")
    for counter in (
            "identity_full", "event_create_failed",
            "stream_capture_skipped", "capture_query_failed",
            "capture_query_unsupported", "elapsed_failed",
            "finalization_timeout", "finalization_incomplete",
            "dimension_overflow"):
        value = diagnostic(profile.output, counter)
        if value != 0:
            raise RuntimeError(f"{counter}={value}, expected 0")
    print("cuda_context_regression_ok "
          f"observed={observed} accepted={accepted} completed={completed} "
          f"pool_full={pool_full} "
          f"multi_device_called={multi_device_called} "
          f"unsupported_multi_device={unsupported}")


def run_finalization(args: argparse.Namespace, directory: Path) -> None:
    baseline = run_process([str(args.exe)], clean_environment(), 15, directory)
    require_success("finalization-baseline", baseline)
    environment = profiled_environment(
        args.peak,
        PEAK_GPU_MONITOR_ALL="TRUE",
        PEAK_CUDA_EVENT_POOL_CAPACITY="4",
        PEAK_CUDA_FINALIZATION_TIMEOUT_MS="100",
    )
    profile = run_process(
        [str(args.exe)],
        environment,
        15,
        directory,
    )
    require_success("finalization-profile", profile, allow_skip=False)
    print_result("finalization-profile", profile)
    require_contains(profile.output, "cuda_finalization_deadline_ok",
                     "finalization application output")
    require_contains(profile.output, "peak_cuda_ready_finalization_kernel",
                     "ready CUDA record before finalization timeout")
    require_contains(profile.output, "CUDA incomplete event context=",
                     "bounded finalization diagnostic")
    require_contains(profile.output, "device=",
                     "bounded finalization diagnostic")
    timed_out = diagnostic(profile.output, "finalization_timeout")
    incomplete = diagnostic(profile.output, "finalization_incomplete")
    observed = diagnostic(profile.output, "observed")
    accepted = diagnostic(profile.output, "accepted")
    completed = diagnostic(profile.output, "completed")
    unavailable = diagnostic(profile.output, "harvester_unavailable")
    warmup_match = re.search(r"initialization_warmups=([0-9]+)",
                             profile.output)
    if warmup_match is None:
        raise RuntimeError("finalization fixture did not report harvester "
                           "warmup attempts")
    warmups = int(warmup_match.group(1))
    if timed_out != 1 or incomplete != 1 or warmups < 1 or \
            unavailable < 1 or unavailable > warmups or \
            observed != warmups + 2 or \
            accepted != observed - unavailable or \
            completed != accepted - 1:
        raise RuntimeError(
            "ready and incomplete GPU work were not accounted exactly at "
            "the deadline: "
            f"finalization_timeout={timed_out} "
            f"finalization_incomplete={incomplete} observed={observed} "
            f"accepted={accepted} completed={completed} "
            f"harvester_unavailable={unavailable} "
            f"initialization_warmups={warmups}")
    for counter in (
            "pool_full", "identity_full", "event_create_failed",
            "timing_error", "stream_capture_skipped",
            "capture_query_failed", "capture_query_unsupported",
            "unsupported_multi_device", "event_query_failed",
            "elapsed_failed", "context_query_failed",
            "context_switch_failed", "context_restore_failed",
            "dimension_overflow"):
        value = diagnostic(profile.output, counter)
        if value != 0:
            raise RuntimeError(
                f"finalization profile reported {counter}={value}, "
                "expected 0")
    print("cuda_finalization_regression_ok "
          f"finalization_timeout={timed_out} "
          f"finalization_incomplete={incomplete}")

    forced = run_process(
        [str(args.exe), "--forced-incomplete"],
        environment,
        15,
        directory,
    )
    require_success("finalization-forced-incomplete", forced,
                    allow_skip=False)
    print_result("finalization-forced-incomplete", forced)
    require_contains(forced.output,
                     "cuda_forced_incomplete_finalization_ok",
                     "forced-incomplete application output")
    require_contains(forced.output, "CUDA incomplete event context=",
                     "forced-incomplete context diagnostic")
    require_contains(forced.output, "device=",
                     "forced-incomplete device diagnostic")
    forced_timed_out = diagnostic(forced.output, "finalization_timeout")
    forced_incomplete = diagnostic(forced.output, "finalization_incomplete")
    forced_observed = diagnostic(forced.output, "observed")
    forced_accepted = diagnostic(forced.output, "accepted")
    forced_completed = diagnostic(forced.output, "completed")
    forced_unavailable = diagnostic(forced.output, "harvester_unavailable")
    forced_warmup_match = re.search(
        r"initialization_warmups=([0-9]+)", forced.output)
    if forced_warmup_match is None:
        raise RuntimeError("forced finalization fixture did not report "
                           "harvester warmup attempts")
    forced_warmups = int(forced_warmup_match.group(1))
    if forced_timed_out != 1 or forced_incomplete != 1 or \
            forced_warmups < 1 or forced_unavailable < 1 or \
            forced_unavailable > forced_warmups or \
            forced_observed != forced_warmups + 2 or \
            forced_accepted != forced_observed - forced_unavailable or \
            forced_completed != forced_accepted - 1:
        raise RuntimeError(
            "forced-incomplete event state was not retained at the deadline: "
            f"finalization_timeout={forced_timed_out} "
            f"finalization_incomplete={forced_incomplete} "
            f"observed={forced_observed} accepted={forced_accepted} "
            f"completed={forced_completed} "
            f"harvester_unavailable={forced_unavailable} "
            f"initialization_warmups={forced_warmups}")
    for counter in (
            "pool_full", "identity_full", "event_create_failed",
            "timing_error", "stream_capture_skipped",
            "capture_query_failed", "capture_query_unsupported",
            "unsupported_multi_device", "event_query_failed",
            "elapsed_failed", "context_query_failed",
            "context_switch_failed", "context_restore_failed",
            "dimension_overflow"):
        value = diagnostic(forced.output, counter)
        if value != 0:
            raise RuntimeError(
                f"forced finalization profile reported {counter}={value}, "
                "expected 0")
    print("cuda_forced_incomplete_regression_ok "
          f"finalization_timeout={forced_timed_out} "
          f"finalization_incomplete={forced_incomplete}")


def benchmark_metric(result: RunResult) -> float:
    match = re.search(r"host_ns_per_launch=([0-9]+(?:\.[0-9]+)?)",
                      result.output)
    if match is None:
        raise RuntimeError("benchmark output is missing host_ns_per_launch")
    value = float(match.group(1))
    if value <= 0:
        raise RuntimeError(f"invalid host_ns_per_launch={value}")
    return value


def benchmark_throughput(result: RunResult) -> float:
    match = re.search(
        r"aggregate_launches_per_second=([0-9]+(?:\.[0-9]+)?)",
        result.output)
    if match is None:
        raise RuntimeError(
            "benchmark output is missing aggregate_launches_per_second")
    value = float(match.group(1))
    if value <= 0:
        raise RuntimeError(f"invalid aggregate_launches_per_second={value}")
    return value


def benchmark_samples(args: argparse.Namespace, directory: Path, api: str,
                      mode: str, threads: int,
                      profiled: bool,
                      native_events: bool = False) -> tuple[float, float]:
    latencies = []
    throughputs = []
    variant = "profile" if profiled else (
        "event-floor" if native_events else "base")
    for sample in range(args.samples):
        command = [
            str(args.exe),
            "--api", api,
            "--mode", mode,
            "--threads", str(threads),
            "--iterations", str(args.iterations),
            "--batch", str(args.batch),
        ]
        if native_events:
            command.append("--native-events")
        if profiled:
            pool_capacity = max(64, threads * args.batch * 2)
            profiler_options = {
                "PEAK_CUDA_EVENT_POOL_CAPACITY": str(pool_capacity),
            }
            if api == "driver" and mode == "target":
                # Driver function-handle names are toolkit-specific. Monitor
                # all launches to exercise the same accepted timing path
                # without making this performance gate depend on that name.
                profiler_options["PEAK_GPU_MONITOR_ALL"] = "TRUE"
            else:
                profiler_options["PEAK_GPU_TARGET"] = (
                    "peak_cuda_benchmark_target_kernel")
            env = profiled_environment(args.peak, **profiler_options)
        else:
            env = clean_environment()
        result = run_process(command, env, 20, directory)
        require_success(
            f"benchmark-{api}-{mode}-{threads}-{variant}-{sample}",
            result, allow_skip=not profiled)
        print_result(
            f"benchmark-{api}-{mode}-{threads}-{variant}-{sample}", result)
        require_contains(result.output, f"api={api}",
                         f"benchmark {api} API evidence")
        require_contains(
            result.output,
            f"native_events={1 if native_events else 0}",
            f"benchmark {api} native-event evidence",
        )
        available_cpus = diagnostic(result.output, "available_cpus")
        pinned_workers = diagnostic(result.output, "pinned_workers")
        if available_cpus < threads or pinned_workers != threads:
            raise RuntimeError(
                "benchmark workers did not use distinct allowed CPUs: "
                f"sample={sample} api={api} threads={threads} "
                f"available_cpus={available_cpus} "
                f"pinned_workers={pinned_workers}")
        overlap = re.search(r"overlap_batches=([0-9]+)/([0-9]+)",
                            result.output)
        expected_batches = (args.iterations + args.batch - 1) // args.batch
        if overlap is None or int(overlap.group(1)) != expected_batches or \
                int(overlap.group(2)) != expected_batches:
            observed_overlap = overlap.group(0) if overlap is not None \
                else "missing"
            raise RuntimeError(
                "benchmark submission intervals did not overlap in every "
                f"batch: sample={sample} api={api} threads={threads} "
                f"overlap={observed_overlap} expected={expected_batches}")
        if profiled:
            expected_ready = 1 if mode == "target" else 0
            # Production builds intentionally omit the test-only readiness
            # symbol. Exact accounting below still proves that every measured
            # target launch was admitted (unavailable cannot exceed warmup).
            ready = diagnostic(
                result.output, "harvester_ready_before_measurement",
                allow_negative=True)
            if ready not in (expected_ready, -1):
                raise RuntimeError(
                    f"benchmark {api} {mode} measured-phase admission "
                    f"reported harvester_ready_before_measurement={ready}, "
                    f"expected {expected_ready} or unavailable (-1)")
            observed = diagnostic(result.output, "observed")
            accepted = diagnostic(result.output, "accepted")
            completed = diagnostic(result.output, "completed")
            high_water = diagnostic(result.output, "pool_high_water")
            unavailable = diagnostic(result.output,
                                     "harvester_unavailable")
            if mode == "target":
                application_launches = (
                    threads * (1 + args.batch + args.iterations))
                pre_measurement = threads * (1 + args.batch)
                if observed != application_launches or unavailable < 1 or \
                        unavailable > pre_measurement or \
                        accepted + unavailable != observed or \
                        completed != accepted or high_water == 0:
                    raise RuntimeError(
                        "target benchmark did not keep the measured workload "
                        "on the sampled path: "
                        f"sample={sample} api={api} threads={threads} "
                        f"observed={observed} accepted={accepted} "
                        f"completed={completed} high_water={high_water} "
                        f"harvester_unavailable={unavailable} "
                        f"application_launches={application_launches} "
                        f"pre_measurement={pre_measurement}")
            elif observed != 0 or accepted != 0 or completed != 0 or \
                    high_water != 0 or unavailable != 0:
                raise RuntimeError(
                    "non-target benchmark entered CUDA timing accounting: "
                    f"sample={sample} api={api} threads={threads} "
                    f"observed={observed} accepted={accepted} "
                    f"completed={completed} high_water={high_water} "
                    f"harvester_unavailable={unavailable}")
            failure_counters = (
                "pool_full", "identity_full", "event_create_failed",
                "timing_error", "stream_capture_skipped",
                "capture_query_failed", "capture_query_unsupported",
                "unsupported_multi_device", "event_query_failed",
                "elapsed_failed", "context_query_failed",
                "context_switch_failed", "context_restore_failed",
                "finalization_timeout", "finalization_incomplete",
                "dimension_overflow",
            )
            for counter in failure_counters:
                value = diagnostic(result.output, counter)
                if value != 0:
                    raise RuntimeError(
                        "benchmark reported a CUDA profiler failure: "
                        f"sample={sample} api={api} threads={threads} "
                        f"{counter}={value}")
        latencies.append(benchmark_metric(result))
        throughputs.append(benchmark_throughput(result))
    return (statistics.median(latencies),
            statistics.median(throughputs))


def run_benchmark(args: argparse.Namespace, directory: Path) -> None:
    thread_counts = [1]
    for threads in (8, 32, args.concurrent_threads):
        if threads <= args.concurrent_threads and threads not in thread_counts:
            thread_counts.append(threads)
    thread_counts.sort()
    measurements: dict[tuple[str, str, int, bool], float] = {}
    throughputs: dict[tuple[str, str, int, bool], float] = {}
    target_event_floors: dict[tuple[str, int], float] = {}
    target_event_floor_throughputs: dict[tuple[str, int], float] = {}
    for api in ("runtime", "driver"):
        for mode in ("non-target", "target"):
            for threads in thread_counts:
                baseline, baseline_throughput = benchmark_samples(
                    args, directory, api, mode, threads, False)
                if mode == "target":
                    event_floor, event_floor_throughput = benchmark_samples(
                        args, directory, api, mode, threads, False,
                        native_events=True)
                    target_event_floors[(api, threads)] = event_floor
                    target_event_floor_throughputs[(api, threads)] = \
                        event_floor_throughput
                profile, profile_throughput = benchmark_samples(
                    args, directory, api, mode, threads, True)
                measurements[(api, mode, threads, False)] = baseline
                measurements[(api, mode, threads, True)] = profile
                throughputs[(api, mode, threads, False)] = baseline_throughput
                throughputs[(api, mode, threads, True)] = profile_throughput

    for api in ("runtime", "driver"):
        for threads in thread_counts:
            baseline = measurements[(api, "non-target", threads, False)]
            profile = measurements[(api, "non-target", threads, True)]
            limit = baseline * args.max_non_target_relative + \
                args.max_non_target_additive_ns
            if profile > limit:
                raise RuntimeError(
                    f"{api}/non-target/{threads} overhead is too high: "
                    f"baseline={baseline:.3f}ns profile={profile:.3f}ns "
                    f"limit={limit:.3f}ns")

            target_baseline = measurements[(api, "target", threads, False)]
            target_profile = measurements[(api, "target", threads, True)]
            target_event_floor = target_event_floors[(api, threads)]
            target_increment = target_profile - target_event_floor
            target_ratio = target_profile / target_event_floor
            if target_ratio > args.max_target_relative:
                raise RuntimeError(
                    f"{api}/target/{threads} relative overhead is too high: "
                    f"event_floor={target_event_floor:.3f}ns "
                    f"profile={target_profile:.3f}ns "
                    f"ratio={target_ratio:.3f}x "
                    f"limit={args.max_target_relative:.3f}x")
            if target_increment > args.max_target_additive_ns:
                raise RuntimeError(
                    f"{api}/target/{threads} incremental overhead is too "
                    "high: "
                    f"baseline={target_baseline:.3f}ns "
                    f"event_floor={target_event_floor:.3f}ns "
                    f"profile={target_profile:.3f}ns "
                    f"increment={target_increment:.3f}ns")

    for api in ("runtime", "driver"):
        for mode in ("non-target", "target"):
            def scaled_throughput(threads: int) -> float:
                profile = throughputs[(api, mode, threads, True)]
                if mode == "target":
                    return profile / target_event_floor_throughputs[
                        (api, threads)]
                # CUDA launch throughput itself can saturate as concurrency
                # rises. Normalize the no-record path by its matching
                # unprofiled sample so this unchanged scaling gate measures
                # PEAK retention instead of the driver's native curve.
                return profile / throughputs[
                    (api, mode, threads, False)]

            one_thread = scaled_throughput(1)
            previous = one_thread
            for threads in thread_counts[1:]:
                current = scaled_throughput(threads)
                reference = max(one_thread, previous)
                floor = reference * args.min_profile_throughput_scaling
                if current < floor:
                    raise RuntimeError(
                        f"{api}/{mode}/{threads} profiled throughput "
                        "declined with concurrency: "
                        f"one_thread={one_thread:.3f} "
                        f"previous={previous:.3f} current={current:.3f} "
                        f"floor={floor:.3f}")
                previous = current

    for api in ("runtime", "driver"):
        for mode in ("non-target", "target"):
            def comparison_latency(threads: int) -> float:
                if mode == "target":
                    return target_event_floors[(api, threads)]
                return measurements[(api, mode, threads, False)]

            def comparison_throughput(threads: int) -> float:
                if mode == "target":
                    return target_event_floor_throughputs[(api, threads)]
                return throughputs[(api, mode, threads, False)]

            one_factor = (measurements[(api, mode, 1, True)] /
                          comparison_latency(1))
            one_retention = (throughputs[(api, mode, 1, True)] /
                             comparison_throughput(1))
            for threads in thread_counts[1:]:
                concurrent_factor = (
                    measurements[(api, mode, threads, True)] /
                    comparison_latency(threads))
                retention = (throughputs[(api, mode, threads, True)] /
                             comparison_throughput(threads))
                if concurrent_factor > one_factor * \
                        args.max_concurrency_regression or \
                        retention * args.max_concurrency_regression < \
                        one_retention:
                    raise RuntimeError(
                        f"{api}/{mode}/{threads} profiler scaling regressed: "
                        f"one_thread_slowdown={one_factor:.3f}x "
                        f"concurrent_slowdown={concurrent_factor:.3f}x "
                        f"one_thread_retention={one_retention:.3f} "
                        f"concurrent_retention={retention:.3f}")

    for api in ("runtime", "driver"):
        for mode in ("non-target", "target"):
            for threads in thread_counts:
                baseline = measurements[(api, mode, threads, False)]
                profile = measurements[(api, mode, threads, True)]
                baseline_throughput = throughputs[
                    (api, mode, threads, False)]
                profile_throughput = throughputs[
                    (api, mode, threads, True)]
                print("cuda_launch_benchmark_result "
                      f"api={api} mode={mode} threads={threads} "
                      f"baseline_ns={baseline:.3f} "
                      f"profile_ns={profile:.3f} "
                      f"ratio={profile / baseline:.6f} "
                      f"baseline_launches_per_second="
                      f"{baseline_throughput:.3f} "
                      f"profile_launches_per_second="
                      f"{profile_throughput:.3f} throughput_retention="
                      f"{profile_throughput / baseline_throughput:.6f}" +
                      (f" native_event_floor_ns="
                       f"{target_event_floors[(api, threads)]:.3f} "
                       f"software_increment_ns="
                       f"{profile - target_event_floors[(api, threads)]:.3f} "
                       f"event_floor_ratio="
                       f"{profile / target_event_floors[(api, threads)]:.6f} "
                       f"native_event_floor_launches_per_second="
                       f"{target_event_floor_throughputs[(api, threads)]:.3f} "
                       f"event_floor_throughput_retention="
                       f"{profile_throughput / target_event_floor_throughputs[(api, threads)]:.6f}"
                       if mode == "target" else ""))
    print("cuda_launch_benchmark_validation_ok")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=(
        "sampling", "capture", "context", "finalization", "benchmark"))
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--peak", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=64)
    parser.add_argument("--batch", type=int, default=4)
    parser.add_argument("--concurrent-threads", type=int, default=8)
    parser.add_argument("--max-non-target-relative", type=float, default=1.25)
    parser.add_argument("--max-non-target-additive-ns", type=float, default=500.0)
    parser.add_argument("--max-target-relative", type=float, default=2.00)
    parser.add_argument("--max-target-additive-ns", type=float, default=10000.0)
    parser.add_argument("--max-concurrency-regression", type=float, default=1.10)
    parser.add_argument("--min-profile-throughput-scaling", type=float,
                        default=0.90)
    args = parser.parse_args()
    if args.samples <= 0 or args.iterations <= 0 or args.batch <= 0 or \
            args.concurrent_threads <= 0:
        parser.error("benchmark counts must be positive")
    if args.max_non_target_relative < 1.0 or \
            args.max_target_relative < 1.0 or \
            args.max_concurrency_regression < 1.0:
        parser.error("relative overhead limits must be at least 1.0")
    if args.max_non_target_additive_ns < 0 or \
            args.max_target_additive_ns < 0:
        parser.error("additive overhead limits must be non-negative")
    if not 0.0 < args.min_profile_throughput_scaling <= 1.0:
        parser.error("profile throughput scaling must be in (0, 1]")
    return args


def main() -> int:
    args = parse_arguments()
    if sys.platform != "linux":
        print("cuda_test_skip: CUDA preload regressions require Linux")
        return SKIP
    if not args.exe.is_file() or not args.peak.is_file():
        raise RuntimeError("CUDA fixture executable or PEAK library is missing")
    args.exe = args.exe.resolve()
    args.peak = args.peak.resolve()

    with tempfile.TemporaryDirectory(prefix=f"peak-cuda-{args.case}-") as temp:
        directory = Path(temp)
        if args.case == "sampling":
            run_sampling(args, directory)
        elif args.case == "capture":
            run_capture(args, directory)
        elif args.case == "context":
            run_context(args, directory)
        elif args.case == "finalization":
            run_finalization(args, directory)
        else:
            run_benchmark(args, directory)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"cuda_regression_error: {error}", file=sys.stderr)
        raise SystemExit(1)
