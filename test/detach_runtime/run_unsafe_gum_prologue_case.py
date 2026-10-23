#!/usr/bin/env python3

import argparse
import os
import re
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
    if args.mode in ("guard", "override-rejected"):
        # Expected safety-policy skips are INFO diagnostics. Keep the guard
        # tests diagnostic while production's default WARN level stays quiet.
        env["PEAK_VERBOSITY"] = "info"

    if args.mode == "override-rejected":
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

    # These fixtures have a body backedge into the minimum x86 entry patch.
    # Overriding PEAK's heuristic does not override Gum's relocation safety.
    rejected_cases = {
        "x86_dl_inc", "x86_dl_inc_rax_base", "x86_dl_add", "x86_edx_inc",
        "x86_edx_add", "x86_dl_sub_zero", "x86_edx_sub_zero", "x86_dl_mov_zero",
    }
    if args.case not in rejected_cases or args.target != f"peak_unsafe_gum_{args.case}_target":
        raise AssertionError("override-rejected requires a confirmed backedge fixture")
    expected_ok = f"unsafe_gum_prologue_guard_ok:{args.case}"
    if proc.returncode != 0 or expected_ok not in proc.stdout:
        raise AssertionError(f"rejected target changed original behavior\n{combined}")
    if "unsafe_gum_policy_override_enabled" not in proc.stdout:
        raise AssertionError(f"fixture did not observe the PEAK policy override\n{combined}")
    if line_mentions_peak_policy_skip_for_target(proc.stderr, args.target):
        raise AssertionError(f"PEAK policy unexpectedly skipped the target\n{combined}")
    refusal = rf"^\[peak\] Gum initial attach failed for hook 0 \({re.escape(args.target)}\), status=-1$"
    if re.search(refusal, proc.stderr, re.MULTILINE) is None:
        raise AssertionError(f"Gum did not explicitly refuse the unsafe target\n{combined}")
    if line_mentions_profile_row_for_target(combined, args.target):
        raise AssertionError(f"rejected target has a profile row\n{combined}")
    for metric in ("Instrumented targets", "Recorded calls"):
        if re.search(rf"^{metric}: 0$", combined, re.MULTILINE) is None:
            raise AssertionError(f"rejected target has nonzero {metric}\n{combined}")
    print(f"unsafe_gum_prologue_override_rejected_ok case={args.case}")
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
                 "override-rejected"),
        required=True,
    )
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
