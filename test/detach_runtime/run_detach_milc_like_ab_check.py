#!/usr/bin/env python3

import argparse
import csv
import io
import math
import os
import re
import subprocess
import sys
from pathlib import Path


TARGET_PHASES = 13
PHASE_TARGETS = 64
TARGET_COUNT = TARGET_PHASES * PHASE_TARGETS
PROFILE_SECONDS_FALLBACK = 1e-12
STATS_CSV_FIELDS = (
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
)
HOT_TARGETS = [
    f"peak_milc_hot_{phase}_s{slot:02d}"
    for phase in range(TARGET_PHASES)
    for slot in range(PHASE_TARGETS)
]
TARGET_LIST = ",".join(HOT_TARGETS)
RESULT_RE = re.compile(
    r"milc_like .*elapsed=(?P<elapsed>[0-9.]+) "
    r"calls=(?P<calls>[0-9]+) "
    r"calls_per_sec=(?P<cps>[0-9.]+)"
)
TARGET_CALLS_RE = re.compile(
    r"^milc_like_target_calls count=(?P<count>[0-9]+) "
    r"values=(?P<values>[0-9,]+)$",
    re.MULTILINE,
)
PHASE_BREADTH_RE = re.compile(
    r"^milc_like_phase_breadth phases=(?P<count>[0-9]+) "
    r"values=(?P<values>[0-9:,]+)$",
    re.MULTILINE,
)
TARGET_PHASE_CALLS_RE = re.compile(
    r"^milc_like_target_phase_calls phases=(?P<phases>[0-9]+) "
    r"targets=(?P<targets>[0-9]+) values=(?P<values>[0-9,]+)$",
    re.MULTILINE,
)
TARGET_THREAD_INFO_RE = re.compile(
    r"^milc_like_target_thread_info threads=(?P<threads>[0-9]+) "
    r"targets=(?P<targets>[0-9]+) breadth=(?P<breadth>[0-9,]+) "
    r"max=(?P<max>[0-9,]+)$",
    re.MULTILINE,
)
WINDOW_RE = re.compile(
    r"^milc_like_window index=(?P<index>[0-9]+) "
    r"elapsed=(?P<elapsed>[0-9.]+) true_calls=(?P<calls>[0-9]+)$",
    re.MULTILINE,
)
CHECKPOINT_RE = re.compile(
    r"^milc_like_checkpoint index=(?P<index>[0-9]+) "
    r"elapsed=(?P<elapsed>[0-9.]+) true_calls=(?P<calls>[0-9]+)$",
    re.MULTILINE,
)
AFFINITY_RE = re.compile(
    r"^milc_like_affinity enabled=(?P<enabled>[01]) "
    r"allowed_cpus=(?P<allowed>[0-9]+) "
    r"pinned_workers=(?P<pinned>[0-9]+) "
    r"first_cpu=(?P<first>-?[0-9]+)$",
    re.MULTILINE,
)
METRICS_RE = re.compile(r"^milc_like_metrics (?P<metrics>.*)$", re.MULTILINE)
CONTROL_STOP_WINDOW_RE = re.compile(
    r"^\[peak\] (?P<prefix>local|rank-0/local) control stop-window overhead: "
    r"windows=(?P<windows>[0-9]+) "
    r"wall_seconds=(?P<seconds>[0-9.eE+-]+) "
    r"ratio=(?P<ratio>[0-9.eE+-]+)$",
    re.MULTILINE,
)
FAILED_STOP_ATTEMPT_RE = re.compile(
    r"^\[peak\] (?P<prefix>local|aggregate) failed control windows: "
    r"windows=(?P<attempts>[0-9]+) "
    r"snapshot_valid=(?P<snapshot_valid>[01])$",
    re.MULTILINE,
)
LEGACY_CONTROL_STOP_WINDOW_RE = re.compile(
    r"^\[peak\] control stop-window overhead: .*$",
    re.MULTILINE,
)
PROFILE_CONTROL_OVERHEAD_RE = re.compile(
    r"^\[peak\] local profile\+control overhead: "
    r"profile_seconds=(?P<profile>[0-9.eE+-]+) "
    r"control_seconds=(?P<control>[0-9.eE+-]+) "
    r"profile_ratio=(?P<profile_ratio>[0-9.eE+-]+) "
    r"control_ratio=(?P<control_ratio>[0-9.eE+-]+) "
    r"ratio=(?P<ratio>[0-9.eE+-]+)$",
    re.MULTILINE,
)
LOCAL_RANK_RISK_OVERHEAD_RE = re.compile(
    r"^\[peak\] local profile\+local-rank-control risk: "
    r"profile_seconds=(?P<profile>[0-9.eE+-]+) "
    r"raw_control_seconds=(?P<raw_control>[0-9.eE+-]+) "
    r"local_ranks=(?P<local_ranks>[0-9]+) "
    r"risk_control_seconds=(?P<risk_control>[0-9.eE+-]+) "
    r"ratio=(?P<ratio>[0-9.eE+-]+)$",
    re.MULTILINE,
)
LEGACY_PROFILE_CONTROL_MANAGEMENT_OVERHEAD_RE = re.compile(
    r"^\[peak\] local profile\+control\+management overhead: "
    r".*$",
    re.MULTILINE,
)
MANAGEMENT_OVERHEAD_RE = re.compile(
    r"^\[peak\] heartbeat management overhead: "
    r"cpu_seconds=(?P<seconds>[0-9.eE+-]+) "
    r"ratio=(?P<ratio>[0-9.eE+-]+)$",
    re.MULTILINE,
)
FINAL_TRANSITION_COVERAGE_RE = re.compile(
    r"^\[peak\] (?P<prefix>local|aggregate) final transition coverage: "
    r"detached_targets=(?P<detached>[0-9]+) "
    r"reattached_targets=(?P<reattached>[0-9]+) "
    r"revisited_targets=(?P<revisited>[0-9]+)$",
    re.MULTILINE,
)
PEAK_TIME_RE = re.compile(r"^Time: (?P<time>[0-9.eE+-]+)$", re.MULTILINE)
ESTIMATED_OVERHEAD_RE = re.compile(
    r"Estimated overhead: [0-9.eE+-]+s per call and "
    r"(?P<total>[0-9.eE+-]+)s total"
)
BAD_OUTPUT_RE = re.compile(
    r"fatal|not proven safe|leaving listener state alive|"
    r"Gum .* failed|tracked thread snapshot exceeded|"
    r"thread id .* exceeds|detach helper shutdown failed|"
    r"timed out draining pending target hook detach/reattach requests|"
    r"Abandoning .* (detach|reattach)|retry-abandoned",
    re.IGNORECASE,
)
CONTROL_SECONDS_TOLERANCE = 1.0e-6


TRACE_COLUMNS = {
    "hook_id": 1,
    "function": 2,
    "operation": 3,
    "result": 4,
    "physical": 5,
    "status": 6,
    "pending_age_s": 8,
    "batch_size": 9,
    "stop_window_us": 10,
    "batch_id": 11,
    "request_source": 17,
    "request_calls": 18,
    "request_ratio": 19,
    "request_global_overhead": 20,
    "request_total_time": 21,
    "request_rate": 22,
    "accounting_wall_s": 24,
    "accounting_ratio": 25,
    "accounting_snapshot_valid": 26,
    "failed_stop_windows": 27,
}


def target_name(index):
    return HOT_TARGETS[index]


def parse_target_calls(output):
    match = TARGET_CALLS_RE.search(output)
    if match is None:
        raise AssertionError("missing milc_like target call counts")
    count = int(match.group("count"))
    values = [int(value) for value in match.group("values").split(",")]
    if count != TARGET_COUNT or len(values) != TARGET_COUNT:
        raise AssertionError(
            f"expected {TARGET_COUNT} target counts, got count={count} "
            f"values={len(values)}"
        )
    return values


def parse_phase_breadth(output):
    match = PHASE_BREADTH_RE.search(output)
    if match is None:
        raise AssertionError("missing milc_like phase breadth")
    count = int(match.group("count"))
    if count != TARGET_PHASES:
        raise AssertionError(f"expected {TARGET_PHASES} phase breadth entries")
    breadth = {}
    for item in match.group("values").split(","):
        phase, value = item.split(":", 1)
        breadth[int(phase)] = int(value)
    if set(breadth) != set(range(TARGET_PHASES)):
        raise AssertionError(f"incomplete phase breadth: {breadth}")
    return breadth


def parse_target_phase_calls(output):
    match = TARGET_PHASE_CALLS_RE.search(output)
    if match is None:
        raise AssertionError("missing milc_like target phase call matrix")
    phases = int(match.group("phases"))
    targets = int(match.group("targets"))
    values = [int(value) for value in match.group("values").split(",")]
    if phases != TARGET_PHASES or targets != TARGET_COUNT:
        raise AssertionError(
            f"unexpected target phase shape phases={phases} targets={targets}"
        )
    expected = TARGET_PHASES * TARGET_COUNT
    if len(values) != expected:
        raise AssertionError(
            f"expected {expected} target phase counts, got {len(values)}"
        )
    return [
        values[index * TARGET_COUNT:(index + 1) * TARGET_COUNT]
        for index in range(TARGET_PHASES)
    ]


def parse_target_thread_info(output):
    match = TARGET_THREAD_INFO_RE.search(output)
    if match is None:
        raise AssertionError("missing milc_like target thread info")
    threads = int(match.group("threads"))
    targets = int(match.group("targets"))
    breadth = [int(value) for value in match.group("breadth").split(",")]
    max_calls = [int(value) for value in match.group("max").split(",")]
    if targets != TARGET_COUNT:
        raise AssertionError(f"expected {TARGET_COUNT} thread-info targets")
    if len(breadth) != TARGET_COUNT or len(max_calls) != TARGET_COUNT:
        raise AssertionError("incomplete target thread info")
    return {
        "threads": threads,
        "breadth": breadth,
        "max_calls": max_calls,
    }


def parse_windows(output, required):
    windows = [
        {
            "index": int(match.group("index")),
            "elapsed": float(match.group("elapsed")),
            "true_calls": int(match.group("calls")),
        }
        for match in WINDOW_RE.finditer(output)
    ]
    if required and len(windows) < 2:
        raise AssertionError("missing timestamped true-call windows")
    for expected_index, window in enumerate(windows):
        if window["index"] != expected_index:
            raise AssertionError("non-contiguous workload window indexes")
        if expected_index == 0:
            if window["elapsed"] != 0.0:
                raise AssertionError("workload windows must start at elapsed zero")
            continue
        previous = windows[expected_index - 1]
        if (window["elapsed"] <= previous["elapsed"] or
                window["true_calls"] < previous["true_calls"]):
            raise AssertionError("workload windows are not monotonic")
    return windows


def parse_checkpoints(output, required):
    checkpoints = [
        {
            "index": int(match.group("index")),
            "elapsed": float(match.group("elapsed")),
            "true_calls": int(match.group("calls")),
        }
        for match in CHECKPOINT_RE.finditer(output)
    ]
    if required and len(checkpoints) < 2:
        raise AssertionError("missing progress-aligned true-call checkpoints")
    for expected_index, checkpoint in enumerate(checkpoints):
        if checkpoint["index"] != expected_index:
            raise AssertionError("non-contiguous progress checkpoint indexes")
        if expected_index == 0:
            if checkpoint["elapsed"] != 0.0:
                raise AssertionError("progress checkpoints must start at elapsed zero")
            continue
        previous = checkpoints[expected_index - 1]
        if (checkpoint["elapsed"] <= previous["elapsed"] or
                checkpoint["true_calls"] <= previous["true_calls"]):
            raise AssertionError("progress checkpoints are not monotonic")
    return checkpoints


def parse_affinity(output, expected_threads):
    match = AFFINITY_RE.search(output)
    if match is None:
        raise AssertionError("missing milc_like affinity summary")
    affinity = {
        "enabled": int(match.group("enabled")),
        "allowed_cpus": int(match.group("allowed")),
        "pinned_workers": int(match.group("pinned")),
        "first_cpu": int(match.group("first")),
    }
    if affinity["enabled"] and affinity["pinned_workers"] != expected_threads:
        raise AssertionError(
            f"only {affinity['pinned_workers']}/{expected_threads} workers pinned"
        )
    return affinity


def progress_gate_required(args):
    return (
        args.iterations > 0 or
        args.min_progress_intervals > 0 or
        args.min_progress_average_overhead >= 0.0 or
        args.max_progress_average_overhead >= 0.0 or
        args.max_progress_sustained_overhead >= 0.0 or
        args.max_progress_baseline_drift >= 0.0
    )


def reset_peak_artifacts(args):
    for path in args.work_dir.glob("milc-like-stats-p*.csv"):
        path.unlink()


def validate_application_count_shapes(target_calls, target_phase_calls,
                                      target_thread_info):
    for index, total in enumerate(target_calls):
        phase_total = sum(row[index] for row in target_phase_calls)
        if phase_total != total:
            raise AssertionError(
                f"target phase calls for {target_name(index)} sum to "
                f"{phase_total}, expected {total}"
            )
        breadth = target_thread_info["breadth"][index]
        max_calls = target_thread_info["max_calls"][index]
        if total > 0 and breadth <= 0:
            raise AssertionError(
                f"{target_name(index)} has calls but no calling thread"
            )
        if total == 0 and (breadth != 0 or max_calls != 0):
            raise AssertionError(
                f"{target_name(index)} has thread info but no calls"
            )
        if max_calls > total:
            raise AssertionError(
                f"{target_name(index)} thread max exceeds total calls"
            )


def parse_reported_overheads(output):
    if LEGACY_PROFILE_CONTROL_MANAGEMENT_OVERHEAD_RE.search(output):
        raise AssertionError(
            "legacy combined profile+control+management overhead report "
            "is not accepted"
        )
    if LEGACY_CONTROL_STOP_WINDOW_RE.search(output):
        raise AssertionError(
            "legacy unqualified control stop-window overhead report "
            "is not accepted"
        )
    profile_control_matches = list(PROFILE_CONTROL_OVERHEAD_RE.finditer(output))
    risk_matches = list(LOCAL_RANK_RISK_OVERHEAD_RE.finditer(output))
    management_matches = list(MANAGEMENT_OVERHEAD_RE.finditer(output))
    control_matches = list(CONTROL_STOP_WINDOW_RE.finditer(output))
    failed_stop_matches = list(FAILED_STOP_ATTEMPT_RE.finditer(output))
    if (not profile_control_matches and
            not risk_matches and
            not management_matches and
            not control_matches and
            not failed_stop_matches):
        return {
            "profile_seconds": None,
            "control_seconds": None,
            "management_seconds": None,
            "reported_profile_ratio": None,
            "reported_control_ratio": None,
            "profile_control_ratio": None,
            "risk_profile_control_ratio": None,
            "risk_local_ranks": 1,
            "risk_control_seconds": None,
            "management_ratio": None,
            "control_stop_window_prefix": None,
            "control_stop_window_windows": 0,
            "control_stop_window_seconds": None,
            "control_stop_window_ratio": None,
            "failed_stop_attempt_prefix": None,
            "failed_stop_attempts": 0,
            "accounting_snapshot_valid": None,
        }
    if len(profile_control_matches) > 1:
        raise AssertionError("multiple local profile+control overhead reports")
    if len(risk_matches) > 1:
        raise AssertionError("multiple local-rank risk overhead reports")
    if len(management_matches) > 1:
        raise AssertionError("multiple heartbeat management overhead reports")
    if len(control_matches) > 1:
        raise AssertionError("multiple local control stop-window reports")
    if len(failed_stop_matches) > 1:
        raise AssertionError("multiple failed control-window reports")
    if not profile_control_matches:
        raise AssertionError("missing local profile+control overhead report")
    if not risk_matches:
        raise AssertionError("missing local-rank risk overhead report")
    if not management_matches:
        raise AssertionError("missing heartbeat management overhead report")
    if not control_matches:
        raise AssertionError(
            "missing local or rank-0/local control stop-window overhead report"
        )
    if not failed_stop_matches:
        raise AssertionError("missing failed control-window diagnostic report")
    profile_control_match = profile_control_matches[0]
    risk_match = risk_matches[0]
    management_match = management_matches[0]
    control_match = control_matches[0]
    failed_stop_match = failed_stop_matches[0]
    profile_control_seconds = float(profile_control_match.group("control"))
    control_stop_window_seconds = float(control_match.group("seconds"))
    if (abs(profile_control_seconds - control_stop_window_seconds) >
            CONTROL_SECONDS_TOLERANCE):
        raise AssertionError(
            "profile+control control_seconds does not match independent "
            "control stop-window wall_seconds"
        )
    profile_seconds = float(profile_control_match.group("profile"))
    risk_profile_seconds = float(risk_match.group("profile"))
    risk_raw_control_seconds = float(risk_match.group("raw_control"))
    risk_local_ranks = int(risk_match.group("local_ranks"))
    risk_control_seconds = float(risk_match.group("risk_control"))
    raw_profile_control_ratio = float(profile_control_match.group("ratio"))
    risk_profile_control_ratio = float(risk_match.group("ratio"))
    profile_ratio = float(profile_control_match.group("profile_ratio"))
    raw_control_ratio = float(profile_control_match.group("control_ratio"))
    if risk_local_ranks < 1:
        raise AssertionError("local-rank risk report must fall back to one rank")
    if abs(risk_profile_seconds - profile_seconds) > CONTROL_SECONDS_TOLERANCE:
        raise AssertionError("risk report profile seconds differ from raw report")
    if (abs(risk_raw_control_seconds - profile_control_seconds) >
            CONTROL_SECONDS_TOLERANCE):
        raise AssertionError("risk report raw control differs from raw report")
    expected_risk_control_seconds = (
        risk_local_ranks * risk_raw_control_seconds
    )
    if (abs(risk_control_seconds - expected_risk_control_seconds) >
            CONTROL_SECONDS_TOLERANCE):
        raise AssertionError(
            "risk control seconds do not equal local_ranks * raw control"
        )
    if (risk_profile_control_ratio + CONTROL_SECONDS_TOLERANCE <
            raw_profile_control_ratio):
        raise AssertionError("risk ratio must not be below the raw ratio")
    if abs(
        raw_profile_control_ratio - (profile_ratio + raw_control_ratio)
    ) > CONTROL_SECONDS_TOLERANCE:
        raise AssertionError("raw ratio does not equal profile + raw control")
    if abs(
        risk_profile_control_ratio -
        (profile_ratio + risk_local_ranks * raw_control_ratio)
    ) > CONTROL_SECONDS_TOLERANCE:
        raise AssertionError(
            "risk ratio does not equal profile + local_ranks * raw control"
        )
    failed_attempt_prefix = failed_stop_match.group("prefix")
    control_prefix = control_match.group("prefix")
    if control_prefix == "local" and failed_attempt_prefix != "local":
        raise AssertionError("local control report must use local failed-window prefix")
    if control_prefix == "rank-0/local" and failed_attempt_prefix != "aggregate":
        raise AssertionError(
            "aggregate control report must use aggregate failed-window prefix"
        )
    return {
        "profile_seconds": profile_seconds,
        "control_seconds": profile_control_seconds,
        "management_seconds": float(management_match.group("seconds")),
        "reported_profile_ratio": profile_ratio,
        "reported_control_ratio": raw_control_ratio,
        "profile_control_ratio": raw_profile_control_ratio,
        "risk_profile_control_ratio": risk_profile_control_ratio,
        "risk_local_ranks": risk_local_ranks,
        "risk_control_seconds": risk_control_seconds,
        "management_ratio": float(management_match.group("ratio")),
        "control_stop_window_prefix": control_match.group("prefix"),
        "control_stop_window_windows": int(control_match.group("windows")),
        "control_stop_window_seconds": control_stop_window_seconds,
        "control_stop_window_ratio": float(control_match.group("ratio")),
        "failed_stop_attempt_prefix": failed_attempt_prefix,
        "failed_stop_attempts": int(failed_stop_match.group("attempts")),
        "accounting_snapshot_valid": int(
            failed_stop_match.group("snapshot_valid")
        ),
    }


def parse_final_transition_coverage(output):
    matches = list(FINAL_TRANSITION_COVERAGE_RE.finditer(output))
    if not matches:
        return {
            "present": False,
            "prefix": "",
            "detached_targets": 0,
            "reattached_targets": 0,
            "revisited_targets": 0,
        }
    if len(matches) > 1:
        raise AssertionError("multiple final transition coverage reports")
    match = matches[0]
    return {
        "present": True,
        "prefix": match.group("prefix"),
        "detached_targets": int(match.group("detached")),
        "reattached_targets": int(match.group("reattached")),
        "revisited_targets": int(match.group("revisited")),
    }


def child_affinity_preexec(args):
    if args.affinity_cpus <= 0:
        return None
    if not sys.platform.startswith("linux"):
        raise AssertionError("--affinity-cpus is only supported on Linux")
    if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        raise AssertionError("--affinity-cpus requires Linux CPU affinity APIs")
    allowed = sorted(os.sched_getaffinity(0))
    if args.affinity_cpus > len(allowed):
        raise AssertionError(
            f"--affinity-cpus {args.affinity_cpus} exceeds available "
            f"CPU set size {len(allowed)}"
        )
    selected = set(allowed[:args.affinity_cpus])

    def apply_child_affinity():
        os.sched_setaffinity(0, selected)

    return apply_child_affinity


def run_case(args, peak, arm_name, trace_path=None):
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    for name in list(env):
        if name.startswith("PEAK_"):
            env.pop(name, None)
    if "LD_PRELOAD" in env or any(name.startswith("PEAK_") for name in env):
        raise AssertionError("baseline environment still contains PEAK preload state")
    if peak:
        env.update(
            {
                "LD_PRELOAD": args.libpeak,
                "PEAK_TARGET": TARGET_LIST,
                "PEAK_MAX_NUM_THREADS": str(args.peak_max_threads),
                "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
                "PEAK_ENABLE_GLOBAL_HEARTBEAT": "1",
                "PEAK_ENABLE_REATTACH": "1",
                "PEAK_OVERHEAD_RATIO": args.target_overhead_ratio,
                "PEAK_GLOBAL_OVERHEAD_RATIO": args.global_overhead_ratio,
                "PEAK_GLOBAL_REATTACH_FACTOR": args.global_reattach_factor,
                "PEAK_HEARTBEAT_INTERVAL": args.heartbeat_interval,
                "PEAK_HIBERNATION_CYCLE": "1",
                "PEAK_REATTACH_COOLDOWN_MS": args.reattach_cooldown_ms,
                "PEAK_REQUIRE_SAFE_DETACH": "1",
                "PEAK_SAFE_DETACH_MODE": args.detach_backend,
                "PEAK_STATSLOG_PATH": str(args.work_dir / "milc-like-stats"),
            }
        )
        env["PEAK_DETACH_COUNT"] = args.detach_count
        if args.detach_backend not in ("auto", "strict"):
            env["PEAK_DETACH_BACKEND"] = args.detach_backend
        if args.detach_backend == "signal":
            env.setdefault("PEAK_DETACH_HELPER", "/no/such/peak_detach_helper")
        if trace_path is not None:
            env["PEAK_DETACH_TRACE_PATH"] = str(trace_path)

    command = [
            args.exe,
            "--threads",
            str(args.threads),
            "--seconds",
            str(args.seconds),
            "--iterations",
            str(args.iterations),
            "--inner-work",
            str(args.inner_work),
            "--phase-repeats",
            str(args.phase_repeats),
            "--window-ms",
            str(args.window_ms),
            "--checkpoint-calls",
            str(args.checkpoint_calls),
        ]
    if args.pin_workers:
        command.append("--pin-workers")
    completed = subprocess.run(
        command,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=child_affinity_preexec(args),
        timeout=args.timeout,
    )
    output = completed.stdout + completed.stderr
    output_path = args.work_dir / f"{arm_name}.log"
    output_path.write_text(output, encoding="utf-8")
    if completed.returncode != 0:
        raise AssertionError(
            f"{arm_name} failed with rc={completed.returncode}; "
            f"output={output_path}"
        )
    bad_output = BAD_OUTPUT_RE.search(output)
    if bad_output is not None:
        raise AssertionError(
            f"{arm_name} matched unhealthy PEAK diagnostic "
            f"{bad_output.group(0)!r}; output={output_path}"
        )
    match = RESULT_RE.search(output)
    if match is None:
        raise AssertionError(f"missing milc_like summary; output={output_path}")
    reported = parse_reported_overheads(output)
    transition_coverage = parse_final_transition_coverage(output)
    peak_time_match = PEAK_TIME_RE.search(output)
    estimated_overhead_match = ESTIMATED_OVERHEAD_RE.search(output)
    peak_time = (
        float(peak_time_match.group("time"))
        if peak_time_match is not None else 0.0
    )
    estimated_overhead_total = (
        float(estimated_overhead_match.group("total"))
        if estimated_overhead_match is not None else 0.0
    )
    target_calls = parse_target_calls(output)
    target_phase_calls = parse_target_phase_calls(output)
    target_thread_info = parse_target_thread_info(output)
    validate_application_count_shapes(target_calls,
                                      target_phase_calls,
                                      target_thread_info)

    return {
        "elapsed": float(match.group("elapsed")),
        "summary_calls": int(match.group("calls")),
        "calls_per_sec": float(match.group("cps")),
        "target_calls": target_calls,
        "phase_breadth": parse_phase_breadth(output),
        "target_phase_calls": target_phase_calls,
        "target_thread_info": target_thread_info,
        "windows": parse_windows(output, args.iterations == 0),
        "checkpoints": parse_checkpoints(output, progress_gate_required(args)),
        "affinity": parse_affinity(output, args.threads),
        "control_stop_window_ratio": (
            reported["control_stop_window_ratio"]
            if reported["control_stop_window_ratio"] is not None else 0.0
        ),
        "control_stop_window_seconds": (
            reported["control_stop_window_seconds"]
            if reported["control_stop_window_seconds"] is not None else 0.0
        ),
        "failed_stop_attempts": reported["failed_stop_attempts"],
        "accounting_snapshot_valid": reported["accounting_snapshot_valid"],
        "reported_profile_seconds": (
            reported["profile_seconds"]
        ),
        "reported_control_seconds": (
            reported["control_seconds"]
        ),
        "reported_management_seconds": (
            reported["management_seconds"]
        ),
        "reported_profile_ratio": (
            reported["reported_profile_ratio"]
        ),
        "reported_control_ratio": (
            reported["reported_control_ratio"]
        ),
        "reported_raw_profile_control_ratio": (
            reported["profile_control_ratio"]
        ),
        "reported_risk_profile_control_ratio": (
            reported["risk_profile_control_ratio"]
        ),
        "reported_risk_local_ranks": (
            reported["risk_local_ranks"]
        ),
        "reported_risk_control_seconds": (
            reported["risk_control_seconds"]
        ),
        "reported_management_ratio": (
            reported["management_ratio"]
        ),
        "final_transition_coverage": transition_coverage,
        "estimated_profile_ratio": (
            estimated_overhead_total / peak_time
            if peak_time > 0.0 else 0.0
        ),
        "peak_time": peak_time,
        "output_path": output_path,
        "output": output,
    }


def validate_arm_modes(args, *cases):
    expected_enabled = 1 if args.pin_workers else 0
    modes = {
        (case["affinity"]["enabled"],
         case["affinity"]["allowed_cpus"],
         case["affinity"]["first_cpu"])
        for case in cases
    }
    if any(case["affinity"]["enabled"] != expected_enabled for case in cases):
        raise AssertionError("B-A-B arms did not use the requested pinning mode")
    if len(modes) != 1:
        raise AssertionError("B-A-B arms observed different allowed CPU sets")
    if args.affinity_cpus > 0 and args.pin_workers:
        observed = {case["affinity"]["allowed_cpus"] for case in cases}
        if observed != {args.affinity_cpus}:
            raise AssertionError(
                "B-A-B arms did not inherit requested affinity size: "
                f"expected={args.affinity_cpus} observed={sorted(observed)}"
            )


def interval_rates(windows):
    rates = []
    for previous, current in zip(windows, windows[1:]):
        elapsed = current["elapsed"] - previous["elapsed"]
        calls = current["true_calls"] - previous["true_calls"]
        if elapsed <= 0.0:
            raise AssertionError("workload window has non-positive duration")
        rates.append(calls / elapsed)
    return rates


def interval_durations(checkpoints):
    durations = []
    for previous, current in zip(checkpoints, checkpoints[1:]):
        elapsed = current["elapsed"] - previous["elapsed"]
        calls = current["true_calls"] - previous["true_calls"]
        if elapsed <= 0.0:
            raise AssertionError("progress checkpoint has non-positive duration")
        if calls <= 0:
            raise AssertionError("progress checkpoint has no work delta")
        durations.append({"elapsed": elapsed, "calls": calls})
    return durations


def validate_fixed_work_checkpoint_shape(args, *cases):
    if args.iterations <= 0:
        return
    for case in cases:
        if not case["checkpoints"]:
            raise AssertionError("fixed-work arm has no progress checkpoints")
        final_checkpoint_calls = case["checkpoints"][-1]["true_calls"]
        if final_checkpoint_calls != case["summary_calls"]:
            raise AssertionError(
                "fixed-work final checkpoint does not include tail: "
                f"checkpoint={final_checkpoint_calls} "
                f"summary={case['summary_calls']}"
            )

    summary_calls = {case["summary_calls"] for case in cases}
    if len(summary_calls) != 1:
        raise AssertionError(
            f"fixed-work B-A-B arms ended at different true work: "
            f"{sorted(summary_calls)}"
        )

    checkpoint_counts = {len(case["checkpoints"]) for case in cases}
    if len(checkpoint_counts) != 1:
        raise AssertionError(
            "fixed-work B-A-B arms produced mismatched checkpoint counts: "
            f"{sorted(checkpoint_counts)}"
        )

    expected_intervals = None
    for case in cases:
        intervals = [item["calls"] for item in interval_durations(
            case["checkpoints"]
        )]
        if expected_intervals is None:
            expected_intervals = intervals
            continue
        if intervals != expected_intervals:
            raise AssertionError(
                "fixed-work B-A-B arms produced mismatched checkpoint "
                "thresholds"
            )


def matched_window_summary(args, baseline_before, peak, baseline_after):
    before_rates = interval_rates(baseline_before["windows"])
    peak_rates = interval_rates(peak["windows"])
    after_rates = interval_rates(baseline_after["windows"])
    if not (len(before_rates) == len(peak_rates) == len(after_rates)):
        raise AssertionError(
            "B-A-B arms produced different timestamped window counts"
        )
    if args.warmup_windows >= len(peak_rates):
        raise AssertionError(
            f"warmup_windows={args.warmup_windows} leaves no measured windows"
        )

    overheads = []
    baseline_drifts = []
    for index in range(args.warmup_windows, len(peak_rates)):
        baseline_rate = (before_rates[index] + after_rates[index]) / 2.0
        if baseline_rate <= 0.0:
            raise AssertionError(f"matched baseline window {index} has no calls")
        overheads.append((baseline_rate - peak_rates[index]) / baseline_rate)
        baseline_drifts.append(
            abs(before_rates[index] - after_rates[index]) / baseline_rate
        )

    return {
        "count": len(overheads),
        "average_actual_overhead": sum(overheads) / len(overheads),
        "max_actual_overhead": max(overheads),
        "max_baseline_drift": max(baseline_drifts),
        "overheads": overheads,
        "baseline_drifts": baseline_drifts,
    }


def progress_aligned_summary(args, baseline_before, peak, baseline_after):
    validate_fixed_work_checkpoint_shape(args,
                                         baseline_before,
                                         peak,
                                         baseline_after)
    before_intervals = interval_durations(baseline_before["checkpoints"])
    peak_intervals = interval_durations(peak["checkpoints"])
    after_intervals = interval_durations(baseline_after["checkpoints"])
    common_count = min(
        len(before_intervals),
        len(peak_intervals),
        len(after_intervals),
    )
    if args.warmup_checkpoints >= common_count:
        raise AssertionError(
            f"warmup_checkpoints={args.warmup_checkpoints} leaves no "
            f"measured progress intervals out of {common_count}"
        )
    measured_count = common_count - args.warmup_checkpoints
    if measured_count < args.min_progress_intervals:
        raise AssertionError(
            f"measured progress intervals={measured_count} below "
            f"{args.min_progress_intervals}"
        )

    overheads = []
    baseline_drifts = []
    interval_calls = []
    for index in range(args.warmup_checkpoints, common_count):
        before = before_intervals[index]
        profiled = peak_intervals[index]
        after = after_intervals[index]
        common_calls = min(before["calls"], profiled["calls"], after["calls"])
        if common_calls <= 0:
            raise AssertionError(f"progress interval {index} has no common work")
        baseline_before_seconds = before["elapsed"] * common_calls / before["calls"]
        peak_seconds = profiled["elapsed"] * common_calls / profiled["calls"]
        baseline_after_seconds = after["elapsed"] * common_calls / after["calls"]
        baseline_seconds = (baseline_before_seconds + baseline_after_seconds) / 2.0
        if baseline_seconds <= 0.0:
            raise AssertionError(
                f"progress interval {index} has no baseline duration"
            )
        overheads.append((peak_seconds - baseline_seconds) / baseline_seconds)
        baseline_drifts.append(
            abs(baseline_before_seconds - baseline_after_seconds) /
            baseline_seconds
        )
        interval_calls.append(common_calls)

    sustained_window = min(5, len(overheads))
    max_sustained_overhead = max(
        sum(overheads[index:index + sustained_window]) /
        sustained_window
        for index in range(len(overheads) - sustained_window + 1)
    )

    return {
        "count": len(overheads),
        "common_intervals": common_count,
        "average_actual_overhead": sum(overheads) / len(overheads),
        "max_actual_overhead": max(overheads),
        "max_sustained_overhead": max_sustained_overhead,
        "max_baseline_drift": max(baseline_drifts),
        "overheads": overheads,
        "baseline_drifts": baseline_drifts,
        "interval_calls": interval_calls,
    }


def validated_profiled_stats_rows(handle, allowed_targets, rank_count=1):
    if rank_count <= 0:
        raise AssertionError(f"invalid stats report rank count: {rank_count}")
    reader = csv.DictReader(handle)
    if tuple(reader.fieldnames or ()) != STATS_CSV_FIELDS:
        raise AssertionError(
            f"unexpected stats CSV fields: {reader.fieldnames}"
        )
    rows = []
    seen = set()
    for row in reader:
        if (None in row or
                any(row[field] is None or not row[field].strip()
                    for field in STATS_CSV_FIELDS)):
            raise AssertionError(f"stats CSV row has missing/extra values: {row}")
        function = row["function"]
        if function not in allowed_targets:
            raise AssertionError(f"stats CSV row is not a target: {function!r}")
        if function in seen:
            raise AssertionError(f"duplicate stats CSV function: {function!r}")
        if not re.fullmatch(r"[1-9][0-9]*", row["count"]):
            raise AssertionError(f"invalid positive stats count: {row}")
        count = int(row["count"])
        if not re.fullmatch(r"[1-9][0-9]*", row["per_thread"]):
            raise AssertionError(f"invalid positive per-thread count: {row}")
        try:
            per_rank = float(row["per_rank"])
        except ValueError as exc:
            raise AssertionError(
                f"invalid numeric per-rank average: {row}"
            ) from exc
        expected_per_rank = count / rank_count
        if (not math.isfinite(per_rank) or per_rank <= 0.0 or
                not math.isclose(
                    per_rank,
                    expected_per_rank,
                    rel_tol=1e-10,
                    abs_tol=1e-15,
                )):
            raise AssertionError(
                "invalid per-rank average "
                f"(expected {expected_per_rank:.12g}): {row}"
            )
        for field in STATS_CSV_FIELDS[4:]:
            try:
                value = float(row[field])
            except ValueError as exc:
                raise AssertionError(
                    f"invalid numeric stats field {field}: {row}"
                ) from exc
            if not math.isfinite(value):
                raise AssertionError(f"nonfinite stats field {field}: {row}")
        seen.add(function)
        rows.append(row)
    return rows


def read_profiled_stats(args):
    paths = sorted(args.work_dir.glob("milc-like-stats-p*.csv"))
    if not paths:
        raise AssertionError("missing PEAK stats CSV for profiled call coverage")
    if len(paths) != 1:
        raise AssertionError(f"expected one PEAK stats CSV, found {paths}")

    profiled = {
        name: {
            "count": 0,
            "per_thread": 0,
            "overhead_s": 0.0,
            "thread_count_estimate": 0,
        }
        for name in HOT_TARGETS
    }
    with paths[0].open("r", encoding="utf-8", newline="") as handle:
        rows = validated_profiled_stats_rows(handle, set(profiled))
        for row in rows:
            function = row["function"]
            try:
                count = int(row["count"])
                per_thread = int(row["per_thread"])
                overhead_s = float(row["overhead_s"])
            except (KeyError, ValueError) as exc:
                raise AssertionError(f"invalid stats CSV row: {row}") from exc
            profiled[function]["count"] += count
            profiled[function]["per_thread"] += per_thread
            profiled[function]["overhead_s"] += overhead_s
            if per_thread > 0:
                estimated_threads = (count + per_thread - 1) // per_thread
                profiled[function]["thread_count_estimate"] = max(
                    profiled[function]["thread_count_estimate"],
                    estimated_threads,
                )

    return [profiled[target_name(index)] for index in range(TARGET_COUNT)], paths[0]


read_profiled_counts = read_profiled_stats


def projected_detach_seconds(detach_ratio, detach_elapsed):
    if (not math.isfinite(detach_ratio) or
            not math.isfinite(detach_elapsed) or
            detach_ratio < 0.0 or detach_elapsed < 0.0):
        return math.inf
    projected = detach_ratio * detach_elapsed
    return projected if math.isfinite(projected) else math.inf


def reference_profile_seconds_floor(calibrated_profile_cost):
    if (math.isfinite(calibrated_profile_cost) and
            calibrated_profile_cost > 0.0):
        return calibrated_profile_cost
    return PROFILE_SECONDS_FALLBACK


def reference_projected_profile_seconds(saved_seconds,
                                        current_profile_seconds,
                                        calibrated_profile_cost=math.nan):
    positive = [
        value
        for value in (saved_seconds, current_profile_seconds)
        if math.isfinite(value) and value > 0.0
    ]
    return (
        max(positive)
        if positive
        else reference_profile_seconds_floor(calibrated_profile_cost)
    )


def projected_detach_ratio(detach_ratio, detach_elapsed, current_elapsed,
                           current_profile_seconds=0.0):
    if not math.isfinite(current_elapsed) or current_elapsed <= 0.0:
        raise AssertionError("current elapsed must be positive")
    saved_seconds = projected_detach_seconds(detach_ratio, detach_elapsed)
    return (
        reference_projected_profile_seconds(saved_seconds,
                                            current_profile_seconds) /
        current_elapsed
    )


def reference_projected_profile_ratio(saved_seconds,
                                      current_profile_seconds,
                                      current_elapsed):
    if not math.isfinite(current_elapsed) or current_elapsed <= 0.0:
        raise AssertionError("current elapsed must be positive")
    return (
        reference_projected_profile_seconds(saved_seconds,
                                            current_profile_seconds) /
        current_elapsed
    )


def reference_dynamic_observation(previous_calls,
                                  previous_time,
                                  total_calls,
                                  now,
                                  has_observation_baseline):
    if now <= previous_time:
        raise AssertionError("dynamic observation time must advance")
    if not has_observation_baseline:
        return {
            "recent_calls": 0,
            "recent_rate": 0.0,
            "recent_only_would_detach": False,
            "previous_calls": total_calls,
            "previous_time": now,
            "has_observation_baseline": True,
        }
    recent_calls = (
        total_calls - previous_calls
        if total_calls >= previous_calls else total_calls
    )
    recent_rate = recent_calls / (now - previous_time)
    return {
        "recent_calls": recent_calls,
        "recent_rate": recent_rate,
        "recent_only_would_detach": recent_rate > 0.0,
        "previous_calls": total_calls,
        "previous_time": now,
        "has_observation_baseline": True,
    }


def reference_per_target_detach(lifetime_ratio, target_ratio):
    if (not math.isfinite(lifetime_ratio) or
            not math.isfinite(target_ratio) or target_ratio < 0.0):
        return False
    return lifetime_ratio > target_ratio


def reference_attached_lifetime_pressure(hooks):
    return sum(
        hook["lifetime_ratio"]
        for hook in hooks
        if hook["state"] == "attached"
    )


def reference_attached_pressure_components(hooks):
    recent_sum = sum(
        hook.get("recent_rate", 0.0)
        for hook in hooks
        if hook["state"] == "attached"
    )
    lifetime_sum = reference_attached_lifetime_pressure(hooks)
    return recent_sum, lifetime_sum


def reference_attached_hybrid_pressure(hooks):
    recent_sum, lifetime_sum = reference_attached_pressure_components(hooks)
    return max(recent_sum, lifetime_sum)


def reference_global_detach(hooks, global_ratio, detach_factor=1.0):
    if (not math.isfinite(global_ratio) or global_ratio < 0.0 or
            not math.isfinite(detach_factor) or detach_factor < 0.0):
        return False
    return (
        reference_attached_hybrid_pressure(hooks) >
        global_ratio * detach_factor
    )


def reference_hybrid_global_selection(hooks, global_ratio):
    recent_sum, lifetime_sum = reference_attached_pressure_components(hooks)
    candidates = sorted(
        (
            hook for hook in hooks
            if (hook["state"] == "attached" and
                (hook["lifetime_ratio"] > 0.0 or
                 hook.get("recent_rate", 0.0) > 0.0))
        ),
        key=lambda hook: (
            -max(hook["lifetime_ratio"], hook.get("recent_rate", 0.0)),
            -hook.get("recent_rate", 0.0),
        ),
    )
    selected = []
    for hook in candidates:
        if max(recent_sum, lifetime_sum) <= global_ratio:
            break
        selected.append(hook)
        recent_sum = max(0.0, recent_sum - hook.get("recent_rate", 0.0))
        lifetime_sum = max(0.0, lifetime_sum - hook["lifetime_ratio"])
    return selected, recent_sum, lifetime_sum


def reference_predicted_stop_seconds(last_stop_seconds,
                                     average_stop_seconds,
                                     peak_cost_seconds=None):
    # The admission predictor uses the latest measured transition window only.
    # Average stop-window totals and PEAK_COST are reporting inputs, not policy.
    _ = average_stop_seconds
    _ = peak_cost_seconds
    if not math.isfinite(last_stop_seconds):
        return math.inf
    return max(0.0, last_stop_seconds)


def reference_batch_windows(hook_count, batch_capacity):
    if hook_count < 0:
        raise AssertionError("hook count must be non-negative")
    if batch_capacity <= 0:
        raise AssertionError("batch capacity must be positive")
    return math.ceil(hook_count / batch_capacity)


def reference_candidate_saved_seconds(candidate):
    if "saved_seconds" in candidate:
        return candidate["saved_seconds"]
    return projected_detach_seconds(candidate["detach_ratio"],
                                    candidate["detach_elapsed"])


def reference_candidate_projected_seconds(candidate):
    return reference_projected_profile_seconds(
        reference_candidate_saved_seconds(candidate),
        candidate.get("current_profile_seconds", 0.0),
        candidate.get("profile_cost", math.nan),
    )


def reference_candidate_historical_projection(candidate, elapsed):
    if not math.isfinite(elapsed) or elapsed <= 0.0:
        return math.inf
    projected_seconds = reference_candidate_projected_seconds(candidate)
    rate = projected_seconds / elapsed
    return rate if math.isfinite(rate) and rate > 0.0 else math.inf


def reference_candidate_rate(candidate, elapsed):
    return reference_candidate_historical_projection(candidate, elapsed)


def reference_rate_future_lease_seconds(rate, hb_max_seconds):
    if (not math.isfinite(rate) or rate <= 0.0 or
            not math.isfinite(hb_max_seconds) or hb_max_seconds <= 0.0):
        return math.inf
    lease = rate * hb_max_seconds
    return lease if math.isfinite(lease) and lease > 0.0 else math.inf


def reference_candidate_future_lease_seconds(candidate, elapsed,
                                             hb_max_seconds):
    return reference_rate_future_lease_seconds(
        reference_candidate_rate(candidate, elapsed),
        hb_max_seconds,
    )


def reference_pending_future_lease_seconds(pending_requests,
                                           elapsed,
                                           hb_max_seconds):
    total = 0.0
    for request in pending_requests:
        if request["state"] not in ("reattach_requested", "reattaching"):
            continue
        historical_rate = reference_candidate_rate(request, elapsed)
        stored_rate = request.get("request_rate", math.inf)
        request_rate = (
            stored_rate
            if math.isfinite(stored_rate) and stored_rate > 0.0
            else historical_rate
        )
        lease = reference_rate_future_lease_seconds(
            request_rate,
            hb_max_seconds,
        )
        if (not math.isfinite(lease) or lease <= 0.0 or
                total > sys.float_info.max - lease):
            return math.inf
        total += lease
    return total


def reference_actual_cumulative_spend(profile_spent_seconds,
                                      control_spent_seconds):
    if (not math.isfinite(profile_spent_seconds) or
            not math.isfinite(control_spent_seconds) or
            profile_spent_seconds < 0.0 or control_spent_seconds < 0.0):
        return math.inf
    total = profile_spent_seconds + control_spent_seconds
    return total if math.isfinite(total) else math.inf


def reference_multiply_nonnegative_finite(lhs, rhs):
    if (not math.isfinite(lhs) or lhs < 0.0 or
            not math.isfinite(rhs) or rhs < 0.0 or
            (rhs > 0.0 and lhs > sys.float_info.max / rhs)):
        return math.inf
    product = lhs * rhs
    return product if math.isfinite(product) and product >= 0.0 else math.inf


def reference_admitted_candidates(candidates,
                                  elapsed,
                                  hb_max_seconds,
                                  global_ratio,
                                  reattach_factor,
                                  profile_spent_seconds,
                                  control_spent_seconds,
                                  management_spent_seconds,
                                  pending_requests,
                                  pending_count,
                                  predicted_stop_seconds,
                                  batch_capacity):
    # Management CPU is diagnostic only; it may overlap application work.
    _ = management_spent_seconds
    if (not math.isfinite(elapsed) or elapsed <= 0.0 or
            not math.isfinite(global_ratio) or global_ratio < 0.0 or
            not math.isfinite(reattach_factor) or reattach_factor < 0.0 or
            not math.isfinite(predicted_stop_seconds) or
            predicted_stop_seconds < 0.0):
        return [], -math.inf
    actual_spent_seconds = reference_actual_cumulative_spend(
        profile_spent_seconds,
        control_spent_seconds,
    )
    pending_seconds = reference_pending_future_lease_seconds(
        pending_requests,
        elapsed,
        hb_max_seconds,
    )
    if (not math.isfinite(actual_spent_seconds) or
            not math.isfinite(pending_seconds)):
        return [], -math.inf
    spent_ratio = actual_spent_seconds / elapsed
    reattach_gate_ratio = reference_multiply_nonnegative_finite(
        reattach_factor,
        global_ratio,
    )
    if (not math.isfinite(spent_ratio) or
            not math.isfinite(reattach_gate_ratio) or
            spent_ratio > reattach_gate_ratio):
        return [], -math.inf
    headroom = (
        global_ratio * elapsed -
        actual_spent_seconds -
        pending_seconds
    )
    if not math.isfinite(headroom):
        return [], -math.inf
    before_windows = reference_batch_windows(pending_count, batch_capacity)
    headroom -= before_windows * predicted_stop_seconds
    admitted = []
    admitted_count = 0
    finite_candidates = [
        candidate
        for candidate in candidates
        if (math.isfinite(candidate["detach_time"]) and
            math.isfinite(
                reference_candidate_historical_projection(candidate, elapsed)
            ))
    ]
    ordered = sorted(
        finite_candidates,
        key=lambda item: (
            item["detach_time"],
            reference_candidate_historical_projection(item, elapsed),
            item["name"],
        ),
    )
    for candidate in ordered:
        lease_seconds = reference_candidate_future_lease_seconds(
            candidate,
            elapsed,
            hb_max_seconds,
        )
        before_count = pending_count + admitted_count
        after_count = before_count + 1
        before_candidate_windows = reference_batch_windows(before_count,
                                                           batch_capacity)
        after_candidate_windows = reference_batch_windows(after_count,
                                                          batch_capacity)
        extra_stop_seconds = (
            after_candidate_windows - before_candidate_windows
        ) * predicted_stop_seconds
        charge_seconds = lease_seconds + extra_stop_seconds
        if charge_seconds > headroom:
            continue
        admitted.append(candidate["name"])
        admitted_count += 1
        headroom -= charge_seconds
    return admitted, headroom


def coverage_summary(args, true_calls, target_phase_calls, profiled_stats):
    profiled_calls = [item["count"] for item in profiled_stats]
    true_total = sum(true_calls)
    profiled_total = sum(min(profiled, true)
                         for true, profiled in zip(true_calls, profiled_calls))
    weighted_coverage = profiled_total / true_total if true_total > 0 else 0.0
    unprofiled_targets = [
        target_name(index)
        for index, (true, profiled) in enumerate(zip(true_calls, profiled_calls))
        if true > 0 and profiled <= 0
    ]

    phase_ratios = []
    phase_breadths = []
    for phase in range(TARGET_PHASES):
        start = phase * PHASE_TARGETS
        end = start + PHASE_TARGETS
        phase_subset = list(range(start, end))
        true_breadth = sum(
            1
            for index in phase_subset
            if target_phase_calls[phase][index] > 0
        )
        profiled_breadth = sum(
            1
            for index in phase_subset
            if target_phase_calls[phase][index] > 0 and
            profiled_calls[index] > 0
        )
        ratio = profiled_breadth / true_breadth if true_breadth > 0 else 1.0
        phase_ratios.append(ratio)
        phase_breadths.append((phase, profiled_breadth, true_breadth, ratio))

    return {
        "weighted_call_coverage": weighted_coverage,
        "profiled_calls": profiled_total,
        "true_calls": true_total,
        "unprofiled_targets": len(unprofiled_targets),
        "unprofiled_target_sample": unprofiled_targets[:3],
        "phase_target_breadth_min": min(phase_ratios),
        "phase_target_breadth_avg": sum(phase_ratios) / len(phase_ratios),
        "phase_target_breadth_failures": [
            phase for phase, _, _, ratio in phase_breadths
            if ratio < args.min_phase_target_breadth
        ],
        "phase_profiled_evidence": "explicit-subset+phase-target-matrix",
    }


def denominator_summary(args, peak, profiled_stats):
    profile_aggregate_wall_seconds = sum(
        item["overhead_s"] for item in profiled_stats
    )
    profile_thread_normalized_seconds = 0.0
    for item in profiled_stats:
        if item["count"] <= 0:
            continue
        profile_thread_normalized_seconds += (
            item["overhead_s"] * item["per_thread"] / item["count"]
        )

    profile_thread_normalized_ratio = (
        profile_thread_normalized_seconds / peak["peak_time"]
        if peak["peak_time"] > 0.0 else 0.0
    )
    profile_aggregate_wall_ratio = (
        profile_aggregate_wall_seconds / peak["peak_time"]
        if peak["peak_time"] > 0.0 else 0.0
    )
    denominator_error = abs(
        peak["estimated_profile_ratio"] - profile_aggregate_wall_ratio
    )
    if (peak["reported_profile_seconds"] is None or
            peak["reported_control_seconds"] is None):
        raise AssertionError(
            "missing local profile+control overhead report"
        )
    if peak["reported_management_seconds"] is None:
        raise AssertionError("missing heartbeat management overhead report")
    reported_profile_seconds = peak["reported_profile_seconds"]
    reported_control_seconds = peak["reported_control_seconds"]
    reported_management_seconds = peak["reported_management_seconds"]
    reported_risk_control_seconds = peak["reported_risk_control_seconds"]
    reported_raw_profile_control_ratio = (
        peak["reported_raw_profile_control_ratio"]
    )
    reported_risk_profile_control_ratio = (
        peak["reported_risk_profile_control_ratio"]
    )
    if (reported_raw_profile_control_ratio is None or
            reported_risk_profile_control_ratio is None or
            reported_risk_control_seconds is None):
        raise AssertionError("missing separated raw/risk overhead report")
    if peak["accounting_snapshot_valid"] != 1:
        raise AssertionError("final accounting snapshot must be valid")
    if peak["failed_stop_attempts"] < 0:
        raise AssertionError("failed control-window count must be nonnegative")
    reported_profile_ratio = peak["reported_profile_ratio"]
    reported_control_ratio = peak["reported_control_ratio"]
    if reported_profile_ratio is None or reported_control_ratio is None:
        raise AssertionError(
            "missing explicit profile_ratio/control_ratio fields in "
            "profile+control overhead report"
        )
    reported_management_ratio = (
        peak["reported_management_ratio"]
        if peak["reported_management_ratio"] is not None else
        reported_management_seconds / peak["peak_time"]
        if peak["peak_time"] > 0.0 else 0.0
    )
    calculated_raw_profile_control_ratio = (
        (profile_aggregate_wall_seconds + reported_control_seconds) /
        peak["peak_time"]
        if peak["peak_time"] > 0.0 else 0.0
    )
    calculated_risk_profile_control_ratio = (
        (profile_aggregate_wall_seconds + reported_risk_control_seconds) /
        peak["peak_time"]
        if peak["peak_time"] > 0.0 else 0.0
    )
    reported_raw_profile_control_denominator_error = abs(
        reported_raw_profile_control_ratio -
        calculated_raw_profile_control_ratio
    )
    reported_risk_profile_control_denominator_error = abs(
        reported_risk_profile_control_ratio -
        calculated_risk_profile_control_ratio
    )
    reported_profile_denominator_error = abs(
        reported_profile_ratio - profile_aggregate_wall_ratio
    )
    reported_control_denominator_error = abs(
        reported_control_seconds - peak["control_stop_window_seconds"]
    ) / peak["peak_time"] if peak["peak_time"] > 0.0 else 0.0
    true_thread_info = peak["target_thread_info"]
    stats_thread_estimate_too_wide = []
    for index, item in enumerate(profiled_stats):
        if item["count"] <= 0:
            continue
        true_breadth = true_thread_info["breadth"][index]
        if item["thread_count_estimate"] > true_breadth:
            stats_thread_estimate_too_wide.append(
                (target_name(index), item["thread_count_estimate"], true_breadth)
            )
    return {
        "profile_thread_normalized_ratio": profile_thread_normalized_ratio,
        "profile_aggregate_wall_ratio": profile_aggregate_wall_ratio,
        "profile_thread_normalized_seconds":
            profile_thread_normalized_seconds,
        "profile_aggregate_wall_seconds": profile_aggregate_wall_seconds,
        "final_profile_denominator_error": denominator_error,
        "reported_profile_denominator_error":
            reported_profile_denominator_error,
        "reported_control_denominator_error":
            reported_control_denominator_error,
        "reported_raw_profile_control_ratio":
            reported_raw_profile_control_ratio,
        "reported_risk_profile_control_ratio":
            reported_risk_profile_control_ratio,
        "reported_risk_local_ranks": peak["reported_risk_local_ranks"],
        "reported_risk_control_seconds": reported_risk_control_seconds,
        "failed_stop_attempts": peak["failed_stop_attempts"],
        "accounting_snapshot_valid": peak["accounting_snapshot_valid"],
        "calculated_raw_profile_control_ratio":
            calculated_raw_profile_control_ratio,
        "calculated_risk_profile_control_ratio":
            calculated_risk_profile_control_ratio,
        "reported_profile_ratio": reported_profile_ratio,
        "reported_control_ratio": reported_control_ratio,
        "reported_management_ratio": reported_management_ratio,
        "reported_raw_profile_control_denominator_error":
            reported_raw_profile_control_denominator_error,
        "reported_risk_profile_control_denominator_error":
            reported_risk_profile_control_denominator_error,
        "stats_thread_estimate_too_wide":
            len(stats_thread_estimate_too_wide),
        "stats_thread_estimate_sample": stats_thread_estimate_too_wide[:3],
    }


def run_synthetic_policy_diagnostics():
    def expect_assertion(label, callback):
        try:
            callback()
        except AssertionError:
            return
        raise AssertionError(f"synthetic negative did not fail: {label}")

    def synthetic_case(summary_calls, checkpoint_calls):
        return {
            "summary_calls": summary_calls,
            "checkpoints": [
                {
                    "index": index,
                    "elapsed": (double_index + 1.0) * 0.1,
                    "true_calls": calls,
                }
                for index, (double_index, calls) in enumerate(
                    zip(range(len(checkpoint_calls)), checkpoint_calls)
                )
            ],
        }

    historical_threads = 13
    active_threads = 1
    calls_after_interval = 100
    per_active_thread = math.ceil(calls_after_interval / active_threads)
    per_historical_thread = math.ceil(calls_after_interval / historical_threads)
    if per_active_thread == per_historical_thread:
        raise AssertionError("historical thread denominator hid active interval")

    per_target_limit = 0.01
    global_limit = 0.05
    global_recent_hooks = [
        {
            "state": "attached",
            "lifetime_ratio": 0.001,
            "recent_rate": 0.009,
        }
        for _ in range(6)
    ]
    global_recent_sum, global_recent_lifetime_sum = (
        reference_attached_pressure_components(global_recent_hooks)
    )
    recent_local_detach_count = sum(
        reference_per_target_detach(
            hook["lifetime_ratio"], per_target_limit
        )
        for hook in global_recent_hooks
    )
    recent_selected, recent_reduced_sum, recent_reduced_lifetime = (
        reference_hybrid_global_selection(global_recent_hooks, global_limit)
    )
    if (global_recent_sum <= global_limit or
            global_recent_lifetime_sum >= global_limit or
            recent_local_detach_count != 0 or
            not reference_global_detach(global_recent_hooks, global_limit) or
            not recent_selected or
            max(recent_reduced_sum, recent_reduced_lifetime) > global_limit):
        raise AssertionError(
            "global recent aggregate counterexample did not detach"
        )

    global_lifetime_ratios = [0.009, 0.009, 0.009, 0.009, 0.009, 0.009]
    if any(value > per_target_limit for value in global_lifetime_ratios):
        raise AssertionError("synthetic per-target setup is invalid")
    global_lifetime_hooks = [
        {
            "state": "attached",
            "lifetime_ratio": value,
            "recent_rate": 0.001,
        }
        for value in global_lifetime_ratios
    ]
    global_cumulative_recent_sum, global_cumulative_sum = (
        reference_attached_pressure_components(global_lifetime_hooks)
    )
    cumulative_selected, cumulative_reduced_recent, cumulative_reduced_sum = (
        reference_hybrid_global_selection(global_lifetime_hooks, global_limit)
    )
    if (global_cumulative_sum <= global_limit or
            global_cumulative_recent_sum >= global_limit or
            not reference_global_detach(global_lifetime_hooks, global_limit) or
            not cumulative_selected or
            max(cumulative_reduced_recent, cumulative_reduced_sum) > global_limit):
        raise AssertionError(
            "global cumulative lifetime counterexample did not detach"
        )

    lifetime = {"a": 0.030, "b": 0.001}
    recent = {"a": 0.000, "b": 0.040}
    lifetime_leader = max(lifetime, key=lifetime.get)
    recent_leader = max(recent, key=recent.get)
    if lifetime_leader == recent_leader:
        raise AssertionError("recent/lifetime dominance did not split")
    hot_recent_burst_target = 0.005
    hot_recent_burst_lifetime = 0.0045
    hot_recent_burst_rate = 0.040
    if hot_recent_burst_rate <= hot_recent_burst_target:
        raise AssertionError("hot recent burst is not hot enough")
    if reference_per_target_detach(
            hot_recent_burst_lifetime,
            hot_recent_burst_target):
        raise AssertionError(
            "recent-only redetach counterexample detached below lifetime target"
        )

    detach_ratio = 0.40
    detach_elapsed = 10.0
    early_elapsed = 20.0
    late_elapsed = 100.0
    early_projected = projected_detach_ratio(
        detach_ratio, detach_elapsed, early_elapsed
    )
    late_projected = projected_detach_ratio(
        detach_ratio, detach_elapsed, late_elapsed
    )
    if not (math.isclose(early_projected, 0.20) and
            math.isclose(late_projected, 0.04)):
        raise AssertionError("detach-time seconds did not decay with elapsed")
    saved_dominates_ratio = reference_projected_profile_ratio(
        saved_seconds=0.40,
        current_profile_seconds=0.20,
        current_elapsed=10.0,
    )
    observed_dominates_ratio = reference_projected_profile_ratio(
        saved_seconds=0.10,
        current_profile_seconds=0.80,
        current_elapsed=10.0,
    )
    if not (math.isclose(saved_dominates_ratio, 0.04) and
            math.isclose(observed_dominates_ratio, 0.08)):
        raise AssertionError(
            "reattach projection must max saved and current observed seconds"
        )
    observed_candidate = {
        "name": "observed-history",
        "saved_seconds": 0.10,
        "current_profile_seconds": 0.80,
        "recent_rate": 99.0,
        "detach_time": 1.0,
    }
    observed_candidate_rate = reference_candidate_rate(
        observed_candidate,
        elapsed=10.0,
    )
    observed_candidate_lease = reference_candidate_future_lease_seconds(
        observed_candidate,
        elapsed=10.0,
        hb_max_seconds=0.5,
    )
    if (not math.isclose(observed_candidate_rate, 0.08) or
            not math.isclose(observed_candidate_lease, 0.04)):
        raise AssertionError(
            "future lease must use historical projection rate times hb_max"
        )

    old_last_nonzero_rate = 0.20
    candidate = {
        "name": "old-hot",
        "detach_ratio": detach_ratio,
        "detach_elapsed": 1.0,
        "detach_time": 1.0,
    }
    admitted_decay, _ = reference_admitted_candidates(
        [candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.35,
        control_spent_seconds=0.10,
        management_spent_seconds=0.05,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_decay != ["old-hot"]:
        raise AssertionError("decaying detach-time seconds should admit old-hot")
    if old_last_nonzero_rate <= global_limit:
        raise AssertionError("last-nonzero counterexample is not hot enough")

    lease_candidate = {
        "name": "leased-hot",
        "detach_ratio": 4.0,
        "detach_elapsed": 1.0,
        "detach_time": 1.0,
    }
    lease_seconds = reference_candidate_future_lease_seconds(
        lease_candidate,
        elapsed=100.0,
        hb_max_seconds=0.5,
    )
    if not math.isclose(lease_seconds, 0.02):
        raise AssertionError("future lease did not use the hb_max horizon")
    admitted_lease, _ = reference_admitted_candidates(
        [lease_candidate],
        elapsed=100.0,
        hb_max_seconds=0.5,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=4.74,
        control_spent_seconds=0.0,
        management_spent_seconds=0.05,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_lease != ["leased-hot"]:
        raise AssertionError(
            "headroom must charge a bounded future lease, not full history"
        )

    actual_spent_candidate = {
        "name": "actual-spent-blocked",
        "detach_ratio": 0.01,
        "detach_elapsed": 1.0,
        "detach_time": 1.0,
    }
    admitted_actual_spent, _ = reference_admitted_candidates(
        [actual_spent_candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=1.01,
        control_spent_seconds=0.0,
        management_spent_seconds=99.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_actual_spent:
        raise AssertionError(
            "actual historical spend, not management or projections, must gate"
        )
    actual_cumulative_spend = reference_actual_cumulative_spend(0.60, 0.20)
    if not math.isclose(actual_cumulative_spend, 0.80):
        raise AssertionError("actual spend included a future lease")

    wave_gate_candidate = {
        "name": "wave-gate",
        "detach_ratio": 0.20,
        "detach_elapsed": 1.0,
        "detach_time": 1.0,
    }
    admitted_wave_boundary, _ = reference_admitted_candidates(
        [wave_gate_candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.95,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    admitted_wave_above, _ = reference_admitted_candidates(
        [wave_gate_candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.96,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    admitted_wave_overflow, _ = reference_admitted_candidates(
        [wave_gate_candidate],
        elapsed=1.0,
        hb_max_seconds=1.0,
        global_ratio=2.0,
        reattach_factor=sys.float_info.max,
        profile_spent_seconds=0.0,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if (admitted_wave_boundary != ["wave-gate"] or
            admitted_wave_above or admitted_wave_overflow):
        raise AssertionError(
            "reattach wave gate boundary/overflow check failed"
        )

    cumulative_candidates = [
        {
            "name": "old-a",
            "detach_ratio": 0.60,
            "detach_elapsed": 1.0,
            "detach_time": 1.0,
        },
        {
            "name": "old-b",
            "detach_ratio": 0.60,
            "detach_elapsed": 1.0,
            "detach_time": 2.0,
        },
        {
            "name": "new-c",
            "detach_ratio": 0.60,
            "detach_elapsed": 1.0,
            "detach_time": 3.0,
        },
    ]
    admitted_shared, remaining_headroom = reference_admitted_candidates(
        cumulative_candidates,
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.86,
        control_spent_seconds=0.06,
        management_spent_seconds=0.05,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_shared != ["old-a", "old-b"]:
        raise AssertionError(
            f"shared cumulative headroom admitted {admitted_shared}"
        )
    if remaining_headroom >= 0.03:
        raise AssertionError("headroom was not debited cumulatively")

    pending_candidate = {
        "name": "pending-blocked",
        "detach_ratio": 0.60,
        "detach_elapsed": 1.0,
        "detach_time": 1.0,
    }
    pending_rate = reference_candidate_rate(pending_candidate, elapsed=20.0)
    pending_request = dict(
        pending_candidate,
        state="reattach_requested",
        request_rate=pending_rate,
    )
    pending_lease_seconds = reference_pending_future_lease_seconds(
        [pending_request],
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    if not math.isclose(pending_lease_seconds, 0.03):
        raise AssertionError("pending request did not carry candidate rate")
    admitted_pending, _ = reference_admitted_candidates(
        [pending_candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.95,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[pending_request],
        pending_count=1,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_pending:
        raise AssertionError("pending future lease was not charged")
    cleared_request = dict(pending_request, state="attached")
    cleared_pending_seconds = reference_pending_future_lease_seconds(
        [cleared_request],
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    admitted_after_pending_clear, _ = reference_admitted_candidates(
        [pending_candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.95,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[cleared_request],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if (cleared_pending_seconds != 0.0 or
            admitted_after_pending_clear != ["pending-blocked"]):
        raise AssertionError("cleared pending request retained a future lease")

    zero_candidate = {
        "name": "zero-history",
        "saved_seconds": 0.0,
        "current_profile_seconds": 0.0,
        "detach_time": 1.0,
    }
    calibrated_candidate = dict(
        zero_candidate,
        name="calibrated-floor",
        profile_cost=2e-6,
    )
    nonfinite_candidate = {
        "name": "nonfinite-history",
        "saved_seconds": math.inf,
        "current_profile_seconds": math.nan,
        "detach_time": 1.0,
    }
    zero_candidate_lease = reference_candidate_future_lease_seconds(
        zero_candidate,
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    calibrated_candidate_lease = reference_candidate_future_lease_seconds(
        calibrated_candidate,
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    nonfinite_candidate_lease = reference_candidate_future_lease_seconds(
        nonfinite_candidate,
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    admitted_nonfinite_spend, _ = reference_admitted_candidates(
        [pending_candidate],
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=math.nan,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    zero_pending_seconds = reference_pending_future_lease_seconds(
        [dict(zero_candidate, state="reattaching", request_rate=0.0)],
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    calibrated_pending_seconds = reference_pending_future_lease_seconds(
        [dict(calibrated_candidate,
              state="reattaching",
              request_rate=0.0)],
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    nonfinite_pending_seconds = reference_pending_future_lease_seconds(
        [dict(nonfinite_candidate,
              state="reattaching",
              request_rate=math.nan)],
        elapsed=20.0,
        hb_max_seconds=1.0,
    )
    nonfinite_horizon_lease = reference_candidate_future_lease_seconds(
        pending_candidate,
        elapsed=20.0,
        hb_max_seconds=math.nan,
    )
    underflow_lease = reference_rate_future_lease_seconds(
        math.nextafter(0.0, 1.0),
        0.5,
    )
    expected_floor_lease = PROFILE_SECONDS_FALLBACK / 20.0
    expected_calibrated_lease = calibrated_candidate["profile_cost"] / 20.0
    if (admitted_nonfinite_spend or
            not math.isclose(zero_candidate_lease, expected_floor_lease) or
            not math.isclose(calibrated_candidate_lease,
                             expected_calibrated_lease) or
            not math.isclose(nonfinite_candidate_lease,
                             expected_floor_lease) or
            not math.isclose(zero_pending_seconds, expected_floor_lease) or
            not math.isclose(calibrated_pending_seconds,
                             expected_calibrated_lease) or
            not math.isclose(nonfinite_pending_seconds,
                             expected_floor_lease) or
            not math.isinf(nonfinite_horizon_lease) or
            not math.isinf(underflow_lease)):
        raise AssertionError(
            "calibrated/fallback floor or post-product validation regressed"
        )

    expected_batch_extras = {
        0: 1,
        63: 0,
        64: 1,
        65: 0,
        128: 1,
    }
    for pending_count_value, expected_extra in expected_batch_extras.items():
        before_windows = reference_batch_windows(pending_count_value,
                                                 batch_capacity=64)
        after_windows = reference_batch_windows(pending_count_value + 1,
                                                batch_capacity=64)
        actual_extra = after_windows - before_windows
        if actual_extra != expected_extra:
            raise AssertionError(
                "batch boundary reserve mismatch for pending="
                f"{pending_count_value}: expected {expected_extra}, "
                f"got {actual_extra}"
            )

    batch_candidates = [
        {
            "name": "batch-boundary",
            "detach_ratio": 0.20,
            "detach_elapsed": 1.0,
            "detach_time": 1.0,
        },
    ]
    admitted_batch, _ = reference_admitted_candidates(
        batch_candidates,
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.50,
        control_spent_seconds=0.05,
        management_spent_seconds=0.05,
        pending_requests=[],
        pending_count=64,
        predicted_stop_seconds=0.25,
        batch_capacity=64,
    )
    if admitted_batch:
        raise AssertionError("batch-boundary stop-window reserve was not shared")

    skip_candidates = [
        {
            "name": "old-expensive",
            "detach_ratio": 4.0,
            "detach_elapsed": 1.0,
            "detach_time": 1.0,
        },
        {
            "name": "new-affordable",
            "detach_ratio": 0.20,
            "detach_elapsed": 1.0,
            "detach_time": 2.0,
        },
    ]
    admitted_skip, _ = reference_admitted_candidates(
        skip_candidates,
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.95,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_skip != ["new-affordable"]:
        raise AssertionError(
            f"unaffordable oldest candidate was not skipped: {admitted_skip}"
        )

    fairness_candidates = [
        {
            "name": "newest",
            "detach_ratio": 0.40,
            "detach_elapsed": 1.0,
            "detach_time": 3.0,
        },
        {
            "name": "oldest",
            "detach_ratio": 0.40,
            "detach_elapsed": 1.0,
            "detach_time": 1.0,
        },
        {
            "name": "middle",
            "detach_ratio": 0.40,
            "detach_elapsed": 1.0,
            "detach_time": 2.0,
        },
    ]
    admitted_fairness, _ = reference_admitted_candidates(
        fairness_candidates,
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.95,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_fairness != ["oldest", "middle"]:
        raise AssertionError(
            f"oldest-first fairness admitted {admitted_fairness}"
        )
    fairness_tie_candidates = [
        {
            "name": "tie-high",
            "saved_seconds": 0.40,
            "current_profile_seconds": 0.80,
            "detach_time": 1.0,
        },
        {
            "name": "tie-low",
            "saved_seconds": 0.40,
            "current_profile_seconds": 0.10,
            "detach_time": 1.0,
        },
    ]
    admitted_fairness_tie, _ = reference_admitted_candidates(
        fairness_tie_candidates,
        elapsed=20.0,
        hb_max_seconds=1.0,
        global_ratio=0.05,
        reattach_factor=0.95,
        profile_spent_seconds=0.95,
        control_spent_seconds=0.0,
        management_spent_seconds=0.0,
        pending_requests=[],
        pending_count=0,
        predicted_stop_seconds=0.0,
        batch_capacity=64,
    )
    if admitted_fairness_tie != ["tie-low"]:
        raise AssertionError(
            "historical projection did not break detach-time fairness tie"
        )

    first_dynamic = reference_dynamic_observation(
        previous_calls=0,
        previous_time=1.0,
        total_calls=500,
        now=1.1,
        has_observation_baseline=False,
    )
    second_dynamic = reference_dynamic_observation(
        previous_calls=first_dynamic["previous_calls"],
        previous_time=first_dynamic["previous_time"],
        total_calls=700,
        now=1.3,
        has_observation_baseline=first_dynamic["has_observation_baseline"],
    )
    if (first_dynamic["recent_only_would_detach"] or
            first_dynamic["recent_calls"] != 0):
        raise AssertionError("first dynamic observation must seed baseline")
    if (second_dynamic["recent_calls"] != 200 or
            not second_dynamic["recent_only_would_detach"]):
        raise AssertionError("second dynamic observation must expose baseline delta")

    pressure_hooks = [
        {"state": "attached", "lifetime_ratio": 0.03, "recent_rate": 0.04},
        {"state": "detach_requested", "lifetime_ratio": 0.40, "recent_rate": 0.50},
        {"state": "detaching", "lifetime_ratio": 0.20, "recent_rate": 0.30},
        {"state": "detached", "lifetime_ratio": 0.10, "recent_rate": 0.20},
    ]
    attached_recent_pressure, attached_lifetime_pressure = (
        reference_attached_pressure_components(pressure_hooks)
    )
    pressure = reference_attached_hybrid_pressure(pressure_hooks)
    if (not math.isclose(attached_recent_pressure, 0.04) or
            not math.isclose(attached_lifetime_pressure, 0.03) or
            not math.isclose(pressure, 0.04)):
        raise AssertionError("pending detach pressure was not excluded")

    predicted_without_measurement = reference_predicted_stop_seconds(
        last_stop_seconds=0.0,
        average_stop_seconds=0.0,
        peak_cost_seconds=99.0,
    )
    predicted_with_measurement = reference_predicted_stop_seconds(
        last_stop_seconds=0.02,
        average_stop_seconds=0.03,
        peak_cost_seconds=99.0,
    )
    if predicted_without_measurement != 0.0:
        raise AssertionError("PEAK_COST fallback influenced stop-window model")
    if not math.isclose(predicted_with_measurement, 0.02):
        raise AssertionError("last measured stop-window prediction did not dominate")

    dynamic_capacity = 64
    dynamic_hook_count = 832
    previous_calls = [0] * dynamic_capacity
    if dynamic_hook_count > dynamic_capacity:
        previous_calls.extend([0] * (dynamic_hook_count - dynamic_capacity))
        dynamic_capacity = dynamic_hook_count
    previous_calls[831] = 1
    if dynamic_capacity != 832 or previous_calls[831] != 1:
        raise AssertionError("dynamic hook heartbeat growth went out of bounds")

    def reference_local_rank_risk(profile_seconds, raw_control_seconds,
                                  detected_local_ranks):
        local_ranks = (
            detected_local_ranks
            if isinstance(detected_local_ranks, int) and
            detected_local_ranks > 0 else 1
        )
        return (
            profile_seconds + local_ranks * raw_control_seconds,
            local_ranks,
        )

    fallback_risk_seconds, fallback_local_ranks = reference_local_rank_risk(
        1.0, 2.0, 0
    )
    if fallback_local_ranks != 1 or fallback_risk_seconds != 3.0:
        raise AssertionError("invalid local-rank discovery must fall back to one")

    one_second_raw_ratio = 0.01 + 0.02
    one_second_risk_ratio, one_second_local_ranks = (
        reference_local_rank_risk(0.01, 0.02, 4)
    )
    reattach_gate_ratio = 0.05
    if (one_second_local_ranks != 4 or
            not one_second_raw_ratio < reattach_gate_ratio or
            not one_second_risk_ratio > reattach_gate_ratio):
        raise AssertionError(
            "local-rank risk guard did not reject the one-second exposure"
        )

    local_overhead_report = (
        "[peak] local profile+control overhead: "
        "profile_seconds=1.000000000 control_seconds=2.000000000 "
        "profile_ratio=0.100000000 control_ratio=0.200000000 "
        "ratio=0.300000000\n"
        "[peak] local profile+local-rank-control risk: "
        "profile_seconds=1.000000000 raw_control_seconds=2.000000000 "
        "local_ranks=1 risk_control_seconds=2.000000000 "
        "ratio=0.300000000\n"
        "[peak] heartbeat management overhead: "
        "cpu_seconds=0.125000000 ratio=0.012500000\n"
        "[peak] local control stop-window overhead: "
        "windows=4 wall_seconds=2.000000000 ratio=0.200000000\n"
        "[peak] local failed control windows: "
        "windows=0 snapshot_valid=1\n"
    )
    local_reported = parse_reported_overheads(local_overhead_report)
    if (local_reported["control_stop_window_prefix"] != "local" or
            local_reported["control_stop_window_windows"] != 4 or
            local_reported["failed_stop_attempt_prefix"] != "local" or
            local_reported["failed_stop_attempts"] != 0 or
            local_reported["accounting_snapshot_valid"] != 1 or
            local_reported["risk_local_ranks"] != 1 or
            not math.isclose(local_reported["profile_control_ratio"], 0.3) or
            not math.isclose(
                local_reported["risk_profile_control_ratio"], 0.3
            )):
        raise AssertionError("local control stop-window parser regressed")

    rank_zero_overhead_report = (
        "[peak] local profile+control overhead: "
        "profile_seconds=1.000000000 control_seconds=2.500000000 "
        "profile_ratio=0.100000000 control_ratio=0.250000000 "
        "ratio=0.350000000\n"
        "[peak] local profile+local-rank-control risk: "
        "profile_seconds=1.000000000 raw_control_seconds=2.500000000 "
        "local_ranks=4 risk_control_seconds=10.000000000 "
        "ratio=1.100000000\n"
        "[peak] heartbeat management overhead: "
        "cpu_seconds=0.125000000 ratio=0.012500000\n"
        "[peak] rank-0/local control stop-window overhead: "
        "windows=5 wall_seconds=2.500000000 ratio=0.250000000\n"
        "[peak] aggregate failed control windows: "
        "windows=2 snapshot_valid=0\n"
    )
    rank_zero_reported = parse_reported_overheads(rank_zero_overhead_report)
    if (rank_zero_reported["control_stop_window_prefix"] != "rank-0/local" or
            rank_zero_reported["failed_stop_attempt_prefix"] != "aggregate" or
            rank_zero_reported["failed_stop_attempts"] != 2 or
            rank_zero_reported["accounting_snapshot_valid"] != 0 or
            rank_zero_reported["risk_local_ranks"] != 4 or
            not math.isclose(
                rank_zero_reported["profile_control_ratio"], 0.35
            ) or
            not math.isclose(
                rank_zero_reported["risk_profile_control_ratio"], 1.1
            )):
        raise AssertionError("rank-0/local control stop-window parser regressed")
    expect_assertion(
        "zero local-rank risk report",
        lambda: parse_reported_overheads(
            local_overhead_report.replace("local_ranks=1", "local_ranks=0")
        ),
    )
    expect_assertion(
        "mismatched local-rank risk control",
        lambda: parse_reported_overheads(
            rank_zero_overhead_report.replace(
                "risk_control_seconds=10.000000000",
                "risk_control_seconds=2.500000000",
            )
        ),
    )
    expect_assertion(
        "risk ratio below raw ratio",
        lambda: parse_reported_overheads(
            rank_zero_overhead_report.replace(
                "ratio=1.100000000", "ratio=0.300000000"
            )
        ),
    )
    expect_assertion(
        "missing failed control-window diagnostics",
        lambda: parse_reported_overheads(
            local_overhead_report.replace(
                "[peak] local failed control windows: "
                "windows=0 snapshot_valid=1\n",
                "",
            )
        ),
    )
    expect_assertion(
        "duplicate failed control-window diagnostics",
        lambda: parse_reported_overheads(
            local_overhead_report +
            "[peak] local failed control windows: "
            "windows=1 snapshot_valid=1\n"
        ),
    )
    expect_assertion(
        "aggregate failed control-window prefix mismatch",
        lambda: parse_reported_overheads(
            rank_zero_overhead_report.replace(
                "[peak] aggregate failed control windows:",
                "[peak] local failed control windows:",
            )
        ),
    )
    final_transition_report = parse_final_transition_coverage(
        "[peak] local final transition coverage: "
        "detached_targets=832 reattached_targets=84 revisited_targets=42\n"
    )
    if (not final_transition_report["present"] or
            final_transition_report["prefix"] != "local" or
            final_transition_report["detached_targets"] != 832 or
            final_transition_report["reattached_targets"] != 84 or
            final_transition_report["revisited_targets"] != 42):
        raise AssertionError("final transition coverage parser regressed")
    reattached_only_report = parse_final_transition_coverage(
        "[peak] local final transition coverage: "
        "detached_targets=832 reattached_targets=832\n"
    )
    if reattached_only_report["present"]:
        raise AssertionError("reattach transitions were accepted as revisit coverage")
    aggregate_transition_report = parse_final_transition_coverage(
        "[peak] aggregate final transition coverage: "
        "detached_targets=832 reattached_targets=84 revisited_targets=42\n"
    )
    if (not aggregate_transition_report["present"] or
            aggregate_transition_report["prefix"] != "aggregate"):
        raise AssertionError("aggregate transition coverage parser regressed")
    rank_local_transition_report = parse_final_transition_coverage(
        "[peak] rank-0/local final transition coverage: "
        "detached_targets=832 reattached_targets=84 revisited_targets=42\n"
    )
    if rank_local_transition_report["present"]:
        raise AssertionError("rank-local transition evidence parsed as aggregate")
    expect_assertion(
        "duplicate final transition coverage",
        lambda: parse_final_transition_coverage(
            "[peak] local final transition coverage: "
            "detached_targets=1 reattached_targets=1 revisited_targets=1\n"
            "[peak] local final transition coverage: "
            "detached_targets=2 reattached_targets=2 revisited_targets=2\n"
        ),
    )

    def synthetic_stats_csv(fields, values):
        output = io.StringIO()
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(fields)
        writer.writerow(values)
        output.seek(0)
        return output

    valid_stats_values = [
        "target", "1", "1", "1", "0", "0", "0", "0", "0", "0", "1e-6"
    ]
    valid_stats_rows = validated_profiled_stats_rows(
        synthetic_stats_csv(STATS_CSV_FIELDS, valid_stats_values),
        {"target"},
    )
    if len(valid_stats_rows) != 1:
        raise AssertionError("valid stats CSV row was not accepted")
    expect_assertion(
        "malformed stats field set",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(STATS_CSV_FIELDS[:-1], valid_stats_values[:-1]),
            {"target"},
        ),
    )
    expect_assertion(
        "missing stats value",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(STATS_CSV_FIELDS, valid_stats_values[:-1]),
            {"target"},
        ),
    )
    expect_assertion(
        "extra stats value",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(
                STATS_CSV_FIELDS,
                valid_stats_values + ["unexpected"],
            ),
            {"target"},
        ),
    )
    expect_assertion(
        "foreign stats target",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(STATS_CSV_FIELDS, valid_stats_values),
            {"different-target"},
        ),
    )
    malformed_integer_values = list(valid_stats_values)
    malformed_integer_values[2] = "1.5"
    expect_assertion(
        "malformed stats integer",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(STATS_CSV_FIELDS, malformed_integer_values),
            {"target"},
        ),
    )
    sparse_rank_values = list(valid_stats_values)
    sparse_rank_values[3] = "0.000244140625"
    sparse_rank_rows = validated_profiled_stats_rows(
        synthetic_stats_csv(STATS_CSV_FIELDS, sparse_rank_values),
        {"target"},
        rank_count=4096,
    )
    if len(sparse_rank_rows) != 1:
        raise AssertionError("fractional per-rank average was not accepted")
    invalid_per_rank_values = list(valid_stats_values)
    invalid_per_rank_values[3] = "0"
    expect_assertion(
        "zero per-rank average",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(
                STATS_CSV_FIELDS,
                invalid_per_rank_values,
            ),
            {"target"},
        ),
    )
    inconsistent_per_rank_values = list(valid_stats_values)
    inconsistent_per_rank_values[3] = "0.5"
    expect_assertion(
        "inconsistent per-rank average",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(
                STATS_CSV_FIELDS,
                inconsistent_per_rank_values,
            ),
            {"target"},
        ),
    )
    nonfinite_float_values = list(valid_stats_values)
    nonfinite_float_values[4] = "nan"
    expect_assertion(
        "nonfinite stats float",
        lambda: validated_profiled_stats_rows(
            synthetic_stats_csv(STATS_CSV_FIELDS, nonfinite_float_values),
            {"target"},
        ),
    )

    expect_assertion(
        "legacy combined overhead report",
        lambda: parse_reported_overheads(
            "[peak] local profile+control+management overhead: "
            "profile_seconds=1 control_seconds=2 management_seconds=3 "
            "ratio=4\n"
        ),
    )
    expect_assertion(
        "legacy unqualified control stop-window report",
        lambda: parse_reported_overheads(
            "[peak] local profile+control overhead: "
            "profile_seconds=1 control_seconds=2 "
            "profile_ratio=1 control_ratio=2 ratio=3\n"
            "[peak] heartbeat management overhead: "
            "cpu_seconds=0.1 ratio=0.01\n"
            "[peak] control stop-window overhead: "
            "windows=1 wall_seconds=2 ratio=0.2\n"
        ),
    )
    expect_assertion(
        "control seconds mismatch",
        lambda: parse_reported_overheads(
            "[peak] local profile+control overhead: "
            "profile_seconds=1 control_seconds=2 "
            "profile_ratio=1 control_ratio=2 ratio=3\n"
            "[peak] heartbeat management overhead: "
            "cpu_seconds=0.1 ratio=0.01\n"
            "[peak] local control stop-window overhead: "
            "windows=1 wall_seconds=2.1 ratio=0.21\n"
        ),
    )
    expect_assertion(
        "legacy observed profile+control report",
        lambda: parse_reported_overheads(
            "[peak] local profile+control observed overhead: "
            "profile_seconds=1 control_seconds=2 "
            "profile_ratio=1 control_ratio=2 ratio=3\n"
            "[peak] heartbeat management overhead: "
            "cpu_seconds=0.1 ratio=0.01\n"
            "[peak] local control stop-window overhead: "
            "windows=1 wall_seconds=2 ratio=0.2\n"
        ),
    )
    expect_assertion(
        "missing management overhead report",
        lambda: parse_reported_overheads(
            "[peak] local profile+control overhead: "
            "profile_seconds=1 control_seconds=2 "
            "profile_ratio=1 control_ratio=2 ratio=3\n"
            "[peak] local control stop-window overhead: "
            "windows=1 wall_seconds=2 ratio=0.2\n"
        ),
    )
    expect_assertion(
        "missing explicit profile/control ratios",
        lambda: parse_reported_overheads(
            "[peak] local profile+control overhead: "
            "profile_seconds=1 control_seconds=2 ratio=3\n"
            "[peak] heartbeat management overhead: "
            "cpu_seconds=0.1 ratio=0.01\n"
            "[peak] local control stop-window overhead: "
            "windows=1 wall_seconds=2 ratio=0.2\n"
        ),
    )

    fixed_args = argparse.Namespace(
        iterations=1,
        warmup_checkpoints=0,
        min_progress_intervals=1,
    )
    expect_assertion(
        "fixed-work final tail",
        lambda: progress_aligned_summary(
            fixed_args,
            synthetic_case(150, [0, 100]),
            synthetic_case(150, [0, 100]),
            synthetic_case(150, [0, 100]),
        ),
    )
    expect_assertion(
        "fixed-work mismatched true work",
        lambda: progress_aligned_summary(
            fixed_args,
            synthetic_case(150, [0, 100, 150]),
            synthetic_case(149, [0, 100, 149]),
            synthetic_case(150, [0, 100, 150]),
        ),
    )
    insufficient_args = argparse.Namespace(
        iterations=0,
        warmup_checkpoints=0,
        min_progress_intervals=2,
    )
    expect_assertion(
        "insufficient progress intervals",
        lambda: progress_aligned_summary(
            insufficient_args,
            synthetic_case(100, [0, 100]),
            synthetic_case(100, [0, 100]),
            synthetic_case(100, [0, 100]),
        ),
    )

    print(
        "milc_like_synthetic_policy_diagnostics_ok "
        f"historical_threads={historical_threads} "
        f"active_threads={active_threads} "
        f"global_recent_sum={global_recent_sum:.6f} "
        f"global_recent_lifetime_sum={global_recent_lifetime_sum:.6f} "
        f"global_recent_detach=1 "
        f"global_recent_local_detach_count={recent_local_detach_count} "
        f"global_recent_selected={len(recent_selected)} "
        f"global_cumulative_sum={global_cumulative_sum:.6f} "
        f"global_cumulative_recent_sum={global_cumulative_recent_sum:.6f} "
        f"global_cumulative_detach=1 "
        f"global_cumulative_selected={len(cumulative_selected)} "
        f"lifetime_leader={lifetime_leader} "
        f"recent_leader={recent_leader} "
        f"hot_recent_burst_lifetime={hot_recent_burst_lifetime:.6f} "
        f"hot_recent_burst_rate={hot_recent_burst_rate:.6f} "
        "hot_recent_burst_redetach=0 "
        f"early_projected={early_projected:.6f} "
        f"late_projected={late_projected:.6f} "
        f"saved_dominates_ratio={saved_dominates_ratio:.6f} "
        f"observed_dominates_ratio={observed_dominates_ratio:.6f} "
        f"observed_candidate_rate={observed_candidate_rate:.6f} "
        f"observed_candidate_lease={observed_candidate_lease:.6f} "
        f"old_last_nonzero_rate={old_last_nonzero_rate:.6f} "
        f"decay_admitted={len(admitted_decay)} "
        f"lease_seconds={lease_seconds:.6f} "
        f"lease_admitted={len(admitted_lease)} "
        f"actual_spent_admitted={len(admitted_actual_spent)} "
        f"wave_boundary_admitted={len(admitted_wave_boundary)} "
        f"wave_above_admitted={len(admitted_wave_above)} "
        f"wave_overflow_admitted={len(admitted_wave_overflow)} "
        f"shared_headroom_admitted={len(admitted_shared)} "
        f"pending_lease_admitted={len(admitted_pending)} "
        f"pending_clear_admitted={len(admitted_after_pending_clear)} "
        f"pending_clear_seconds={cleared_pending_seconds:.6f} "
        f"fallback_floor_lease={zero_candidate_lease:.3e} "
        f"calibrated_floor_lease={calibrated_candidate_lease:.3e} "
        f"underflow_rejected={int(math.isinf(underflow_lease))} "
        f"batch_boundary_admitted={len(admitted_batch)} "
        f"skip_unaffordable_admitted={len(admitted_skip)} "
        f"oldest_first_admitted={len(admitted_fairness)} "
        f"fairness_tie_admitted={len(admitted_fairness_tie)} "
        f"dynamic_first_skip={int(not first_dynamic['recent_only_would_detach'])} "
        f"attached_recent_pressure={attached_recent_pressure:.6f} "
        f"attached_lifetime_pressure={attached_lifetime_pressure:.6f} "
        f"attached_pressure={pressure:.6f} "
        f"predicted_without_peak_cost={predicted_without_measurement:.6f} "
        f"dynamic_capacity={dynamic_capacity} "
        f"fallback_local_ranks={fallback_local_ranks} "
        f"fallback_risk_seconds={fallback_risk_seconds:.6f} "
        f"raw_report_ratio={local_reported['profile_control_ratio']:.6f} "
        "risk_report_ratio="
        f"{local_reported['risk_profile_control_ratio']:.6f} "
        "rank4_raw_report_ratio="
        f"{rank_zero_reported['profile_control_ratio']:.6f} "
        "rank4_risk_report_ratio="
        f"{rank_zero_reported['risk_profile_control_ratio']:.6f} "
        f"one_second_raw_ratio={one_second_raw_ratio:.6f} "
        f"one_second_risk_ratio={one_second_risk_ratio:.6f} "
        "one_second_guard_reject=1 "
        "nonfinite_fail_closed=1 "
        "negative_gate_checks=22"
    )


def finite_float(value):
    try:
        parsed = float(value)
    except ValueError:
        return False
    return math.isfinite(parsed)


def parse_trace(trace_path):
    rows = []
    if not trace_path.exists():
        return rows
    with trace_path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.reader(handle):
            if not row:
                continue
            if len(row) <= max(TRACE_COLUMNS.values()):
                raise AssertionError(f"short trace row: {row}")
            rows.append(row)
    return rows


def trace_summary(args, trace_path):
    rows = parse_trace(trace_path)
    summary = {
        "detach": 0,
        "reattach": 0,
        "failures": 0,
        "heartbeat_detach": 0,
        "distinct_detach": set(),
        "distinct_reattach": set(),
        "distinct_redetach_after_reattach": set(),
        "reattach_redetach_intervals": [],
        "reattach_request_calls": {},
        "detach_episodes": [],
        "heartbeat_wall_signal_max": 0.0,
        "heartbeat_target_wall_signal_max": 0.0,
        "max_accounting_ratio": 0.0,
        "max_stable_accounting_ratio": 0.0,
        "final_accounting_ratio": 0.0,
    }
    active_detached_functions = set()
    per_target_limit = float(args.target_overhead_ratio)
    global_limit = float(args.global_overhead_ratio)
    reattach_limit = global_limit * float(args.global_reattach_factor)
    last_reattach_time = {}
    open_detach_episodes = {}

    for row in rows:
        operation = row[TRACE_COLUMNS["operation"]]
        result = row[TRACE_COLUMNS["result"]]
        function = row[TRACE_COLUMNS["function"]]
        status = row[TRACE_COLUMNS["status"]]

        if (result in ("prepare-failed", "gum-failed", "classify-failed",
                       "retry-abandoned") or
                status == "classify-failed" or
                row[12] == "classify-failed"):
            summary["failures"] += 1
        if (operation in ("detach", "reattach") and
                finite_float(row[TRACE_COLUMNS["accounting_ratio"]])):
            accounting_ratio = float(row[TRACE_COLUMNS["accounting_ratio"]])
            if row[TRACE_COLUMNS["accounting_snapshot_valid"]] not in ("0", "1"):
                raise AssertionError(
                    f"trace row has invalid accounting snapshot validity: {row}"
                )
            if not re.fullmatch(
                r"[0-9]+",
                row[TRACE_COLUMNS["failed_stop_windows"]],
            ):
                raise AssertionError(
                    f"trace row has invalid failed control-window count: {row}"
                )
            summary["max_accounting_ratio"] = max(
                summary["max_accounting_ratio"],
                accounting_ratio,
            )
            if (finite_float(row[TRACE_COLUMNS["request_total_time"]]) and
                    float(row[TRACE_COLUMNS["request_total_time"]]) >=
                    args.stable_accounting_after_seconds):
                summary["max_stable_accounting_ratio"] = max(
                    summary["max_stable_accounting_ratio"],
                    accounting_ratio,
                )
            summary["final_accounting_ratio"] = accounting_ratio
        if result != "success" or operation not in ("detach", "reattach"):
            continue

        source = row[TRACE_COLUMNS["request_source"]]
        if (not args.allow_detach_count and
                source == "detach-count"):
            raise AssertionError(f"unexpected detach-count transition: {row}")
        if row[TRACE_COLUMNS["physical"]] != "1":
            raise AssertionError(f"success row was not physical: {row}")
        if status != "safe":
            raise AssertionError(f"success row status was not safe: {row}")
        if int(row[TRACE_COLUMNS["batch_id"]]) == 0:
            raise AssertionError(f"success row has zero batch id: {row}")
        if float(row[TRACE_COLUMNS["stop_window_us"]]) <= 0.0:
            raise AssertionError(f"success row has no stop-window cost: {row}")
        if float(row[TRACE_COLUMNS["pending_age_s"]]) < 0.0:
            raise AssertionError(f"success row has negative pending age: {row}")
        if int(row[TRACE_COLUMNS["request_calls"]]) < 0:
            raise AssertionError(f"success row has invalid request calls: {row}")
        for name in ("request_ratio", "request_global_overhead",
                     "request_total_time"):
            if not finite_float(row[TRACE_COLUMNS[name]]):
                raise AssertionError(f"success row has non-finite {name}: {row}")
        summary["heartbeat_wall_signal_max"] = max(
            summary["heartbeat_wall_signal_max"],
            float(row[TRACE_COLUMNS["request_global_overhead"]]),
        )
        summary["heartbeat_target_wall_signal_max"] = max(
            summary["heartbeat_target_wall_signal_max"],
            float(row[TRACE_COLUMNS["request_ratio"]]),
        )

        if operation == "detach":
            if source not in ("per-target-heartbeat", "global-heartbeat"):
                raise AssertionError(f"detach source was not heartbeat: {row}")
            if int(row[TRACE_COLUMNS["request_calls"]]) <= 0:
                raise AssertionError(f"heartbeat detach has no request calls: {row}")
            if float(row[TRACE_COLUMNS["request_ratio"]]) <= 0.0:
                raise AssertionError(f"heartbeat detach has no request ratio: {row}")
            if (source == "per-target-heartbeat" and
                    float(row[TRACE_COLUMNS["request_ratio"]]) <= per_target_limit):
                raise AssertionError(
                    "per-target detach did not exceed cumulative target "
                    f"ratio: {row}"
                )
            if (source == "global-heartbeat" and
                    float(row[TRACE_COLUMNS["request_global_overhead"]]) <= global_limit):
                raise AssertionError(
                    "global detach did not exceed cumulative attached "
                    f"global ratio: {row}"
                )
            if function in active_detached_functions:
                raise AssertionError(f"duplicate detach before reattach: {row}")
            summary["detach"] += 1
            summary["heartbeat_detach"] += 1
            summary["distinct_detach"].add(function)
            active_detached_functions.add(function)
            episode = {
                "function": function,
                "request_calls": int(row[TRACE_COLUMNS["request_calls"]]),
                "detach_time": float(row[TRACE_COLUMNS["request_total_time"]]),
                "reattached": False,
                "reattach_time": None,
            }
            summary["detach_episodes"].append(episode)
            open_detach_episodes[function] = episode
            if function in last_reattach_time:
                interval = (
                    float(row[TRACE_COLUMNS["request_total_time"]]) -
                    last_reattach_time.pop(function)
                )
                if interval >= 0.0:
                    summary["reattach_redetach_intervals"].append(interval)
                    summary["distinct_redetach_after_reattach"].add(function)
        else:
            if function not in active_detached_functions:
                raise AssertionError(f"reattach without active same-hook detach: {row}")
            if source not in ("per-target-heartbeat", "global-heartbeat"):
                raise AssertionError(f"reattach source was not heartbeat: {row}")
            if float(row[TRACE_COLUMNS["request_global_overhead"]]) > reattach_limit:
                raise AssertionError(f"reattach exceeded global reattach limit: {row}")
            summary["reattach"] += 1
            summary["distinct_reattach"].add(function)
            active_detached_functions.remove(function)
            episode = open_detach_episodes.pop(function)
            episode["reattached"] = True
            episode["reattach_time"] = float(
                row[TRACE_COLUMNS["request_total_time"]]
            )
            summary["reattach_request_calls"][function] = max(
                summary["reattach_request_calls"].get(function, 0),
                int(row[TRACE_COLUMNS["request_calls"]]),
            )
            last_reattach_time[function] = episode["reattach_time"]

    return summary


def empty_trace_summary():
    return {
        "detach": 0,
        "reattach": 0,
        "failures": 0,
        "heartbeat_detach": 0,
        "distinct_detach": set(),
        "distinct_reattach": set(),
        "distinct_redetach_after_reattach": set(),
        "reattach_redetach_intervals": [],
        "reattach_request_calls": {},
        "detach_episodes": [],
        "heartbeat_wall_signal_max": 0.0,
        "heartbeat_target_wall_signal_max": 0.0,
        "max_accounting_ratio": 0.0,
        "max_stable_accounting_ratio": 0.0,
        "final_accounting_ratio": 0.0,
    }


def revisit_summary(args, trace_counts, profiled_stats, true_calls):
    profiled_by_name = {
        target_name(index): item["count"]
        for index, item in enumerate(profiled_stats)
    }
    revisited = set()
    reattached_without_growth = set()
    for function, request_calls in trace_counts["reattach_request_calls"].items():
        final_profiled_calls = profiled_by_name.get(function, 0)
        if final_profiled_calls > request_calls:
            revisited.add(function)
        else:
            reattached_without_growth.add(function)

    return {
        "revisit_profile_growth": len(revisited),
        "revisit_target_breadth":
            len(revisited) / TARGET_COUNT if TARGET_COUNT > 0 else 0.0,
        "revisit_observed": len(trace_counts["reattach_request_calls"]),
        "reattached_without_growth": len(reattached_without_growth),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--synthetic-policy-diagnostics", action="store_true")
    parser.add_argument("--exe")
    parser.add_argument("--libpeak")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--seconds", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=8000000)
    parser.add_argument("--inner-work", type=int, default=0)
    parser.add_argument("--phase-repeats", type=int, default=8192)
    parser.add_argument("--window-ms", type=int, default=1000)
    parser.add_argument("--checkpoint-calls", type=int, default=0)
    parser.add_argument("--warmup-windows", type=int, default=2)
    parser.add_argument("--warmup-checkpoints", type=int, default=2)
    parser.add_argument("--min-progress-intervals", type=int, default=0)
    parser.add_argument("--pin-workers", action="store_true")
    parser.add_argument("--affinity-cpus", type=int, default=0)
    parser.add_argument("--max-window-overhead", type=float, default=-1.0)
    parser.add_argument("--max-window-baseline-drift", type=float, default=-1.0)
    parser.add_argument("--min-progress-average-overhead",
                        type=float, default=-1.0)
    parser.add_argument("--max-progress-average-overhead",
                        type=float, default=-1.0)
    parser.add_argument("--max-progress-sustained-overhead",
                        type=float, default=-1.0)
    parser.add_argument("--max-progress-baseline-drift",
                        type=float, default=-1.0)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--peak-max-threads", type=int, default=128)
    parser.add_argument("--target-overhead-ratio", default="0.01")
    parser.add_argument("--global-overhead-ratio", default="0.05")
    parser.add_argument("--global-reattach-factor", default="1.0")
    parser.add_argument("--heartbeat-interval", default="0.005")
    parser.add_argument("--reattach-cooldown-ms", default="0")
    parser.add_argument("--detach-backend",
                        choices=("auto", "strict", "signal", "helper"),
                        default="strict")
    parser.add_argument("--detach-count", default="1000000000000")
    parser.add_argument("--max-overhead-ratio", type=float, default=0.06)
    parser.add_argument("--min-overhead-ratio", type=float, default=-1.0)
    parser.add_argument("--min-detach-success", type=int, default=1)
    parser.add_argument("--min-reattach-success", type=int, default=0)
    parser.add_argument("--min-distinct-detach", type=int, default=1)
    parser.add_argument("--min-distinct-reattach", type=int, default=0)
    parser.add_argument("--min-redetach-after-reattach", type=int, default=0)
    parser.add_argument("--max-reattach-redetach-seconds",
                        type=float, default=-1.0)
    parser.add_argument("--min-revisit-profile-growth", type=int, default=0)
    parser.add_argument("--min-revisit-target-breadth", type=float, default=0.0)
    parser.add_argument("--max-reattached-without-growth",
                        type=int, default=-1)
    parser.add_argument("--max-revisit-starvation", type=int, default=0)
    parser.add_argument("--max-trace-failures", type=int, default=0)
    parser.add_argument("--min-max-accounting-ratio", type=float, default=-1.0)
    parser.add_argument("--max-stable-accounting-ratio", type=float, default=-1.0)
    parser.add_argument("--stable-accounting-after-seconds", type=float, default=1.0)
    parser.add_argument("--min-final-accounting-ratio", type=float, default=-1.0)
    parser.add_argument("--max-final-accounting-ratio", type=float, default=-1.0)
    parser.add_argument("--min-final-detached-targets", type=int, default=0)
    parser.add_argument("--min-final-reattached-targets", type=int, default=0)
    parser.add_argument("--min-final-revisited-targets", type=int, default=0)
    parser.add_argument("--max-final-profile-denominator-error",
                        type=float, default=0.01)
    parser.add_argument("--max-reported-profile-control-denominator-error",
                        type=float, default=0.01)
    parser.add_argument("--max-reported-aggregate-denominator-error",
                        dest="max_reported_profile_control_denominator_error",
                        type=float)
    parser.add_argument("--max-baseline-drift", type=float, default=-1.0)
    parser.add_argument("--min-weighted-call-coverage",
                        type=float, default=0.01)
    parser.add_argument("--min-phase-target-breadth", type=float, default=1.0)
    parser.add_argument("--max-starved-targets", type=int, default=0)
    parser.add_argument("--allow-detach-count", action="store_true")
    parser.add_argument("--require-reattach", action="store_true")
    parser.add_argument("--disable-trace", action="store_true")
    args = parser.parse_args()

    if args.synthetic_policy_diagnostics:
        run_synthetic_policy_diagnostics()
        return
    missing = [
        name
        for name in ("exe", "libpeak", "work_dir")
        if getattr(args, name) is None
    ]
    if missing:
        parser.error("missing required arguments: " + ", ".join(missing))
    if args.affinity_cpus < 0:
        parser.error("--affinity-cpus must be non-negative")
    if args.checkpoint_calls <= 0:
        args.checkpoint_calls = (
            args.threads * args.phase_repeats * PHASE_TARGETS * TARGET_PHASES
        )

    args.work_dir.mkdir(parents=True, exist_ok=True)
    trace_path = args.work_dir / "milc-like-peak-trace.csv"
    try:
        trace_path.unlink()
    except FileNotFoundError:
        pass
    peak_trace_path = None if args.disable_trace else trace_path

    baseline_before = run_case(args, peak=False, arm_name="baseline-before")
    reset_peak_artifacts(args)
    peak = run_case(args, peak=True, arm_name="peak", trace_path=peak_trace_path)
    profiled_stats, stats_path = read_profiled_stats(args)
    baseline_after = run_case(args, peak=False, arm_name="baseline-after")
    validate_arm_modes(args, baseline_before, peak, baseline_after)
    coverage = coverage_summary(args,
                                peak["target_calls"],
                                peak["target_phase_calls"],
                                profiled_stats)
    denominators = denominator_summary(args, peak, profiled_stats)
    baseline = {
        "elapsed": (baseline_before["elapsed"] + baseline_after["elapsed"]) / 2.0,
        "calls_per_sec": (
            baseline_before["calls_per_sec"] + baseline_after["calls_per_sec"]
        ) / 2.0,
        "output": baseline_before["output"] + baseline_after["output"],
    }
    baseline_drift = (
        abs(baseline_before["calls_per_sec"] - baseline_after["calls_per_sec"]) /
        baseline["calls_per_sec"]
        if baseline["calls_per_sec"] > 0.0 else 0.0
    )
    counts = (
        empty_trace_summary()
        if args.disable_trace else trace_summary(args, trace_path)
    )
    revisit = revisit_summary(
        args, counts, profiled_stats, peak["target_calls"]
    )
    final_accounting_ratio = (
        peak["control_stop_window_ratio"]
        if peak["control_stop_window_ratio"] > 0.0
        else counts["final_accounting_ratio"]
    )
    final_transition = peak["final_transition_coverage"]

    if args.iterations > 0:
        whole_run_overhead = (
            (peak["elapsed"] - baseline["elapsed"]) / baseline["elapsed"]
            if baseline["elapsed"] > 0.0
            else 0.0
        )
        whole_run_metric = "elapsed"
    else:
        whole_run_overhead = (
            (baseline["calls_per_sec"] - peak["calls_per_sec"]) /
            baseline["calls_per_sec"]
            if baseline["calls_per_sec"] > 0.0
            else 0.0
        )
        whole_run_metric = "throughput"
    window_summary = None
    progress_summary = None
    if args.iterations == 0:
        window_summary = matched_window_summary(
            args, baseline_before, peak, baseline_after
        )
        print(
            "milc_like_window_metrics "
            f"average_actual_overhead={window_summary['average_actual_overhead']:.6f} "
            f"max_actual_overhead={window_summary['max_actual_overhead']:.6f} "
            f"max_baseline_drift={window_summary['max_baseline_drift']:.6f} "
            f"windows={window_summary['count']}",
            flush=True,
        )
    have_progress_checkpoints = all(
        len(case["checkpoints"]) >= 2
        for case in (baseline_before, peak, baseline_after)
    )
    if progress_gate_required(args) or have_progress_checkpoints:
        progress_summary = progress_aligned_summary(
            args, baseline_before, peak, baseline_after
        )
        print(
            "milc_like_progress_metrics "
            "average_actual_overhead="
            f"{progress_summary['average_actual_overhead']:.6f} "
            f"max_actual_overhead={progress_summary['max_actual_overhead']:.6f} "
            f"max_baseline_drift={progress_summary['max_baseline_drift']:.6f} "
            f"intervals={progress_summary['count']} "
            f"common_intervals={progress_summary['common_intervals']}",
            flush=True,
        )

    progress_average = (
        progress_summary["average_actual_overhead"]
        if progress_summary is not None else whole_run_overhead
    )
    progress_max = (
        progress_summary["max_actual_overhead"]
        if progress_summary is not None else whole_run_overhead
    )
    progress_sustained_max = (
        progress_summary["max_sustained_overhead"]
        if progress_summary is not None else whole_run_overhead
    )
    progress_baseline_drift = (
        progress_summary["max_baseline_drift"]
        if progress_summary is not None else baseline_drift
    )
    if args.iterations > 0:
        actual_overhead = whole_run_overhead
        actual_overhead_metric = "whole-run-fixed-work"
    else:
        actual_overhead = (
            progress_average if progress_summary is not None
            else whole_run_overhead
        )
        actual_overhead_metric = (
            "progress-aligned-timed-work"
            if progress_summary is not None else whole_run_metric
        )
    raw_reported_delta = (
        denominators["reported_raw_profile_control_ratio"] - actual_overhead
    )
    risk_reported_delta = (
        denominators["reported_risk_profile_control_ratio"] - actual_overhead
    )
    print(
        "milc_like_metrics "
        f"baseline_before_cps={baseline_before['calls_per_sec']:.3f} "
        f"baseline_after_cps={baseline_after['calls_per_sec']:.3f} "
        f"baseline_drift={baseline_drift:.6f} "
        f"whole_run_overhead={whole_run_overhead:.6f} "
        f"whole_run_metric={whole_run_metric} "
        f"actual_overhead={actual_overhead:.6f} "
        f"actual_overhead_metric={actual_overhead_metric} "
        f"progress_average_overhead={progress_average:.6f} "
        f"progress_max_overhead={progress_max:.6f} "
        f"progress_sustained_max_overhead={progress_sustained_max:.6f} "
        f"progress_baseline_drift={progress_baseline_drift:.6f} "
        f"detach_success={counts['detach']} "
        f"reattach_success={counts['reattach']} "
        f"distinct_detach={len(counts['distinct_detach'])} "
        f"distinct_reattach={len(counts['distinct_reattach'])} "
        f"final_detached_targets={final_transition['detached_targets']} "
        f"final_reattached_targets={final_transition['reattached_targets']} "
        f"final_revisited_targets={final_transition['revisited_targets']} "
        f"weighted_call_coverage={coverage['weighted_call_coverage']:.6f} "
        f"phase_target_breadth_min={coverage['phase_target_breadth_min']:.6f} "
        f"phase_target_breadth_avg={coverage['phase_target_breadth_avg']:.6f} "
        f"revisit_target_breadth={revisit['revisit_target_breadth']:.6f} "
        f"unprofiled_targets={coverage['unprofiled_targets']} "
        f"revisit_profile_growth={revisit['revisit_profile_growth']} "
        f"revisit_observed={revisit['revisit_observed']} "
        f"reattached_without_growth={revisit['reattached_without_growth']} "
        f"final_accounting_ratio={final_accounting_ratio:.6f} "
        "profile_aggregate_wall_ratio="
        f"{denominators['profile_aggregate_wall_ratio']:.6f} "
        "reported_raw_profile_control_ratio="
        f"{denominators['reported_raw_profile_control_ratio']:.6f} "
        "reported_risk_profile_control_ratio="
        f"{denominators['reported_risk_profile_control_ratio']:.6f} "
        f"reported_risk_local_ranks={denominators['reported_risk_local_ranks']} "
        f"failed_stop_attempts={denominators['failed_stop_attempts']} "
        f"accounting_snapshot_valid={denominators['accounting_snapshot_valid']} "
        "final_profile_denominator_error="
        f"{denominators['final_profile_denominator_error']:.6f} "
        "reported_profile_denominator_error="
        f"{denominators['reported_profile_denominator_error']:.6f} "
        "reported_raw_profile_control_denominator_error="
        f"{denominators['reported_raw_profile_control_denominator_error']:.6f} "
        "reported_risk_profile_control_denominator_error="
        f"{denominators['reported_risk_profile_control_denominator_error']:.6f} "
        f"raw_reported_delta={raw_reported_delta:.6f} "
        f"risk_reported_delta={risk_reported_delta:.6f}",
        flush=True,
    )

    if (args.max_baseline_drift >= 0.0 and
            baseline_drift > args.max_baseline_drift):
        raise AssertionError(
            f"baseline_drift={baseline_drift:.6f} exceeds "
            f"{args.max_baseline_drift:.6f}; invalid B-A-B baseline; "
            "use Frontera same-node B-A-B as the performance authority "
            "instead of repeated local retries"
        )
    if (args.iterations == 0 and
            args.max_progress_baseline_drift >= 0.0 and
            progress_baseline_drift > args.max_progress_baseline_drift):
        raise AssertionError(
            f"progress_baseline_drift={progress_baseline_drift:.6f} exceeds "
            f"{args.max_progress_baseline_drift:.6f}; "
            "invalid progress-aligned B-A-B baseline; use Frontera "
            "same-node B-A-B as the performance authority instead of "
            "repeated local retries"
        )
    if (args.disable_trace and
            (args.min_final_detached_targets > 0 or
             args.min_final_reattached_targets > 0 or
             args.min_final_revisited_targets > 0) and
            not final_transition["present"]):
        raise AssertionError(
            "missing final transition coverage report in trace-disabled run"
        )
    if (args.disable_trace and
            final_transition["detached_targets"] <
            args.min_final_detached_targets):
        raise AssertionError(
            "final_detached_targets="
            f"{final_transition['detached_targets']} below "
            f"{args.min_final_detached_targets}"
        )
    if (args.disable_trace and
            final_transition["reattached_targets"] <
            args.min_final_reattached_targets):
        raise AssertionError(
            "final_reattached_targets="
            f"{final_transition['reattached_targets']} below "
            f"{args.min_final_reattached_targets}"
        )
    if (args.disable_trace and
            final_transition["revisited_targets"] <
            args.min_final_revisited_targets):
        raise AssertionError(
            "final_revisited_targets="
            f"{final_transition['revisited_targets']} below "
            f"{args.min_final_revisited_targets}"
        )
    if (args.iterations == 0 and
            args.min_progress_average_overhead >= 0.0 and
            progress_average < args.min_progress_average_overhead):
        raise AssertionError(
            f"progress_average_overhead={progress_average:.6f} below "
            f"{args.min_progress_average_overhead:.6f}; coverage is likely too low"
        )
    if (args.iterations == 0 and
            args.max_progress_average_overhead >= 0.0 and
            progress_average > args.max_progress_average_overhead):
        raise AssertionError(
            f"progress_average_overhead={progress_average:.6f} exceeds "
            f"{args.max_progress_average_overhead:.6f}"
        )
    if (args.max_progress_sustained_overhead >= 0.0 and
            progress_sustained_max > args.max_progress_sustained_overhead):
        raise AssertionError(
            "progress_sustained_max_overhead="
            f"{progress_sustained_max:.6f} exceeds "
            f"{args.max_progress_sustained_overhead:.6f}"
        )
    if (coverage["weighted_call_coverage"] <
            args.min_weighted_call_coverage):
        raise AssertionError(
            f"weighted_call_coverage={coverage['weighted_call_coverage']:.6f} "
            f"below {args.min_weighted_call_coverage:.6f}"
        )
    if coverage["phase_target_breadth_min"] < args.min_phase_target_breadth:
        raise AssertionError(
            f"phase_target_breadth_min={coverage['phase_target_breadth_min']:.6f} "
            f"below {args.min_phase_target_breadth:.6f}; "
            f"failures={coverage['phase_target_breadth_failures']}"
        )
    if (revisit["revisit_profile_growth"] <
            args.min_revisit_profile_growth):
        raise AssertionError(
            f"revisit_profile_growth={revisit['revisit_profile_growth']} "
            f"below {args.min_revisit_profile_growth}; "
            f"observed={revisit['revisit_observed']}"
        )
    if (revisit["revisit_target_breadth"] <
            args.min_revisit_target_breadth):
        raise AssertionError(
            f"revisit_target_breadth={revisit['revisit_target_breadth']:.6f} "
            f"below {args.min_revisit_target_breadth:.6f}; "
            f"observed={revisit['revisit_observed']}"
        )
    if (args.max_reattached_without_growth >= 0 and
            revisit["reattached_without_growth"] >
            args.max_reattached_without_growth):
        raise AssertionError(
            "reattached_without_growth="
            f"{revisit['reattached_without_growth']} exceeds "
            f"{args.max_reattached_without_growth}"
        )
    if (args.max_final_profile_denominator_error >= 0.0 and
            denominators["final_profile_denominator_error"] >
            args.max_final_profile_denominator_error):
        raise AssertionError(
            "final_profile_denominator_error="
            f"{denominators['final_profile_denominator_error']:.6f} exceeds "
            f"{args.max_final_profile_denominator_error:.6f}"
        )
    if (args.max_final_profile_denominator_error >= 0.0 and
            denominators["reported_profile_denominator_error"] >
            args.max_final_profile_denominator_error):
        raise AssertionError(
            "reported_profile_denominator_error="
            f"{denominators['reported_profile_denominator_error']:.6f} exceeds "
            f"{args.max_final_profile_denominator_error:.6f}"
        )
    if (args.max_reported_profile_control_denominator_error >= 0.0 and
            denominators["reported_raw_profile_control_denominator_error"] >
            args.max_reported_profile_control_denominator_error):
        raise AssertionError(
            "reported_raw_profile_control_denominator_error="
            f"{denominators['reported_raw_profile_control_denominator_error']:.6f} "
            "exceeds "
            f"{args.max_reported_profile_control_denominator_error:.6f}"
        )
    if (args.max_reported_profile_control_denominator_error >= 0.0 and
            denominators["reported_risk_profile_control_denominator_error"] >
            args.max_reported_profile_control_denominator_error):
        raise AssertionError(
            "reported_risk_profile_control_denominator_error="
            f"{denominators['reported_risk_profile_control_denominator_error']:.6f} "
            "exceeds "
            f"{args.max_reported_profile_control_denominator_error:.6f}"
        )
    if actual_overhead > args.max_overhead_ratio:
        raise AssertionError(
            f"PEAK actual overhead {actual_overhead:.6f} exceeds "
            f"{args.max_overhead_ratio:.6f} "
            f"(metric={actual_overhead_metric}, "
            f"baseline_cps={baseline['calls_per_sec']:.3f}, "
            f"peak_cps={peak['calls_per_sec']:.3f}, "
            f"baseline_drift={baseline_drift:.6f})"
        )
    if actual_overhead < args.min_overhead_ratio:
        raise AssertionError(
            f"PEAK actual overhead {actual_overhead:.6f} below "
            f"{args.min_overhead_ratio:.6f}; "
            f"metric={actual_overhead_metric}; coverage is likely too low"
        )
    if counts["failures"] > args.max_trace_failures:
        raise AssertionError(
            f"trace_failures={counts['failures']} exceeds "
            f"{args.max_trace_failures}"
        )
    if (args.min_final_accounting_ratio >= 0.0 and
            counts["final_accounting_ratio"] < args.min_final_accounting_ratio):
        raise AssertionError(
            f"final_accounting_ratio={counts['final_accounting_ratio']:.6f} "
            f"below {args.min_final_accounting_ratio:.6f}; coverage is likely too low"
        )
    if (args.min_max_accounting_ratio >= 0.0 and
            counts["max_accounting_ratio"] < args.min_max_accounting_ratio):
        raise AssertionError(
            f"max_accounting_ratio={counts['max_accounting_ratio']:.6f} "
            f"below {args.min_max_accounting_ratio:.6f}; accounting did not reach budget"
        )
    if (args.max_stable_accounting_ratio >= 0.0 and
            counts["max_stable_accounting_ratio"] >
            args.max_stable_accounting_ratio):
        raise AssertionError(
            f"max_stable_accounting_ratio="
            f"{counts['max_stable_accounting_ratio']:.6f} exceeds "
            f"{args.max_stable_accounting_ratio:.6f}"
        )
    if (args.max_final_accounting_ratio >= 0.0 and
            final_accounting_ratio > args.max_final_accounting_ratio):
        raise AssertionError(
            f"final_accounting_ratio={final_accounting_ratio:.6f} "
            f"exceeds {args.max_final_accounting_ratio:.6f}"
        )
    if not args.disable_trace:
        if counts["detach"] <= 0:
            raise AssertionError("expected at least one detach success")
        if args.require_reattach and counts["reattach"] <= 0:
            raise AssertionError("expected at least one reattach success")
        if counts["detach"] < args.min_detach_success:
            raise AssertionError(
                f"detach_success={counts['detach']} below "
                f"{args.min_detach_success}"
            )
        if counts["reattach"] < args.min_reattach_success:
            raise AssertionError(
                f"reattach_success={counts['reattach']} below "
                f"{args.min_reattach_success}"
            )
        if len(counts["distinct_detach"]) < args.min_distinct_detach:
            raise AssertionError(
                f"distinct_detach={len(counts['distinct_detach'])} below "
                f"{args.min_distinct_detach}"
            )
        if len(counts["distinct_reattach"]) < args.min_distinct_reattach:
            raise AssertionError(
                f"distinct_reattach={len(counts['distinct_reattach'])} below "
                f"{args.min_distinct_reattach}"
            )
        if (len(counts["distinct_redetach_after_reattach"]) <
                args.min_redetach_after_reattach):
            raise AssertionError(
                "redetach_after_reattach="
                f"{len(counts['distinct_redetach_after_reattach'])} below "
                f"{args.min_redetach_after_reattach}"
            )
        if (args.max_reattach_redetach_seconds >= 0.0 and
                counts["reattach_redetach_intervals"]):
            slowest_redetach = max(counts["reattach_redetach_intervals"])
            if slowest_redetach > args.max_reattach_redetach_seconds:
                raise AssertionError(
                    f"reattach_redetach_slowest={slowest_redetach:.6f} exceeds "
                    f"{args.max_reattach_redetach_seconds:.6f}; hot targets are "
                    "not being detached again after cumulative ratio crosses "
                    "the target"
                )

    print(
        "milc_like_ab_check_ok "
        f"baseline_before_cps={baseline_before['calls_per_sec']:.3f} "
        f"baseline_after_cps={baseline_after['calls_per_sec']:.3f} "
        f"baseline_drift={baseline_drift:.6f} "
        f"baseline_elapsed={baseline['elapsed']:.9f} "
        f"peak_elapsed={peak['elapsed']:.9f} "
        f"whole_run_overhead={whole_run_overhead:.6f} "
        f"whole_run_metric={whole_run_metric} "
        f"actual_overhead={actual_overhead:.6f} "
        f"actual_overhead_metric={actual_overhead_metric} "
        "progress_average_overhead="
        f"{progress_average:.6f} "
        "progress_max_overhead="
        f"{progress_max:.6f} "
        "progress_baseline_drift="
        f"{progress_baseline_drift:.6f} "
        "window_average_overhead="
        f"{window_summary['average_actual_overhead'] if window_summary else whole_run_overhead:.6f} "
        "window_max_overhead="
        f"{window_summary['max_actual_overhead'] if window_summary else whole_run_overhead:.6f} "
        f"detach_success={counts['detach']} "
        f"reattach_success={counts['reattach']} "
        f"distinct_detach={len(counts['distinct_detach'])} "
        f"distinct_reattach={len(counts['distinct_reattach'])} "
        f"final_detached_targets={final_transition['detached_targets']} "
        f"final_reattached_targets={final_transition['reattached_targets']} "
        f"final_revisited_targets={final_transition['revisited_targets']} "
        "redetach_after_reattach="
        f"{len(counts['distinct_redetach_after_reattach'])} "
        f"revisit_profile_growth={revisit['revisit_profile_growth']} "
        f"revisit_target_breadth={revisit['revisit_target_breadth']:.6f} "
        f"revisit_observed={revisit['revisit_observed']} "
        f"reattached_without_growth={revisit['reattached_without_growth']} "
        f"trace_failures={counts['failures']} "
        "heartbeat_wall_signal_max="
        f"{counts['heartbeat_wall_signal_max']:.6f} "
        "heartbeat_target_wall_signal_max="
        f"{counts['heartbeat_target_wall_signal_max']:.6f} "
        "weighted_call_coverage="
        f"{coverage['weighted_call_coverage']:.6f} "
        "phase_target_breadth_min="
        f"{coverage['phase_target_breadth_min']:.6f} "
        "phase_target_breadth_avg="
        f"{coverage['phase_target_breadth_avg']:.6f} "
        f"phase_profiled_evidence={coverage['phase_profiled_evidence']} "
        f"unprofiled_targets={coverage['unprofiled_targets']} "
        f"profiled_calls={coverage['profiled_calls']} "
        f"true_calls={coverage['true_calls']} "
        f"max_accounting_ratio={counts['max_accounting_ratio']:.6f} "
        f"max_stable_accounting_ratio={counts['max_stable_accounting_ratio']:.6f} "
        f"final_accounting_ratio={final_accounting_ratio:.6f} "
        "profile_thread_normalized_ratio="
        f"{denominators['profile_thread_normalized_ratio']:.6f} "
        "profile_aggregate_wall_ratio="
        f"{denominators['profile_aggregate_wall_ratio']:.6f} "
        "reported_raw_profile_control_ratio="
        f"{denominators['reported_raw_profile_control_ratio']:.6f} "
        "reported_risk_profile_control_ratio="
        f"{denominators['reported_risk_profile_control_ratio']:.6f} "
        f"reported_risk_local_ranks={denominators['reported_risk_local_ranks']} "
        f"failed_stop_attempts={denominators['failed_stop_attempts']} "
        f"accounting_snapshot_valid={denominators['accounting_snapshot_valid']} "
        "final_profile_denominator_error="
        f"{denominators['final_profile_denominator_error']:.6f} "
        "reported_profile_denominator_error="
        f"{denominators['reported_profile_denominator_error']:.6f} "
        "reported_raw_profile_control_denominator_error="
        f"{denominators['reported_raw_profile_control_denominator_error']:.6f} "
        "reported_risk_profile_control_denominator_error="
        f"{denominators['reported_risk_profile_control_denominator_error']:.6f} "
        f"raw_reported_delta={raw_reported_delta:.6f} "
        f"risk_reported_delta={risk_reported_delta:.6f} "
        f"stats_csv={stats_path} "
        f"baseline_cps={baseline['calls_per_sec']:.3f} "
        f"peak_cps={peak['calls_per_sec']:.3f}"
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
