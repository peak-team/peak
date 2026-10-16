#!/usr/bin/env python3
"""Exercise opt-in reattach diagnostics through the real controller API."""

import argparse
import csv
import importlib.util
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path


def load_lifecycle_checker():
    path = Path(__file__).with_name("run_detach_hotloop_trace_check.py")
    spec = importlib.util.spec_from_file_location("hotloop_trace_check", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load lifecycle checker: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_records(path):
    text = path.read_text(encoding="utf-8")
    rows = [line.split() for line in text.splitlines() if line]
    if not rows or rows[0][0] != "RD_BEGIN":
        raise ValueError("missing RD_BEGIN")
    if rows[-1][0] != "RD_END":
        raise ValueError("missing terminal RD_END")
    if sum(row[0] == "RD_BEGIN" for row in rows) != 1 or sum(
        row[0] == "RD_END" for row in rows
    ) != 1:
        raise ValueError("expected exactly one begin and end record")
    records = []
    for row in rows:
        if row[0] not in {"RD_BEGIN", "RD_EVENT", "RD_HOOK", "RD_REASON", "RD_END"}:
            raise ValueError(f"unknown record kind {row[0]!r}")
        fields = {}
        for token in row[1:]:
            if "=" not in token:
                raise ValueError(f"malformed token {token!r}")
            key, value = token.split("=", 1)
            if key in fields:
                raise ValueError(f"duplicate field {key!r}")
            fields[key] = value
        records.append((row[0], fields))
    return records


def run_bounded(command, env, timeout):
    process = subprocess.Popen(
        command,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=timeout)
        return process.returncode, output, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            output, _ = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, _ = process.communicate()
        return process.returncode, output, True


def run_once(args, enabled, preexisting_sidecar=False, fail_first_stop=False):
    checker = load_lifecycle_checker()
    with tempfile.TemporaryDirectory(prefix="peak-reattach-diag-") as tmp:
        root = Path(tmp)
        trace = root / "controller.csv"
        sidecar = Path(str(trace) + ".reattach-diagnostics")
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("PEAK_") and not key.startswith("FAKE_DETACH_HELPER_")
        }
        env.update({
            "LD_PRELOAD": os.path.realpath(args.libpeak),
            "PEAK_DETACH_TRACE_PATH": str(trace),
            "PEAK_TARGET": "peak_detach_hot_target,peak_detach_hot_target_two",
            "PEAK_MAX_NUM_THREADS": "16",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            "PEAK_ENABLE_REATTACH": "0",
            "PEAK_COST": "0",
            "PEAK_HEARTBEAT_INTERVAL": "0",
            "PEAK_HIBERNATION_CYCLE": "1",
            "PEAK_REQUIRE_SAFE_DETACH": "1",
            "PEAK_SAFE_DETACH_MODE": "strict",
            "PEAK_DETACH_BACKEND": "helper",
            "PEAK_DETACH_HELPER": os.path.realpath(args.helper),
            "FAKE_DETACH_HELPER_SCENARIO": (
                "synthetic-stop-file-once" if fail_first_stop else "success-zero"
            ),
        })
        if fail_first_stop:
            env["PEAK_TEST_DIAG_INJECT_FIRST_STOP"] = "1"
            env["FAKE_DETACH_HELPER_STOP_PC_FILE"] = str(root / "stop-pc.txt")
        if enabled:
            env["PEAK_REATTACH_DIAGNOSTICS"] = "1"
        if preexisting_sidecar:
            sidecar.write_text("existing diagnostic destination\n", encoding="utf-8")
            original_sidecar = sidecar.read_bytes()
        rc, output, timed_out = run_bounded(
            [args.exe, "--controller-two-target-lifecycle-check"], env, args.timeout)
        if timed_out:
            print(f"controller lifecycle timed out after {args.timeout}s", file=sys.stderr)
            if output:
                print(output, file=sys.stderr)
            return False

        if rc != 0 or "controller_two_target_lifecycle_ok" not in output:
            print(f"controller lifecycle failed rc={rc}", file=sys.stderr)
            print(output, file=sys.stderr)
            return False
        try:
            with trace.open(newline="", encoding="utf-8") as stream:
                trace_rows = list(csv.reader(stream))
        except OSError as exc:
            print(f"missing lifecycle trace: {exc}", file=sys.stderr)
            return False
        failed_rows = [
            row for row in trace_rows
            if len(row) > 4 and row[2] == "peak_detach_hot_target"
            and row[3] == "detach" and row[4] == "prepare-failed"
        ]
        lifecycle_rows = [row for row in trace_rows if row not in failed_rows]
        if fail_first_stop and len(failed_rows) != 1:
            print(f"expected one injected first-stop failure, found {len(failed_rows)}", file=sys.stderr)
            return False
        if not fail_first_stop and failed_rows:
            print("unexpected failed stop in non-injection lifecycle", file=sys.stderr)
            return False
        ok, reason = checker.controller_two_target_lifecycle_rows_ok(lifecycle_rows, True)
        if not ok:
            print(f"strict lifecycle trace failed: {reason}", file=sys.stderr)
            return False

        if not enabled:
            if sidecar.exists():
                print("diagnostics sidecar exists while opt-in is disabled", file=sys.stderr)
                return False
            print("diagnostics_off_absent lifecycle=ok")
            return True

        if preexisting_sidecar:
            if sidecar.read_bytes() != original_sidecar:
                print("existing diagnostics destination was overwritten", file=sys.stderr)
                return False
            print("diagnostics_existing_destination_preserved lifecycle=ok")
            return True

        if not sidecar.is_file():
            print("opt-in diagnostics sidecar was not created", file=sys.stderr)
            return False
        try:
            records = parse_records(sidecar)
        except (OSError, ValueError) as exc:
            print(f"invalid diagnostics sidecar: {exc}", file=sys.stderr)
            return False
        begin, end = records[0][1], records[-1][1]
        if begin.get("v") != "1" or begin.get("capture") != "init-to-first-controller-stop":
            print("unexpected diagnostics header", file=sys.stderr)
            return False
        if end.get("v") != "1" or end.get("complete") != "1":
            print("diagnostics capture is not complete", file=sys.stderr)
            return False
        if any(end.get(k) != "0" for k in ("busy_drops", "overflow", "out_of_range", "concurrent")):
            print("diagnostics capture reports dropped, overflowed, or concurrent data", file=sys.stderr)
            return False

        events = [fields for kind, fields in records if kind == "RD_EVENT"]
        if not events:
            print("diagnostics capture contains no controller events", file=sys.stderr)
            return False
        try:
            seqs = [int(event["seq"]) for event in events]
            hooks = {int(event["hook"]) for event in events}
        except (KeyError, ValueError):
            print("malformed event sequence or hook id", file=sys.stderr)
            return False
        kinds = {event.get("kind") for event in events}
        if end.get("events") != str(len(events)) or end.get("hooks") != "2":
            print("diagnostics footer counts do not match the records", file=sys.stderr)
            return False
        if seqs != list(range(1, len(events) + 1)) or hooks != {0, 1}:
            print("event sequence or two-hook coverage is incomplete", file=sys.stderr)
            return False
        for hook in (0, 1):
            indexed = [
                (i, event) for i, event in enumerate(events)
                if int(event.get("hook", "-1")) == hook
            ]
            seed = next(((i, e) for i, e in indexed if e.get("kind") == "seed"), None)
            if seed is None or int(seed[1].get("gen", "0")) < 1:
                print(f"hook {hook} is missing a seeded detach generation", file=sys.stderr)
                return False
            accepted = next((
                (i, e) for i, e in indexed
                if e.get("kind") == "request_accept"
                and e.get("operation") == "2"
                and int(e.get("req", "0")) > 0
                and int(e.get("gen", "0")) == int(seed[1]["gen"])
                and i > seed[0]
            ), None)
            if accepted is None:
                print(f"hook {hook} is missing a reattach request linked to its seed", file=sys.stderr)
                return False
            req = accepted[1]["req"]
            gen = accepted[1]["gen"]
            attempt = next((
                (i, e) for i, e in indexed
                if e.get("kind") == "controller_attempt"
                and e.get("operation") == "2" and e.get("req") == req
                and e.get("gen") == gen and i > accepted[0]
            ), None)
            prepared = next((
                (i, e) for i, e in indexed
                if e.get("kind") == "prepare_success"
                and e.get("operation") == "2" and e.get("req") == req
                and e.get("gen") == gen and e.get("state") == "6"
                and i > (attempt[0] if attempt else accepted[0])
            ), None)
            attached = next((
                (i, e) for i, e in indexed
                if e.get("kind") == "state" and e.get("state") == "1"
                and e.get("req") == req
                and e.get("gen") == gen and prepared is not None and i > prepared[0]
            ), None)
            if attempt is None or prepared is None or attached is None:
                print(f"hook {hook} lacks linked attempt/prepare/attached-state events", file=sys.stderr)
                return False
        if fail_first_stop:
            attempts = [e for e in events if
                e.get("hook") == "0" and e.get("operation") == "1"
                and e.get("kind") == "controller_attempt"]
            retry = next((e for e in events if
                e.get("hook") == "0"
                and e.get("kind") == "retry_scheduled"), None)
            failed_prepare = next((e for e in events if
                e.get("hook") == "0" and e.get("operation") == "1"
                and e.get("kind") == "prepare-failed"), None)
            later_prepare = next((e for e in events if
                e.get("hook") == "0" and e.get("operation") == "1"
                and e.get("kind") == "prepare_success"
                and retry is not None and int(e.get("seq", "0")) > int(retry.get("seq", "0"))), None)
            same_request = (
                retry is not None and failed_prepare is not None and attempts
                and all(e.get("req") == attempts[0].get("req")
                        and e.get("gen") == attempts[0].get("gen")
                        for e in (retry, failed_prepare))
            )
            if (retry is None or failed_prepare is None or len(attempts) < 2
                    or not int(attempts[0].get("seq", "0")) < int(retry.get("seq", "0"))
                    < int(failed_prepare.get("seq", "0")) < int(attempts[1].get("seq", "0"))
                    or later_prepare is None):
                print("injected detach failure lacks attempt/retry/failure/success diagnostics", file=sys.stderr)
                return False
            if not same_request:
                print("injected detach retry events changed request/generation identity", file=sys.stderr)
                return False
        hook_summaries = {
            int(fields["hook"]): fields
            for kind, fields in records if kind == "RD_HOOK"
        }
        counts = " ".join(
            f"hook{hook}_gen={hook_summaries[hook].get('gen')}"
            f"_req={hook_summaries[hook].get('req')}"
            for hook in (0, 1)
        )
        print(f"diagnostics_on_complete events={len(events)} {counts}")
        return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--helper", required=True)
    parser.add_argument("--timeout", type=int, default=20)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if not run_once(args, enabled=False):
        return 1
    if not run_once(args, enabled=True):
        return 1
    if not run_once(args, enabled=True, fail_first_stop=True):
        return 1
    if not run_once(args, enabled=True, preexisting_sidecar=True):
        return 1
    print("reattach_diagnostics_lifecycle_ok scenarios=off,complete,one-shot-failure-retry,existing-preserved")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
