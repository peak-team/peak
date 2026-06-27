#!/usr/bin/env python3
"""Patch Frida Gum 17.15.3's online ELF memory fallback safety checks."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys


OBJECT_NAME = "gumelfmodule.c.o"
LOAD_SECTION = ".text.gum_elf_module_load"
OLD_MARKER_X86_64 = b"PEAK_GUM_ELF_HEADER_GUARD_X86_64_V1\0"
OLD_MARKER_AARCH64 = b"PEAK_GUM_ELF_HEADER_GUARD_AARCH64_V1\0"
MARKER_X86_64 = b"PEAK_GUM_ELF_HEADER_GUARD_X86_64_V2\0"
MARKER_AARCH64 = b"PEAK_GUM_ELF_HEADER_GUARD_AARCH64_V2\0"


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd is not None else None,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def find_all(data: bytes | bytearray, needle: bytes) -> list[int]:
    offsets: list[int] = []
    start = 0
    while True:
        offset = data.find(needle, start)
        if offset == -1:
            return offsets
        offsets.append(offset)
        start = offset + 1


def elf_machine(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"\x7fELF":
        fail(f"{path} is not an ELF object")
    if data[5] != 1:
        fail(f"{path} is not little-endian ELF")
    return struct.unpack_from("<H", data, 18)[0]


class Elf64Section:
    def __init__(self, index: int, header: tuple[int, ...]) -> None:
        self.index = index
        (
            self.name_offset,
            self.type,
            self.flags,
            self.addr,
            self.offset,
            self.size,
            self.link,
            self.info,
            self.addralign,
            self.entsize,
        ) = header
        self.name = ""

    def pack(self) -> bytes:
        return struct.pack(
            "<IIQQQQIIQQ",
            self.name_offset,
            self.type,
            self.flags,
            self.addr,
            self.offset,
            self.size,
            self.link,
            self.info,
            self.addralign,
            self.entsize,
        )


def read_c_string(data: bytes | bytearray, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end == -1:
        fail(f"unterminated ELF section name at offset {offset}")
    return data[offset:end].decode("ascii")


def elf64_load_sections(data: bytes | bytearray) -> tuple[int, int, list[Elf64Section]]:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail("object is not an ELF file")
    if data[4] != 2 or data[5] != 1:
        fail("object is not a little-endian ELF64 file")

    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize = struct.unpack_from("<H", data, 58)[0]
    e_shnum = struct.unpack_from("<H", data, 60)[0]
    e_shstrndx = struct.unpack_from("<H", data, 62)[0]
    if e_shentsize != 64:
        fail(f"unsupported ELF section header size {e_shentsize}")
    if e_shstrndx >= e_shnum:
        fail("invalid ELF section-name string table index")

    sections: list[Elf64Section] = []
    for index in range(e_shnum):
        header_offset = e_shoff + index * e_shentsize
        header = struct.unpack_from("<IIQQQQIIQQ", data, header_offset)
        sections.append(Elf64Section(index, header))

    shstr = sections[e_shstrndx]
    shstr_data = data[shstr.offset:shstr.offset + shstr.size]
    for section in sections:
        section.name = read_c_string(shstr_data, section.name_offset)

    return e_shoff, e_shentsize, sections


def replace_elf64_section(object_path: Path, section_name: str,
                          new_section_data: bytes | bytearray) -> None:
    data = bytearray(object_path.read_bytes())
    e_shoff, e_shentsize, sections = elf64_load_sections(data)
    matches = [section for section in sections if section.name == section_name]
    if len(matches) != 1:
        fail(f"expected exactly one {section_name} section, found {len(matches)}")
    target = matches[0]
    if target.type == 8:  # SHT_NOBITS
        fail(f"cannot replace NOBITS section {section_name}")

    old_start = target.offset
    old_end = old_start + target.size
    delta = len(new_section_data) - target.size
    if delta == 0:
        data[old_start:old_end] = new_section_data
    else:
        data = data[:old_start] + bytearray(new_section_data) + data[old_end:]
        for section in sections:
            if section.index != target.index and section.offset > old_start:
                section.offset += delta
        if e_shoff > old_start:
            e_shoff += delta
            struct.pack_into("<Q", data, 40, e_shoff)
    target.size = len(new_section_data)

    for section in sections:
        header_offset = e_shoff + section.index * e_shentsize
        data[header_offset:header_offset + e_shentsize] = section.pack()

    object_path.write_bytes(data)


def get_elf64_section(object_path: Path, section_name: str) -> bytearray:
    data = object_path.read_bytes()
    _, _, sections = elf64_load_sections(data)
    matches = [section for section in sections if section.name == section_name]
    if len(matches) != 1:
        fail(f"expected exactly one {section_name} section, found {len(matches)}")
    section = matches[0]
    return bytearray(data[section.offset:section.offset + section.size])


def rel32(source: int, target: int) -> bytes:
    delta = target - (source + 4)
    if not -(1 << 31) <= delta < (1 << 31):
        fail(f"x86_64 relative branch out of range: {delta}")
    return struct.pack("<i", delta)


def patch_x86_jump(section: bytearray, source: int, target: int) -> bytes:
    return b"\xe9" + rel32(source + 1, target)


def patch_x86_jcc(section: bytearray, opcode: bytes, source: int, target: int) -> bytes:
    if len(opcode) != 2:
        raise ValueError("x86 jcc opcode must be two bytes")
    return opcode + rel32(source + 2, target)


def encode_aarch64_b(source: int, target: int) -> bytes:
    delta = target - source
    if delta % 4 != 0:
        fail(f"aarch64 branch target is not 4-byte aligned: {source:#x}->{target:#x}")
    imm26 = delta // 4
    if not -(1 << 25) <= imm26 < (1 << 25):
        fail(f"aarch64 branch out of range: {source:#x}->{target:#x}")
    insn = 0x14000000 | (imm26 & 0x03ffffff)
    return struct.pack("<I", insn)


def encode_aarch64_b_cond(source: int, target: int, cond: int) -> bytes:
    delta = target - source
    if delta % 4 != 0:
        fail(f"aarch64 conditional branch target is not 4-byte aligned: {source:#x}->{target:#x}")
    imm19 = delta // 4
    if not -(1 << 18) <= imm19 < (1 << 18):
        fail(f"aarch64 conditional branch out of range: {source:#x}->{target:#x}")
    insn = 0x54000000 | ((imm19 & 0x7ffff) << 5) | cond
    return struct.pack("<I", insn)


def a64_movz_x(reg: int, imm: int) -> bytes:
    return struct.pack("<I", 0xd2800000 | ((imm & 0xffff) << 5) | reg)


def a64_mul_x(rd: int, rn: int, rm: int) -> bytes:
    return struct.pack("<I", 0x9b007c00 | (rm << 16) | (rn << 5) | rd)


def a64_adds_x_reg(rd: int, rn: int, rm: int) -> bytes:
    return struct.pack("<I", 0xab000000 | (rm << 16) | (rn << 5) | rd)


def a64_adds_w_reg(rd: int, rn: int, rm: int) -> bytes:
    return struct.pack("<I", 0x2b000000 | (rm << 16) | (rn << 5) | rd)


def a64_add_x_imm(rd: int, rn: int, imm: int) -> bytes:
    if imm % 1 != 0 or not 0 <= imm <= 4095:
        fail(f"unsupported aarch64 add immediate: {imm}")
    return struct.pack("<I", 0x91000000 | (imm << 10) | (rn << 5) | rd)


def a64_sub_x_imm(rd: int, rn: int, imm: int) -> bytes:
    if imm % 1 != 0 or not 0 <= imm <= 4095:
        fail(f"unsupported aarch64 sub immediate: {imm}")
    return struct.pack("<I", 0xd1000000 | (imm << 10) | (rn << 5) | rd)


def a64_str_x(rt: int, rn: int, offset: int) -> bytes:
    if offset % 8 != 0:
        fail(f"unaligned aarch64 x str offset: {offset}")
    return struct.pack("<I", 0xf9000000 | ((offset // 8) << 10) | (rn << 5) | rt)


def a64_ldr_w(rt: int, rn: int, offset: int) -> bytes:
    if offset % 4 != 0:
        fail(f"unaligned aarch64 w ldr offset: {offset}")
    return struct.pack("<I", 0xb9400000 | ((offset // 4) << 10) | (rn << 5) | rt)


def a64_ldr_x(rt: int, rn: int, offset: int) -> bytes:
    if offset % 8 != 0:
        fail(f"unaligned aarch64 x ldr offset: {offset}")
    return struct.pack("<I", 0xf9400000 | ((offset // 8) << 10) | (rn << 5) | rt)


def a64_ldrb_w(rt: int, rn: int, offset: int) -> bytes:
    if not 0 <= offset <= 4095:
        fail(f"unsupported aarch64 ldrb offset: {offset}")
    return struct.pack("<I", 0x39400000 | (offset << 10) | (rn << 5) | rt)


def a64_ldrh_w(rt: int, rn: int, offset: int) -> bytes:
    if offset % 2 != 0:
        fail(f"unaligned aarch64 h ldr offset: {offset}")
    return struct.pack("<I", 0x79400000 | ((offset // 2) << 10) | (rn << 5) | rt)


def a64_cmp_x_imm(rn: int, imm: int) -> bytes:
    if not 0 <= imm <= 4095:
        fail(f"unsupported aarch64 x cmp immediate: {imm}")
    return struct.pack("<I", 0xf100001f | (imm << 10) | (rn << 5))


def a64_cmp_w_imm(rn: int, imm: int) -> bytes:
    if not 0 <= imm <= 4095:
        fail(f"unsupported aarch64 w cmp immediate: {imm}")
    return struct.pack("<I", 0x7100001f | (imm << 10) | (rn << 5))


def a64_cmp_x_reg(rn: int, rm: int) -> bytes:
    return struct.pack("<I", 0xeb00001f | (rm << 16) | (rn << 5))


def a64_svc() -> bytes:
    return struct.pack("<I", 0xd4000001)


class X86Cave:
    def __init__(self, base: int) -> None:
        self.base = base
        self.data = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str, str]] = []

    @property
    def pos(self) -> int:
        return self.base + len(self.data)

    def label(self, name: str) -> None:
        self.labels[name] = self.pos

    def emit(self, data: bytes) -> None:
        self.data.extend(data)

    def jcc(self, opcode: bytes, label: str) -> None:
        pos = self.pos
        self.emit(opcode + b"\x00\x00\x00\x00")
        self.fixups.append((pos, "jcc", label))

    def jmp(self, label: str) -> None:
        pos = self.pos
        self.emit(b"\xe9\x00\x00\x00\x00")
        self.fixups.append((pos, "jmp", label))

    def finish(self) -> bytes:
        for pos, kind, label in self.fixups:
            target = self.labels[label]
            local = pos - self.base
            if kind == "jcc":
                self.data[local:local + 6] = patch_x86_jcc(self.data, self.data[local:local + 2], pos, target)
            else:
                self.data[local:local + 5] = patch_x86_jump(self.data, pos, target)
        return bytes(self.data)


class AArch64Cave:
    def __init__(self, base: int) -> None:
        self.base = base
        self.data = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str, str, int | None]] = []

    @property
    def pos(self) -> int:
        return self.base + len(self.data)

    def label(self, name: str) -> None:
        self.labels[name] = self.pos

    def emit(self, data: bytes) -> None:
        self.data.extend(data)

    def b(self, label: str) -> None:
        pos = self.pos
        self.emit(b"\x00\x00\x00\x14")
        self.fixups.append((pos, "b", label, None))

    def b_cond(self, cond: int, label: str) -> None:
        pos = self.pos
        self.emit(b"\x00\x00\x00\x54")
        self.fixups.append((pos, "b.cond", label, cond))

    def finish(self) -> bytes:
        for pos, kind, label, cond in self.fixups:
            target = self.labels[label]
            local = pos - self.base
            if kind == "b":
                self.data[local:local + 4] = encode_aarch64_b(pos, target)
            else:
                assert cond is not None
                self.data[local:local + 4] = encode_aarch64_b_cond(pos, target, cond)
        return bytes(self.data)


def restore_x86_64_fallback_branch(section: bytearray) -> bool:
    original = bytes.fromhex(
        "41 83 7f 28 00 74 18"
        "49 8b bf b8 00 00 00"
        "48 89 fe"
        "48 f7 d6"
    )
    patched = bytes.fromhex(
        "41 83 7f 28 00 eb 18"
        "49 8b bf b8 00 00 00"
        "48 89 fe"
        "48 f7 d6"
    )

    original_offsets = find_all(section, original)
    patched_offsets = find_all(section, patched)
    if len(original_offsets) == 1 and not patched_offsets:
        return False
    if len(original_offsets) == 0 and len(patched_offsets) == 1:
        offset = patched_offsets[0]
        section[offset:offset + len(original)] = original
        return True
    fail(
        "unexpected x86_64 Gum ELF fallback branch match count: "
        f"original={len(original_offsets)} patched={len(patched_offsets)}"
    )


def restore_aarch64_fallback_branch(section: bytearray) -> bool:
    original = bytes.fromhex(
        "60 2a 40 b9"
        "80 00 00 34"
        "60 5e 40 f9"
        "e1 03 20 aa"
        "49 ff ff 17"
    )
    patched = bytes.fromhex(
        "60 2a 40 b9"
        "04 00 00 14"
        "60 5e 40 f9"
        "e1 03 20 aa"
        "49 ff ff 17"
    )

    original_offsets = find_all(section, original)
    patched_offsets = find_all(section, patched)
    if len(original_offsets) == 1 and not patched_offsets:
        return False
    if len(original_offsets) == 0 and len(patched_offsets) == 1:
        offset = patched_offsets[0]
        section[offset:offset + len(original)] = original
        return True
    fail(
        "unexpected aarch64 Gum ELF fallback branch match count: "
        f"original={len(original_offsets)} patched={len(patched_offsets)}"
    )


def add_x86_64_header_guard(section: bytearray) -> bool:
    if MARKER_X86_64 in section:
        return False
    if OLD_MARKER_X86_64 in section:
        fail("found obsolete x86_64 Gum ELF header guard; rebuild from an unpatched Frida Gum archive")

    original = bytes.fromhex(
        "0f 10 4d 00"
        "41 0f 11 4f 58"
        "8a 45 04"
        "3c 01"
        "0f 84"
    )
    offsets = find_all(section, original)
    if len(offsets) != 1:
        fail(f"unexpected x86_64 ELF header-read match count: {len(offsets)}")

    patch_offset = offsets[0]
    invalid_jne = patch_offset + 22
    if section[invalid_jne:invalid_jne + 2] != b"\x0f\x85":
        fail("unexpected x86_64 ELF header invalid-branch encoding")
    invalid_target = invalid_jne + 6 + struct.unpack_from("<i", section, invalid_jne + 2)[0]
    back_target = patch_offset + 9
    cave_offset = (len(section) + 15) & ~15
    section.extend(b"\x90" * (cave_offset - len(section)))

    cave = X86Cave(cave_offset)
    cave.emit(bytes.fromhex("49 39 6f 38"))                    # self->file_data == live data?
    cave.jcc(b"\x0f\x85", "copy")
    cave.emit(bytes.fromhex("48 81 ec 80 00 00 00"))           # sub rsp, 0x80
    cave.emit(bytes.fromhex("48 8d 44 24 40"))                 # lea rax, [rsp + 0x40]
    cave.emit(bytes.fromhex("48 89 04 24"))                    # mov [rsp], rax
    cave.emit(bytes.fromhex("48 c7 44 24 08 40 00 00 00"))     # local_iov.len = 64
    cave.emit(bytes.fromhex("48 89 6c 24 10"))                 # remote_iov.base = rbp
    cave.emit(bytes.fromhex("48 c7 44 24 18 40 00 00 00"))     # remote_iov.len = 64
    cave.emit(bytes.fromhex("b8 27 00 00 00 0f 05 89 c7"))     # getpid(); mov edi, eax
    cave.emit(bytes.fromhex("48 8d 34 24"))                    # lea rsi, [rsp]
    cave.emit(bytes.fromhex("ba 01 00 00 00"))                 # mov edx, 1
    cave.emit(bytes.fromhex("4c 8d 54 24 10"))                 # lea r10, [rsp + 0x10]
    cave.emit(bytes.fromhex("41 b8 01 00 00 00"))              # mov r8d, 1
    cave.emit(bytes.fromhex("45 31 c9"))                       # xor r9d, r9d
    cave.emit(bytes.fromhex("b8 36 01 00 00 0f 05"))           # process_vm_readv
    cave.emit(bytes.fromhex("48 83 f8 40"))                    # cmp rax, 64
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("81 7c 24 40 7f 45 4c 46"))        # ELF magic
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("0f b6 44 24 44 3c 01"))           # EI_CLASS == 1
    cave.jcc(b"\x0f\x84", "class_ok")
    cave.emit(bytes.fromhex("3c 02"))                          # EI_CLASS == 2
    cave.jcc(b"\x0f\x85", "invalid")
    cave.label("class_ok")
    cave.emit(bytes.fromhex("80 7c 24 45 01"))                 # native little-endian ELF
    cave.jcc(b"\x0f\x85", "invalid")
    cave.label("data_ok")
    cave.emit(bytes.fromhex("80 7c 24 46 01"))                 # EI_VERSION == 1
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("83 7c 24 54 01"))                 # e_version == EV_CURRENT
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("80 7c 24 44 01"))                 # ELFCLASS32?
    cave.jcc(b"\x0f\x84", "elf32")

    cave.label("elf64")
    cave.emit(bytes.fromhex("66 83 7c 24 74 40"))              # e_ehsize == sizeof(Elf64_Ehdr)
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("0f b7 44 24 78"))                 # e_phnum
    cave.emit(bytes.fromhex("66 85 c0"))                       # test ax, ax
    cave.jcc(b"\x0f\x84", "elf64_sh")
    cave.emit(bytes.fromhex("66 83 7c 24 76 38"))              # e_phentsize == sizeof(Elf64_Phdr)
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("48 8b 44 24 60"))                 # e_phoff
    cave.emit(bytes.fromhex("48 83 f8 40"))                    # e_phoff >= e_ehsize
    cave.jcc(b"\x0f\x82", "invalid")
    cave.emit(bytes.fromhex("0f b7 4c 24 78"))                 # e_phnum
    cave.emit(bytes.fromhex("48 6b c9 38"))                    # e_phentsize * e_phnum
    cave.emit(bytes.fromhex("48 01 c8"))                       # e_phoff + table size
    cave.jcc(b"\x0f\x82", "invalid")
    cave.label("elf64_sh")
    cave.emit(bytes.fromhex("0f b7 44 24 7c"))                 # e_shnum
    cave.emit(bytes.fromhex("66 85 c0"))
    cave.jcc(b"\x0f\x84", "header_ok")
    cave.emit(bytes.fromhex("66 83 7c 24 7a 40"))              # e_shentsize == sizeof(Elf64_Shdr)
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("48 8b 44 24 68"))                 # e_shoff
    cave.emit(bytes.fromhex("48 83 f8 40"))                    # e_shoff >= e_ehsize
    cave.jcc(b"\x0f\x82", "invalid")
    cave.emit(bytes.fromhex("0f b7 4c 24 7c"))                 # e_shnum
    cave.emit(bytes.fromhex("48 c1 e1 06"))                    # e_shentsize * e_shnum
    cave.emit(bytes.fromhex("48 01 c8"))
    cave.jcc(b"\x0f\x82", "invalid")
    cave.jmp("header_ok")

    cave.label("elf32")
    cave.emit(bytes.fromhex("66 83 7c 24 68 34"))              # e_ehsize == sizeof(Elf32_Ehdr)
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("0f b7 44 24 6c"))                 # e_phnum
    cave.emit(bytes.fromhex("66 85 c0"))
    cave.jcc(b"\x0f\x84", "elf32_sh")
    cave.emit(bytes.fromhex("66 83 7c 24 6a 20"))              # e_phentsize == sizeof(Elf32_Phdr)
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("8b 44 24 5c"))                    # e_phoff
    cave.emit(bytes.fromhex("83 f8 34"))                       # e_phoff >= e_ehsize
    cave.jcc(b"\x0f\x82", "invalid")
    cave.emit(bytes.fromhex("0f b7 4c 24 6c"))                 # e_phnum
    cave.emit(bytes.fromhex("6b c9 20"))                       # e_phentsize * e_phnum
    cave.emit(bytes.fromhex("01 c8"))                          # e_phoff + table size
    cave.jcc(b"\x0f\x82", "invalid")
    cave.label("elf32_sh")
    cave.emit(bytes.fromhex("0f b7 44 24 70"))                 # e_shnum
    cave.emit(bytes.fromhex("66 85 c0"))
    cave.jcc(b"\x0f\x84", "header_ok")
    cave.emit(bytes.fromhex("66 83 7c 24 6e 28"))              # e_shentsize == sizeof(Elf32_Shdr)
    cave.jcc(b"\x0f\x85", "invalid")
    cave.emit(bytes.fromhex("8b 44 24 60"))                    # e_shoff
    cave.emit(bytes.fromhex("83 f8 34"))                       # e_shoff >= e_ehsize
    cave.jcc(b"\x0f\x82", "invalid")
    cave.emit(bytes.fromhex("0f b7 4c 24 70"))                 # e_shnum
    cave.emit(bytes.fromhex("6b c9 28"))                       # e_shentsize * e_shnum
    cave.emit(bytes.fromhex("01 c8"))
    cave.jcc(b"\x0f\x82", "invalid")

    cave.label("header_ok")
    cave.emit(bytes.fromhex("48 81 c4 80 00 00 00"))           # add rsp, 0x80
    cave.label("copy")
    cave.emit(section[patch_offset:patch_offset + 9])          # original identity copy
    cave.emit(patch_x86_jump(section, cave.pos, back_target))
    cave.label("invalid")
    cave.emit(bytes.fromhex("48 81 c4 80 00 00 00"))
    cave.emit(patch_x86_jump(section, cave.pos, invalid_target))
    section.extend(cave.finish())
    section.extend(MARKER_X86_64)

    section[patch_offset:patch_offset + 9] = (
        patch_x86_jump(section, patch_offset, cave_offset) + b"\x90\x90\x90\x90"
    )
    return True


def add_aarch64_header_guard(section: bytearray) -> bool:
    if MARKER_AARCH64 in section:
        return False
    if OLD_MARKER_AARCH64 in section:
        fail("found obsolete aarch64 Gum ELF header guard; rebuild from an unpatched Frida Gum archive")

    original = bytes.fromhex(
        "80 06 40 a9"  # ldp x0, x1, [x20]
        "60 86 05 a9"  # stp x0, x1, [x19, #0x58]
        "80 12 40 39"  # ldrb w0, [x20, #0x4]
        "1f 04 00 71"  # cmp w0, #0x1
        "60 13 00 54"  # b.eq
        "1f 08 00 71"  # cmp w0, #0x2
    )
    offsets = find_all(section, original)
    if len(offsets) != 1:
        fail(f"unexpected aarch64 ELF header-read match count: {len(offsets)}")

    patch_offset = offsets[0]
    invalid_bne = patch_offset + 24
    invalid_insn = struct.unpack_from("<I", section, invalid_bne)[0]
    if (invalid_insn & 0xff00001f) != 0x54000001:
        fail("unexpected aarch64 ELF header invalid-branch encoding")
    imm19 = (invalid_insn >> 5) & 0x7ffff
    if imm19 & (1 << 18):
        imm19 -= 1 << 19
    invalid_target = invalid_bne + (imm19 * 4)
    back_target = patch_offset + 4
    cave_offset = (len(section) + 15) & ~15
    section.extend(b"\x1f\x20\x03\xd5" * ((cave_offset - len(section)) // 4))

    sp = 31
    cave = AArch64Cave(cave_offset)
    cave.emit(a64_ldr_x(0, 19, 0x38))            # self->file_data
    cave.emit(a64_cmp_x_reg(0, 20))              # file_data == live data?
    cave.b_cond(1, "copy")                       # b.ne
    cave.emit(a64_sub_x_imm(sp, sp, 0x80))       # sub sp, sp, #0x80
    cave.emit(a64_add_x_imm(0, sp, 0x40))        # local buffer
    cave.emit(a64_str_x(0, sp, 0x00))
    cave.emit(a64_movz_x(0, 0x40))
    cave.emit(a64_str_x(0, sp, 0x08))
    cave.emit(a64_str_x(20, sp, 0x10))           # remote base = x20
    cave.emit(a64_str_x(0, sp, 0x18))
    cave.emit(a64_movz_x(8, 172))                # getpid
    cave.emit(a64_svc())
    cave.emit(a64_add_x_imm(1, sp, 0x00))        # local_iov
    cave.emit(a64_movz_x(2, 1))
    cave.emit(a64_add_x_imm(3, sp, 0x10))        # remote_iov
    cave.emit(a64_movz_x(4, 1))
    cave.emit(a64_movz_x(5, 0))
    cave.emit(a64_movz_x(8, 270))                # process_vm_readv
    cave.emit(a64_svc())
    cave.emit(a64_cmp_x_imm(0, 0x40))
    cave.b_cond(1, "invalid")                    # b.ne
    cave.emit(a64_ldrb_w(0, sp, 0x40))
    cave.emit(a64_cmp_w_imm(0, 0x7f))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrb_w(0, sp, 0x41))
    cave.emit(a64_cmp_w_imm(0, 0x45))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrb_w(0, sp, 0x42))
    cave.emit(a64_cmp_w_imm(0, 0x4c))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrb_w(0, sp, 0x43))
    cave.emit(a64_cmp_w_imm(0, 0x46))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrb_w(0, sp, 0x44))
    cave.emit(a64_cmp_w_imm(0, 1))
    cave.b_cond(0, "class_ok")                   # b.eq
    cave.emit(a64_cmp_w_imm(0, 2))
    cave.b_cond(1, "invalid")
    cave.label("class_ok")
    cave.emit(a64_ldrb_w(0, sp, 0x45))
    cave.emit(a64_cmp_w_imm(0, 1))                # native little-endian ELF
    cave.b_cond(1, "invalid")
    cave.label("data_ok")
    cave.emit(a64_ldrb_w(0, sp, 0x46))
    cave.emit(a64_cmp_w_imm(0, 1))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldr_w(0, sp, 0x54))             # e_version == EV_CURRENT
    cave.emit(a64_cmp_w_imm(0, 1))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrb_w(0, sp, 0x44))
    cave.emit(a64_cmp_w_imm(0, 1))
    cave.b_cond(0, "elf32")

    cave.label("elf64")
    cave.emit(a64_ldrh_w(0, sp, 0x74))            # e_ehsize == sizeof(Elf64_Ehdr)
    cave.emit(a64_cmp_w_imm(0, 0x40))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrh_w(0, sp, 0x78))            # e_phnum
    cave.emit(a64_cmp_w_imm(0, 0))
    cave.b_cond(0, "elf64_sh")
    cave.emit(a64_ldrh_w(0, sp, 0x76))            # e_phentsize
    cave.emit(a64_cmp_w_imm(0, 0x38))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldr_x(0, sp, 0x60))             # e_phoff
    cave.emit(a64_cmp_x_imm(0, 0x40))
    cave.b_cond(3, "invalid")                     # b.lo
    cave.emit(a64_ldrh_w(1, sp, 0x78))
    cave.emit(a64_movz_x(2, 0x38))
    cave.emit(a64_mul_x(1, 1, 2))
    cave.emit(a64_adds_x_reg(0, 0, 1))
    cave.b_cond(2, "invalid")                     # b.cs
    cave.label("elf64_sh")
    cave.emit(a64_ldrh_w(0, sp, 0x7c))            # e_shnum
    cave.emit(a64_cmp_w_imm(0, 0))
    cave.b_cond(0, "header_ok")
    cave.emit(a64_ldrh_w(0, sp, 0x7a))            # e_shentsize
    cave.emit(a64_cmp_w_imm(0, 0x40))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldr_x(0, sp, 0x68))             # e_shoff
    cave.emit(a64_cmp_x_imm(0, 0x40))
    cave.b_cond(3, "invalid")                     # b.lo
    cave.emit(a64_ldrh_w(1, sp, 0x7c))
    cave.emit(a64_movz_x(2, 0x40))
    cave.emit(a64_mul_x(1, 1, 2))
    cave.emit(a64_adds_x_reg(0, 0, 1))
    cave.b_cond(2, "invalid")                     # b.cs
    cave.b("header_ok")

    cave.label("elf32")
    cave.emit(a64_ldrh_w(0, sp, 0x68))            # e_ehsize == sizeof(Elf32_Ehdr)
    cave.emit(a64_cmp_w_imm(0, 0x34))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldrh_w(0, sp, 0x6c))            # e_phnum
    cave.emit(a64_cmp_w_imm(0, 0))
    cave.b_cond(0, "elf32_sh")
    cave.emit(a64_ldrh_w(0, sp, 0x6a))            # e_phentsize
    cave.emit(a64_cmp_w_imm(0, 0x20))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldr_w(0, sp, 0x5c))             # e_phoff
    cave.emit(a64_cmp_w_imm(0, 0x34))
    cave.b_cond(3, "invalid")                     # b.lo
    cave.emit(a64_ldrh_w(1, sp, 0x6c))
    cave.emit(a64_movz_x(2, 0x20))
    cave.emit(a64_mul_x(1, 1, 2))
    cave.emit(a64_adds_w_reg(0, 0, 1))
    cave.b_cond(2, "invalid")                     # b.cs
    cave.label("elf32_sh")
    cave.emit(a64_ldrh_w(0, sp, 0x70))            # e_shnum
    cave.emit(a64_cmp_w_imm(0, 0))
    cave.b_cond(0, "header_ok")
    cave.emit(a64_ldrh_w(0, sp, 0x6e))            # e_shentsize
    cave.emit(a64_cmp_w_imm(0, 0x28))
    cave.b_cond(1, "invalid")
    cave.emit(a64_ldr_w(0, sp, 0x60))             # e_shoff
    cave.emit(a64_cmp_w_imm(0, 0x34))
    cave.b_cond(3, "invalid")                     # b.lo
    cave.emit(a64_ldrh_w(1, sp, 0x70))
    cave.emit(a64_movz_x(2, 0x28))
    cave.emit(a64_mul_x(1, 1, 2))
    cave.emit(a64_adds_w_reg(0, 0, 1))
    cave.b_cond(2, "invalid")                     # b.cs

    cave.label("header_ok")
    cave.emit(a64_add_x_imm(sp, sp, 0x80))
    cave.label("copy")
    cave.emit(section[patch_offset:patch_offset + 4])          # original ldp
    cave.emit(encode_aarch64_b(cave.pos, back_target))
    cave.label("invalid")
    cave.emit(a64_add_x_imm(sp, sp, 0x80))
    cave.emit(encode_aarch64_b(cave.pos, invalid_target))
    section.extend(cave.finish())
    section.extend(MARKER_AARCH64)

    section[patch_offset:patch_offset + 4] = encode_aarch64_b(patch_offset, cave_offset)
    return True


def patch_object(path: Path) -> bool:
    machine = elf_machine(path)
    section = get_elf64_section(path, LOAD_SECTION)

    if machine == 62:  # EM_X86_64
        restored = restore_x86_64_fallback_branch(section)
        guarded = add_x86_64_header_guard(section)
        name = "x86_64"
    elif machine == 183:  # EM_AARCH64
        restored = restore_aarch64_fallback_branch(section)
        guarded = add_aarch64_header_guard(section)
        name = "aarch64"
    else:
        fail(f"unsupported {OBJECT_NAME} ELF machine id {machine}")

    if restored or guarded:
        replace_elf64_section(path, LOAD_SECTION, section)
        actions = []
        if restored:
            actions.append("restored online memory fallback")
        if guarded:
            actions.append("added ELF header guard")
        print(f"patched {name} Gum ELF module: {', '.join(actions)}")
        return True

    print(f"{name} Gum ELF module header guard already patched")
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--ar", required=True)
    parser.add_argument("--ranlib")
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    library = args.library.resolve()
    if not library.exists():
        fail(f"missing Frida Gum archive: {library}")

    work_dir = args.work_dir.resolve()
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    listing = run([args.ar, "t", str(library)])
    if listing.returncode != 0:
        fail(f"failed to list {library}:\n{listing.stdout}\n{listing.stderr}")
    members = listing.stdout.splitlines()
    if members.count(OBJECT_NAME) != 1:
        fail(f"expected exactly one {OBJECT_NAME} in {library}, found {members.count(OBJECT_NAME)}")

    extracted = work_dir / OBJECT_NAME
    result = run([args.ar, "x", str(library), OBJECT_NAME], cwd=work_dir)
    if result.returncode != 0 or not extracted.exists():
        fail(f"failed to extract {OBJECT_NAME}:\n{result.stdout}\n{result.stderr}")

    changed = patch_object(extracted)
    if changed:
        result = run([args.ar, "d", str(library), OBJECT_NAME])
        if result.returncode != 0:
            fail(f"failed to remove original {OBJECT_NAME}:\n{result.stdout}\n{result.stderr}")

        result = run([args.ar, "qcs", str(library), str(extracted)])
        if result.returncode != 0:
            fail(f"failed to append patched {OBJECT_NAME}:\n{result.stdout}\n{result.stderr}")

        if args.ranlib:
            result = run([args.ranlib, str(library)])
            if result.returncode != 0:
                fail(f"failed to index patched archive:\n{result.stdout}\n{result.stderr}")

    shutil.rmtree(work_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
