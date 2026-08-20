#!/usr/bin/env python3
"""Compare descriptor-2 semantics with and without PEAK preloaded."""

import argparse
import os
import pathlib
import subprocess
import tempfile


MARKER = b"application reused descriptor 2\n"


def run_case(executable, libpeak, mode, profiled, fail_dup=False):
    with tempfile.TemporaryDirectory(
        prefix=f"peak-close-{mode}-", dir="/tmp"
    ) as directory:
        output = pathlib.Path(directory) / "descriptor-2.out"
        stats = pathlib.Path(directory) / "stats.csv"
        env = os.environ.copy()
        env.pop("LD_PRELOAD", None)
        for name in list(env):
            if name.startswith("PEAK_"):
                env.pop(name)
        if profiled:
            env.update({
                "LD_PRELOAD": str(libpeak),
                "PEAK_TARGET": "peak_close_semantics_target",
                "PEAK_HEARTBEAT_INTERVAL": "0",
                "PEAK_STATSLOG_TEMPLATE": str(stats),
                "PEAK_VERBOSITY": "quiet",
            })
            if fail_dup:
                env["PEAK_TEST_FAIL_LOG_DESCRIPTOR_DUP"] = "1"
        result = subprocess.run(
            [str(executable), mode, str(output)],
            env=env,
            cwd=directory,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"{mode} profiled={profiled} fail_dup={fail_dup} "
                f"returned {result.returncode}: stdout={result.stdout!r} "
                f"stderr={result.stderr!r}"
            )
        if output.read_bytes() != MARKER:
            raise AssertionError(
                f"{mode} profiled={profiled} fail_dup={fail_dup} "
                "did not preserve descriptor-2 reuse"
            )
        csv_files = list(pathlib.Path(directory).glob("*.csv"))
        if profiled:
            if len(csv_files) != 1:
                raise AssertionError(
                    f"{mode} fail_dup={fail_dup} expected one profile CSV, "
                    f"found {csv_files!r}"
                )
            csv_content = csv_files[0].read_text(encoding="utf-8")
            if "peak_close_semantics_target" not in csv_content:
                raise AssertionError("profile CSV omitted the exercised target")
        elif csv_files:
            raise AssertionError(f"baseline unexpectedly wrote CSVs: {csv_files!r}")
        return result.stdout, result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=pathlib.Path)
    parser.add_argument("--libpeak", required=True, type=pathlib.Path)
    args = parser.parse_args()

    for mode in ("close", "fclose", "raw"):
        baseline_stdout, baseline_stderr = run_case(
            args.exe, args.libpeak, mode, False
        )
        profiled_stdout, profiled_stderr = run_case(
            args.exe, args.libpeak, mode, True
        )
        if baseline_stdout != profiled_stdout:
            raise AssertionError(
                f"{mode} descriptor behavior differs under PEAK: "
                f"baseline={baseline_stdout!r} profiled={profiled_stdout!r}"
            )
        if baseline_stderr:
            raise AssertionError(f"{mode} baseline wrote stderr: {baseline_stderr!r}")
        if b"PEAK done with:" not in profiled_stderr:
            raise AssertionError(
                f"{mode} lost the final PEAK report after descriptor 2 closed"
            )

    baseline_stdout, _ = run_case(args.exe, args.libpeak, "close", False)
    failed_stdout, failed_stderr = run_case(
        args.exe, args.libpeak, "close", True, fail_dup=True
    )
    if failed_stdout != baseline_stdout:
        raise AssertionError("descriptor duplication failure changed application behavior")
    warning = b"unable to duplicate the report descriptor"
    if failed_stderr.count(warning) != 1:
        raise AssertionError(
            f"expected one duplication warning, got {failed_stderr!r}"
        )
    if b"PEAK done with:" in failed_stderr:
        raise AssertionError("disabled logging path emitted a final text report")

    print("close_semantics_ok")


if __name__ == "__main__":
    main()
