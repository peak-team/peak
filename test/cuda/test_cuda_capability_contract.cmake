if(NOT DEFINED PEAK_SOURCE_ROOT)
    message(FATAL_ERROR "PEAK_SOURCE_ROOT is required")
endif()

include("${PEAK_SOURCE_ROOT}/cmake/cuda-capability.cmake")

function(expect_capability found version enabled launch_ex)
    peak_cuda_evaluate_capability("${found}" "${version}" ACTUAL)
    if(NOT ACTUAL_ENABLED STREQUAL enabled OR
       NOT ACTUAL_HAS_RUNTIME_LAUNCH_EX STREQUAL launch_ex OR
       NOT ACTUAL_HAS_DRIVER_LAUNCH_EX STREQUAL launch_ex)
        message(FATAL_ERROR
            "CUDA capability mismatch: found=${found} version=${version} "
            "enabled=${ACTUAL_ENABLED}/${enabled} "
            "runtime_ex=${ACTUAL_HAS_RUNTIME_LAUNCH_EX}/${launch_ex} "
            "driver_ex=${ACTUAL_HAS_DRIVER_LAUNCH_EX}/${launch_ex}")
    endif()
endfunction()

expect_capability(FALSE "" OFF OFF)
expect_capability(TRUE "11.1" OFF OFF)
expect_capability(TRUE "11.2" ON OFF)
expect_capability(TRUE "11.6" ON OFF)
expect_capability(TRUE "11.7" ON OFF)
expect_capability(TRUE "11.8" ON ON)
expect_capability(TRUE "13.0" ON ON)

message("cuda_capability_contract_ok")
