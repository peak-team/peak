cmake_minimum_required(VERSION 3.9)
project(peak_exec_platform_selection_probe LANGUAGES NONE)

if(NOT DEFINED PEAK_SOURCE_ROOT)
    message(FATAL_ERROR "PEAK_SOURCE_ROOT is required")
endif()
if(NOT DEFINED PEAK_HOST_C_COMPILER)
    message(FATAL_ERROR "PEAK_HOST_C_COMPILER is required")
endif()

include("${PEAK_SOURCE_ROOT}/cmake/exec-platform.cmake")
peak_exec_configure_platform_support()
if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
    message(FATAL_ERROR "the platform selection probe enabled raw syscalls")
endif()
if(PEAK_DETACH_HELPER_SUPPORTED)
    message(FATAL_ERROR "the platform selection probe enabled the detach helper")
endif()

# This selection probe verifies that unsupported platform metadata cannot
# initialize either architecture-specific source path or helper construction.
if(DEFINED CMAKE_ASM_COMPILER)
    message(FATAL_ERROR "platform selection shape initialized an ASM compiler")
endif()

peak_exec_enable_raw_syscall_language()
set(probe_sources portable_probe.c)
set(trampoline_source
    "${PEAK_SOURCE_ROOT}/src/exec_raw_syscall_trampoline.S")
peak_exec_append_raw_syscall_trampoline(
    probe_sources "${trampoline_source}")

if(NOT "${probe_sources}" STREQUAL "portable_probe.c")
    message(FATAL_ERROR
        "platform selection shape selected unexpected production sources: "
        "${probe_sources}")
endif()

get_property(enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
list(FIND enabled_languages "ASM" asm_language_index)
if(NOT asm_language_index EQUAL -1)
    message(FATAL_ERROR "platform selection shape enabled ASM: ${enabled_languages}")
endif()
list(FIND probe_sources "${trampoline_source}" trampoline_source_index)
if(NOT trampoline_source_index EQUAL -1)
    message(FATAL_ERROR
        "platform selection shape selected the production syscall trampoline")
endif()

if(PEAK_DETACH_HELPER_SUPPORTED)
    add_executable(peak_detach_helper_selection_probe portable_probe.c)
endif()
if(TARGET peak_detach_helper_selection_probe)
    message(FATAL_ERROR "platform selection shape selected detach-helper construction")
endif()

set(probe_object "${CMAKE_CURRENT_BINARY_DIR}/portable_probe.o")
add_custom_command(
    OUTPUT "${probe_object}"
    COMMAND "${PEAK_HOST_C_COMPILER}" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/portable_probe.c"
            -o "${probe_object}"
    DEPENDS ${probe_sources}
    VERBATIM)
add_custom_target(peak_exec_platform_selection_probe ALL DEPENDS "${probe_object}")
message(STATUS
    "peak_exec_platform_selection_configure_ok target=${CMAKE_SYSTEM_NAME}/"
    "${CMAKE_SYSTEM_PROCESSOR} raw=${PEAK_EXEC_RAW_SYSCALL_SUPPORTED} "
    "detach_helper=${PEAK_DETACH_HELPER_SUPPORTED}")
