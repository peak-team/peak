#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--libpeak", required=True)
    args = parser.parse_args()

    timeout = shutil.which("timeout")
    if timeout is None:
        print("filtered_tool_preload_ok timeout_unavailable")
        return 0

    code = (
        "import signal; "
        "signal.signal(signal.SIGRTMAX, signal.SIG_DFL); "
        "print('filtered_tool_preload_ok')"
    )

    env = os.environ.copy()
    env.pop("PEAK_PROFILE_INTERPRETERS", None)
    env.pop("PEAK_JIT_ENABLE", None)
    env.update({
        "LD_PRELOAD": args.libpeak,
        "PEAK_DETACH_SIGNAL": "SIGRTMAX-0",
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_SIGNAL_RESERVE_EARLY": "always",
    })

    with tempfile.NamedTemporaryFile("w", delete=False) as target_file:
        target_file.write("su3_rhmd_hisq\n")
        target_path = target_file.name

    env["PEAK_TARGET_FILE"] = target_path
    try:
        proc = subprocess.run(
            [timeout, "5", sys.executable, "-c", code],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
        )
    finally:
        try:
            os.unlink(target_path)
        except OSError:
            pass

    output = proc.stdout + proc.stderr
    bad_terms = ("PEAK Library", "overhead calibration", "Estimated overhead")
    if proc.returncode != 0 or "filtered_tool_preload_ok" not in proc.stdout:
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        return proc.returncode if proc.returncode != 0 else 1
    for term in bad_terms:
        if term in output:
            sys.stdout.write(proc.stdout)
            sys.stderr.write(proc.stderr)
            return 1

    sys.stdout.write(proc.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
