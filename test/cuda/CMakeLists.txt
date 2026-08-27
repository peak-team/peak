if(NOT PEAK_CUDA_ENABLED)
    message(STATUS "Skipping CUDA tests because CUDA profiling is disabled")
    return()
endif()

if (NOT CMAKE_CUDA_COMPILER)
    message(STATUS "Skipping CUDA tests because CUDA language is not enabled")
    return()
endif()

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=-fopenmp -fPIC")

set(PEAK_CUDA_TEST_LIBRARIES ${PEAK_CUDA_RUNTIME_LIBRARIES})
set(PEAK_CUDA_DRIVER_TEST_LIBRARY ${PEAK_CUDA_DRIVER_LIBRARY})
include_directories(${PEAK_CUDA_INCLUDE_DIRS})

find_package(OpenMP QUIET)
find_package(MPI QUIET COMPONENTS CXX)

function(peak_add_cuda_fixture target source)
    add_executable(${target} ${source})
    set_source_files_properties(${source} PROPERTIES LANGUAGE CUDA)
    set_target_properties(${target} PROPERTIES CUDA_ARCHITECTURES all)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target}
        PRIVATE
            Threads::Threads
            ${PEAK_CUDA_TEST_LIBRARIES}
            ${ARGN})
endfunction()

# Executable: test_pthread
add_executable(test_pthread test_pthread.cu)
set_source_files_properties(test_pthread.cu PROPERTIES LANGUAGE CUDA)
set_target_properties(test_pthread PROPERTIES CUDA_ARCHITECTURES all)
target_link_libraries(test_pthread PRIVATE pthread ${PEAK_CUDA_TEST_LIBRARIES})

# Executable: test_mpi
if (MPI_CXX_FOUND)
    add_executable(test_mpi test_mpi.cu)
    set_source_files_properties(test_mpi.cu PROPERTIES LANGUAGE CUDA)
    set_target_properties(test_mpi PROPERTIES CUDA_ARCHITECTURES all)
    target_link_libraries(test_mpi PRIVATE MPI::MPI_CXX ${PEAK_CUDA_TEST_LIBRARIES})
else()
    message(STATUS "Skipping CUDA MPI test because MPI CXX was not found")
endif()

# Executable: test_ompmpi (CUDA + OpenMP + MPI)
if (MPI_CXX_FOUND AND OpenMP_C_FOUND)
    add_executable(test_ompmpi test_ompmpi.cu)
    set_source_files_properties(test_ompmpi.cu PROPERTIES LANGUAGE CUDA)
    set_target_properties(test_ompmpi PROPERTIES CUDA_ARCHITECTURES all)
    target_link_libraries(test_ompmpi
        PRIVATE
            MPI::MPI_CXX
            OpenMP::OpenMP_C
            ${PEAK_CUDA_TEST_LIBRARIES}
            gomp  # Needed for GOMP_critical_start
    )
else()
    message(STATUS "Skipping CUDA OpenMP/MPI test because OpenMP C or MPI CXX was not found")
endif()

peak_add_cuda_fixture(test_cuda_event_recycling test_event_recycling.cu
    ${CMAKE_DL_LIBS})
peak_add_cuda_fixture(test_cuda_error_transparency test_error_transparency.cu
    ${PEAK_CUDA_DRIVER_TEST_LIBRARY})
if(PEAK_CUDA_DRIVER_TEST_LIBRARY)
    target_compile_definitions(test_cuda_error_transparency
        PRIVATE PEAK_TEST_CUDA_DRIVER=1)
endif()
if (PEAK_CUDA_DRIVER_TEST_LIBRARY)
    peak_add_cuda_fixture(test_cuda_stream_capture test_stream_capture.cu
        ${PEAK_CUDA_DRIVER_TEST_LIBRARY})
    peak_add_cuda_fixture(test_cuda_multi_context test_multi_context.cu
        ${PEAK_CUDA_DRIVER_TEST_LIBRARY} ${CMAKE_DL_LIBS})
else()
    message(STATUS
        "Skipping CUDA Driver fixtures because the Driver library was not found")
endif()
peak_add_cuda_fixture(test_cuda_finalization_deadline
    test_finalization_deadline.cu ${CMAKE_DL_LIBS})
add_executable(test_optional_backend_gating
    test_optional_backend_gating.c)
set_target_properties(test_optional_backend_gating PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(test_optional_backend_gating PRIVATE ${CMAKE_DL_LIBS})
add_executable(test_cuda_key_lifetime test_key_lifetime.c)
target_link_libraries(test_cuda_key_lifetime PRIVATE peak)
add_test(NAME test_cuda_key_lifetime COMMAND test_cuda_key_lifetime)
set_tests_properties(test_cuda_key_lifetime PROPERTIES
    PASS_REGULAR_EXPRESSION "cuda_key_lifetime_ok"
    TIMEOUT 15)
if (PEAK_CUDA_DRIVER_TEST_LIBRARY)
    peak_add_cuda_fixture(benchmark_cuda_launch_overhead
        benchmark_launch_overhead.cu
        ${PEAK_CUDA_DRIVER_TEST_LIBRARY} ${CMAKE_DL_LIBS})
else()
    message(STATUS
        "Skipping CUDA launch benchmark because the Driver library was not found")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND Python3_EXECUTABLE)
    add_test(NAME test_optional_backend_gating_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/utils/run_expect_exit.py
                --exit-code 0
                --stdout-regex "cuda_attach_calls=0"
                --stderr-regex "Profiler capability \\[cuda\\]: requested=0 compiled=1 active=0 partial=0 retained=0 failed=0"
                -- $<TARGET_FILE:test_optional_backend_gating>)
    set_tests_properties(test_optional_backend_gating_runtime
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=peak_optional_backend_cpu_target;PEAK_HEARTBEAT_INTERVAL=0;LD_PRELOAD=$<TARGET_FILE:peak>"
            TIMEOUT 15)
    add_test(NAME test_unavailable_jit_backend_fail_open
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/utils/run_expect_exit.py
                --exit-code 0
                --stdout-regex "cuda_attach_calls=0"
                --stderr-regex "Profiler capability \\[jit\\]: requested=1 compiled=1 active=0 partial=0 retained=0 failed=1"
                --stderr-count-regex "disabling non-critical profiler subsystem"
                --stderr-count 1
                -- $<TARGET_FILE:test_optional_backend_gating>)
    set_tests_properties(test_unavailable_jit_backend_fail_open
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=;PEAK_JIT_ENABLE=1;PEAK_JIT_PROVIDER=unsupported;PEAK_HEARTBEAT_INTERVAL=0;LD_PRELOAD=$<TARGET_FILE:peak>"
            TIMEOUT 15)
    add_test(NAME test_cuda_event_recycling_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                sampling
                --exe $<TARGET_FILE:test_cuda_event_recycling>
                --peak $<TARGET_FILE:peak>)
    add_test(NAME test_cuda_partial_capability_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                partial-capability
                --exe $<TARGET_FILE:test_cuda_event_recycling>
                --peak $<TARGET_FILE:peak>)
    add_test(NAME test_cuda_error_transparency_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                errors
                --exe $<TARGET_FILE:test_cuda_error_transparency>
                --peak $<TARGET_FILE:peak>)
    if (PEAK_CUDA_DRIVER_TEST_LIBRARY)
        add_test(NAME test_cuda_stream_capture_runtime
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                    capture
                    --exe $<TARGET_FILE:test_cuda_stream_capture>
                    --peak $<TARGET_FILE:peak>)
        add_test(NAME test_cuda_multi_context_runtime
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                    context
                    --exe $<TARGET_FILE:test_cuda_multi_context>
                    --peak $<TARGET_FILE:peak>)
    endif()
    add_test(NAME test_cuda_finalization_deadline_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                finalization
                --exe $<TARGET_FILE:test_cuda_finalization_deadline>
                --peak $<TARGET_FILE:peak>)
    if (TARGET benchmark_cuda_launch_overhead)
        add_test(NAME test_cuda_launch_overhead_runtime
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_cuda_regressions.py
                    benchmark
                    --exe $<TARGET_FILE:benchmark_cuda_launch_overhead>
                    --peak $<TARGET_FILE:peak>)
    endif()

    set(_peak_cuda_runtime_tests
        test_optional_backend_gating_runtime
        test_unavailable_jit_backend_fail_open
        test_cuda_event_recycling_runtime
        test_cuda_partial_capability_runtime
        test_cuda_error_transparency_runtime
        test_cuda_finalization_deadline_runtime)
    if (TARGET benchmark_cuda_launch_overhead)
        list(APPEND _peak_cuda_runtime_tests
            test_cuda_launch_overhead_runtime)
    endif()
    if (PEAK_CUDA_DRIVER_TEST_LIBRARY)
        list(APPEND _peak_cuda_runtime_tests
            test_cuda_stream_capture_runtime
            test_cuda_multi_context_runtime)
    endif()
    set_tests_properties(${_peak_cuda_runtime_tests}
        PROPERTIES
            LABELS "cuda;gpu"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
    set(_peak_cuda_correctness_tests
        test_cuda_event_recycling_runtime
        test_cuda_partial_capability_runtime
        test_cuda_error_transparency_runtime
        test_cuda_finalization_deadline_runtime)
    if (PEAK_CUDA_DRIVER_TEST_LIBRARY)
        list(APPEND _peak_cuda_correctness_tests
            test_cuda_stream_capture_runtime
            test_cuda_multi_context_runtime)
    endif()
    set_tests_properties(${_peak_cuda_correctness_tests}
        PROPERTIES TIMEOUT 45)
    if (TARGET benchmark_cuda_launch_overhead)
        set_tests_properties(test_cuda_launch_overhead_runtime
            PROPERTIES TIMEOUT 180)
    endif()
endif()
