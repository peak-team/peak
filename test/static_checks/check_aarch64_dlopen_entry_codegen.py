#!/usr/bin/env python3
import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def fail(message, assembly=""):
    detail = f"\n--- generated assembly ---\n{assembly}" if assembly else ""
    raise SystemExit(message + detail)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="peak-aarch64-entry-") as tmp:
        assembly_path = Path(tmp) / "entry.s"
        command = [
            args.compiler,
            "--target=aarch64-linux-gnu",
            "-std=c11",
            "-O2",
            "-S",
            "-nostdinc",
            "-moutline-atomics",
            "-fno-stack-protector",
            "-fno-asynchronous-unwind-tables",
            "-I",
            args.include,
            args.source,
            "-o",
            str(assembly_path),
        ]
        result = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if result.returncode != 0:
            fail("AArch64 entry-accounting cross compilation failed:\n" + result.stdout)
        assembly = assembly_path.read_text(encoding="utf-8")

    match = re.search(
        r"(?ms)^peak_dlopen_entry_codegen_probe:\s*(.*?)"
        r"^\s*\.size\s+peak_dlopen_entry_codegen_probe\b",
        assembly,
    )
    if match is None:
        fail("missing probe body in generated AArch64 assembly", assembly)
    body = match.group(1)

    ldaxr = body.find("ldaxr")
    stlxr = body.find("stlxr")
    cbnz = body.find("cbnz")
    if not (0 <= ldaxr < stlxr < cbnz):
        fail("entry registration is not an inline LDAXR/STLXR retry loop", assembly)
    if re.search(r"(?m)^\s*(?:bl|blr[a-z0-9]*)\s", body):
        fail("entry registration emitted an out-of-line call", assembly)
    if "__aarch64_" in body:
        fail("entry registration references an AArch64 outline-atomic helper", assembly)

    instruction_lines = [
        line
        for line in body.splitlines()
        if re.match(r"^\s*[a-z][a-z0-9.]*\s", line)
    ]
    stlxr_index = next(
        (index for index, line in enumerate(instruction_lines) if "stlxr" in line),
        None,
    )
    if stlxr_index is None or stlxr_index * 4 >= 256:
        fail("entry publication falls outside the 256-byte teardown guard", assembly)

    print("aarch64_dlopen_entry_codegen_ok")


if __name__ == "__main__":
    main()
