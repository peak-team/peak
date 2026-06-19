add_executable(test_dlopen_controller test_dlopen_controller.c)
target_link_libraries(test_dlopen_controller PRIVATE peak)
target_include_directories(test_dlopen_controller PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(test_dlopen_controller PRIVATE PEAK_ENABLE_TEST_HOOKS=1)

add_test(NAME test_dlopen_controller_diagnostics
    COMMAND $<TARGET_FILE:test_dlopen_controller>)
set_tests_properties(test_dlopen_controller_diagnostics
    PROPERTIES
        ENVIRONMENT "PEAK_DLOPEN_TRACE_PATH=${PROJECT_BINARY_DIR}/test/dlopen_controller/peak-dlopen-diagnostics-trace.csv"
        PASS_REGULAR_EXPRESSION "dlopen_controller_diagnostics_ok")
