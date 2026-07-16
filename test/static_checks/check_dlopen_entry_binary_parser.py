#!/usr/bin/env python3
import importlib.util
from pathlib import Path


def load_checker():
    checker_path = Path(__file__).with_name("check_dlopen_entry_binary.py")
    spec = importlib.util.spec_from_file_location(
        "check_dlopen_entry_binary", checker_path)
    if spec is None or spec.loader is None:
        raise SystemExit("unable to load dlopen entry binary checker")
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    return checker


def expect_rejection(checker, body, message):
    try:
        checker.validate_aarch64_publication(
            checker.parse_instructions(body), 0xB3D2C0, body)
    except SystemExit as error:
        if message not in str(error):
            raise SystemExit(
                f"unexpected AArch64 checker rejection: {error}") from error
    else:
        raise SystemExit("AArch64 checker accepted a corrupted entry sequence")


def main():
    checker = load_checker()
    # GNU objdump omits hidden local-symbol names from linked AArch64 ADRP/ADD
    # operands.  This is the exact output shape observed on Vista.
    body = """
297fb0:\td0004529 \tadrp\tx9, b3d000 <_frida_decode+0x698>
297fb4:\t910b0129 \tadd\tx9, x9, #0x2c0
297fb8:\t885ffd2a \tldaxr\tw10, [x9]
297fbc:\t1100054a \tadd\tw10, w10, #0x1
297fc0:\t880bfd2a \tstlxr\tw11, w10, [x9]
297fc4:\t35ffffab \tcbnz\tw11, 297fb8 <peak_dlopen+0x8>
297fc8:\t17ff676a \tb\t271d70 <peak_dlopen_body>
"""
    instructions = checker.parse_instructions(body)
    publication_index, retry_index = checker.validate_aarch64_publication(
        instructions, 0xB3D2C0, body)
    if instructions[publication_index][0] != 0x297FC0:
        raise SystemExit("AArch64 checker selected the wrong publication")
    if instructions[retry_index][0] != 0x297FC4:
        raise SystemExit("AArch64 checker selected the wrong retry branch")

    expect_rejection(
        checker,
        body.replace("#0x2c0", "#0x2c4"),
        "does not target the active counter",
    )
    expect_rejection(
        checker,
        body.replace("w10, w10, #0x1", "w10, w10, #0x2"),
        "does not increment the active count by one",
    )
    expect_rejection(
        checker,
        body.replace("w11, 297fb8", "w11, 297fbc"),
        "does not retry from its LDAXR instruction",
    )
    print("dlopen_entry_binary_parser_ok")


if __name__ == "__main__":
    main()
