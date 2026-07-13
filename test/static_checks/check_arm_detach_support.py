#!/usr/bin/env python3
import re
import shutil
import subprocess
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def read(root, rel):
    return (root / rel).read_text(encoding="utf-8")


def cmake_function_body(cmake, function_name):
    match = re.search(
        rf"function\({function_name}\b[\s\S]*?\nendfunction\(\)", cmake
    )
    require(match is not None, f"missing CMake function: {function_name}")
    return match.group(0)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_arm_detach_support.py <repo-root>")
    root = Path(sys.argv[1])

    top_cmake = read(root, "CMakeLists.txt")
    frida_cmake = read(root, "cmake/frida-gum.cmake")
    peak_api = read(root, "cmake/peak-gum/frida-gum-peak-api.h")
    gum_overlay = read(root, "cmake/peak-gum/gum_peak_pc_api.c")
    helper = read(root, "src/detach_helper.c")
    platform_cmake = read(root, "cmake/exec-platform.cmake")
    source_cmake = read(root, "src/CMakeLists.txt")
    detach_tests_cmake = read(root, "test/detach_controller/CMakeLists.txt")

    require("peak_exec_configure_platform_support()" in top_cmake and
            "set(PEAK_DETACH_HELPER_SUPPORTED" not in top_cmake,
            "top-level CMake must use the shared raw-syscall/detach wiring")
    raw_predicate = cmake_function_body(
        platform_cmake, "peak_exec_raw_syscall_supported")
    helper_predicate = cmake_function_body(
        platform_cmake, "peak_detach_helper_supported")
    require("PEAK_EXEC_RAW_SYSCALL_SUPPORTED" not in helper_predicate and
            "peak_exec_raw_syscall_supported" not in helper_predicate,
            "detach-helper support must not derive from raw-syscall support")
    require("aarch64" in raw_predicate and "aarch64" in helper_predicate,
            "raw syscall and detach-helper predicates must retain Linux arm64")
    require("if(PEAK_DETACH_HELPER_SUPPORTED)" in source_cmake,
            "detach-helper construction must use its dedicated predicate")
    require("if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)" in detach_tests_cmake and
            "if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)"
            in detach_tests_cmake,
            "detach lifecycle tests must retain separate raw and helper gates")
    cmake = shutil.which("cmake")
    require(cmake is not None, "cmake is required to evaluate the platform predicate")
    platform_contract = root / "test/exec_chain/test_exec_chain_platform_contract.cmake"
    predicate = subprocess.run(
        [cmake, f"-DPEAK_SOURCE_ROOT={root}", "-P", str(platform_contract)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    require(predicate.returncode == 0 and
            "exec_chain_platform_contract_ok" in predicate.stdout,
            "shared exec/detach platform predicate failed:\n" + predicate.stdout)
    require("aarch64" in frida_cmake and "arm64" in frida_cmake,
            "Frida Gum auto-patch selection must include Linux arm64/aarch64")
    require("GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64" in peak_api,
            "PEAK Gum API header must expose an Arm64 ABI fingerprint")
    require("GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64" in frida_cmake,
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
        "GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64",
        "GumArm64Writer",
        "PeakGumArm64Relocator17",
        "thunks_by_scratch_reg",
        "gum_query_page_size",
        "PEAK_GUM_PC_ABI_FINGERPRINT",
    ]:
        require(token in gum_overlay, f"Gum overlay missing Arm64 token: {token}")
    require(re.search(r"thunks_by_scratch_reg[\s\S]{0,360}gum_query_page_size", gum_overlay),
            "Arm64 shared thunk classification must conservatively cover the selected thunk page")

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
