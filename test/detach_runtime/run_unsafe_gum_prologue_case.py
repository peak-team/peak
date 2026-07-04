#!/usr/bin/env python3

import argparse
import csv
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def line_mentions_peak_policy_skip_for_target(stderr, target):
    for line in stderr.splitlines():
        if "skipping Gum attach" in line and target in line:
            return True
    return False


def line_mentions_gum_attach_failure_for_target(stderr, target):
    for line in stderr.splitlines():
        if (("Gum initial attach failed" in line or
             "skipping initial Gum attach" in line or
             "skipping dynamic Gum attach" in line or
             "skipping JIT attach" in line) and target in line):
            return True
    return False


def line_mentions_profile_row_for_target(output, target):
    for line in output.splitlines():
        if line.startswith("|") and target in line:
            return True
    return False


def stats_file_profiles_target(stats_dir, target):
    for stats_file in Path(stats_dir).glob("peak-stats-p*.csv"):
        try:
            with stats_file.open(newline="", encoding="utf-8",
                                 errors="replace") as handle:
                for row in csv.DictReader(handle):
                    if row.get("function") != target:
                        continue
                    try:
                        if int(row.get("count") or "0") > 0:
                            return True
                    except ValueError:
                        continue
        except OSError:
            continue
    return False


def describe_stats_dir(stats_dir):
    lines = []
    stats_files = sorted(Path(stats_dir).glob("peak-stats-p*.csv"))
    if not stats_files:
        return "stats_files=<none>"
    for stats_file in stats_files:
        lines.append(f"stats_file={stats_file.name}")
        try:
            with stats_file.open(newline="", encoding="utf-8",
                                 errors="replace") as handle:
                for index, row in enumerate(csv.DictReader(handle)):
                    if index >= 20:
                        lines.append("  ...")
                        break
                    lines.append(
                        f"  function={row.get('function', '')} "
                        f"count={row.get('count', '')}"
                    )
        except OSError as exc:
            lines.append(f"  read_error={exc}")
    return "\n".join(lines)


def run_case(args):
    env = os.environ.copy()
    env["LD_PRELOAD"] = args.libpeak
    env["PEAK_TARGET"] = args.target
    env["PEAK_ENABLE_PER_TARGET_HEARTBEAT"] = "0"
    env["PEAK_ENABLE_GLOBAL_HEARTBEAT"] = "0"
    env["PEAK_HEARTBEAT_INTERVAL"] = "0"
    env["PEAK_TEXT_OUTPUT"] = "0"

    if args.mode == "override-exercises":
        env["PEAK_ALLOW_UNSAFE_GUM_PROLOGUE"] = "1"
    else:
        env.pop("PEAK_ALLOW_UNSAFE_GUM_PROLOGUE", None)

    with tempfile.TemporaryDirectory(
            prefix=f"peak-unsafe-{args.case}-") as stats_dir:
        env["PEAK_STATSLOG_PATH"] = str(Path(stats_dir) / "peak-stats")

        proc = subprocess.run(
            [args.exe, "--case", args.case],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
        )
        combined = proc.stdout + proc.stderr
        csv_profile_proven = stats_file_profiles_target(stats_dir, args.target)
        text_profile_proven = line_mentions_profile_row_for_target(
            combined, args.target
        )

        if args.mode == "guard":
            expected_ok = f"unsafe_gum_prologue_guard_ok:{args.case}"
            if proc.returncode != 0:
                raise AssertionError(
                    f"guarded run for {args.case} exited {proc.returncode}\n"
                    f"{combined}"
                )
            if expected_ok not in proc.stdout:
                raise AssertionError(
                    f"guarded run for {args.case} did not print "
                    f"{expected_ok!r}\n{combined}"
                )
            mentions_skip = line_mentions_peak_policy_skip_for_target(
                proc.stderr, args.target)
            if not mentions_skip:
                raise AssertionError(
                    f"guarded run for {args.case} did not prove PEAK skipped "
                    f"{args.target}\n{combined}"
                )
            print(f"unsafe_gum_prologue_guard_check_ok case={args.case}")
            return 0

        if args.mode == "default-allows" or args.mode == "no-policy-skip":
            expected_ok = f"unsafe_gum_prologue_guard_ok:{args.case}"
            if proc.returncode != 0:
                raise AssertionError(
                    f"{args.mode} run for {args.case} exited "
                    f"{proc.returncode}\n{combined}"
                )
            if expected_ok not in proc.stdout:
                raise AssertionError(
                    f"{args.mode} run for {args.case} did not print "
                    f"{expected_ok!r}\n{combined}"
                )
            if line_mentions_peak_policy_skip_for_target(proc.stderr,
                                                         args.target):
                raise AssertionError(
                    f"{args.mode} run for {args.case} unexpectedly skipped "
                    f"{args.target}\n{combined}"
                )
            if args.mode == "no-policy-skip":
                print(f"unsafe_gum_prologue_no_policy_skip_ok case={args.case}")
                return 0
            if line_mentions_gum_attach_failure_for_target(proc.stderr,
                                                           args.target):
                raise AssertionError(
                    f"default run for {args.case} did not attach "
                    f"{args.target}\n{combined}"
                )
            if not csv_profile_proven:
                raise AssertionError(
                    f"default run for {args.case} did not prove PEAK profiled "
                    f"{args.target} through PEAK_STATSLOG_PATH CSV "
                    f"(text_row={int(text_profile_proven)})\n"
                    f"{describe_stats_dir(stats_dir)}\n{combined}"
                )
            print(f"unsafe_gum_prologue_default_allows_ok case={args.case}")
            return 0

        expected_mismatch = f"unsafe_prologue_mismatch case={args.case}"
        if proc.returncode == 0:
            expected_ok = f"unsafe_gum_prologue_guard_ok:{args.case}"
            if expected_ok not in proc.stdout:
                raise AssertionError(
                    f"override run for {args.case} preserved semantics without "
                    f"printing {expected_ok!r}\n{combined}"
                )
            if line_mentions_peak_policy_skip_for_target(proc.stderr,
                                                         args.target):
                raise AssertionError(
                    f"override run for {args.case} unexpectedly policy-skipped "
                    f"{args.target}\n{combined}"
                )
            if line_mentions_gum_attach_failure_for_target(proc.stderr,
                                                           args.target):
                raise AssertionError(
                    f"override run for {args.case} did not attach "
                    f"{args.target}\n{combined}"
                )
            if not csv_profile_proven:
                raise AssertionError(
                    f"override run for {args.case} preserved semantics but did "
                    f"not prove PEAK profiled {args.target} through "
                    f"PEAK_STATSLOG_PATH CSV "
                    f"(text_row={int(text_profile_proven)})\n"
                    f"{describe_stats_dir(stats_dir)}\n{combined}"
                )
            print(
                f"unsafe_gum_prologue_override_exercises_ok case={args.case} "
                "preserved=1"
            )
            return 0
        if proc.returncode < 0:
            print(
                f"unsafe_gum_prologue_override_exercises_ok case={args.case} "
                f"signal={-proc.returncode}"
            )
            return 0
        if args.expected_byte is not None:
            expected_byte = f"byte={args.expected_byte}"
            if (expected_mismatch not in proc.stderr or
                    expected_byte not in proc.stderr):
                raise AssertionError(
                    f"override run for {args.case} did not show expected "
                    f"corruption {expected_mismatch!r} and "
                    f"{expected_byte!r}\n{combined}"
                )
        elif expected_mismatch not in proc.stderr:
            raise AssertionError(
                f"override run for {args.case} failed, but not with the "
                f"expected mismatch\n{combined}"
            )
        print(
            f"unsafe_gum_prologue_override_exercises_ok case={args.case} "
            f"rc={proc.returncode}"
        )
        return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument(
        "--mode",
        choices=("guard", "default-allows", "no-policy-skip",
                 "override-exercises"),
        required=True,
    )
    parser.add_argument("--expected-byte", type=int)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    try:
        return run_case(args)
    except subprocess.TimeoutExpired as exc:
        print(f"unsafe_gum_prologue_check_timeout case={args.case}: {exc}",
              file=sys.stderr)
        return 124
    except AssertionError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
