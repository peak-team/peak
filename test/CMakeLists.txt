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

    add_subdirectory(utils)
    add_subdirectory(exec_chain)
    add_subdirectory(detach_controller)
    add_subdirectory(detach_runtime)
    add_subdirectory(dlopen_controller)
    add_subdirectory(jit)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        if(TARGET test_signal_timer_abi)
            add_test(NAME test_preload_timer_create_abi
                COMMAND $<TARGET_FILE:test_signal_timer_abi>)
            set_tests_properties(test_preload_timer_create_abi
                PROPERTIES
                    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:peak>;PEAK_TEXT_OUTPUT=0;PEAK_STATSLOG=0"
                    PASS_REGULAR_EXPRESSION "peak_timer_create_abi_ok")
        endif()
        add_test(NAME test_unsafe_gum_prologue_policy_unit
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue_policy)
        set_tests_properties(test_unsafe_gum_prologue_policy_unit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_policy_unit_ok")

        add_test(NAME test_syscall_interceptor_policy_unit
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_syscall_interceptor_policy)
        set_tests_properties(test_syscall_interceptor_policy_unit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "syscall_interceptor_policy_ok")

        set(UNSAFE_GUM_PROLOGUE_GUARD_CASES)
        set(UNSAFE_GUM_PROLOGUE_CONSERVATIVE_GUARD_CASES)
        set(UNSAFE_GUM_PROLOGUE_ALLOWED_CASES)
        set(UNSAFE_GUM_PROLOGUE_CONSERVATIVE_ALLOWED_CASES)
        set(UNSAFE_GUM_PROLOGUE_NO_POLICY_SKIP_CASES)
        set(UNSAFE_GUM_PROLOGUE_OVERRIDE_CASES)
        set(UNSAFE_GUM_PROLOGUE_DIAGNOSTIC_CASE)
        set(UNSAFE_GUM_PROLOGUE_DIAGNOSTIC_POLICY "default")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            list(APPEND UNSAFE_GUM_PROLOGUE_GUARD_CASES
                "x86_dl_inc|peak_unsafe_gum_x86_dl_inc_target"
                "x86_dl_inc_rax_base|peak_unsafe_gum_x86_dl_inc_rax_base_target"
                "x86_dl_add|peak_unsafe_gum_x86_dl_add_target"
                "x86_edx_inc|peak_unsafe_gum_x86_edx_inc_target"
                "x86_edx_add|peak_unsafe_gum_x86_edx_add_target"
                "x86_dl_sub_zero|peak_unsafe_gum_x86_dl_sub_zero_target"
                "x86_edx_sub_zero|peak_unsafe_gum_x86_edx_sub_zero_target"
                "x86_dl_mov_zero|peak_unsafe_gum_x86_dl_mov_zero_target"
                "x86_rdx_xor|peak_unsafe_gum_x86_rdx_xor_target"
                "x86_edx_mov|peak_unsafe_gum_x86_edx_mov_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_CONSERVATIVE_GUARD_CASES
                "x86_high_movabs|peak_unsafe_gum_x86_high_movabs_target"
                "x86_ret_byte_immediate|peak_unsafe_gum_x86_ret_byte_immediate_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_ALLOWED_CASES
                "x86_plain_immediate|peak_unsafe_gum_x86_plain_immediate_target"
                "x86_high_movabs_benign|peak_unsafe_gum_x86_high_movabs_benign_target"
                "x86_long_copy_safe|peak_unsafe_gum_x86_long_copy_safe_target"
                "x86_dl_dead|peak_unsafe_gum_x86_dl_dead_target"
                "x86_rdx_dead|peak_unsafe_gum_x86_rdx_dead_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_CONSERVATIVE_ALLOWED_CASES
                "x86_plain_immediate|peak_unsafe_gum_x86_plain_immediate_target"
                "x86_high_movabs_benign|peak_unsafe_gum_x86_high_movabs_benign_target"
                "x86_long_copy_safe|peak_unsafe_gum_x86_long_copy_safe_target"
                "x86_dl_dead|peak_unsafe_gum_x86_dl_dead_target"
                "x86_rdx_dead|peak_unsafe_gum_x86_rdx_dead_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_OVERRIDE_CASES
                "x86_dl_inc|peak_unsafe_gum_x86_dl_inc_target|72"
                "x86_dl_inc_rax_base|peak_unsafe_gum_x86_dl_inc_rax_base_target|72"
                "x86_dl_add|peak_unsafe_gum_x86_dl_add_target|72"
                "x86_edx_inc|peak_unsafe_gum_x86_edx_inc_target|72"
                "x86_edx_add|peak_unsafe_gum_x86_edx_add_target|72"
                "x86_dl_sub_zero|peak_unsafe_gum_x86_dl_sub_zero_target|72"
                "x86_edx_sub_zero|peak_unsafe_gum_x86_edx_sub_zero_target|72"
                "x86_dl_mov_zero|peak_unsafe_gum_x86_dl_mov_zero_target|72")
            set(UNSAFE_GUM_PROLOGUE_DIAGNOSTIC_CASE
                "x86_dl_inc|peak_unsafe_gum_x86_dl_inc_target")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            list(APPEND UNSAFE_GUM_PROLOGUE_ALLOWED_CASES
                "arm64_ip_counter|peak_unsafe_gum_arm64_ip_counter_target"
                "arm64_ip_x17_counter|peak_unsafe_gum_arm64_ip_x17_counter_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_NO_POLICY_SKIP_CASES
                "arm64_ip_cursor|peak_unsafe_gum_arm64_ip_cursor_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_CONSERVATIVE_ALLOWED_CASES
                "arm64_ip_counter|peak_unsafe_gum_arm64_ip_counter_target"
                "arm64_ip_x17_counter|peak_unsafe_gum_arm64_ip_x17_counter_target")
            list(APPEND UNSAFE_GUM_PROLOGUE_CONSERVATIVE_GUARD_CASES
                "arm64_ip_cursor|peak_unsafe_gum_arm64_ip_cursor_target")
            set(UNSAFE_GUM_PROLOGUE_DIAGNOSTIC_CASE
                "arm64_ip_cursor|peak_unsafe_gum_arm64_ip_cursor_target")
            set(UNSAFE_GUM_PROLOGUE_DIAGNOSTIC_POLICY "conservative")
        endif()

        foreach(_unsafe_case IN LISTS UNSAFE_GUM_PROLOGUE_GUARD_CASES)
            string(REPLACE "|" ";" _unsafe_parts "${_unsafe_case}")
            list(GET _unsafe_parts 0 _unsafe_name)
            list(GET _unsafe_parts 1 _unsafe_target)
            add_test(NAME test_unsafe_gum_prologue_guard_${_unsafe_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_unsafe_gum_prologue_case.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --case ${_unsafe_name}
                    --target ${_unsafe_target}
                    --mode guard)
            set_tests_properties(test_unsafe_gum_prologue_guard_${_unsafe_name}
                PROPERTIES
                    ENVIRONMENT "PEAK_UNSAFE_GUM_PROLOGUE_POLICY=default"
                    PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_guard_check_ok"
                    FAIL_REGULAR_EXPRESSION "unsafe_prologue_mismatch")
        endforeach()

        foreach(_unsafe_case IN LISTS UNSAFE_GUM_PROLOGUE_CONSERVATIVE_GUARD_CASES)
            string(REPLACE "|" ";" _unsafe_parts "${_unsafe_case}")
            list(GET _unsafe_parts 0 _unsafe_name)
            list(GET _unsafe_parts 1 _unsafe_target)
            add_test(NAME test_unsafe_gum_prologue_conservative_guard_${_unsafe_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_unsafe_gum_prologue_case.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --case ${_unsafe_name}
                    --target ${_unsafe_target}
                    --mode guard)
            set_tests_properties(test_unsafe_gum_prologue_conservative_guard_${_unsafe_name}
                PROPERTIES
                    ENVIRONMENT "PEAK_UNSAFE_GUM_PROLOGUE_POLICY=conservative"
                    PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_guard_check_ok"
                    FAIL_REGULAR_EXPRESSION "unsafe_prologue_mismatch")
        endforeach()

        foreach(_unsafe_case IN LISTS UNSAFE_GUM_PROLOGUE_ALLOWED_CASES)
            string(REPLACE "|" ";" _unsafe_parts "${_unsafe_case}")
            list(GET _unsafe_parts 0 _unsafe_name)
            list(GET _unsafe_parts 1 _unsafe_target)
            add_test(NAME test_unsafe_gum_prologue_default_allows_${_unsafe_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_unsafe_gum_prologue_case.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --case ${_unsafe_name}
                    --target ${_unsafe_target}
                    --mode default-allows)
            set_tests_properties(test_unsafe_gum_prologue_default_allows_${_unsafe_name}
                PROPERTIES
                    ENVIRONMENT "PEAK_UNSAFE_GUM_PROLOGUE_POLICY=default"
                    PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_default_allows_ok")
        endforeach()

        foreach(_unsafe_case IN LISTS UNSAFE_GUM_PROLOGUE_NO_POLICY_SKIP_CASES)
            string(REPLACE "|" ";" _unsafe_parts "${_unsafe_case}")
            list(GET _unsafe_parts 0 _unsafe_name)
            list(GET _unsafe_parts 1 _unsafe_target)
            add_test(NAME test_unsafe_gum_prologue_default_no_policy_skip_${_unsafe_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_unsafe_gum_prologue_case.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --case ${_unsafe_name}
                    --target ${_unsafe_target}
                    --mode no-policy-skip)
            set_tests_properties(test_unsafe_gum_prologue_default_no_policy_skip_${_unsafe_name}
                PROPERTIES
                    ENVIRONMENT "PEAK_UNSAFE_GUM_PROLOGUE_POLICY=default"
                    PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_no_policy_skip_ok")
        endforeach()

        foreach(_unsafe_case IN LISTS UNSAFE_GUM_PROLOGUE_CONSERVATIVE_ALLOWED_CASES)
            string(REPLACE "|" ";" _unsafe_parts "${_unsafe_case}")
            list(GET _unsafe_parts 0 _unsafe_name)
            list(GET _unsafe_parts 1 _unsafe_target)
            add_test(NAME test_unsafe_gum_prologue_conservative_allows_${_unsafe_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_unsafe_gum_prologue_case.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --case ${_unsafe_name}
                    --target ${_unsafe_target}
                    --mode default-allows)
            set_tests_properties(test_unsafe_gum_prologue_conservative_allows_${_unsafe_name}
                PROPERTIES
                    ENVIRONMENT "PEAK_UNSAFE_GUM_PROLOGUE_POLICY=conservative"
                    PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_default_allows_ok")
        endforeach()

        foreach(_unsafe_case IN LISTS UNSAFE_GUM_PROLOGUE_OVERRIDE_CASES)
            string(REPLACE "|" ";" _unsafe_parts "${_unsafe_case}")
            list(GET _unsafe_parts 0 _unsafe_name)
            list(GET _unsafe_parts 1 _unsafe_target)
            list(GET _unsafe_parts 2 _unsafe_expected_byte)
            add_test(NAME test_unsafe_gum_prologue_override_exercises_${_unsafe_name}
                COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_unsafe_gum_prologue_case.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_unsafe_gum_prologue
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --case ${_unsafe_name}
                    --target ${_unsafe_target}
                    --mode override-exercises
                    --expected-byte ${_unsafe_expected_byte})
            set_tests_properties(test_unsafe_gum_prologue_override_exercises_${_unsafe_name}
                PROPERTIES
                    PASS_REGULAR_EXPRESSION "unsafe_gum_prologue_override_exercises_ok")
        endforeach()
    endif()
    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
       PEAK_GUM_PEAK_PC_API_AVAILABLE AND
       TARGET peak_detach_helper)
        add_test(NAME test_detach_hotloop_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --threads 4
                    --seconds 2
                    --wait-for-hook-before-start)
        set_tests_properties(test_detach_hotloop_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/hotloop-strict-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "trace_detach_success=1"
                TIMEOUT 20)
        add_test(NAME test_detach_hotloop_signal_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --threads 32
                    --seconds 2
                    --request-detach-after-start)
        set_tests_properties(test_detach_hotloop_signal_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=128;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_DETACH_COUNT=1000000000000000000;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/signal-hotloop-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "trace_detach_success=1"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 45)

        add_test(NAME test_detach_hotloop_signal_thread_spawn_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --threads 8
                    --seconds 3
                    --spawn-transient-threads
                    --spawner-threads 4
                    --wait-for-hook-before-start)
        set_tests_properties(test_detach_hotloop_signal_thread_spawn_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=256;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/signal-thread-spawn-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "spawned_threads=[1-9][0-9]*.*trace_detach_success=1"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 60)

        add_test(NAME test_detach_hotloop_request_after_start_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --threads 4
                    --seconds 2
                    --request-detach-after-start)
        set_tests_properties(test_detach_hotloop_request_after_start_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_DETACH_COUNT=1000000000000000000;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/hotloop-request-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "trace_detach_success=1"
                TIMEOUT 20)
        add_test(NAME test_detach_hotloop_signal_thread_spawn_request_after_start_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --threads 8
                    --seconds 3
                    --spawn-transient-threads
                    --spawner-threads 4
                    --request-detach-after-start)
        set_tests_properties(test_detach_hotloop_signal_thread_spawn_request_after_start_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=256;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_DETACH_COUNT=1000000000000000000;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/signal-thread-spawn-request-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "spawned_threads=[1-9][0-9]*.*trace_detach_success=1"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 60)

        add_test(NAME test_detach_hotloop_signal_blocked_delivery_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-blocked-delivery-check
                    --threads 4)
        set_tests_properties(test_detach_hotloop_signal_blocked_delivery_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-blocked-delivery-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_blocked_delivery_ok.*blocked_signal_threads=4")

        add_test(NAME test_detach_hotloop_signal_user_collision_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-user-collision-check)
        set_tests_properties(test_detach_hotloop_signal_user_collision_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-user-collision-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_user_collision_ok.*detach_success=1.*migration_count=[1-9][0-9]*.*handler_preserved=1.*raw_query_protected=1.*raw_query_readonly_failed=1.*mask_preserved=1.*wait_preserved=1.*signalfd_preserved=1.*timer_preserved=1.*mq_migrated=1.*syscall_migrated=1.*worker_calls=[1-9][0-9]*")

        add_test(NAME test_detach_hotloop_signal_forced_collision_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-forced-collision-check)
        set_tests_properties(test_detach_hotloop_signal_forced_collision_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_SIGNAL=SIGRTMAX-0;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-forced-collision-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_forced_collision_ok.*detach_success=1.*migration_count=0.*denied=1.*invalid_pointer_guard=1.*mq_denied=1.*aio_denied=1.*syscall_denied=1.*worker_calls=[1-9][0-9]*")

        add_test(NAME test_detach_hotloop_signal_bad_cookie_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-bad-cookie-check)
        set_tests_properties(test_detach_hotloop_signal_bad_cookie_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-bad-cookie-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_bad_cookie_ok.*contamination_seen=1.*detach_blocked=1")

        add_test(NAME test_detach_hotloop_signal_stale_cookie_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-stale-cookie-check)
        set_tests_properties(test_detach_hotloop_signal_stale_cookie_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-stale-cookie-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_stale_cookie_ok.*stale_ignored=1.*detach_success=1.*unexpected_count=0.*stale_count_delta=[1-9][0-9]*")

        add_test(NAME test_detach_hotloop_signal_stale_cookie_during_stop_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-stale-cookie-during-stop-check)
        set_tests_properties(test_detach_hotloop_signal_stale_cookie_during_stop_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=1;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-stale-cookie-during-stop-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_stale_cookie_during_stop_ok.*stale_ignored=1.*detach_success=1.*reattach_success=1.*unexpected_count=0.*stale_count_delta=[1-9][0-9]*")

        add_test(NAME test_detach_hotloop_signal_future_cookie_during_stop_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --signal-future-cookie-during-stop-check)
        set_tests_properties(test_detach_hotloop_signal_future_cookie_during_stop_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=1;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-future-cookie-during-stop-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "signal_future_cookie_during_stop_ok.*contamination_seen=1.*reattach_blocked=1.*unexpected_count_delta=[1-9][0-9]*.*stale_count_delta=0")

        add_test(NAME test_detach_hotloop_signal_pthread_create_gate_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --pthread-gate-race-check
                    --threads 4
                    --creator-threads 4)
        set_tests_properties(test_detach_hotloop_signal_pthread_create_gate_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=128;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;PEAK_TEST_ENABLE_STRICT_GATE_DELAY=1;PEAK_TEST_STRICT_GATE_DELAY_US=1000000;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-signal-pthread-create-gate-trace.csv;${PRELOAD_VAR}"
                TIMEOUT 60
                PASS_REGULAR_EXPRESSION "pthread_gate_race_ok.*creation_blocked_ok=1.*child_started_while_gate=0")

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
                    --wait-for-hook-before-start
                    --disable-reattach
                    --peak-cost 0
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-trace
                    --require-trace-diagnostics
                    --require-detach-source per-target-heartbeat
                    --min-detach-request-calls 1
                    --require-detach-batch-size 2
                    --fail-on-transition-skips)
        set_tests_properties(test_detach_hotloop_batch_strict
            PROPERTIES
                TIMEOUT 45
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok")
        add_test(NAME test_detach_hotloop_global_source_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 30
                    --disable-reattach
                    --disable-per-target-heartbeat
                    --enable-global-heartbeat
                    --wait-for-hook-before-start
                    --peak-cost 0
                    --global-overhead-ratio 0.000001
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-global-source-trace
                    --require-trace-diagnostics
                    --require-detach-source global-overhead-recovery
                    --min-detach-request-calls 1
                    --fail-on-transition-skips)
        set_tests_properties(test_detach_hotloop_global_source_strict
            PROPERTIES
                TIMEOUT 45
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok")
        add_test(NAME test_detach_hotloop_global_ignores_cold_one_shot_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 30
                    --disable-reattach
                    --disable-per-target-heartbeat
                    --enable-global-heartbeat
                    --cold-one-shot-target
                    --wait-for-hook-before-start
                    --peak-cost 0
                    --overhead-ratio 0.005
                    --global-overhead-ratio 0.000001
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-global-cold-trace
                    --require-trace-diagnostics
                    --require-detach-source global-overhead-recovery
                    --min-detach-request-calls 1
                    --fail-on-transition-skips)
        set_tests_properties(test_detach_hotloop_global_ignores_cold_one_shot_strict
            PROPERTIES
                TIMEOUT 45
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok")
        add_test(NAME test_detach_hotloop_cold_one_shot_policy_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 30
                    --enable-global-heartbeat
                    --cold-one-shot-target
                    --peak-cost 0
                    --overhead-ratio 0.005
                    --global-overhead-ratio 0.000001
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-cold-policy-trace
                    --require-trace-diagnostics
                    --fail-on-transition-skips)
        set_tests_properties(test_detach_hotloop_cold_one_shot_policy_strict
            PROPERTIES
                TIMEOUT 45
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok")
        if(TARGET test_detach_fake_helper)
            add_test(NAME test_detach_hotloop_batch_retry_preserves_pending_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-batch-retry-check)
            set_tests_properties(test_detach_hotloop_batch_retry_preserves_pending_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target,peak_detach_hot_target_two;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=synthetic-stop-file-once;FAKE_DETACH_HELPER_STOP_PC_FILE=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-retry-pc.txt;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-retry-trace.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_batch_retry_ok"
                    TIMEOUT 30)
            add_test(NAME test_detach_hotloop_no_trace_pending_age_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-no-trace-pending-age-check)
            set_tests_properties(test_detach_hotloop_no_trace_pending_age_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_CONTROLLER_MAX_PENDING_AGE_MS=20;PEAK_CONTROLLER_MAX_RETRY_COUNT=0;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_no_trace_pending_age_ok"
                    FAIL_REGULAR_EXPRESSION "Abandoning .* for hook"
                    TIMEOUT 30)
            add_test(NAME test_detach_hotloop_trace_retry_abandoned_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-trace-retry-abandoned-check)
            set_tests_properties(test_detach_hotloop_trace_retry_abandoned_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_CONTROLLER_MAX_PENDING_AGE_MS=20;PEAK_CONTROLLER_MAX_RETRY_COUNT=0;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-trace-retry-abandoned.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_trace_retry_abandoned_ok"
                    FAIL_REGULAR_EXPRESSION "Abandoning .* for hook"
                    TIMEOUT 30)
            add_test(NAME test_detach_hotloop_trace_heartbeat_retry_abandoned_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-trace-heartbeat-retry-abandoned-check)
            set_tests_properties(test_detach_hotloop_trace_heartbeat_retry_abandoned_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_HEARTBEAT_MUTATION_MAX_PENDING_AGE_MS=20;PEAK_HEARTBEAT_MUTATION_MAX_RETRY_COUNT=0;PEAK_HEARTBEAT_MUTATION_FAILURE_COOLDOWN_MS=1000;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-trace-heartbeat-retry-abandoned.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_trace_heartbeat_retry_abandoned_ok"
                    FAIL_REGULAR_EXPRESSION "Abandoning .* for hook"
                    TIMEOUT 30)
            add_test(NAME test_detach_hotloop_trace_heartbeat_pacing_deferred_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-trace-heartbeat-pacing-deferred-check)
            set_tests_properties(test_detach_hotloop_trace_heartbeat_pacing_deferred_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_HEARTBEAT_MUTATION_MAX_RETRY_COUNT=0;PEAK_HEARTBEAT_MUTATION_FAILURE_COOLDOWN_MS=1000;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-trace-heartbeat-pacing-deferred.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_trace_heartbeat_pacing_deferred_ok"
                    FAIL_REGULAR_EXPRESSION "Abandoning .* for hook"
                    TIMEOUT 30)
            add_test(NAME test_detach_hotloop_trace_heartbeat_bootstrap_pacing_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --controller-trace-heartbeat-bootstrap-pacing-check)
            set_tests_properties(test_detach_hotloop_trace_heartbeat_bootstrap_pacing_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.005;PEAK_GLOBAL_OVERHEAD_RATIO=0.05;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_HEARTBEAT_MUTATION_TIMEOUT_MS=250;PEAK_HEARTBEAT_MUTATION_MAX_RETRY_COUNT=0;PEAK_HEARTBEAT_MUTATION_FAILURE_COOLDOWN_MS=1000;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-trace-heartbeat-bootstrap-pacing.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_trace_heartbeat_bootstrap_pacing_ok"
                    FAIL_REGULAR_EXPRESSION "late-bootstrap heartbeat detach was pacing-blocked|pacing-deferred"
                    TIMEOUT 30)
            add_test(NAME test_detach_helper_warmup_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --helper-warmup-check)
            set_tests_properties(test_detach_helper_warmup_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-helper-warmup.log;FAKE_DETACH_HELPER_TRUNCATE_LOG_ON_START=1;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "helper_warmup_ok"
                    TIMEOUT 30)
            add_test(NAME test_detach_auto_deferred_helper_first_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --auto-deferred-helper-first-check)
            set_tests_properties(test_detach_auto_deferred_helper_first_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=stop-permission;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-auto-deferred-helper-first.log;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "auto_deferred_helper_first_ok"
                    TIMEOUT 30)
            add_test(NAME test_detach_auto_deferred_helper_success_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                        --auto-deferred-helper-first-check)
            set_tests_properties(test_detach_auto_deferred_helper_success_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_runtime/peak-auto-deferred-helper-success.log;PEAK_TEST_AUTO_DEFERRED_EXPECT_EVACUATE=1;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "auto_deferred_helper_first_ok"
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
                    --seconds 4
                    --samples 1
                    --timeout 45
                    --paired-targets
                    --request-detach-after-start
                    --overhead-ratio 0.01
                    --heartbeat-interval 0.5
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-batch-reattach-trace
                    --require-detach-batch-size 2
                    --require-reattach-batch-size 2
                    --allow-split-reattach-batch)
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
                    --seconds 4
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
                    --samples 1
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
        add_test(NAME test_detach_hotloop_helper_verify_unstopped_retry_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_hotloop_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 90
                    --overhead-ratio 0.000001
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-helper-verify-unstopped-retry-trace
                    --require-trace-diagnostics
                    --detach-backend helper
                    --detach-helper $<TARGET_FILE:test_peak_detach_helper>
                    --helper-verify-retry-log-prefix ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-hotloop-helper-verify-unstopped-retry
                    --disable-reattach
                    --skip-helper-unavailable)
        set_tests_properties(test_detach_hotloop_helper_verify_unstopped_retry_strict
            PROPERTIES
                TIMEOUT 120
                PASS_REGULAR_EXPRESSION "hotloop_trace_check_ok"
                SKIP_RETURN_CODE 77)
        option(PEAK_ENABLE_MANUAL_DETACH_MANY_TARGETS_STRESS
            "Enable manual many-target strict detach heartbeat storm CTest entry" OFF)
        if(PEAK_ENABLE_MANUAL_DETACH_MANY_TARGETS_STRESS)
            foreach(_manual_backend IN ITEMS auto helper signal)
                add_test(NAME manual_detach_many_targets_heartbeat_storm_${_manual_backend}
                    COMMAND ${Python3_EXECUTABLE}
                            ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_many_targets_stress.py
                            --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_many_targets_stress
                            --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                            --helper $<TARGET_FILE:test_peak_detach_helper>
                            --backend ${_manual_backend}
                            --threads 56
                            --samples 1
                            --seconds 180
                            --targets 832
                            --hot-targets 256
                            --burst 128
                            --all-target-period 0
                            --hot-rotation-period-us 50000
                            --active-hot-targets 64
                            --cold-sweep-period 4096
                            --peak-cost ""
                            --overhead-ratio 0.005
                            --global-overhead-ratio 0.05
                            --global-detach-factor 1.2
                            --global-reattach-factor 0.95
                            --heartbeat-interval 0.01
                            --extra-env PEAK_HEARTBEAT_MUTATION_FAILURE_COOLDOWN_MS=0
                            --sample-counts-interval-ms 50
                            --min-trace-rows 1
                            --min-total-calls 20000000
                            --min-rotating-hot-calls 1000000
                            --min-cold-sweep-calls 1000
                            --max-reattached-cold-hooks 0
                            --min-detach-success 128
                            --min-distinct-detached-hooks 64
                            --min-distinct-batches 3
                            --min-global-detach-success 64
                            --min-first-global-detach-batch-size 32
                            --min-first-global-detach-batch-hot-hooks 32
                            --max-first-global-detach-batch-time-s 5
                            --min-global-detached-hot-hooks 64
                            --min-global-reattached-hot-hooks 64
                            --min-global-oscillating-hot-hooks 64
                            --max-global-overhead-recovery-detach-success 64
                            --min-reattach-success 64
                            --min-distinct-reattached-hooks 64
                            --min-global-reattach-success 64
                            --min-reattach-success-per-sec 0.05
                            --min-distinct-oscillating-hot-hooks 64
                            --min-distinct-global-oscillating-hot-hooks 64
                            --min-sampling-bins-with-reattach 8
                            --min-sampling-bins-with-hot-reattach 8
                            --min-sampling-bins-with-global-hot-reattach 8
                            --global-hot-reattach-final-window-s 30
                            --min-global-hot-reattach-final-window 8
                            --min-global-hot-reattach-final-window-distinct-hooks 8
                            --max-global-hot-reattach-gap-s 30
                            --min-post-detach-bins-with-attached-hot 8
                            --min-sampling-reattach-span-s 60
                            --min-reattach-sampled-cycles 64
                            --min-distinct-reattach-sampled-hot-hooks 64
                            --min-distinct-global-reattach-sampled-hot-hooks 64
                            --min-global-hot-probe-detached-dwell-count 128
                            --min-probe-detached-dwell-s 4.5
                            --min-probe-dwell-hot-increases 128
                            --min-probe-dwell-hot-increase-hot-hooks 128
                            --min-probe-dwell-hot-increase-global-hot-hooks 64
                            --max-probe-dwell-invalid-count 0
                            --min-trace-ordered-reattach-cycles 128
                            --min-trace-ordered-reattach-distinct-hot-hooks 128
                            --min-trace-ordered-global-hot-reattach-cycles 64
                            --min-trace-ordered-global-hot-reattach-distinct-hooks 64
                            --min-reattach-sampled-span-s 60
                            --min-reattach-sampled-call-delta-total 1000
                            --min-strict-reattach-ordered-cycles 4
                            --min-strict-reattach-ordered-distinct-hot-hooks 4
                            --min-strict-global-reattach-ordered-hot-cycles 4
                            --min-strict-global-reattach-ordered-distinct-hot-hooks 4
                            --min-strict-reattach-ordered-call-delta-total 1000
                            --min-strict-reattach-ordered-sample-delta-total 1
                            --min-strict-reattach-ordered-sample-bins 1
                            --require-peak-text-output
                            --require-csv-report
                            --require-shutdown-trace
                            --require-exact-shutdown-trace
                            --require-single-stats-file
                            --fail-on-malformed-csv-report
                            --max-distinct-detached-hooks 832
                            --max-detach-success 10000
                            --max-batch 64
                            --max-top-hook-transition-share 0.20
                            --max-top-global-hook-transition-share 0.10
                            --max-reattach-success-per-sec 80
                            --max-batches-per-sec 3.0
                            --max-hook-oscillations 64
                            --max-pacing-blocked 32
                            --max-global-no-admission 32
                            --min-hot-profiled-targets 192
                            --min-hot-profiled-target-ratio 0.75
                            --min-hot-total-coverage 0.000001
                            --max-hot-top-profiled-share 0.30
                            --min-liveness-samples 8
                            --min-liveness-window-count 3
                            --max-liveness-final-effective-overhead 0.06
                            --max-liveness-window-avg-effective-overhead 0.06
                            --max-liveness-window-max-effective-overhead 0.06
                            --max-stop-window-per-sec 0.05
                            --max-stop-window-bin-per-sec 0.30
                            --max-stop-window-over-budget-bins 0
                            --max-consecutive-stop-window-over-budget-bins 0
                            --max-stop-window-us 2000000
                            --min-distinct-called-targets 832
                            --min-distinct-called-hot-targets 256
                            --min-distinct-called-cold-targets 576
                            --timeout 240
                            --work-dir ${PROJECT_BINARY_DIR}/test/detach_runtime/manual-many-targets-heartbeat-storm-${_manual_backend})
                set_tests_properties(manual_detach_many_targets_heartbeat_storm_${_manual_backend}
                    PROPERTIES
                        LABELS "manual;stress;strict-detach"
                        PASS_REGULAR_EXPRESSION "many_targets_stress_all_ok"
                        TIMEOUT 300)
            endforeach()
        endif()
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
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_runtime/run_detach_stale_threads_trace_check.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_runtime/test_detach_stale_threads
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --trace-path ${PROJECT_BINARY_DIR}/test/detach_runtime/peak-stale-reattach-trace.csv
                    --timeout 90
                    --stale-threads 48
                    --stale-calls 2048
                    --overhead-ratio 0.25
                    --
                    --active-threads 16
                    --stale-threads 48
                    --stale-calls 2048
                    --stale-ready-timeout 30
                    --active-after-stale
                    --seconds 3
                    --request-detach-after-start)
        set_tests_properties(test_detach_stale_threads_reattach_strict
            PROPERTIES
                ENVIRONMENT "PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
                PASS_REGULAR_EXPRESSION "stale_threads_trace_check_ok"
                TIMEOUT 100)
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
    add_test(NAME test_rank_local_mpi_env_suppresses_text COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
    set_tests_properties(test_rank_local_mpi_env_suppresses_text
        PROPERTIES
            ENVIRONMENT "PMI_SIZE=4;PEAK_TARGET=my_sleep_func;PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_rank_local_mpi_env_statslog;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            FAIL_REGULAR_EXPRESSION "PEAK Library|function statistics"
    )
    add_test(NAME test_rank_local_mpi_env_text_override COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
    set_tests_properties(test_rank_local_mpi_env_text_override
        PROPERTIES
            ENVIRONMENT "PMI_SIZE=4;PEAK_TARGET=my_sleep_func;PEAK_TEXT_OUTPUT=1;PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_rank_local_mpi_env_override_statslog;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            PASS_REGULAR_EXPRESSION "PEAK Library"
    )
    add_test(NAME test_verbosity_quiet_keeps_report COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
    set_tests_properties(test_verbosity_quiet_keeps_report
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=my_sleep_func;PEAK_VERBOSITY=quiet;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            PASS_REGULAR_EXPRESSION "PEAK Library")
    add_test(NAME test_verbosity_silent_suppresses_report COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
    set_tests_properties(test_verbosity_silent_suppresses_report
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=my_sleep_func;PEAK_VERBOSITY=silent;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            FAIL_REGULAR_EXPRESSION "PEAK Library|function statistics|Estimated overhead")
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
        string(JOIN " " _PEAK_MPIEXEC_PREFLAGS_STRING ${MPIEXEC_PREFLAGS})
        string(JOIN " " _PEAK_MPIEXEC_POSTFLAGS_STRING ${MPIEXEC_POSTFLAGS})
        add_test(NAME test_dgemm_mpi
            COMMAND ${MPIEXEC_EXECUTABLE}
                    ${MPIEXEC_NUMPROC_FLAG} ${_PEAK_MPI_TEST_NPROCS}
                    ${MPIEXEC_PREFLAGS}
                    ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi
                    ${MPIEXEC_POSTFLAGS})
        add_test(NAME test_mpi_exit_before_finalize
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode no-finalize
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_no_finalize_nonzero_exit
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode no-finalize-nonzero
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_clean_exit
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_clean_exit_output_local
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-local
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_clean_exit_output_mpi
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-mpi
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_clean_exit_output_mpi_reducer_fail
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-mpi-reducer-fail
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 15)
        add_test(NAME test_mpi_finalize_clean_exit_output_mpi_intel_default
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-mpi-intel-default
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_clean_exit_output_mpi_intel_real_finalize
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-mpi-intel-real-finalize
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_clean_exit_output_mpi_strict_detach
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-mpi
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --require-pinned-finalize
                    --require-detach-trace
                    --timeout 45)
        add_test(NAME test_mpi_finalize_target_is_not_self_profiled
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-mpi
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --include-finalize-target
                    --forbid-stats-function MPI_Finalize)
        add_test(NAME test_mpi_finalize_clean_exit_output_socket_bad_host
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-socket-bad-host
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 10)
        add_test(NAME test_mpi_finalize_clean_exit_output_socket_token_mismatch
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-socket-token-mismatch
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 10)
        add_test(NAME test_mpi_finalize_clean_exit_output_socket_bad_host_no_fallback
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-socket-bad-host-no-fallback
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 10)
        add_test(NAME test_mpi_finalize_clean_exit_output_socket_token_mismatch_no_fallback
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-socket-token-mismatch-no-fallback
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 10)
        add_test(NAME test_mpi_finalize_clean_exit_output_socket_release_fail
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-clean-output-socket-release-fail
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 10)
        add_test(NAME test_mpi_finalize_socket_post_work
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-socket-post-work
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 30)
        add_test(NAME test_mpi_finalize_defer_post_work
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-defer-post-work
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET test_detach_fake_helper)
            add_test(NAME test_mpi_startup_calibration_does_not_start_helper
                COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                        --mpiexec ${MPIEXEC_EXECUTABLE}
                        --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                        --nprocs ${_PEAK_MPI_TEST_NPROCS}
                        --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                        --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                        --mode startup-calibration-no-helper
                        --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                        --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                        --timeout 20)
            set_tests_properties(test_mpi_startup_calibration_does_not_start_helper
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;PEAK_TEST_PTRACE_SCOPE=0;FAKE_DETACH_HELPER_SCENARIO=success-zero"
                    PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=startup-calibration-no-helper"
            )
        endif()
        add_test(NAME test_mpi_finalize_then_nonzero_exit
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-nonzero
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_subset_finalize_then_nonzero_exit
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode subset-finalize-nonzero
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_subset_finalize_clean_exit
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode subset-finalize-clean
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_subset_finalize_clean_exit_output_mpi
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode subset-finalize-clean-collective
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_subset_finalize_handoff
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode subset-finalize-handoff
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}"
                    --timeout 10)
        add_test(NAME test_mpi_exec_failure_rank0_checkpoint
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode exec-failure-rank0
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_exec_success_rank0_checkpoint
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode exec-success-rank0
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
        add_test(NAME test_mpi_finalize_then_nonzero_return
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/dgemm_mpi/run_mpi_lifecycle_check.py
                    --mpiexec ${MPIEXEC_EXECUTABLE}
                    --nproc-flag=${MPIEXEC_NUMPROC_FLAG}
                    --nprocs ${_PEAK_MPI_TEST_NPROCS}
                    --exe ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_mpi_exit_before_finalize
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --mode finalize-return-nonzero
                    --preflags="${_PEAK_MPIEXEC_PREFLAGS_STRING}"
                    --postflags="${_PEAK_MPIEXEC_POSTFLAGS_STRING}")
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
                    ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;PEAK_MPI_COLLECTIVE_OUTPUT=1;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            )
            set_tests_properties(test_mpi_exit_before_finalize
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_no_finalize_nonzero_exit
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_local
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_reducer_fail
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_intel_default
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_intel_real_finalize
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_strict_detach
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;PEAK_MPI_EXIT_LOOPS=200000;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/dgemm_mpi/mpi-finalize-strict-trace.csv"
            )
            set_tests_properties(test_mpi_finalize_target_is_not_self_profiled
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_bad_host
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_token_mismatch
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_bad_host_no_fallback
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_token_mismatch_no_fallback
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_release_fail
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_socket_post_work
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_defer_post_work
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_then_nonzero_exit
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_then_nonzero_exit
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_clean_exit
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_clean_exit_output_mpi
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_handoff
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_exec_failure_rank0_checkpoint
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_exec_success_rank0_checkpoint
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_then_nonzero_return
                PROPERTIES
                    ENVIRONMENT "OMP_NUM_THREADS=1;PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
                set_tests_properties(test_dgemm_mpi_strict
                    PROPERTIES
                        ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;PEAK_MPI_COLLECTIVE_OUTPUT=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}"
                        TIMEOUT 90
                )
            endif()
        else()
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;PEAK_MPI_COLLECTIVE_OUTPUT=1;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            )
            set_tests_properties(test_mpi_exit_before_finalize
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_no_finalize_nonzero_exit
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_local
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_reducer_fail
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_intel_default
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_intel_real_finalize
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_strict_detach
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;PEAK_MPI_EXIT_LOOPS=200000;PEAK_MAX_NUM_THREADS=64;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0.000000000001;PEAK_DETACH_COUNT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/dgemm_mpi/mpi-finalize-strict-trace.csv"
            )
            set_tests_properties(test_mpi_finalize_target_is_not_self_profiled
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_bad_host
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_token_mismatch
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_bad_host_no_fallback
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_token_mismatch_no_fallback
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_clean_exit_output_socket_release_fail
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_socket_post_work
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_defer_post_work
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_then_nonzero_exit
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_then_nonzero_exit
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_clean_exit
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_clean_exit_output_mpi
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_subset_finalize_handoff
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_exec_failure_rank0_checkpoint
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_exec_success_rank0_checkpoint
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            set_tests_properties(test_mpi_finalize_then_nonzero_return
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_mpi_exit_target;${HEARTBEAT_OFF_ENV}"
            )
            if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
                set_tests_properties(test_dgemm_mpi_strict
                    PROPERTIES
                        ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;PEAK_MPI_COLLECTIVE_OUTPUT=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}"
                        TIMEOUT 90
                )
            endif()
        endif()
        set_tests_properties(test_dgemm_mpi
            PROPERTIES 
                PASS_REGULAR_EXPRESSION "cblas_dgemm[*]*\\|[ \t]+200\\|"
        )
        set(_PEAK_MPI_LIFECYCLE_FATAL_REGEX "Caught signal|Segmentation fault|SIGSEGV|signal 11|Fatal error|MPII_finalize|ofi_cq_read|hwloc")
        set_tests_properties(test_mpi_exit_before_finalize
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=no-finalize"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_no_finalize_nonzero_exit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=no-finalize-nonzero"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_local
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-local"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_mpi
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-mpi"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_reducer_fail
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-mpi-reducer-fail"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_intel_default
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-mpi-intel-default"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_intel_real_finalize
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-mpi-intel-real-finalize"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_mpi_strict_detach
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-mpi"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 45)
        set_tests_properties(test_mpi_finalize_target_is_not_self_profiled
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-mpi"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}|unexpected stats row"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_clean_exit_output_socket_bad_host
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-socket-bad-host"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 15)
        set_tests_properties(test_mpi_finalize_clean_exit_output_socket_token_mismatch
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-socket-token-mismatch"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 15)
        set_tests_properties(test_mpi_finalize_clean_exit_output_socket_bad_host_no_fallback
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-socket-bad-host-no-fallback"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 15)
        set_tests_properties(test_mpi_finalize_clean_exit_output_socket_token_mismatch_no_fallback
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-socket-token-mismatch-no-fallback"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 15)
        set_tests_properties(test_mpi_finalize_clean_exit_output_socket_release_fail
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-clean-output-socket-release-fail"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 15)
        set_tests_properties(
            test_mpi_finalize_clean_exit_output_mpi_reducer_fail
            test_mpi_finalize_clean_exit_output_socket_bad_host
            test_mpi_finalize_clean_exit_output_socket_token_mismatch
            test_mpi_finalize_clean_exit_output_socket_bad_host_no_fallback
            test_mpi_finalize_clean_exit_output_socket_token_mismatch_no_fallback
            test_mpi_finalize_clean_exit_output_socket_release_fail
            test_mpi_finalize_socket_post_work
            PROPERTIES
                RESOURCE_LOCK peak_mpi_socket_aggregation)
        set_tests_properties(test_mpi_finalize_socket_post_work
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-socket-post-work"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 45)
        set_tests_properties(test_mpi_finalize_defer_post_work
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-defer-post-work"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_then_nonzero_exit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-nonzero"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_subset_finalize_then_nonzero_exit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=subset-finalize-nonzero"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_subset_finalize_clean_exit
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=subset-finalize-clean"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_subset_finalize_clean_exit_output_mpi
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=subset-finalize-clean-collective"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_subset_finalize_handoff
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=subset-finalize-handoff"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 15)
        set_tests_properties(test_mpi_exec_failure_rank0_checkpoint
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=exec-failure-rank0"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_exec_success_rank0_checkpoint
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=exec-success-rank0"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
        set_tests_properties(test_mpi_finalize_then_nonzero_return
            PROPERTIES
                PASS_REGULAR_EXPRESSION "mpi_lifecycle_check_ok mode=finalize-return-nonzero"
                FAIL_REGULAR_EXPRESSION "${_PEAK_MPI_LIFECYCLE_FATAL_REGEX}"
                TIMEOUT 30)
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
