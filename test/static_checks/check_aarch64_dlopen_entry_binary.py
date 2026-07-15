#!/usr/bin/env python3
import argparse
import re
import subprocess


def fail(message, disassembly=""):
    detail = f"\n--- peak_dlopen disassembly ---\n{disassembly}" if disassembly else ""
    raise SystemExit(message + detail)


def is_call_mnemonic(mnemonic):
    return mnemonic == "bl" or mnemonic.startswith("blr")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--guard-bytes", type=int, default=256)
    args = parser.parse_args()

    result = subprocess.run(
        [args.objdump, "-d", "--disassemble=peak_dlopen", args.binary],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        fail("unable to disassemble native peak_dlopen:\n" + result.stdout)

    match = re.search(
        r"(?ms)^\s*([0-9a-fA-F]+)\s+<peak_dlopen>:\s*(.*?)"
        r"(?=^\s*[0-9a-fA-F]+\s+<[^>]+>:|\Z)",
        result.stdout,
    )
    if match is None:
        fail("native binary does not contain a disassemblable peak_dlopen symbol",
             result.stdout)
    start = int(match.group(1), 16)
    body = match.group(2)

    instructions = []
    for line in body.splitlines():
        instruction = re.match(
            r"^\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{2,8}\s+)+"
            r"([a-zA-Z][a-zA-Z0-9.]*)\b",
            line,
        )
        if instruction is not None:
            instructions.append(
                (int(instruction.group(1), 16), instruction.group(2).lower(), line)
            )

    ldaxr_index = next(
        (index for index, item in enumerate(instructions) if item[1] == "ldaxr"),
        None,
    )
    stlxr_index = next(
        (index for index, item in enumerate(instructions) if item[1] == "stlxr"),
        None,
    )
    cbnz_index = next(
        (index for index, item in enumerate(instructions) if item[1] == "cbnz"),
        None,
    )
    if (ldaxr_index is None or stlxr_index is None or cbnz_index is None or
            not ldaxr_index < stlxr_index < cbnz_index):
        fail("native peak_dlopen lacks the inline LDAXR/STLXR retry loop", body)

    publication_address = instructions[stlxr_index][0]
    if publication_address - start >= args.guard_bytes:
        fail("native entry publication lies outside the teardown PC guard", body)
    for _, mnemonic, line in instructions[:stlxr_index]:
        if is_call_mnemonic(mnemonic):
            fail("native peak_dlopen calls out before publishing entry ownership: " +
                 line.strip(), body)

    print(
        "aarch64_dlopen_entry_binary_ok "
        f"publication_offset={publication_address - start}"
    )


if __name__ == "__main__":
    main()
