#!/usr/bin/env python3
"""Keep MPI report transport on its bounded nonblocking collective path."""

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_mpi_report_transport_nonblocking.py <repo-root>",
              file=sys.stderr)
        return 2

    source = (pathlib.Path(sys.argv[1]).resolve() /
              "src/general_listener/mpi_report_transport.c").read_text(
                  encoding="utf-8")
    if "MPI_Allreduce(" in source:
        print("MPI report transport must not use blocking MPI_Allreduce",
              file=sys.stderr)
        return 1
    if "MPI_Iallreduce(" not in source or "MPI_Test(" not in source:
        print("MPI report transport requires timed nonblocking agreement",
              file=sys.stderr)
        return 1

    print("mpi_report_transport_nonblocking_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
