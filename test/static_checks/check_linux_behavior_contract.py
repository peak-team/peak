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
            re.search(
                r"#if PEAK_HAVE_SYS_RANDOM_H\s*\n"
                r"#include <sys/random\.h>\s*\n#endif",
                source,
            ),
            f"{relative} must include sys/random.h before testing GRND_NONBLOCK",
        )
        require(
            "#if PEAK_HAVE_SYS_RANDOM_H && defined(GRND_NONBLOCK)\n"
            in source,
            f"{relative} getrandom call must retain the GRND_NONBLOCK guard",
        )
        require(
            "#if PEAK_HAVE_SYS_RANDOM_H && defined(GRND_NONBLOCK)\n"
            "#include <sys/random.h>" not in source,
            f"{relative} must not test a header-provided macro before inclusion",
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

    symbols = subprocess.run(
        [args.nm, "-D", str(args.library)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout
    require(
        re.search(r"\bU\s+getrandom(?:@|\s|$)", symbols),
        "built Linux libpeak must retain the getrandom fast path",
    )

    print("linux_behavior_contract_ok")


if __name__ == "__main__":
    main()
