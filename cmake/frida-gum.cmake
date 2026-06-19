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
    "Require the selected Frida Gum headers to expose PEAK PC classification APIs"
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
       _peak_processor MATCHES "^(x86_64|amd64)$")
        set(${_out_var} ON PARENT_SCOPE)
    else()
        set(${_out_var} OFF PARENT_SCOPE)
    endif()
endfunction()

function(_peak_compile_peak_gum_overlay _source_dir _input_dir _output_dir)
    file(MAKE_DIRECTORY "${_output_dir}")
    configure_file("${_input_dir}/libfrida-gum.a"
                   "${_output_dir}/libfrida-gum.a" COPYONLY)
    configure_file("${_input_dir}/frida-gum.h"
                   "${_output_dir}/frida-gum.h" COPYONLY)

    file(READ "${_source_dir}/peak-gum/frida-gum-peak-api.h" _peak_gum_api_header)
    file(APPEND "${_output_dir}/frida-gum.h"
         "\n\n/* PEAK local extension ABI. */\n${_peak_gum_api_header}\n")

    set(_overlay_object "${_output_dir}/gum_peak_pc_api.c.o")
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

    execute_process(
        COMMAND "${CMAKE_AR}" qcs "${_output_dir}/libfrida-gum.a" "${_overlay_object}"
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
#if !defined(GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_X86_64)
#error Missing PEAK Gum private-layout ABI fingerprint
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
    PeakAbiFingerprintFunc abi_fingerprint =
        gum_interceptor_peak_abi_fingerprint;
    PeakClassifyPcFunc classify_pc = gum_interceptor_peak_classify_pc;
    PeakSafePcFunc safe_pc = gum_interceptor_peak_safe_pc;
    PeakGetFunctionPatchFunc get_function_patch =
        gum_interceptor_peak_get_function_patch;
    PeakGetPcDiagnosticsFunc get_pc_diagnostics =
        gum_interceptor_peak_get_pc_diagnostics;

    (void) GUM_PEAK_PC_API_VERSION;
    (void) GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_X86_64;
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
    if (abi_fingerprint() !=
        GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_X86_64) {
        return 1;
    }
    (void) classify_pc;
    (void) safe_pc;
    (void) get_function_patch;
    (void) get_pc_diagnostics;
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

    set(CMAKE_REQUIRED_INCLUDES "${_saved_required_includes}")
    set(CMAKE_REQUIRED_LIBRARIES "${_saved_required_libraries}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_try_compile_target_type}")

    if(NOT PEAK_GUM_HAS_PEAK_PC_API)
        message(FATAL_ERROR
            "The selected Frida Gum devkit does not expose a linkable PEAK PC classification API. "
            "Use the stock prebuilt provider, or point PEAK_PATCHED_GUM_ROOT at patched headers and libfrida-gum.a.")
    endif()
endfunction()

macro(configure_frida_gum _download_module_path _download_root)
    set(PEAK_GUM_PEAK_API_AVAILABLE OFF)
    set(PEAK_GUM_PEAK_PC_API_AVAILABLE OFF)
    set(_peak_using_peak_patched_gum_api OFF)

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
        message(STATUS "Fetching prebuilt frida-gum")
        fetch_frida_gum(
            ${_download_module_path}
            ${_download_root}
        )
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

    if(PEAK_REQUIRE_GUM_PEAK_API OR
       _peak_using_peak_patched_gum_api)
        _peak_validate_frida_gum_peak_api()
        set(PEAK_GUM_PEAK_API_AVAILABLE ON)
        set(PEAK_GUM_PEAK_PC_API_AVAILABLE ON)
    endif()
endmacro()
