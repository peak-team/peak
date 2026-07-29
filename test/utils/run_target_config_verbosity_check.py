#!/usr/bin/env python3
"""Verify malformed target-list warnings honor PEAK_VERBOSITY."""

import argparse
import os
import subprocess
import sys


WARNING = "[peak] warning: ignoring empty target token"


def run_mode(exe: str, library: str, preload_env: str, mode: str, expected: int) -> None:
    env = os.environ.copy()
    existing_preload = env.get(preload_env, "")
    env[preload_env] = library if not existing_preload else f"{library}:{existing_preload}"
    env.update(
        {
            "PEAK_TARGET": ",peak_target_config_missing,,",
            "PEAK_TARGET_FILE": "",
            "PEAK_TARGET_GROUP": "",
            "PEAK_HEARTBEAT_INTERVAL": "0",
            "PEAK_VERBOSITY": mode,
        }
    )
    completed = subprocess.run(
        [exe], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise RuntimeError(f"{mode}: command failed ({completed.returncode})\n{output}")
    actual = output.count(WARNING)
    if actual != expected:
        raise RuntimeError(f"{mode}: warning count {actual}, expected {expected}\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--preload-env", required=True)
    args = parser.parse_args()
    try:
        run_mode(args.exe, args.libpeak, args.preload_env, "quiet", 0)
        run_mode(args.exe, args.libpeak, args.preload_env, "silent", 0)
        run_mode(args.exe, args.libpeak, args.preload_env, "warn", 1)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"target_config_verbosity_check_failed: {error}", file=sys.stderr)
        return 1
    print("target_config_verbosity_check_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
