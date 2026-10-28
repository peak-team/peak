set(PEAK_FRIDA_GUM_PROVIDER "auto" CACHE STRING
    "Frida Gum provider: auto, prebuilt, auto-patched-devkit, or patched-devkit")
set_property(CACHE PEAK_FRIDA_GUM_PROVIDER PROPERTY STRINGS auto prebuilt auto-patched-devkit patched-devkit)

set(PEAK_PATCHED_GUM_ROOT "" CACHE PATH
    "Root of a PEAK-patched Frida Gum devkit containing frida-gum.h and libfrida-gum.a")
set(PEAK_PATCHED_GUM_INCLUDE_DIR "" CACHE PATH
    "Include directory for a PEAK-patched Frida Gum devkit")
set(PEAK_PATCHED_GUM_LIBRARY "" CACHE FILEPATH
    "Static library for a PEAK-patched Frida Gum devkit")
option(PEAK_REQUIRE_GUM_PEAK_API
    "Require the selected Frida Gum headers to expose PEAK PC classification and exact-entry APIs"
    OFF)

macro(fetch_frida_gum _download_module_path _download_root)
    set(FRIDA_GUM_DOWNLOAD_ROOT ${_download_root})
    configure_file(
        ${_download_module_path}/frida-gum-download.cmake
        ${_download_root}/CMakeLists.txt
        @ONLY
        )
    unset(FRIDA_GUM_DOWNLOAD_ROOT)

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}"
                "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
                "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
                .
        WORKING_DIRECTORY
            ${_download_root}
        RESULT_VARIABLE _frida_gum_configure_result
        )
    if(NOT _frida_gum_configure_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to configure Frida Gum download project in ${_download_root}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" --build .
        WORKING_DIRECTORY
            ${_download_root}
        RESULT_VARIABLE _frida_gum_build_result
        )
    if(NOT _frida_gum_build_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to build Frida Gum download project in ${_download_root}")
    endif()

    set (FRIDA_GUM_LIBRARIES ${PROJECT_BINARY_DIR}/frida-gum/libfrida-gum.a)
    set (FRIDA_GUM_INCLUDE_DIRS ${PROJECT_BINARY_DIR}/frida-gum)
endmacro()


function(_peak_can_auto_patch_frida_gum _out_var)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _peak_processor)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
       _peak_processor MATCHES "^(x86_64|amd64|aarch64|arm64)$")
        set(${_out_var} ON PARENT_SCOPE)
    else()
        set(${_out_var} OFF PARENT_SCOPE)
    endif()
endfunction()

function(_peak_patch_frida_gum_elf_module _source_dir _output_dir)
    if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
        return()
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _peak_processor)
    if(NOT _peak_processor MATCHES "^(x86_64|amd64|aarch64|arm64)$")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_patch_command
        "${Python3_EXECUTABLE}"
        "${_source_dir}/peak-gum/patch_frida_gum_elf_module.py"
        --library "${_output_dir}/libfrida-gum.a"
        --ar "${CMAKE_AR}"
        --work-dir "${_output_dir}/frida-gum-elf-module-patch"
    )
    list(APPEND _patch_command
        --nm "${CMAKE_NM}"
        --objcopy "${CMAKE_OBJCOPY}")
    if(CMAKE_RANLIB)
        list(APPEND _patch_command --ranlib "${CMAKE_RANLIB}")
    endif()

    execute_process(
        COMMAND ${_patch_command}
        RESULT_VARIABLE _patch_result
        OUTPUT_VARIABLE _patch_stdout
        ERROR_VARIABLE _patch_stderr)
    if(NOT _patch_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to patch Frida Gum archive:\n${_patch_stdout}\n${_patch_stderr}")
    endif()
    string(STRIP "${_patch_stdout}" _patch_stdout_stripped)
    if(_patch_stdout_stripped)
        message(STATUS "${_patch_stdout_stripped}")
    endif()
endfunction()

set(PEAK_GUM_RELOCATOR_SOURCE_DIR "" CACHE PATH
    "Directory containing hash-pinned Gum 17.15.3 gumarm64relocator.c/gumx86relocator.c")

function(_peak_replace_gum_relocator _source_dir _output_dir)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _processor)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    if(_processor MATCHES "^(aarch64|arm64)$")
        set(_arch arm64)
        set(_hash 94ce96c7783d944443c37682dbd51c71b81829e9af0cd89b84075abc01a90f54)
    elseif(_processor MATCHES "^(x86_64|amd64)$")
        set(_arch x86)
        set(_hash cab6091265282f49d3f0adea72108e0a73ea218622e5b51b4ffccf668410e0ea)
    else()
        return()
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(PEAK_GUM_RELOCATOR_SOURCE_DIR)
        set(_cache "${PEAK_GUM_RELOCATOR_SOURCE_DIR}")
    else()
        set(_cache "${PROJECT_BINARY_DIR}/frida-gum-relocator-source")
    endif()
    set(_original "${_cache}/gum${_arch}relocator.c")
    if(NOT EXISTS "${_original}")
        if(NOT PEAK_FETCH_DEPS OR PEAK_GUM_RELOCATOR_SOURCE_DIR)
            message(FATAL_ERROR "Missing pinned relocator source ${_original}; provide PEAK_GUM_RELOCATOR_SOURCE_DIR or enable PEAK_FETCH_DEPS")
        endif()
        file(MAKE_DIRECTORY "${_cache}")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/frida/frida-gum/e5b0d7d6a9a83da1750b6fc654006a714b81a890/gum/arch-${_arch}/gum${_arch}relocator.c"
            "${_original}" EXPECTED_HASH "SHA256=${_hash}" TLS_VERIFY ON
            TIMEOUT 60 INACTIVITY_TIMEOUT 20 STATUS _download_status)
        list(GET _download_status 0 _download_code)
        if(NOT _download_code EQUAL 0)
            file(REMOVE "${_original}")
            message(FATAL_ERROR "Failed to download pinned Gum relocator: ${_download_status}")
        endif()
    endif()
    file(SHA256 "${_original}" _actual_hash)
    if(NOT _actual_hash STREQUAL _hash)
        message(FATAL_ERROR "Pinned Gum relocator SHA256 mismatch: ${_original}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_original}"
        "${_source_dir}/peak-gum/prepare_relocator_source.py"
        "${_source_dir}/peak-gum/replace_relocator_member.py"
        "${_source_dir}/peak-gum/gum${_arch}relocator-compat.h"
        "${_source_dir}/peak-gum/${_arch}-relocator-range.inc"
        "${_source_dir}/peak-gum/gum_peak_pc_api.c"
        "${_source_dir}/peak-gum/gum_peak_staged_redirect.inc"
        "${_source_dir}/peak-gum/frida-gum-peak-api.h"
        "${_source_dir}/peak-gum/patch_frida_gum_elf_module.py")
    set(_prepared "${_output_dir}/gum${_arch}relocator-peak.c")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${_source_dir}/peak-gum/prepare_relocator_source.py"
        --source "${_original}" --output "${_prepared}"
        --arch "${_arch}" --sha256 "${_hash}"
        TIMEOUT 30 RESULT_VARIABLE _prepare_result ERROR_VARIABLE _prepare_error)
    if(NOT _prepare_result EQUAL 0)
        message(FATAL_ERROR "Cannot prepare Gum relocator: ${_prepare_error}")
    endif()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type)
    separate_arguments(_flags NATIVE_COMMAND
        "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_build_type}}")
    set(_toolchain_flags)
    if(CMAKE_C_COMPILER_TARGET)
        list(APPEND _toolchain_flags "--target=${CMAKE_C_COMPILER_TARGET}")
    endif()
    if(CMAKE_SYSROOT)
        list(APPEND _toolchain_flags "--sysroot=${CMAKE_SYSROOT}")
    endif()
    set(_object "${_output_dir}/arch-${_arch}_gum${_arch}relocator.c.o")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_toolchain_flags}
        -std=c11 -fPIC -O2 -DG_DISABLE_ASSERT -D_GNU_SOURCE -DHAVE_LINUX ${_flags}
        "-I${_output_dir}" "-I${_source_dir}/peak-gum"
        -c "${_prepared}" -o "${_object}"
        TIMEOUT 120 RESULT_VARIABLE _compile_result ERROR_VARIABLE _compile_error)
    if(NOT _compile_result EQUAL 0)
        message(FATAL_ERROR "Cannot compile Gum relocator: ${_compile_error}")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${_source_dir}/peak-gum/replace_relocator_member.py"
        --library "${_output_dir}/libfrida-gum.a" --object "${_object}"
        --ar "${CMAKE_AR}" --nm "${CMAKE_NM}"
        --work-dir "${_output_dir}/relocator-member-patch"
        TIMEOUT 120 RESULT_VARIABLE _replace_result OUTPUT_VARIABLE _replace_output ERROR_VARIABLE _replace_error)
    if(NOT _replace_result EQUAL 0)
        message(FATAL_ERROR "Cannot replace Gum relocator: ${_replace_error}")
    endif()
    string(STRIP "${_replace_output}" _replace_output)
    message(STATUS "${_replace_output}")
    set(PEAK_GUM_ENTRY_RANGE_REPLACED ON PARENT_SCOPE)
endfunction()

function(_peak_compile_peak_gum_overlay _source_dir _input_dir _output_dir)
    file(MAKE_DIRECTORY "${_output_dir}")
    configure_file("${_input_dir}/libfrida-gum.a"
                   "${_output_dir}/libfrida-gum.a" COPYONLY)
    configure_file("${_input_dir}/frida-gum.h"
                   "${_output_dir}/frida-gum.h" COPYONLY)

    set(PEAK_GUM_ENTRY_RANGE_REPLACED OFF)
    _peak_replace_gum_relocator("${_source_dir}" "${_output_dir}")
    if(PEAK_GUM_ENTRY_RANGE_REPLACED)
        file(APPEND "${_output_dir}/frida-gum.h"
            "\n/* Capability of this generated archive/header pair. */\n#define PEAK_GUM_ENTRY_RANGE_CHECKS 3\n")
    endif()
    _peak_patch_frida_gum_elf_module("${_source_dir}" "${_output_dir}")

    file(READ "${_source_dir}/peak-gum/frida-gum-peak-api.h" _peak_gum_api_header)
    file(APPEND "${_output_dir}/frida-gum.h"
         "\n\n/* PEAK local extension ABI. */\n${_peak_gum_api_header}\n")

    set(_overlay_object "${_output_dir}/gum_peak_pc_api.c.o")
    set(_overlay_objects "${_overlay_object}")
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _peak_build_type_upper)
    set(_peak_overlay_c_flags
        "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_peak_build_type_upper}}")
    separate_arguments(_peak_overlay_c_flags_list
        NATIVE_COMMAND "${_peak_overlay_c_flags}")
    set(_peak_overlay_toolchain_flags)
    if(CMAKE_C_COMPILER_TARGET)
        list(APPEND _peak_overlay_toolchain_flags
             "--target=${CMAKE_C_COMPILER_TARGET}")
    endif()
    if(CMAKE_SYSROOT)
        list(APPEND _peak_overlay_toolchain_flags
             "--sysroot=${CMAKE_SYSROOT}")
    endif()
    # Route only the code allocator's near-page call on Linux x86-64.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _peak_near_processor)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       _peak_near_processor MATCHES "^(x86_64|amd64)$")
        # Bind auxiliary-vector/Gum imports at load, avoiding first-call lazy
        # loader resolution while the mutation controller has stopped threads.
        set(_near_object "${_output_dir}/peak_gum_low_gap.c.o")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" ${_peak_overlay_toolchain_flags}
                -std=c11 -fPIC -O2 ${_peak_overlay_c_flags_list} -fno-plt
                "-I${_output_dir}" "-I${_source_dir}/../include"
                -c "${_source_dir}/peak-gum/peak_gum_low_gap.c"
                -o "${_near_object}"
            RESULT_VARIABLE _near_compile_result
            OUTPUT_VARIABLE _near_compile_stdout
            ERROR_VARIABLE _near_compile_stderr)
        if(NOT _near_compile_result EQUAL 0)
            message(FATAL_ERROR "Failed to compile PEAK near-page overlay:\n${_near_compile_stdout}\n${_near_compile_stderr}")
        endif()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                "${_source_dir}/peak-gum/patch_gum_codeallocator_near.py"
                --library "${_output_dir}/libfrida-gum.a"
                --helper "${_near_object}"
                --work-dir "${_output_dir}/near-page-member-patch"
                --ar "${CMAKE_AR}" --nm "${CMAKE_NM}"
                --objcopy "${CMAKE_OBJCOPY}"
            RESULT_VARIABLE _near_patch_result
            OUTPUT_VARIABLE _near_patch_stdout
            ERROR_VARIABLE _near_patch_stderr)
        if(NOT _near_patch_result EQUAL 0)
            message(FATAL_ERROR "Failed near-page member route:\n${_near_patch_stdout}\n${_near_patch_stderr}")
        endif()
        string(STRIP "${_near_patch_stdout}" _near_patch_stdout_stripped)
        if(_near_patch_stdout_stripped)
            message(STATUS "${_near_patch_stdout_stripped}")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_C_COMPILER}"
            ${_peak_overlay_toolchain_flags}
            -std=c11
            -fPIC
            -O2
            ${_peak_overlay_c_flags_list}
            "-I${_output_dir}"
            -c "${_source_dir}/peak-gum/gum_peak_pc_api.c"
            -o "${_overlay_object}"
        RESULT_VARIABLE _compile_result
        OUTPUT_VARIABLE _compile_stdout
        ERROR_VARIABLE _compile_stderr)
    if(NOT _compile_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to compile PEAK Frida Gum PC overlay:\n${_compile_stdout}\n${_compile_stderr}")
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _peak_overlay_processor)
    if(_peak_overlay_processor MATCHES "^(aarch64|arm64)$")
        set(_overlay_syscall_object
            "${_output_dir}/peak_aarch64_raw_syscall.S.o")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}"
                ${_peak_overlay_toolchain_flags}
                -fPIC
                -c "${_source_dir}/peak-gum/peak_aarch64_raw_syscall.S"
                -o "${_overlay_syscall_object}"
            RESULT_VARIABLE _syscall_compile_result
            OUTPUT_VARIABLE _syscall_compile_stdout
            ERROR_VARIABLE _syscall_compile_stderr)
        if(NOT _syscall_compile_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to compile PEAK Frida Gum AArch64 raw-syscall stub:\n${_syscall_compile_stdout}\n${_syscall_compile_stderr}")
        endif()
        list(APPEND _overlay_objects "${_overlay_syscall_object}")
    endif()

    execute_process(
        COMMAND "${CMAKE_AR}" qcs
            "${_output_dir}/libfrida-gum.a" ${_overlay_objects}
        RESULT_VARIABLE _ar_result
        OUTPUT_VARIABLE _ar_stdout
        ERROR_VARIABLE _ar_stderr)
    if(NOT _ar_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to append PEAK Frida Gum PC overlay to libfrida-gum.a:\n${_ar_stdout}\n${_ar_stderr}")
    endif()

    if(CMAKE_RANLIB)
        execute_process(
            COMMAND "${CMAKE_RANLIB}" "${_output_dir}/libfrida-gum.a"
            RESULT_VARIABLE _ranlib_result
            OUTPUT_VARIABLE _ranlib_stdout
            ERROR_VARIABLE _ranlib_stderr)
        if(NOT _ranlib_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to index PEAK-patched libfrida-gum.a:\n${_ranlib_stdout}\n${_ranlib_stderr}")
        endif()
    endif()

    set(FRIDA_GUM_LIBRARIES "${_output_dir}/libfrida-gum.a" PARENT_SCOPE)
    set(FRIDA_GUM_INCLUDE_DIRS "${_output_dir}" PARENT_SCOPE)
endfunction()

function(_peak_first_existing_path _out_var)
    foreach(_candidate IN LISTS ARGN)
        if(EXISTS "${_candidate}")
            set(${_out_var} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${_out_var} "" PARENT_SCOPE)
endfunction()

function(_peak_configure_patched_frida_gum)
    set(_library_candidates)
    set(_include_candidates)

    if(PEAK_PATCHED_GUM_LIBRARY)
        list(APPEND _library_candidates "${PEAK_PATCHED_GUM_LIBRARY}")
    endif()
    if(PEAK_PATCHED_GUM_INCLUDE_DIR)
        list(APPEND _include_candidates "${PEAK_PATCHED_GUM_INCLUDE_DIR}")
    endif()
    if(PEAK_PATCHED_GUM_ROOT)
        list(APPEND _library_candidates
            "${PEAK_PATCHED_GUM_ROOT}/libfrida-gum.a"
            "${PEAK_PATCHED_GUM_ROOT}/lib/libfrida-gum.a"
            "${PEAK_PATCHED_GUM_ROOT}/lib64/libfrida-gum.a"
        )
        list(APPEND _include_candidates
            "${PEAK_PATCHED_GUM_ROOT}/include"
            "${PEAK_PATCHED_GUM_ROOT}"
        )
    endif()

    _peak_first_existing_path(_gum_library ${_library_candidates})
    _peak_first_existing_path(_gum_include_dir ${_include_candidates})

    if(NOT _gum_library)
        message(FATAL_ERROR
            "PEAK_FRIDA_GUM_PROVIDER=patched-devkit requires PEAK_PATCHED_GUM_LIBRARY "
            "or PEAK_PATCHED_GUM_ROOT with libfrida-gum.a")
    endif()
    if(NOT _gum_include_dir OR NOT EXISTS "${_gum_include_dir}/frida-gum.h")
        message(FATAL_ERROR
            "PEAK_FRIDA_GUM_PROVIDER=patched-devkit requires PEAK_PATCHED_GUM_INCLUDE_DIR "
            "or PEAK_PATCHED_GUM_ROOT with frida-gum.h")
    endif()

    set(FRIDA_GUM_LIBRARIES "${_gum_library}" PARENT_SCOPE)
    set(FRIDA_GUM_INCLUDE_DIRS "${_gum_include_dir}" PARENT_SCOPE)
endfunction()

function(_peak_validate_gum_entry_range)
    include(CheckCSourceCompiles)
    include(CheckCSourceRuns)
    set(CMAKE_REQUIRED_INCLUDES "${FRIDA_GUM_INCLUDE_DIRS}")
    set(CMAKE_REQUIRED_LIBRARIES
        "${FRIDA_GUM_LIBRARIES};Threads::Threads;${DL_LIBRARY};${RT_LIBRARY};${RESOLV_LIBRARY};${M_LIBRARY}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(_probe "
#include <frida-gum.h>
#if !defined(PEAK_GUM_ENTRY_RANGE_CHECKS) || PEAK_GUM_ENTRY_RANGE_CHECKS != 3
#error Missing PEAK Gum checked entry-range revision 3
#endif
#if defined(__aarch64__)
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint *, arm64_reg *);
#elif defined(__x86_64__) || defined(__amd64__)
extern gboolean gum_x86_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, guint *);
#endif
int main(void)
{
    guint (*revision)(void) = gum_interceptor_peak_entry_range_revision;
    guint maximum = 0;
    gboolean result;
#if defined(__aarch64__)
    /* Each buffer supplies the bounded walker's full readable 1024 bytes. */
    static const guint32 loop[256] = {
        0x52800000, 0x11000400, 0x71000c1f, 0xd503201f, 0x54ffffab, 0xd65f03c0
    };
    static const guint32 early_return[256] = {
        0x340000c0, 0x11000400, 0xd503201f, 0xd503201f,
        0xd503201f, 0xd65f03c0, 0xd65f03c0
    };
    arm64_reg scratch = ARM64_REG_INVALID;
    gboolean (*can_relocate)(gpointer, guint, GumRelocationScenario,
        GumRelocationPolicy, guint *, arm64_reg *) =
        gum_arm64_relocator_can_relocate;
#elif defined(__x86_64__) || defined(__amd64__)
    static const guint8 code[] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    gboolean (*can_relocate)(gpointer, guint, GumRelocationScenario, guint *) =
        gum_x86_relocator_can_relocate;
#else
#error Unsupported checked entry-range architecture
#endif
    if (revision() != 3) return 1;
    gum_init_embedded();
#if defined(__aarch64__)
    result = !can_relocate((gpointer) loop, 16, GUM_SCENARIO_ONLINE,
        GUM_RELOCATION_CHECKED, &maximum, &scratch) && maximum == 4;
    maximum = 0;
    scratch = ARM64_REG_INVALID;
    result = can_relocate((gpointer) early_return, 16, GUM_SCENARIO_ONLINE,
        GUM_RELOCATION_CHECKED, &maximum, &scratch) && maximum == 16 && result;
#else
    result = can_relocate((gpointer) code, 5, GUM_SCENARIO_ONLINE, &maximum);
#endif
    gum_deinit_embedded();
    return !result;
}
")
    unset(PEAK_GUM_HAS_ENTRY_RANGE_CHECKS CACHE)
    if(CMAKE_CROSSCOMPILING)
        check_c_source_compiles("${_probe}" PEAK_GUM_HAS_ENTRY_RANGE_CHECKS)
    else()
        check_c_source_runs("${_probe}" PEAK_GUM_HAS_ENTRY_RANGE_CHECKS)
    endif()
    if(NOT PEAK_GUM_HAS_ENTRY_RANGE_CHECKS)
        message(FATAL_ERROR
            "Linux x86-64/Arm64 instrumentation requires Frida Gum checked entry-range revision 3 with ARM bounded suffix backedge checks and x86 bounded entry-range checks. "
            "Update the supplied devkit and select patched-devkit, or use PEAK_FRIDA_GUM_PROVIDER=auto "
            "with FRIDA_GUM_* unset. Unpatched prebuilt devkits cannot safely instrument these targets.")
    endif()
endfunction()

function(_peak_validate_frida_gum_peak_api)
    include(CheckCSourceCompiles)
    include(CheckCSourceRuns)

    set(_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")

    set(CMAKE_REQUIRED_INCLUDES "${FRIDA_GUM_INCLUDE_DIRS}")
    set(CMAKE_REQUIRED_LIBRARIES
        "${FRIDA_GUM_LIBRARIES};Threads::Threads;${DL_LIBRARY};${RT_LIBRARY};${RESOLV_LIBRARY};${M_LIBRARY}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(_peak_gum_api_probe_source "
#include <frida-gum.h>

#if !defined(GUM_PEAK_PC_API_VERSION) || GUM_PEAK_PC_API_VERSION != 1
#error Unsupported PEAK Gum PC API version
#endif
#if !defined(GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64)
#error Missing PEAK Gum x86_64 private-layout ABI fingerprint
#endif
#if !defined(GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64)
#error Missing PEAK Gum arm64 private-layout ABI fingerprint
#endif
#if !defined(GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION) || \
    GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION != 5
#error Missing PEAK deferred module-sync lifecycle API
#endif
#if !defined(GUM_PEAK_FAST_LISTENER_VERSION) || \
    GUM_PEAK_FAST_LISTENER_VERSION != 10
#error Unsupported PEAK Gum fast-listener ABI
#endif

int main(void)
{
    typedef guint (*PeakAbiFingerprintFunc)(void);
    typedef gboolean (*PeakClassifyPcFunc)(
        GumInterceptor *,
        gpointer,
        GumInvocationListener *,
        gpointer,
        GumPeakFunctionContext **,
        GumPeakPcState *);
    typedef gpointer (*PeakSafePcFunc)(
        GumPeakFunctionContext *,
        gpointer,
        GumPeakPcState);
    typedef gboolean (*PeakGetFunctionPatchFunc)(
        GumInterceptor *,
        gpointer,
        GumInvocationListener *,
        guint8 *,
        guint8 *,
        guint *);
    typedef gboolean (*PeakGetPcDiagnosticsFunc)(
        GumInterceptor *,
        gpointer,
        GumInvocationListener *,
        GumPeakPcDiagnostics *);
    typedef gboolean (*PeakDrainDeferredModuleSyncFunc)(void);
    typedef gboolean (*PeakInvocationStackEntryMatchesFunc)(
        guint,
        gpointer,
        gpointer);
    typedef gboolean (*PeakUnwindLifecycleInitFunc)(void);
    typedef void (*PeakUnwindThreadJoinedFunc)(
        guint64,
        guintptr,
        gboolean,
        guint64,
        guint64);
    PeakAbiFingerprintFunc abi_fingerprint =
        gum_interceptor_peak_abi_fingerprint;
    PeakClassifyPcFunc classify_pc = gum_interceptor_peak_classify_pc;
    PeakSafePcFunc safe_pc = gum_interceptor_peak_safe_pc;
    PeakGetFunctionPatchFunc get_function_patch =
        gum_interceptor_peak_get_function_patch;
    PeakGetPcDiagnosticsFunc get_pc_diagnostics =
        gum_interceptor_peak_get_pc_diagnostics;
    PeakDrainDeferredModuleSyncFunc drain_deferred_module_sync =
        gum_interceptor_peak_drain_deferred_module_sync;
    PeakInvocationStackEntryMatchesFunc invocation_stack_entry_matches =
        gum_interceptor_peak_invocation_stack_entry_matches;
    PeakUnwindLifecycleInitFunc unwind_lifecycle_init =
        gum_interceptor_peak_unwind_lifecycle_init;
    PeakUnwindThreadJoinedFunc unwind_thread_joined =
        gum_interceptor_peak_unwind_thread_joined;

    (void) GUM_PEAK_PC_API_VERSION;
    (void) GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64;
    (void) GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64;
    (void) sizeof(GumPeakFunctionContext *);
    (void) sizeof(GumPeakPcState);
    (void) sizeof(GumPeakPcDiagnostics);
    (void) GUM_PEAK_PC_SAFE;
    (void) GUM_PEAK_PC_AT_PATCH_ENTRY;
    (void) GUM_PEAK_PC_IN_ENTER_TRAMPOLINE;
    (void) GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE;
    (void) GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE;
    (void) GUM_PEAK_PC_IN_DISPATCH;
    (void) GUM_PEAK_PC_UNKNOWN;
#if defined(__x86_64__) || defined(__amd64__)
    if (abi_fingerprint() !=
        GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64) {
        return 1;
    }
#elif defined(__aarch64__)
    if (abi_fingerprint() !=
        GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64) {
        return 1;
    }
#else
    if (abi_fingerprint() !=
        GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64 &&
        abi_fingerprint() !=
        GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64) {
        return 1;
    }
#endif
    (void) classify_pc;
    (void) safe_pc;
    (void) get_function_patch;
    (void) get_pc_diagnostics;
    (void) drain_deferred_module_sync;
    PeakDrainDeferredModuleSyncFunc sync_available =
        gum_interceptor_peak_deferred_module_sync_available;
    (void) sync_available;
    (void) invocation_stack_entry_matches;
    (void) unwind_thread_joined;
#if !defined(GUM_PEAK_UNWIND_FRAME_API_VERSION) || GUM_PEAK_UNWIND_FRAME_API_VERSION != 4
#error Missing PEAK unwind frame capability
#endif
    if (gum_interceptor_peak_unwind_frame_api_version() != GUM_PEAK_UNWIND_FRAME_API_VERSION) return 2;
    (void) gum_interceptor_peak_unwind_reconcile_current_thread;
    if (!unwind_lifecycle_init()) return 3;
    gum_init_embedded();
    GumInterceptor * interceptor = gum_interceptor_obtain();
    gum_interceptor_ignore_current_thread(interceptor);
    gum_interceptor_unignore_current_thread(interceptor);
    gum_interceptor_peak_begin_module_mutation();
    gum_interceptor_peak_end_module_mutation();
    gum_interceptor_peak_quiesce_deferred_module_sync();
    g_object_unref(interceptor);
    gum_deinit_embedded();
    return 0;
}
")
    unset(PEAK_GUM_HAS_PEAK_PC_API CACHE)
    if(CMAKE_CROSSCOMPILING)
        message(WARNING
            "Cross-compiling: PEAK Frida Gum ABI fingerprint can only be "
            "compile/link checked, not executed.")
        check_c_source_compiles("${_peak_gum_api_probe_source}"
            PEAK_GUM_HAS_PEAK_PC_API)
    else()
        check_c_source_runs("${_peak_gum_api_probe_source}"
            PEAK_GUM_HAS_PEAK_PC_API)
    endif()

    if(NOT PEAK_GUM_HAS_PEAK_PC_API)
        message(FATAL_ERROR
            "The selected Frida Gum devkit does not expose a linkable PEAK PC classification API. "
            "Update the caller-provided patched devkit (including checked entry-range revision 3), or use PEAK_FRIDA_GUM_PROVIDER=auto with FRIDA_GUM_* unset.")
    endif()

    set(_peak_gum_exact_attach_probe_source "
#include <frida-gum.h>

#if !defined(GUM_PEAK_EXACT_ATTACH_API_VERSION) || \
    GUM_PEAK_EXACT_ATTACH_API_VERSION != 1 || \
    !defined(GUM_PEAK_REDIRECT_RESOLVER_API_VERSION) || \
    GUM_PEAK_REDIRECT_RESOLVER_API_VERSION != 1
#error Missing PEAK exact-entry APIs
#endif

int main(void)
{
    typedef GumAttachReturn (*PeakAttachExactFunc)(
        GumInterceptor *,
        gpointer,
        GumInvocationListener *,
        const GumAttachOptions *);
    PeakAttachExactFunc volatile attach_exact =
        gum_interceptor_peak_attach_exact;
    typedef gboolean (*PeakResolveRedirectFunc)(
        GumInterceptor *, gpointer, gpointer *);
    PeakResolveRedirectFunc volatile resolve_redirect =
        gum_interceptor_peak_resolve_redirect_chain;
    return attach_exact == NULL || resolve_redirect == NULL;
}
")
    unset(PEAK_GUM_HAS_PEAK_EXACT_ATTACH_API CACHE)
    check_c_source_compiles("${_peak_gum_exact_attach_probe_source}"
        PEAK_GUM_HAS_PEAK_EXACT_ATTACH_API)

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _peak_processor)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
       _peak_processor MATCHES "^(x86_64|amd64|aarch64|arm64)$" AND
       NOT PEAK_GUM_HAS_PEAK_EXACT_ATTACH_API)
        message(FATAL_ERROR
            "The selected PEAK-patched Frida Gum devkit does not expose the exact-entry attach and redirect-resolution APIs for this architecture. "
            "Use the current auto-patched devkit, or rebuild the caller-provided devkit with the current PEAK Gum API overlay.")
    endif()

    if(PEAK_BUILD_ATTACH AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        unset(PEAK_GUM_HAS_STAGED_MUTATION_API CACHE)
        check_c_source_compiles("
#include <frida-gum.h>
#if !defined(GUM_PEAK_STAGED_MUTATION_API_VERSION) || GUM_PEAK_STAGED_MUTATION_API_VERSION != 1
#error Missing staged mutation API
#endif
int main(void) {
    void * volatile prepare = (void *) gum_interceptor_peak_prepare_staged_mutation;
    void * volatile patch = (void *) gum_interceptor_peak_staged_mutation_patch;
    void * volatile finish = (void *) gum_interceptor_peak_finish_staged_mutation;
    return prepare == 0 || patch == 0 || finish == 0;
}" PEAK_GUM_HAS_STAGED_MUTATION_API)
        if(NOT PEAK_GUM_HAS_STAGED_MUTATION_API)
            message(FATAL_ERROR "PEAK_BUILD_ATTACH requires the staged mutation API in the Linux Gum devkit. Rebuild the caller-provided patched devkit using the current PEAK overlay, or use PEAK_FRIDA_GUM_PROVIDER=auto with FRIDA_GUM_* unset.")
        endif()
    endif()

    set(CMAKE_REQUIRED_INCLUDES "${_saved_required_includes}")
    set(CMAKE_REQUIRED_LIBRARIES "${_saved_required_libraries}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_try_compile_target_type}")
endfunction()

macro(configure_frida_gum _download_module_path _download_root)
    set(PEAK_GUM_PEAK_API_AVAILABLE OFF)
    set(PEAK_GUM_PEAK_PC_API_AVAILABLE OFF)
    set(PEAK_GUM_PEAK_EXACT_ATTACH_API_AVAILABLE OFF)
    set(PEAK_GUM_DARWIN_PATCH_API_AVAILABLE OFF)
    set(_peak_using_peak_patched_gum_api OFF)
    set(_peak_using_downloaded_stock_gum OFF)

    if(NOT PEAK_FRIDA_GUM_PROVIDER STREQUAL "auto" AND
       NOT PEAK_FRIDA_GUM_PROVIDER STREQUAL "prebuilt" AND
       NOT PEAK_FRIDA_GUM_PROVIDER STREQUAL "auto-patched-devkit" AND
       NOT PEAK_FRIDA_GUM_PROVIDER STREQUAL "patched-devkit")
        message(FATAL_ERROR
            "Unsupported PEAK_FRIDA_GUM_PROVIDER='${PEAK_FRIDA_GUM_PROVIDER}'. "
            "Expected 'auto', 'prebuilt', 'auto-patched-devkit', or 'patched-devkit'.")
    endif()

    set(_peak_effective_gum_provider "${PEAK_FRIDA_GUM_PROVIDER}")
    if(_peak_effective_gum_provider STREQUAL "auto")
        _peak_can_auto_patch_frida_gum(_peak_auto_patch_supported)
        if(_peak_auto_patch_supported)
            set(_peak_effective_gum_provider "auto-patched-devkit")
        else()
            set(_peak_effective_gum_provider "prebuilt")
        endif()
    endif()

    if(_peak_effective_gum_provider STREQUAL "patched-devkit")
        message(STATUS "Using caller-provided PEAK-patched Frida Gum devkit")
        _peak_configure_patched_frida_gum()
        set(_peak_using_peak_patched_gum_api ON)
    elseif(FRIDA_GUM_LIBRARIES AND FRIDA_GUM_INCLUDE_DIRS)
        if(_peak_effective_gum_provider STREQUAL "auto-patched-devkit")
            if(PEAK_FRIDA_GUM_PROVIDER STREQUAL "auto")
                message(STATUS
                    "Using caller-provided Frida Gum without auto-patching; "
                    "leave FRIDA_GUM_LIBRARIES/INCLUDE_DIRS unset for the default "
                    "downloaded PEAK-patched devkit")
                set(_peak_effective_gum_provider "prebuilt")
            else()
                message(FATAL_ERROR
                    "PEAK_FRIDA_GUM_PROVIDER=auto-patched-devkit cannot patch "
                    "caller-provided FRIDA_GUM_LIBRARIES/FRIDA_GUM_INCLUDE_DIRS. "
                    "Use patched-devkit with PEAK_PATCHED_GUM_ROOT, or leave "
                    "FRIDA_GUM_* unset.")
            endif()
        endif()
        message(STATUS "Using caller-provided Frida Gum")
    elseif(FRIDA_GUM_LIBRARIES OR FRIDA_GUM_INCLUDE_DIRS)
        message(FATAL_ERROR
            "Set both FRIDA_GUM_LIBRARIES and FRIDA_GUM_INCLUDE_DIRS, "
            "or leave both unset to use the prebuilt devkit.")
    else()
        if(NOT PEAK_FETCH_DEPS)
            message(FATAL_ERROR
                "Frida Gum is required and PEAK_FETCH_DEPS=OFF. Set "
                "FRIDA_GUM_LIBRARIES and FRIDA_GUM_INCLUDE_DIRS for a "
                "site-provided devkit, select patched-devkit with "
                "PEAK_PATCHED_GUM_ROOT, or set PEAK_FETCH_DEPS=ON.")
        endif()
        message(STATUS "Fetching prebuilt frida-gum")
        fetch_frida_gum(
            ${_download_module_path}
            ${_download_root}
        )
        set(_peak_using_downloaded_stock_gum ON)
        if(_peak_effective_gum_provider STREQUAL "auto-patched-devkit")
            message(STATUS "Building PEAK-patched Frida Gum devkit overlay")
            _peak_compile_peak_gum_overlay(
                ${_download_module_path}
                ${PROJECT_BINARY_DIR}/frida-gum
                ${PROJECT_BINARY_DIR}/frida-gum-peak-patched
            )
            set(_peak_using_peak_patched_gum_api ON)
        endif()
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _peak_processor)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin" AND
       _peak_processor MATCHES "^(aarch64|arm64)$" AND
       _peak_using_downloaded_stock_gum)
        # The small Darwin patch-metadata overlay mirrors private fields from
        # the exact 17.15.3 devkit pinned by frida-gum-download.cmake.  Fail
        # closed for caller-provided Gum, whose private ABI is unknown.
        set(PEAK_GUM_DARWIN_PATCH_API_AVAILABLE ON)
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       _peak_processor MATCHES "^(x86_64|amd64|aarch64|arm64)$")
        _peak_validate_gum_entry_range()
    endif()

    if(PEAK_REQUIRE_GUM_PEAK_API OR
       _peak_using_peak_patched_gum_api)
        _peak_validate_frida_gum_peak_api()
        set(PEAK_GUM_PEAK_API_AVAILABLE ON)
        set(PEAK_GUM_PEAK_PC_API_AVAILABLE ON)
        if(PEAK_GUM_HAS_PEAK_EXACT_ATTACH_API)
            set(PEAK_GUM_PEAK_EXACT_ATTACH_API_AVAILABLE ON)
        endif()
    endif()
endmacro()
