#!/usr/bin/env python3
import argparse
import subprocess
import tempfile
from pathlib import Path


def run(command, label):
    proc = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"platform selection {label} failed with {proc.returncode}:\n"
            f"{proc.stdout}"
        )
    return proc.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--c-compiler", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--probe-source", required=True)
    args = parser.parse_args()

    # These synthetic system values exercise CMake platform selection only.
    # The portable object is compiled by the configured host compiler.
    platform_shapes = [("Darwin", "x86_64"), ("Linux", "ppc64le")]
    with tempfile.TemporaryDirectory(prefix="peak-exec-platform-probe-") as tmp:
        tmp_path = Path(tmp)
        for system_name, system_processor in platform_shapes:
            label = f"{system_name}/{system_processor}"
            toolchain = tmp_path / f"{system_name}-{system_processor}.cmake"
            build_dir = tmp_path / f"build-{system_name}-{system_processor}"
            toolchain.write_text(
                f'set(CMAKE_SYSTEM_NAME "{system_name}")\n'
                f'set(CMAKE_SYSTEM_PROCESSOR "{system_processor}")\n',
                encoding="utf-8",
            )
            configure_output = run(
                [
                    args.cmake,
                    "-S",
                    str(Path(args.probe_source).resolve()),
                    "-B",
                    str(build_dir),
                    f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                    f"-DPEAK_HOST_C_COMPILER={args.c_compiler}",
                    f"-DPEAK_SOURCE_ROOT={Path(args.source_root).resolve()}",
                ],
                f"configure {label}",
            )
            expected_marker = (
                "peak_exec_platform_selection_configure_ok "
                f"target={label} raw=OFF detach_helper=OFF"
            )
            if expected_marker not in configure_output:
                raise AssertionError(
                    f"platform selection configure marker missing for {label}:\n"
                    + configure_output
                )
            run(
                [
                    args.cmake,
                    "--build",
                    str(build_dir),
                    "--target",
                    "peak_exec_platform_selection_probe",
                ],
                f"build {label}",
            )
            probe_object = build_dir / "portable_probe.o"
            if not probe_object.is_file() or probe_object.stat().st_size == 0:
                raise AssertionError(
                    f"platform selection portable object was not built for {label}"
                )

    print("exec_chain_platform_selection_contract_ok")


if __name__ == "__main__":
    main()
