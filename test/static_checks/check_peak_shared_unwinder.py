#!/usr/bin/env python3
"""Reject a private canonical unwind runtime in the PEAK shared library."""

import argparse
import re
import subprocess
import sys


CANONICAL_UNWINDER = re.compile(
    r"^(?:_Unwind_|__gcc_personality_v0$|"
    r"__(?:de)?register_frame)"
)


def readelf_output(readelf, *arguments):
    completed = subprocess.run(
        [readelf, *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"readelf failed: {detail}")
    return completed.stdout


def defined_unwind_symbols(symbol_table):
    defined = []
    for line in symbol_table.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        section = fields[6]
        symbol = fields[7].split("@", 1)[0]
        if section != "UND" and CANONICAL_UNWINDER.match(symbol):
            defined.append(symbol)
    return sorted(set(defined))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--compiler-id", required=True)
    arguments = parser.parse_args()

    try:
        symbols = readelf_output(
            arguments.readelf, "--syms", "--wide", arguments.library
        )
        defined = defined_unwind_symbols(symbols)
        if defined:
            raise RuntimeError(
                "shared library defines canonical unwind runtime symbols: "
                + ", ".join(defined)
            )

        dynamic = readelf_output(
            arguments.readelf, "--dynamic", "--wide", arguments.library
        )
        if arguments.compiler_id == "GNU" and not re.search(
            r"Shared library: \[libgcc_s\.so(?:\.[^\]]+)?\]", dynamic
        ):
            raise RuntimeError(
                "GNU build does not directly depend on the shared libgcc unwinder"
            )
    except RuntimeError as error:
        print(f"peak_shared_unwinder_error: {error}", file=sys.stderr)
        return 1

    print("peak_shared_unwinder_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
