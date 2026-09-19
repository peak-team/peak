#!/usr/bin/env python3
"""Guard Linux entropy and constructor/source-order behavior."""

import argparse
import pathlib
import re
import subprocess


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("repo_root", type=pathlib.Path)
    parser.add_argument("--library", type=pathlib.Path, required=True)
    parser.add_argument("--nm", required=True)
    args = parser.parse_args()

    for relative in (
        "src/general_listener/output_identity.c",
        "src/general_listener/socket_report_transport.c",
    ):
        source = (args.repo_root / relative).read_text(encoding="utf-8")
        require(
            '#include "internal/exec_raw_syscall.h"' in source,
            f"{relative} must use PEAK's raw syscall primitive for entropy",
        )
        require(
            "#include <sys/syscall.h>" in source,
            f"{relative} must include Linux syscall numbers",
        )
        require(
            "#if defined(__linux__) && defined(SYS_getrandom)\n" in source,
            f"{relative} must guard the getrandom syscall for old kernel headers",
        )
        require(
            "peak_exec_raw_syscall6(" in source,
            f"{relative} must issue getrandom without a libc wrapper",
        )
        require(
            re.search(r"\bgetrandom\s*\(", source) is None,
            f"{relative} must not call libc getrandom()",
        )
        require(
            "PEAK_HAVE_SYS_RANDOM_H" not in source,
            f"{relative} must not restore the libc-header getrandom probe",
        )

    root_cmake = (args.repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    require(
        "PEAK_HAVE_SYS_RANDOM_H" not in root_cmake,
        "root CMake must not gate runtime compatibility on sys/random.h",
    )

    cmake = (args.repo_root / "src/CMakeLists.txt").read_text(encoding="utf-8")
    require(
        re.search(
            r"if\(CMAKE_SYSTEM_NAME MATCHES \"Linux\"\)\s*\n"
            r"\s*set\(PEAK_SIGNAL_POLICY_SOURCE signal_policy\.c\)",
            cmake,
        ),
        "Linux must select signal_policy.c",
    )
    require(
        re.search(
            r"peak\.c\s*\n"
            r"\s*\$\{PEAK_DETACH_CONTROLLER_SOURCE\}\s*\n"
            r"\s*jit_provider\.c\s*\n"
            r"\s*\$\{PEAK_SIGNAL_POLICY_SOURCE\}\s*\n"
            r"\s*pthread_listener\.c",
            cmake,
        ),
        "signal policy source must stay between jit_provider and pthread_listener",
    )

    malloc_header = (
        args.repo_root / "include/malloc_interceptor.h"
    ).read_text(encoding="utf-8")
    require(
        "#if defined(__linux__)\n#include <linux/limits.h>\n#else\n"
        "#include <limits.h>\n#endif" in malloc_header,
        "Linux must retain its original linux/limits.h include",
    )

    symbols = subprocess.run(
        [args.nm, "-D", str(args.library)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout
    require(
        re.search(r"\bU\s+getrandom(?:@|\s|$)", symbols) is None,
        "built Linux libpeak must not depend on libc getrandom",
    )

    print("linux_behavior_contract_ok")


if __name__ == "__main__":
    main()
