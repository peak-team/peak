add_library(test_deferred_module_a MODULE deferred_module_fixture.c)
add_library(test_deferred_module_b MODULE deferred_module_fixture.c)
add_library(test_forking_module MODULE forking_module_fixture.c)
set_target_properties(test_deferred_module_a test_deferred_module_b
    test_forking_module
    PROPERTIES
        PREFIX ""
        C_VISIBILITY_PRESET hidden)

add_executable(test_dlopen_controller test_dlopen_controller.c)
target_link_libraries(test_dlopen_controller PRIVATE peak ${CMAKE_DL_LIBS})
target_include_directories(test_dlopen_controller PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(test_dlopen_controller PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
target_compile_definitions(test_dlopen_controller PRIVATE
    PEAK_TEST_DEFERRED_MODULE_A="$<TARGET_FILE:test_deferred_module_a>"
    PEAK_TEST_DEFERRED_MODULE_B="$<TARGET_FILE:test_deferred_module_b>")
add_dependencies(test_dlopen_controller
    test_deferred_module_a
    test_deferred_module_b)

add_test(NAME test_dlopen_controller_diagnostics
    COMMAND $<TARGET_FILE:test_dlopen_controller>)
set_tests_properties(test_dlopen_controller_diagnostics
    PROPERTIES
        ENVIRONMENT "PEAK_DLOPEN_TRACE_PATH=${PROJECT_BINARY_DIR}/test/dlopen_controller/peak-dlopen-diagnostics-trace.csv"
        PASS_REGULAR_EXPRESSION "dlopen_controller_diagnostics_ok")

if(PEAK_GUM_PEAK_API_AVAILABLE AND CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_library(test_dlopen_owned_module MODULE owned_module_fixture.c)
    set_target_properties(test_dlopen_owned_module
        PROPERTIES
            PREFIX ""
            C_VISIBILITY_PRESET hidden)

    add_executable(test_dlopen_loader_ownership
        test_dlopen_loader_ownership.c)
    target_include_directories(test_dlopen_loader_ownership PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS})
    target_link_libraries(test_dlopen_loader_ownership PRIVATE
        ${CMAKE_DL_LIBS}
        Threads::Threads)
    target_link_options(test_dlopen_loader_ownership
        PRIVATE -Wl,--export-dynamic)
    target_compile_definitions(test_dlopen_loader_ownership PRIVATE
        PEAK_TEST_OWNED_MODULE="$<TARGET_FILE:test_dlopen_owned_module>")
    add_dependencies(test_dlopen_loader_ownership
        peak
        test_dlopen_owned_module)

    add_test(NAME test_dlopen_callback_owned_handle
        COMMAND $<TARGET_FILE:test_dlopen_loader_ownership> ownership)
    set_tests_properties(test_dlopen_callback_owned_handle
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=peak_dlopen_owned_fixture_intentionally_missing;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            FAIL_REGULAR_EXPRESSION "not ok -"
            TIMEOUT 30)

    add_test(NAME test_dlopen_namespace_identity
        COMMAND $<TARGET_FILE:test_dlopen_loader_ownership> namespace)
    set_tests_properties(test_dlopen_namespace_identity
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=peak_dlopen_owned_fixture_intentionally_missing;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            FAIL_REGULAR_EXPRESSION "not ok -"
            TIMEOUT 30)

    add_test(NAME test_dlopen_destructor_reentrant_drain
        COMMAND $<TARGET_FILE:test_dlopen_loader_ownership> destructor)
    set_tests_properties(test_dlopen_destructor_reentrant_drain
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=peak_dlopen_owned_fixture_intentionally_missing;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            FAIL_REGULAR_EXPRESSION "not ok -"
            TIMEOUT 30)

    add_test(NAME test_dlopen_concurrent_owned_handle_stress
        COMMAND $<TARGET_FILE:test_dlopen_loader_ownership> stress)
    set_tests_properties(test_dlopen_concurrent_owned_handle_stress
        PROPERTIES
            ENVIRONMENT "PEAK_TARGET=peak_dlopen_owned_fixture_intentionally_missing;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            FAIL_REGULAR_EXPRESSION "not ok -"
            TIMEOUT 60)
endif()

if(PEAK_GUM_PEAK_API_AVAILABLE)
    add_executable(test_gum_module_sync_idle_quiesce
        test_gum_module_sync_idle_quiesce.c)
    target_link_libraries(test_gum_module_sync_idle_quiesce
        PRIVATE peak ${CMAKE_DL_LIBS})

    add_test(NAME test_gum_module_sync_idle_quiesce
        COMMAND $<TARGET_FILE:test_gum_module_sync_idle_quiesce>)
    set_tests_properties(test_gum_module_sync_idle_quiesce
        PROPERTIES
            TIMEOUT 5
            PASS_REGULAR_EXPRESSION "gum_module_sync_idle_quiesce_ok")

    add_executable(test_gum_module_sync_lifecycle
        test_gum_module_sync_lifecycle.c)
    target_link_libraries(test_gum_module_sync_lifecycle
        PRIVATE peak ${CMAKE_DL_LIBS})
    target_link_options(test_gum_module_sync_lifecycle
        PRIVATE -Wl,--export-dynamic)
    target_compile_definitions(test_gum_module_sync_lifecycle PRIVATE
        PEAK_TEST_DEFERRED_MODULE_A="$<TARGET_FILE:test_deferred_module_a>"
        PEAK_TEST_DEFERRED_MODULE_B="$<TARGET_FILE:test_deferred_module_b>"
        PEAK_TEST_FORKING_MODULE="$<TARGET_FILE:test_forking_module>")
    add_dependencies(test_gum_module_sync_lifecycle
        test_deferred_module_a
        test_deferred_module_b
        test_forking_module)

    add_test(NAME test_gum_module_sync_lifecycle
        COMMAND $<TARGET_FILE:test_gum_module_sync_lifecycle>)
    set_tests_properties(test_gum_module_sync_lifecycle
        PROPERTIES
            TIMEOUT 30
            PASS_REGULAR_EXPRESSION "gum_module_sync_lifecycle_ok")

    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        add_executable(test_gum_raw_syscall_aarch64
            test_gum_raw_syscall_aarch64.c
            ${PROJECT_SOURCE_DIR}/cmake/peak-gum/peak_aarch64_raw_syscall.S)
        target_include_directories(test_gum_raw_syscall_aarch64
            PRIVATE ${PROJECT_SOURCE_DIR}/include)
        add_test(NAME test_gum_raw_syscall_aarch64
            COMMAND $<TARGET_FILE:test_gum_raw_syscall_aarch64>)
        set_tests_properties(test_gum_raw_syscall_aarch64
            PROPERTIES
                TIMEOUT 5
                PASS_REGULAR_EXPRESSION "gum_raw_syscall_aarch64_ok")
    endif()
endif()
