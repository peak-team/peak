include(CTest)
if(BUILD_TESTING)
    option(PEAK_BUILD_DYNAMIC_LIB_TESTS "Build dynamic dlopen/Python extension tests" ON)

    # Find or download blas only for tests
    find_package(BLAS)
    if(NOT BLAS_FOUND)
        message(STATUS "Building OpenBLAS")
        include(${PROJECT_SOURCE_DIR}/cmake/openblas.cmake)
        fetch_openblas(
            ${PROJECT_SOURCE_DIR}/cmake
            ${PROJECT_BINARY_DIR}/openblas
            )
        set(BLAS_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    endif()

    find_package(LAPACK)
    if(NOT LAPACK_FOUND AND NOT BLAS_FOUND)
        message(STATUS "Use OpenBLAS for LAPACK")
        set(BLAS_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
        set(LAPACK_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    elseif(NOT LAPACK_FOUND)
        message(STATUS "Building OpenBLAS for LAPACK")
        include(${PROJECT_SOURCE_DIR}/cmake/openblas.cmake)
        fetch_openblas(
            ${PROJECT_SOURCE_DIR}/cmake
            ${PROJECT_BINARY_DIR}/openblas
            )
        set(LAPACK_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    endif()

    find_package(OpenMP)
    find_package(MPI)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(BUILD_CUDA_PROFILE)
        if (CMAKE_VERSION VERSION_LESS 3.17)
            find_package(CUDA)
        else()
            find_package(CUDAToolkit)
        endif()
        if (CUDA_FOUND OR CUDAToolkit_FOUND)
            include_directories(${CUDA_INCLUDE_DIRS} ${CUDAToolkit_INCLUDE_DIRS})
        endif()
    endif()

    include (FortranCInterface)
    list (APPEND FORTRAN_FUNCTIONS sdot dgemm)
    FortranCInterface_HEADER (include/FC.h 
        MACRO_NAMESPACE "FC_"
        SYMBOLS ${FORTRAN_FUNCTIONS}
    )

    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        set(PRELOAD_VAR "LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so")
    elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set(PRELOAD_VAR "DYLD_INSERT_LIBRARIES=${PROJECT_BINARY_DIR}/src/libpeak.dylib")
    endif()

    set(HEARTBEAT_OFF_ENV "PEAK_HEARTBEAT_INTERVAL=0")
    set(HEARTBEAT_ON_ENV "PEAK_HEARTBEAT_INTERVAL=0.1")

    add_test(NAME test_gum_mutation_allowlist
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_gum_mutation_allowlist.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_gum_mutation_allowlist
        PROPERTIES
            PASS_REGULAR_EXPRESSION "gum_mutation_allowlist_ok")

    add_test(NAME test_cuda_interceptor_consistency
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_cuda_interceptor_consistency.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_cuda_interceptor_consistency
        PROPERTIES
            PASS_REGULAR_EXPRESSION "cuda_interceptor_consistency_ok")

    add_test(NAME test_arm_detach_support
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_arm_detach_support.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_arm_detach_support
        PROPERTIES
            PASS_REGULAR_EXPRESSION "arm_detach_support_ok")

    add_test(NAME test_detach_lifecycle_invariants
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_detach_lifecycle_invariants.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_detach_lifecycle_invariants
        PROPERTIES
            PASS_REGULAR_EXPRESSION "detach_lifecycle_invariants_ok")

    if(TARGET peak_detach_helper)
        add_test(NAME test_detach_helper_self_test
            COMMAND $<TARGET_FILE:peak_detach_helper> --self-test)
        set_tests_properties(test_detach_helper_self_test
            PROPERTIES
                PASS_REGULAR_EXPRESSION "peak_detach_helper_self_test_ok")
        if(CMAKE_SYSTEM_NAME MATCHES "Linux")
            add_test(NAME test_detach_helper_self_test_preloaded
                COMMAND $<TARGET_FILE:peak_detach_helper> --self-test)
            set_tests_properties(test_detach_helper_self_test_preloaded
                PROPERTIES
                    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:peak>;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_TARGET=peak_detach_hot_target"
                    PASS_REGULAR_EXPRESSION "peak_detach_helper_self_test_ok"
                    FAIL_REGULAR_EXPRESSION "PEAK Library|overhead calibration|Estimated overhead")
        endif()
    endif()

    configure_file(${PROJECT_SOURCE_DIR}/test/data/blas_symbols.conf ${PROJECT_BINARY_DIR}/test/data/blas_symbols.conf COPYONLY)
    set(PEAK_TARGET_CONFIG_PATH ${PROJECT_BINARY_DIR}/test/data/blas_symbols.conf)

    add_subdirectory(detach_controller)
    add_subdirectory(detach_runtime)
    add_subdirectory(dlopen_controller)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
       PEAK_GUM_PEAK_PC_API_AVAILABLE AND
       TARGET peak_detach_helper)
        add_test(NAME test_detach_hotloop_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop --threads 4 --seconds 2)
        set_tests_properties(test_detach_hotloop_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_hot_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                TIMEOUT 20)
        add_test(NAME test_detach_hotloop_signal_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop --threads 32 --seconds 2)
        set_tests_properties(test_detach_hotloop_signal_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=128;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_hot_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 45)

        add_test(NAME test_detach_hotloop_signal_thread_spawn_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --threads 8
                    --seconds 3
                    --spawn-transient-threads
                    --spawner-threads 4)
        set_tests_properties(test_detach_hotloop_signal_thread_spawn_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=256;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/signal-thread-spawn-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "spawned_threads=[1-9][0-9]*.*trace_detach_success=1"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 60)

        add_test(NAME test_detach_hotloop_batch_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 30
                    --paired-targets
                    --disable-reattach
                    --peak-cost 0
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-trace
                    --require-trace-diagnostics
                    --require-detach-batch-size 2
                    --fail-on-transition-skips)
        set_tests_properties(test_detach_hotloop_batch_strict
            PROPERTIES
                TIMEOUT 45
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok")
        if(TARGET test_detach_fake_helper)
            add_test(NAME test_detach_hotloop_batch_retry_preserves_pending_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-batch-retry-check)
            set_tests_properties(test_detach_hotloop_batch_retry_preserves_pending_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target,peak_detach_hot_target_two;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-retry-trace.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_batch_retry_ok"
                    TIMEOUT 30)
            add_test(NAME test_detach_hotloop_reattach_entry_byte_gum_pc_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-reattach-retry-check)
            set_tests_properties(test_detach_hotloop_reattach_entry_byte_gum_pc_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=synthetic-stop-file-once;FAKE_DETACH_HELPER_STOP_PC_FILE=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-reattach-retry-pc.txt;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-reattach-retry-helper.log;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-reattach-retry-trace.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_reattach_entry_bytes_ok"
                    SKIP_RETURN_CODE 77
                    TIMEOUT 30)
            foreach(_reattach_gum_pc_case IN ITEMS on-enter-plus-one on-leave on-invoke enter-thunk)
                add_test(NAME test_detach_hotloop_reattach_entry_byte_gum_pc_${_reattach_gum_pc_case}_strict
                    COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                            --controller-reattach-retry-check
                            --unsupported-gum-pc-case ${_reattach_gum_pc_case})
                set_tests_properties(test_detach_hotloop_reattach_entry_byte_gum_pc_${_reattach_gum_pc_case}_strict
                    PROPERTIES
                        ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=synthetic-stop-file-once;FAKE_DETACH_HELPER_STOP_PC_FILE=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-reattach-retry-${_reattach_gum_pc_case}-pc.txt;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-reattach-retry-${_reattach_gum_pc_case}-helper.log;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-reattach-retry-${_reattach_gum_pc_case}-trace.csv;${PRELOAD_VAR}"
                        PASS_REGULAR_EXPRESSION "controller_reattach_entry_bytes_ok"
                        SKIP_RETURN_CODE 77
                        TIMEOUT 30)
            endforeach()
            foreach(_unsupported_gum_pc_case IN ITEMS on-enter-plus-one on-leave on-invoke enter-thunk)
                add_test(NAME test_detach_hotloop_entry_byte_gum_pc_${_unsupported_gum_pc_case}_strict
                    COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                            --controller-unsupported-gum-pc-retry-check
                            --unsupported-gum-pc-case ${_unsupported_gum_pc_case})
                set_tests_properties(test_detach_hotloop_entry_byte_gum_pc_${_unsupported_gum_pc_case}_strict
                    PROPERTIES
                        ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target,peak_detach_hot_target_two;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=synthetic-stop-file-once;FAKE_DETACH_HELPER_STOP_PC_FILE=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-unsupported-gum-pc-${_unsupported_gum_pc_case}.txt;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-unsupported-gum-pc-${_unsupported_gum_pc_case}-helper.log;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-unsupported-gum-pc-${_unsupported_gum_pc_case}-trace.csv;${PRELOAD_VAR}"
                        PASS_REGULAR_EXPRESSION "controller_unsupported_gum_pc_entry_bytes_ok"
                        SKIP_RETURN_CODE 77
                        TIMEOUT 30)
            endforeach()
        endif()
        add_test(NAME test_detach_hotloop_batch_reattach_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 3
                    --samples 1
                    --timeout 45
                    --paired-targets
                    --overhead-ratio 0.01
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-reattach-trace
                    --require-detach-batch-size 2
                    --require-reattach-batch-size 2)
        set_tests_properties(test_detach_hotloop_batch_reattach_strict
            PROPERTIES
                TIMEOUT 60
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok")
        add_test(NAME test_detach_hotloop_signal_reattach_trace_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 60
                    --overhead-ratio 0.01
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-reattach-trace
                    --require-reattach
                    --require-trace-diagnostics
                    --detach-backend signal
                    --fail-on-transition-skips
                    --max-reattach-classify-failed 0)
        set_tests_properties(test_detach_hotloop_signal_reattach_trace_strict
            PROPERTIES
                TIMEOUT 75
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok"
                SKIP_RETURN_CODE 77)
        add_test(NAME test_detach_hotloop_signal_thread_spawn_reattach_trace_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 8
                    --seconds 5
                    --samples 2
                    --timeout 180
                    --overhead-ratio 0.01
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-spawn-reattach-trace
                    --require-reattach
                    --require-trace-diagnostics
                    --detach-backend signal
                    --spawn-transient-threads
                    --spawner-threads 4
                    --peak-max-threads 256
                    --thread-slack 128
                    --fail-on-transition-skips
                    --max-reattach-classify-failed 0)
        set_tests_properties(test_detach_hotloop_signal_thread_spawn_reattach_trace_strict
            PROPERTIES
                TIMEOUT 210
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok"
                SKIP_RETURN_CODE 77)
        add_test(NAME test_detach_hotloop_helper_thread_spawn_reattach_trace_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 8
                    --seconds 5
                    --samples 2
                    --timeout 180
                    --overhead-ratio 0.01
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-helper-spawn-reattach-trace
                    --require-reattach
                    --require-trace-diagnostics
                    --detach-backend helper
                    --detach-helper $<TARGET_FILE:test_peak_detach_helper>
                    --helper-retry-log-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-helper-spawn-reattach-retry
                    --spawn-transient-threads
                    --spawner-threads 4
                    --peak-max-threads 256
                    --thread-slack 128
                    --fail-on-transition-skips
                    --max-reattach-classify-failed 0
                    --skip-helper-unavailable)
        set_tests_properties(test_detach_hotloop_helper_thread_spawn_reattach_trace_strict
            PROPERTIES
                TIMEOUT 210
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok"
                SKIP_RETURN_CODE 77)
        add_test(NAME test_detach_stale_threads_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_stale_threads
                    --active-threads 8
                    --stale-threads 24
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2)
        set_tests_properties(test_detach_stale_threads_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=96;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests"
                TIMEOUT 40)
        add_test(NAME test_detach_stale_threads_signal_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_stale_threads
                    --active-threads 8
                    --stale-threads 24
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2)
        set_tests_properties(test_detach_stale_threads_signal_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=128;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 45)

        add_test(NAME test_detach_stale_threads_signal_unrelated_spin_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_stale_threads
                    --active-threads 8
                    --stale-threads 16
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2
                    --stale-mode unrelated-spin)
        set_tests_properties(test_detach_stale_threads_signal_unrelated_spin_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=128;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "stale_mode=unrelated-spin.*stale_blocked=16.*stale_calls=32768.*unrelated_effect=[1-9][0-9]*"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 45)

        add_test(NAME test_detach_stale_threads_strict_low_tracking_capacity
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_stale_threads
                    --active-threads 8
                    --stale-threads 24
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2)
        set_tests_properties(test_detach_stale_threads_strict_low_tracking_capacity
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=8;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests"
                TIMEOUT 40)
        add_test(NAME test_detach_stale_threads_reattach_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_stale_threads
                    --active-threads 16
                    --stale-threads 48
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 3)
        set_tests_properties(test_detach_stale_threads_reattach_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=192;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=1;PEAK_COST=0.0000001;PEAK_OVERHEAD_RATIO=0.01;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-stale-reattach-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 60)
    endif()

    # Add test of dgemm
    add_subdirectory(dgemm)
    add_test(NAME test_dgemm COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_dgemm 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=dgemm_,main,dummy;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_dgemm 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of TARGET_CONFIG
    add_test(NAME test_target_config COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_target_config 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_GROUP=BLAS;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_target_config 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of config_file
    add_test(NAME test_config_file COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_config_file 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_FILE=${PEAK_TARGET_CONFIG_PATH};${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_config_file 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of sdot
    add_subdirectory(sdot)
    add_test(NAME test_sdot COMMAND ${PROJECT_BINARY_DIR}/test/sdot/test_sdot)
    if(OpenMP_FOUND)
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=cblas_sdot,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=cblas_sdot,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_sdot
        PROPERTIES 
        PASS_REGULAR_EXPRESSION "cblas_sdot\\|[ \t]+1000000\\|"
    )

    # Add test of sleep
    add_subdirectory(sleep)
    add_test(NAME test_sleep COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
    if(OpenMP_FOUND)
        set_tests_properties(test_sleep
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_sleep
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_sleep
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "my_sleep_func\\|[ \t]+1000\\|.*my_sleep_func\\|[ \t]+10\\."
    )
    if(NOT PEAK_GUM_PEAK_API_AVAILABLE)
        add_test(NAME test_initial_attach_strict_stock_gum COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
        set_tests_properties(test_initial_attach_strict_stock_gum
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=my_sleep_func;PEAK_REQUIRE_SAFE_DETACH=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_initial_attach_strict_statslog;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "skipping initial Gum attach.*my_sleep_func.*missing-gum-api"
                FAIL_REGULAR_EXPRESSION "my_sleep_func\\|[ \t]+[1-9][0-9]*\\|"
        )
    endif()

    # Add test of recursive
    add_subdirectory(recursive)
    add_test(NAME test_recursive COMMAND ${PROJECT_BINARY_DIR}/test/recursive/test_recursive)
    set_tests_properties(test_recursive
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=functionA,functionB;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )

    # Add test of fftw
    add_subdirectory(fftw)
    add_test(NAME test_fftw COMMAND ${PROJECT_BINARY_DIR}/test/fftw/test_fftw)
    set_tests_properties(test_fftw
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_CONFIG=FFTW;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    
    if(MPI_FOUND)
        add_subdirectory(dgemm_mpi)
        set(_PEAK_MPI_TEST_NPROCS 2)
        if(MPIEXEC_MAX_NUMPROCS AND MPIEXEC_MAX_NUMPROCS LESS _PEAK_MPI_TEST_NPROCS)
            set(_PEAK_MPI_TEST_NPROCS ${MPIEXEC_MAX_NUMPROCS})
        endif()
        add_test(NAME test_dgemm_mpi
            COMMAND ${MPIEXEC_EXECUTABLE}
                    ${MPIEXEC_NUMPROC_FLAG} ${_PEAK_MPI_TEST_NPROCS}
                    ${MPIEXEC_PREFLAGS}
                    ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi
                    ${MPIEXEC_POSTFLAGS})
        if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
            add_test(NAME test_dgemm_mpi_strict
                COMMAND ${MPIEXEC_EXECUTABLE}
                        ${MPIEXEC_NUMPROC_FLAG} ${_PEAK_MPI_TEST_NPROCS}
                        ${MPIEXEC_PREFLAGS}
                        ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi
                        500000
                        ${MPIEXEC_POSTFLAGS})
        endif()
        math(EXPR N_CPU_DIV2 "${N_CPU}/2")
        if(OpenMP_FOUND)
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            )
            if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
                set_tests_properties(test_dgemm_mpi_strict
                    PROPERTIES
                        ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}"
                        TIMEOUT 90
                )
            endif()
        else()
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            )
            if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
                set_tests_properties(test_dgemm_mpi_strict
                    PROPERTIES
                        ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}"
                        TIMEOUT 90
                )
            endif()
        endif()
        set_tests_properties(test_dgemm_mpi
            PROPERTIES 
                PASS_REGULAR_EXPRESSION "cblas_dgemm[*]*\\|[ \t]+200\\|"
        )
        if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
            set_tests_properties(test_dgemm_mpi_strict
                PROPERTIES
                    PASS_REGULAR_EXPRESSION "cblas_dgemm[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
            )
        endif()
    endif()

    # Add test of heartbeat
    add_subdirectory(heartbeat)
    add_test(NAME test_heartbeat COMMAND ${PROJECT_BINARY_DIR}/test/heartbeat/test_heartbeat)
    if(OpenMP_FOUND)
        set_tests_properties(test_heartbeat
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_ON_ENV};${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_heartbeat
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_ON_ENV};${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_heartbeat
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "my_sleep_func\\|"
    )

    # Add Cuda Test cases
    if(BUILD_CUDA_PROFILE AND (CUDA_FOUND OR CUDAToolkit_FOUND))
        add_subdirectory(cuda)

        # add_test(NAME test_ompmpi COMMAND ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} 2 ${PROJECT_BINARY_DIR}/test/parallel_tests/test_omp_mpi)
        # set_tests_properties(test_omp_mpi PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=4;PEAK_TARGET=main;${PRELOAD_VAR}")

        # add_test(NAME test_mpi COMMAND ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} 2 ${PROJECT_BINARY_DIR}/test/parallel_tests/test_mpi)
        # set_tests_properties(test_mpi PROPERTIES ENVIRONMENT "PEAK_TARGET=main;${PRELOAD_VAR}")

        # add_test(NAME test_pthread COMMAND ${PROJECT_BINARY_DIR}/test/parallel_tests/test_pthread)
        # set_tests_properties(test_pthread PROPERTIES ENVIRONMENT "PEAK_TARGET=main;${PRELOAD_VAR}")
    endif()
    
    # Add test of dynamic library intercepting
    if(PEAK_BUILD_DYNAMIC_LIB_TESTS)
        add_subdirectory(dynamic_lib)
    endif()

    # Add test of memory allocation intercepting
    add_subdirectory(memory)
    add_test(NAME test_memory COMMAND ${PROJECT_BINARY_DIR}/test/memory/test_memory)
    set_tests_properties(test_memory 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=performAllocation,performFree;PEAK_MEMORY_PROFILE=TRUE;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_memory
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "performAllocation*\\|[ \t]+[0-9]+\\|.*performAllocation*\\|[ \t]+[0-9]+(\\.[0-9]+)?"
            PASS_REGULAR_EXPRESSION "performFree*\\|[ \t]+[0-9]+\\|.*performFree*\\|[ \t]+[0-9]+(\\.[0-9]+)?"
            PASS_REGULAR_EXPRESSION "Max usage \\(bytes\\):[ \t]*[0-9]+"
    )

endif()
