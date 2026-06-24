add_executable(test_command_filter test_command_filter.c)
target_include_directories(test_command_filter PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/utils)
target_link_libraries(test_command_filter PRIVATE $<TARGET_OBJECTS:utils_c>)

add_test(NAME test_command_filter
    COMMAND test_command_filter)
set_tests_properties(test_command_filter
    PROPERTIES
        PASS_REGULAR_EXPRESSION "command_filter_ok")

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_test(NAME test_preload_interpreter_default_skip
        COMMAND ${Python3_EXECUTABLE} -c "print('interpreter_skip_ok')")
    set_tests_properties(test_preload_interpreter_default_skip
        PROPERTIES
            ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:peak>;PEAK_TARGET=main;PEAK_HEARTBEAT_INTERVAL=0"
            PASS_REGULAR_EXPRESSION "interpreter_skip_ok"
            FAIL_REGULAR_EXPRESSION "PEAK Library|overhead calibration|Estimated overhead")
endif()
