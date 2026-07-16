#!/usr/bin/env python3
import argparse
import re
import subprocess


def fail(message, disassembly=""):
    detail = f"\n--- peak_dlopen disassembly ---\n{disassembly}" if disassembly else ""
    raise SystemExit(message + detail)


def normalize_architecture(value):
    architecture = value.lower()
    if architecture in ("x86_64", "amd64"):
        return "x86_64"
    if architecture in ("aarch64", "arm64"):
        return "aarch64"
    fail(f"unsupported entry-stub architecture: {value}")


def disassemble_symbol(objdump, binary, symbol):
    attempts = (
        [objdump, "-d", f"--disassemble={symbol}", binary],
        [objdump, "-d", f"--disassemble-symbols={symbol}", binary],
    )
    outputs = []
    result = None
    for command in attempts:
        result = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        outputs.append(result.stdout)
        if result.returncode == 0:
            break
    if result is None or result.returncode != 0:
        fail(f"unable to disassemble native {symbol}:\n" + "\n".join(outputs))
    match = re.search(
        rf"(?ms)^\s*([0-9a-fA-F]+)\s+<{re.escape(symbol)}>:\s*(.*?)"
        r"(?=^\s*[0-9a-fA-F]+\s+<[^>]+>:\s*|\Z)",
        result.stdout,
    )
    if match is None:
        fail(f"native binary does not contain a disassemblable {symbol} symbol",
             result.stdout)
    return int(match.group(1), 16), match.group(2)


def find_symbol_address(objdump, binary, symbol):
    attempts = (
        [objdump, "-t", binary],
        [objdump, "--syms", binary],
    )
    outputs = []
    for command in attempts:
        result = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        outputs.append(result.stdout)
        if result.returncode != 0:
            continue
        addresses = set()
        for line in result.stdout.splitlines():
            if not re.search(rf"\b{re.escape(symbol)}\s*$", line):
                continue
            match = re.match(r"^\s*([0-9a-fA-F]+)\s+", line)
            if match is not None and "*UND*" not in line:
                addresses.add(int(match.group(1), 16))
        if len(addresses) == 1:
            return addresses.pop()
        if len(addresses) > 1:
            fail(f"native binary contains multiple addresses for {symbol}")
    fail(f"unable to locate {symbol} in the native symbol table:\n" +
         "\n".join(outputs))


def parse_instructions(body):
    instructions = []
    for line in body.splitlines():
        instruction = re.match(
            r"^\s*([0-9a-fA-F]+):\s+"
            r"(?:(?:[0-9a-fA-F]{2}\s+)+|[0-9a-fA-F]{8}\s+)"
            r"([a-zA-Z][a-zA-Z0-9.]*)\b\s*(.*)$",
            line,
        )
        if instruction is not None:
            instructions.append(
                (int(instruction.group(1), 16),
                 instruction.group(2).lower(),
                 instruction.group(3).strip(),
                 line)
            )
    return instructions


def parse_aarch64_number(token, address=False):
    value = token.strip().lstrip("#")
    if value.lower().startswith("0x"):
        return int(value, 16)
    if address or re.search(r"[a-fA-F]", value):
        return int(value, 16)
    return int(value, 10)


def validate_aarch64_publication(instructions, counter_address, body):
    ldaxr_index = next(
        (index for index, item in enumerate(instructions)
         if item[1] == "ldaxr"),
        None,
    )
    if ldaxr_index is None or ldaxr_index < 2:
        fail("native peak_dlopen lacks the inline LDAXR/STLXR retry loop",
             body)

    adrp_index = ldaxr_index - 2
    address_add_index = ldaxr_index - 1
    increment_index = ldaxr_index + 1
    publication_index = ldaxr_index + 2
    retry_index = ldaxr_index + 3
    if retry_index >= len(instructions):
        fail("native peak_dlopen has a truncated LDAXR/STLXR retry loop", body)
    expected_mnemonics = ("adrp", "add", "ldaxr", "add", "stlxr", "cbnz")
    actual_mnemonics = tuple(
        item[1] for item in instructions[adrp_index:retry_index + 1])
    if actual_mnemonics != expected_mnemonics:
        fail("native peak_dlopen lacks the exact inline LDAXR/ADD/STLXR "
             "retry sequence", body)

    adrp = re.fullmatch(
        r"(x[0-9]+),\s*#?(0x[0-9a-fA-F]+|[0-9a-fA-F]+)"
        r"(?:\s+<[^>]+>)?",
        instructions[adrp_index][2],
    )
    address_add = re.fullmatch(
        r"(x[0-9]+),\s*(x[0-9]+),\s*#(0x[0-9a-fA-F]+|[0-9]+)",
        instructions[address_add_index][2],
    )
    load = re.fullmatch(
        r"(w[0-9]+),\s*\[\s*(x[0-9]+)\s*\]",
        instructions[ldaxr_index][2],
    )
    increment = re.fullmatch(
        r"(w[0-9]+),\s*(w[0-9]+),\s*#(0x[0-9a-fA-F]+|[0-9]+)",
        instructions[increment_index][2],
    )
    store = re.fullmatch(
        r"(w[0-9]+),\s*(w[0-9]+),\s*\[\s*(x[0-9]+)\s*\]",
        instructions[publication_index][2],
    )
    retry = re.fullmatch(
        r"(w[0-9]+),\s*#?(0x[0-9a-fA-F]+|[0-9a-fA-F]+)"
        r"(?:\s+<[^>]+>)?",
        instructions[retry_index][2],
    )
    if None in (adrp, address_add, load, increment, store, retry):
        fail("native peak_dlopen has unrecognized AArch64 entry operands", body)

    address_register = adrp.group(1)
    value_register = load.group(1)
    status_register = store.group(1)
    if not (
        address_add.group(1) == address_register and
        address_add.group(2) == address_register and
        load.group(2) == address_register and
        increment.group(1) == value_register and
        increment.group(2) == value_register and
        store.group(2) == value_register and
        store.group(3) == address_register and
        retry.group(1) == status_register
    ):
        fail("native peak_dlopen changes registers within the AArch64 "
             "entry-publication loop", body)
    if parse_aarch64_number(increment.group(3)) != 1:
        fail("native peak_dlopen does not increment the active count by one",
             body)

    adrp_target = parse_aarch64_number(adrp.group(2), address=True)
    low_offset = parse_aarch64_number(address_add.group(3))
    if adrp_target + low_offset != counter_address:
        fail("AArch64 entry publication does not target the active counter",
             body)
    retry_target = parse_aarch64_number(retry.group(2), address=True)
    if retry_target != instructions[ldaxr_index][0]:
        fail("native peak_dlopen does not retry from its LDAXR instruction",
             body)

    return publication_index, retry_index


def is_call_mnemonic(architecture, mnemonic):
    if architecture == "aarch64":
        return mnemonic == "bl" or mnemonic.startswith("blr")
    return mnemonic.startswith("call") or mnemonic.startswith("lcall")


def is_control_transfer(architecture, mnemonic):
    if is_call_mnemonic(architecture, mnemonic):
        return True
    if architecture == "aarch64":
        if mnemonic == "bti":
            return False
        return (mnemonic in ("b", "br", "ret") or
                mnemonic.startswith(("b.", "cb", "tb")))
    return mnemonic.startswith(("j", "loop", "ret"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--guard-bytes", type=int, default=256)
    parser.add_argument("--require-body-instrumentation", action="store_true")
    args = parser.parse_args()
    architecture = normalize_architecture(args.architecture)

    start, body = disassemble_symbol(
        args.objdump, args.binary, "peak_dlopen")

    instructions = parse_instructions(body)
    if not instructions:
        fail("native peak_dlopen contains no parsed instructions", body)

    if architecture == "aarch64":
        counter_address = find_symbol_address(
            args.objdump,
            args.binary,
            "peak_dlopen_active_replacement_count",
        )
        publication_index, retry_index = validate_aarch64_publication(
            instructions, counter_address, body)
        tail_mnemonics = ("b",)
    else:
        publication_index = None
        for index, item in enumerate(instructions):
            combined_lock = re.search(
                r"\block\s+(?:add|inc)[a-z]*\b", item[3].lower())
            split_lock = (
                index > 0 and instructions[index - 1][1] == "lock" and
                item[1].startswith(("add", "inc"))
            )
            if combined_lock or split_lock:
                publication_index = index
                break
        if publication_index is None:
            fail("native peak_dlopen lacks an inline lock add/inc publication",
                 body)
        publication_operation = (
            instructions[publication_index][1] + " " +
            instructions[publication_index][2]
        ).lower()
        if not (
            re.search(r"\binc[a-z]*\s+", publication_operation) or
            re.search(r"\badd[a-z]*\s+\$(?:0x)?0*1\s*,",
                      publication_operation)
        ):
            fail("native peak_dlopen does not increment the active count by "
                 "one", body)
        if "peak_dlopen_active_replacement_count" not in instructions[
                publication_index][3]:
            fail("x86_64 entry publication does not target the active counter",
                 body)
        retry_index = publication_index
        tail_mnemonics = ("jmp", "jmpq")

    for _, mnemonic, _, line in instructions[:publication_index]:
        if is_control_transfer(architecture, mnemonic):
            fail("native peak_dlopen transfers control before publishing entry "
                 "ownership: " + line.strip(), body)
    for _, mnemonic, _, line in instructions:
        if is_call_mnemonic(architecture, mnemonic):
            fail("native peak_dlopen contains a compiler-insertable call: " +
                 line.strip(), body)

    tail_index = next(
        (index for index, item in enumerate(instructions[retry_index + 1:],
                                           retry_index + 1)
         if item[1] in tail_mnemonics and
         "<peak_dlopen_body>" in item[3]),
        None,
    )
    if tail_index is None:
        fail("native peak_dlopen does not tail-branch directly to "
             "peak_dlopen_body", body)
    first_post_publication_transfer = next(
        (index for index, item in enumerate(instructions[retry_index + 1:],
                                           retry_index + 1)
         if is_control_transfer(architecture, item[1])),
        None,
    )
    if first_post_publication_transfer != tail_index:
        fail("native peak_dlopen does not transfer directly from entry "
             "publication to peak_dlopen_body", body)

    publication_address = instructions[publication_index][0]
    tail_address = instructions[tail_index][0]
    if publication_address - start >= args.guard_bytes:
        fail("native entry publication lies outside the teardown PC guard", body)
    if tail_address - start >= args.guard_bytes:
        fail("native entry stub is not contained by the teardown PC guard", body)

    instrumentation = "not-requested"
    if args.require_body_instrumentation:
        _, instrumented_body = disassemble_symbol(
            args.objdump, args.binary, "peak_dlopen_body")
        helpers = ("__sanitizer_cov_trace_pc", "__cyg_profile_func_enter")
        if not any(helper in instrumented_body for helper in helpers):
            fail("hostile fixture body was not instrumented; entry-stub test "
                 "would be vacuous", instrumented_body)
        instrumentation = "present"

    print(
        "dlopen_entry_binary_ok "
        f"architecture={architecture} "
        f"publication_offset={publication_address - start} "
        f"tail_offset={tail_address - start} "
        f"body_instrumentation={instrumentation}"
    )


if __name__ == "__main__":
    main()
