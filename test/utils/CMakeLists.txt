add_executable(test_command_filter test_command_filter.c)
target_include_directories(test_command_filter PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/utils)
target_link_libraries(test_command_filter PRIVATE
    $<TARGET_OBJECTS:utils_c>
    Threads::Threads)

add_test(NAME test_command_filter
    COMMAND test_command_filter)
set_tests_properties(test_command_filter
    PROPERTIES
        PASS_REGULAR_EXPRESSION "command_filter_ok")

add_executable(test_target_config
    test_target_config.c
    ${PROJECT_SOURCE_DIR}/src/utils/target_config.c
    ${PROJECT_SOURCE_DIR}/src/utils/target_selector_syntax.c
    ${PROJECT_SOURCE_DIR}/src/utils/source_target.c
    ${PROJECT_SOURCE_DIR}/src/logging.c)
target_compile_definitions(test_target_config PRIVATE PEAK_TARGET_CONFIG_TESTING=1)
target_include_directories(test_target_config PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/utils)
target_link_libraries(test_target_config PRIVATE Threads::Threads)
add_test(NAME test_target_config_parser COMMAND test_target_config)
set_tests_properties(test_target_config_parser
    PROPERTIES
        PASS_REGULAR_EXPRESSION "target_config_ok"
        ENVIRONMENT "PEAK_VERBOSITY=silent")

option(PEAK_BUILD_FUZZ_TESTS "Build libFuzzer target-parser harnesses" OFF)
if(PEAK_BUILD_FUZZ_TESTS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_executable(fuzz_target_config
            fuzz_target_config.c
            ${PROJECT_SOURCE_DIR}/src/utils/target_config.c
            ${PROJECT_SOURCE_DIR}/src/utils/target_selector_syntax.c
            ${PROJECT_SOURCE_DIR}/src/utils/source_target.c
            ${PROJECT_SOURCE_DIR}/src/logging.c)
        target_include_directories(fuzz_target_config PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/include/utils)
        target_compile_options(fuzz_target_config PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz_target_config PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_libraries(fuzz_target_config PRIVATE Threads::Threads)
    else()
        message(WARNING "PEAK_BUILD_FUZZ_TESTS requires a Clang C compiler")
    endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux" OR CMAKE_SYSTEM_NAME MATCHES "Darwin")
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        set(_PEAK_TARGET_CONFIG_PRELOAD_ENV LD_PRELOAD)
    else()
        set(_PEAK_TARGET_CONFIG_PRELOAD_ENV DYLD_INSERT_LIBRARIES)
    endif()
    add_test(NAME test_target_config_verbosity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_target_config_verbosity_check.py
                --exe $<TARGET_FILE:test_command_filter>
                --libpeak $<TARGET_FILE:peak>
                --preload-env ${_PEAK_TARGET_CONFIG_PRELOAD_ENV})
    set_tests_properties(test_target_config_verbosity
        PROPERTIES
            PASS_REGULAR_EXPRESSION "target_config_verbosity_check_ok"
            TIMEOUT 30)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_executable(test_parent_registry
        test_parent_registry.c
        ${PROJECT_SOURCE_DIR}/src/utils/parent_registry.c)
    target_compile_definitions(test_parent_registry PRIVATE PEAK_UTILS_TESTING=1)
    target_include_directories(test_parent_registry PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include/utils)

    add_test(NAME test_parent_registry
        COMMAND test_parent_registry)
    set_tests_properties(test_parent_registry
        PROPERTIES
            PASS_REGULAR_EXPRESSION "parent_registry_ok")

    add_test(NAME test_preload_no_target_inert
        COMMAND test_command_filter)
    set_tests_properties(test_preload_no_target_inert
        PROPERTIES
            ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:peak>;PEAK_HEARTBEAT_INTERVAL=0"
            PASS_REGULAR_EXPRESSION "command_filter_ok"
            FAIL_REGULAR_EXPRESSION "PEAK Library|overhead calibration|Estimated overhead")

    add_test(NAME test_preload_interpreter_default_skip
        COMMAND ${Python3_EXECUTABLE} -c "print('interpreter_skip_ok')")
    set_tests_properties(test_preload_interpreter_default_skip
        PROPERTIES
            ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:peak>;PEAK_TARGET=main;PEAK_HEARTBEAT_INTERVAL=0"
            PASS_REGULAR_EXPRESSION "interpreter_skip_ok"
            FAIL_REGULAR_EXPRESSION "PEAK Library|overhead calibration|Estimated overhead")

    add_test(NAME test_preload_filtered_tool_inert
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_preload_filtered_tool_check.py
                --libpeak $<TARGET_FILE:peak>)
    set_tests_properties(test_preload_filtered_tool_inert
        PROPERTIES
            PASS_REGULAR_EXPRESSION "filtered_tool_preload_ok"
            FAIL_REGULAR_EXPRESSION "PEAK Library|overhead calibration|Estimated overhead")
endif()
