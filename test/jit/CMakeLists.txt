if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    option(PEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS
        "Enable opt-in PEAK-preloaded real JIT runtime tests" OFF)

    add_executable(test_jit_mmap_fixture test_jit_mmap_fixture.c)
    target_compile_features(test_jit_mmap_fixture PRIVATE c_std_11)

    add_test(NAME test_jit_profiling_mmap_perfmap
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode both
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_auto_no_helper
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode positive
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_auto_no_helper
        PROPERTIES
            ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_signal
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode positive
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_signal
        PROPERTIES
            ENVIRONMENT "PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_SIGNAL_RESERVE_EARLY=never;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_final_drain
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode final-drain
                --iterations 1
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_final_drain
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_final_drain_retry
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode final-drain-retry
                --iterations 1
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_final_drain_retry
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_final_drain_stale_then_valid
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode final-drain-stale-then-valid
                --iterations 1
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_final_drain_stale_then_valid
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_retry_attach
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode retry-attach
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_retry_attach
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_partial_record
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode partial-record
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_partial_record
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_pre_exec_retry
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode pre-exec
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_pre_exec_retry
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_two_generations
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode two-generations
                --iterations 1000000
                --metadata-sleep-us 200000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_two_generations
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_two_generations_heartbeat
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode two-generations-heartbeat
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_two_generations_heartbeat
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_stale_then_valid
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode stale-then-valid
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_stale_then_valid
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_stale_then_valid_default_timeout
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode stale-then-valid-default-timeout
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_stale_then_valid_default_timeout
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_duplicate_record
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode duplicate-record
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_duplicate_record
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_malformed_then_valid
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode malformed-then-valid
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_malformed_then_valid
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_overlong_then_valid
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode overlong-then-valid
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_overlong_then_valid
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_v8_js_optimized_alias
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode v8-js-optimized
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_v8_js_optimized_alias
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_v8_js_csv_name
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode v8-js-csv-name
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_v8_js_csv_name
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_profiling_mmap_perfmap_v8_lazycompile_optimized_alias
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode v8-lazycompile-optimized
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_v8_lazycompile_optimized_alias
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "jit_profiling_check_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_framework_metadata_v8_name_patterns
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_framework_metadata_check.py
                --framework v8-name-patterns
                --timeout 30)
    set_tests_properties(test_jit_framework_metadata_v8_name_patterns
        PROPERTIES
            PASS_REGULAR_EXPRESSION "v8_name_patterns_ok"
            TIMEOUT 45)

    add_test(NAME test_jit_framework_metadata_node_v8
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_framework_metadata_check.py
                --framework node-v8
                --timeout 30)
    set_tests_properties(test_jit_framework_metadata_node_v8
        PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "node_v8_metadata_ok"
            TIMEOUT 45)

    if(PEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS)
        add_test(NAME test_jit_framework_metadata_node_v8_peak_preload
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_framework_metadata_check.py
                    --framework node-v8-peak
                    --libpeak $<TARGET_FILE:peak>
                    --timeout 30)
        set_tests_properties(test_jit_framework_metadata_node_v8_peak_preload
            PROPERTIES
                SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "node_v8_peak_profile_ok"
                TIMEOUT 45)
    endif()
endif()
