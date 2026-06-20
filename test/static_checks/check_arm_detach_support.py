#!/usr/bin/env python3
import re
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def read(root, rel):
    return (root / rel).read_text(encoding="utf-8")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_arm_detach_support.py <repo-root>")
    root = Path(sys.argv[1])

    top_cmake = read(root, "CMakeLists.txt")
    frida_cmake = read(root, "cmake/frida-gum.cmake")
    peak_api = read(root, "cmake/peak-gum/frida-gum-peak-api.h")
    gum_overlay = read(root, "cmake/peak-gum/gum_peak_pc_api.c")
    helper = read(root, "src/peak_detach_helper.c")

    require("aarch64" in top_cmake and "arm64" in top_cmake,
            "top-level CMake must enable the detach helper on Linux arm64/aarch64")
    require("aarch64" in frida_cmake and "arm64" in frida_cmake,
            "Frida Gum auto-patch selection must include Linux arm64/aarch64")
    require("GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_ARM64" in peak_api,
            "PEAK Gum API header must expose an Arm64 ABI fingerprint")
    require("GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_ARM64" in frida_cmake,
            "CMake PEAK Gum validation must accept the Arm64 ABI fingerprint")

    for token in [
        "__aarch64__",
        "PTRACE_GETREGSET",
        "PTRACE_SETREGSET",
        "NT_PRSTATUS",
        "struct user_pt_regs",
        "PEAK_DETACH_HELPER_PLATFORM_SUPPORTED",
        "peak_regs_get",
        "peak_regs_set",
    ]:
        require(token in helper, f"detach helper missing Arm64 register support token: {token}")
    require("regs->pc" in helper,
            "detach helper must read/write the Arm64 program counter field")

    for token in [
        "__aarch64__",
        "GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_ARM64",
        "GumArm64Writer",
        "PeakGumArm64Relocator16",
        "gpointer thunks",
        "gum_query_page_size",
        "PEAK_GUM_PC_ABI_FINGERPRINT",
    ]:
        require(token in gum_overlay, f"Gum overlay missing Arm64 token: {token}")
    require(re.search(r"backend->thunks[\s\S]{0,240}gum_query_page_size", gum_overlay),
            "Arm64 shared thunk classification must conservatively cover the thunk page")

    for rel in [
        "test/detach_controller/test_detach_controller.c",
        "test/detach_controller/test_detach_listener_shutdown.c",
        "test/detach_controller/test_detach_shutdown_preload.c",
    ]:
        source = read(root, rel)
        require("__aarch64__" in source and "nop" in source,
                f"{rel} must provide an Arm64 NOP pad for Gum patch diagnostics")

    print("arm_detach_support_ok")


if __name__ == "__main__":
    main()
