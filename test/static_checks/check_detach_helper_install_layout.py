#!/usr/bin/env python3
"""Keep the installed helper destination and compiled fallback aligned."""

from pathlib import Path
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: check_detach_helper_install_layout.py <source-root>")

    cmake = (Path(sys.argv[1]) / "src/CMakeLists.txt").read_text()
    if 'PEAK_INSTALL_DETACH_HELPER_PATH=\\"${CMAKE_INSTALL_FULL_BINDIR}/peak_detach_helper\\"' not in cmake:
        raise AssertionError("compiled helper path must use CMAKE_INSTALL_FULL_BINDIR")
    if "install(TARGETS peak_detach_helper DESTINATION ${CMAKE_INSTALL_BINDIR})" not in cmake:
        raise AssertionError("helper install destination must use CMAKE_INSTALL_BINDIR")
    print("detach_helper_install_layout_ok")


if __name__ == "__main__":
    main()
