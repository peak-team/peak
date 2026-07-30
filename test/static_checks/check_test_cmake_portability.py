#!/usr/bin/env python3
"""Keep optional test dependencies aligned with their build conditions."""

from pathlib import Path
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_test_cmake_portability.py <source-root>")

    root = Path(sys.argv[1])
    runtime = (root / "test/detach_runtime/CMakeLists.txt").read_text()
    controller = (root / "test/detach_controller/CMakeLists.txt").read_text()

    require(
        "target_link_libraries(test_fastpath_thread_exit\n"
        "    PRIVATE Threads::Threads ${DL_LIBRARY})" in runtime,
        "the dlsym-based fastpath exit test must link libdl",
    )
    mpi_guard = "if(PEAK_ENABLE_MPI AND MPI_FOUND)"
    require(
        controller.count(mpi_guard) >= 2
        and f"{mpi_guard}\n    target_sources(test_detach_listener_shutdown" in controller
        and f"{mpi_guard}\n    target_link_libraries(test_detach_listener_shutdown" in controller,
        "MPI transport sources and MPI linkage must share the supported-MPI guard",
    )
    print("test_cmake_portability_ok")


if __name__ == "__main__":
    main()
