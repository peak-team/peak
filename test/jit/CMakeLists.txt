if(CMAKE_SYSTEM_NAME MATCHES "Linux")
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

    add_test(NAME test_jit_profiling_mmap_perfmap_compatibility
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_jit_profiling_check.py
                --exe $<TARGET_FILE:test_jit_mmap_fixture>
                --libpeak $<TARGET_FILE:peak>
                --mode positive
                --iterations 1000000
                --timeout 30)
    set_tests_properties(test_jit_profiling_mmap_perfmap_compatibility
        PROPERTIES
            ENVIRONMENT "PEAK_SAFE_DETACH_MODE=compatibility;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
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
endif()
