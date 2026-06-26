#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys


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


def run_case(args):
    env = os.environ.copy()
    env["LD_PRELOAD"] = args.libpeak
    env["PEAK_TARGET"] = args.target
    env["PEAK_ENABLE_PER_TARGET_HEARTBEAT"] = "0"
    env["PEAK_ENABLE_GLOBAL_HEARTBEAT"] = "0"
    env["PEAK_HEARTBEAT_INTERVAL"] = "0"

    if args.mode == "override-corrupts":
        env["PEAK_ALLOW_UNSAFE_GUM_PROLOGUE"] = "1"
    else:
        env.pop("PEAK_ALLOW_UNSAFE_GUM_PROLOGUE", None)

    proc = subprocess.run(
        [args.exe, "--case", args.case],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=args.timeout,
    )
    combined = proc.stdout + proc.stderr

    if args.mode == "guard":
        expected_ok = f"unsafe_gum_prologue_guard_ok:{args.case}"
        if proc.returncode != 0:
            raise AssertionError(
                f"guarded run for {args.case} exited {proc.returncode}\n{combined}"
            )
        if expected_ok not in proc.stdout:
            raise AssertionError(
                f"guarded run for {args.case} did not print {expected_ok!r}\n"
                f"{combined}"
            )
        if not line_mentions_peak_policy_skip_for_target(proc.stderr,
                                                         args.target):
            raise AssertionError(
                f"guarded run for {args.case} did not prove PEAK skipped {args.target}\n"
                f"{combined}"
            )
        print(f"unsafe_gum_prologue_guard_check_ok case={args.case}")
        return 0

    if args.mode == "default-allows" or args.mode == "no-policy-skip":
        expected_ok = f"unsafe_gum_prologue_guard_ok:{args.case}"
        if proc.returncode != 0:
            raise AssertionError(
                f"{args.mode} run for {args.case} exited {proc.returncode}\n"
                f"{combined}"
            )
        if expected_ok not in proc.stdout:
            raise AssertionError(
                f"{args.mode} run for {args.case} did not print {expected_ok!r}\n"
                f"{combined}"
            )
        if line_mentions_peak_policy_skip_for_target(proc.stderr, args.target):
            raise AssertionError(
                f"{args.mode} run for {args.case} unexpectedly skipped {args.target}\n"
                f"{combined}"
            )
        if args.mode == "no-policy-skip":
            print(f"unsafe_gum_prologue_no_policy_skip_ok case={args.case}")
            return 0
        if line_mentions_gum_attach_failure_for_target(proc.stderr, args.target):
            raise AssertionError(
                f"default run for {args.case} did not attach {args.target}\n"
                f"{combined}"
            )
        if not line_mentions_profile_row_for_target(combined, args.target):
            raise AssertionError(
                f"default run for {args.case} did not prove PEAK profiled {args.target}\n"
                f"{combined}"
            )
        print(f"unsafe_gum_prologue_default_allows_ok case={args.case}")
        return 0

    expected_mismatch = f"unsafe_prologue_mismatch case={args.case}"
    if proc.returncode == 0:
        raise AssertionError(
            f"override run for {args.case} unexpectedly preserved semantics\n"
            f"{combined}"
        )
    if proc.returncode < 0:
        print(
            f"unsafe_gum_prologue_override_corrupts_ok case={args.case} "
            f"signal={-proc.returncode}"
        )
        return 0
    if args.expected_byte is not None:
        expected_byte = f"byte={args.expected_byte}"
        if expected_mismatch not in proc.stderr or expected_byte not in proc.stderr:
            raise AssertionError(
                f"override run for {args.case} did not show expected corruption "
                f"{expected_mismatch!r} and {expected_byte!r}\n{combined}"
            )
    elif expected_mismatch not in proc.stderr:
        raise AssertionError(
            f"override run for {args.case} failed, but not with the expected mismatch\n"
            f"{combined}"
        )
    print(
        f"unsafe_gum_prologue_override_corrupts_ok case={args.case} "
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
                 "override-corrupts"),
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
