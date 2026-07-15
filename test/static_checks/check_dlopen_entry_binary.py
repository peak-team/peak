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

    instructions = []
    for line in body.splitlines():
        instruction = re.match(
            r"^\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{2,8}\s+)+"
            r"([a-zA-Z][a-zA-Z0-9.]*)\b",
            line,
        )
        if instruction is not None:
            instructions.append(
                (int(instruction.group(1), 16),
                 instruction.group(2).lower(),
                 line)
            )
    if not instructions:
        fail("native peak_dlopen contains no parsed instructions", body)

    if architecture == "aarch64":
        ldaxr_index = next(
            (index for index, item in enumerate(instructions)
             if item[1] == "ldaxr"),
            None,
        )
        publication_index = next(
            (index for index, item in enumerate(instructions)
             if item[1] == "stlxr"),
            None,
        )
        retry_index = next(
            (index for index, item in enumerate(instructions)
             if item[1] == "cbnz"),
            None,
        )
        if (ldaxr_index is None or publication_index is None or
                retry_index is None or
                not ldaxr_index < publication_index < retry_index):
            fail("native peak_dlopen lacks the inline LDAXR/STLXR retry loop",
                 body)
        if "peak_dlopen_active_replacement_count" not in body:
            fail("AArch64 entry publication does not target the active counter",
                 body)
        tail_mnemonics = ("b",)
    else:
        publication_index = None
        for index, item in enumerate(instructions):
            combined_lock = re.search(
                r"\block\s+(?:add|inc)[a-z]*\b", item[2].lower())
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
        if "peak_dlopen_active_replacement_count" not in instructions[
                publication_index][2]:
            fail("x86_64 entry publication does not target the active counter",
                 body)
        retry_index = publication_index
        tail_mnemonics = ("jmp", "jmpq")

    for _, mnemonic, line in instructions[:publication_index]:
        if is_control_transfer(architecture, mnemonic):
            fail("native peak_dlopen transfers control before publishing entry "
                 "ownership: " + line.strip(), body)
    for _, mnemonic, line in instructions:
        if is_call_mnemonic(architecture, mnemonic):
            fail("native peak_dlopen contains a compiler-insertable call: " +
                 line.strip(), body)

    tail_index = next(
        (index for index, item in enumerate(instructions[retry_index + 1:],
                                           retry_index + 1)
         if item[1] in tail_mnemonics and
         "<peak_dlopen_body>" in item[2]),
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
