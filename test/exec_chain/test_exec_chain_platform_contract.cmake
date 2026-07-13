if(NOT DEFINED PEAK_SOURCE_ROOT)
    message(FATAL_ERROR "PEAK_SOURCE_ROOT is required")
endif()

include("${PEAK_SOURCE_ROOT}/cmake/exec-platform.cmake")

function(assert_exec_platform system_name system_processor raw_expected helper_expected)
    set(CMAKE_SYSTEM_NAME "${system_name}")
    set(CMAKE_SYSTEM_PROCESSOR "${system_processor}")
    peak_exec_configure_platform_support()
    if(NOT "${PEAK_EXEC_RAW_SYSCALL_SUPPORTED}" STREQUAL "${raw_expected}")
        message(FATAL_ERROR
            "raw-syscall predicate mismatch: system=${system_name} "
            "processor=${system_processor} expected=${raw_expected} "
            "actual=${PEAK_EXEC_RAW_SYSCALL_SUPPORTED}")
    endif()
    if(NOT "${PEAK_DETACH_HELPER_SUPPORTED}" STREQUAL "${helper_expected}")
        message(FATAL_ERROR
            "detach-helper predicate mismatch: system=${system_name} "
            "processor=${system_processor} expected=${helper_expected} "
            "actual=${PEAK_DETACH_HELPER_SUPPORTED}")
    endif()
endfunction()

assert_exec_platform("Linux" "x86_64" "ON" "ON")
assert_exec_platform("Linux" "AMD64" "ON" "ON")
assert_exec_platform("Linux" "aarch64" "ON" "ON")
assert_exec_platform("Linux" "arm64" "ON" "ON")
assert_exec_platform("Darwin" "arm64" "OFF" "OFF")
assert_exec_platform("Linux" "ppc64le" "OFF" "OFF")
assert_exec_platform("Windows" "x86_64" "OFF" "OFF")

message("exec_chain_platform_contract_ok")
