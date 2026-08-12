add_library(test_symbol_resolution_a MODULE
    symbol_resolution_fixture.cpp
    symbol_resolution_legacy_helper.c)
add_library(test_symbol_resolution_b MODULE
    symbol_resolution_fixture.cpp
    symbol_resolution_legacy_helper.c)
add_library(test_symbol_resolution_rich MODULE
    symbol_resolution_fixture.cpp
    symbol_resolution_legacy_helper.c)
add_library(test_symbol_resolution_c_rich MODULE
    symbol_resolution_c_rich_fixture.c)
add_library(test_symbol_resolution_missing MODULE symbol_resolution_missing_fixture.c)
set_target_properties(test_symbol_resolution_a test_symbol_resolution_b PROPERTIES
    PREFIX ""
    OUTPUT_NAME "symbol_fixture"
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF)
set_target_properties(test_symbol_resolution_rich PROPERTIES
    PREFIX ""
    OUTPUT_NAME "symbol_rich_fixture"
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF)
set_target_properties(test_symbol_resolution_c_rich PROPERTIES
    PREFIX ""
    OUTPUT_NAME "symbol_c_rich_fixture")
set_target_properties(test_symbol_resolution_a PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/module-a")
set_target_properties(test_symbol_resolution_b PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/module-b")
target_compile_definitions(test_symbol_resolution_a PRIVATE PEAK_FIXTURE_VALUE=1)
target_compile_definitions(test_symbol_resolution_b PRIVATE PEAK_FIXTURE_VALUE=2)
target_compile_definitions(test_symbol_resolution_rich PRIVATE PEAK_SYMBOL_RICH=1)

add_executable(test_target_resolver
    test_target_resolver.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/target_resolver.c
    ${PROJECT_SOURCE_DIR}/src/utils/target_selector_syntax.c
    $<TARGET_OBJECTS:utils_cxx>)
target_include_directories(test_target_resolver PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS})
target_link_libraries(test_target_resolver PRIVATE
    ${FRIDA_GUM_LIBRARIES}
    Threads::Threads
    ${DL_LIBRARY}
    ${RT_LIBRARY}
    ${RESOLV_LIBRARY}
    ${M_LIBRARY})
set_target_properties(test_target_resolver PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(test_target_resolver PRIVATE
    PEAK_ENABLE_TEST_HOOKS=1
    PEAK_TARGET_RESOLVER_TESTING=1
    PEAK_TEST_SYMBOL_MODULE_A="$<TARGET_FILE:test_symbol_resolution_a>"
    PEAK_TEST_SYMBOL_MODULE_B="$<TARGET_FILE:test_symbol_resolution_b>"
    PEAK_TEST_SYMBOL_MODULE_RICH="$<TARGET_FILE:test_symbol_resolution_rich>"
    PEAK_TEST_SYMBOL_MODULE_C_RICH="$<TARGET_FILE:test_symbol_resolution_c_rich>")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(test_target_resolver PRIVATE
        PEAK_TEST_EXPECT_GCC_THUNK=1)
endif()
add_dependencies(test_target_resolver test_symbol_resolution_a test_symbol_resolution_b
    test_symbol_resolution_rich test_symbol_resolution_c_rich)
add_test(NAME test_target_resolver COMMAND test_target_resolver)

add_test(NAME test_symbol_resolution_symbol_count_scaling
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_symbol_count_scaling.py
            $<TARGET_FILE:test_target_resolver>)

add_test(NAME test_peak_inspect_symbols
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_inspect_symbols.py
            $<TARGET_FILE:peak_inspect_symbols>
            $<TARGET_FILE:test_symbol_resolution_a>
            $<TARGET_FILE:test_symbol_resolution_b>)

add_executable(test_symbol_resolution_dynamic
    test_symbol_resolution_dynamic.c)
target_include_directories(test_symbol_resolution_dynamic PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_symbol_resolution_dynamic PRIVATE peak ${CMAKE_DL_LIBS})
target_compile_definitions(test_symbol_resolution_dynamic PRIVATE
    PEAK_ENABLE_TEST_HOOKS=1
    PEAK_TARGET_RESOLVER_TESTING=1
    PEAK_TEST_SYMBOL_MODULE_A="$<TARGET_FILE:test_symbol_resolution_a>"
    PEAK_TEST_SYMBOL_MODULE_B="$<TARGET_FILE:test_symbol_resolution_b>"
    PEAK_TEST_SYMBOL_MODULE_RICH="$<TARGET_FILE:test_symbol_resolution_rich>")
target_compile_definitions(test_symbol_resolution_dynamic PRIVATE
    PEAK_TEST_SYMBOL_MODULE_MISSING="$<TARGET_FILE:test_symbol_resolution_missing>")
add_dependencies(test_symbol_resolution_dynamic
    test_symbol_resolution_a test_symbol_resolution_b test_symbol_resolution_rich
    test_symbol_resolution_missing peak)

add_test(NAME test_symbol_resolution_dynamic_unique
    COMMAND test_symbol_resolution_dynamic unique)
set_tests_properties(test_symbol_resolution_dynamic_unique PROPERTIES
    ENVIRONMENT "PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!peak_test::Widget::func(int,double);PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_exact_instance
    COMMAND test_symbol_resolution_dynamic exact-instance)
set_tests_properties(test_symbol_resolution_dynamic_exact_instance PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:test_symbol_resolution_b>;PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!_ZN9peak_test6Widget4funcEid;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_legacy_short
    COMMAND test_symbol_resolution_dynamic legacy)
set_tests_properties(test_symbol_resolution_dynamic_legacy_short PROPERTIES
    ENVIRONMENT "PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!legacy_short_unique;PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_legacy_short_requires_opt_in
    COMMAND test_symbol_resolution_dynamic legacy-disabled)
set_tests_properties(test_symbol_resolution_dynamic_legacy_short_requires_opt_in PROPERTIES
    ENVIRONMENT "PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!legacy_short_unique;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_plain_c_fast_path
    COMMAND test_symbol_resolution_dynamic plain-c)
set_tests_properties(test_symbol_resolution_dynamic_plain_c_fast_path PROPERTIES
    ENVIRONMENT "PEAK_TARGET=peak_symbol_fixture_invoke;PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_plain_c_unrelated_first
    COMMAND test_symbol_resolution_dynamic plain-c-unrelated-first)
set_tests_properties(test_symbol_resolution_dynamic_plain_c_unrelated_first PROPERTIES
    ENVIRONMENT "PEAK_TARGET=peak_symbol_fixture_invoke;PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_selector_unrelated_first
    COMMAND test_symbol_resolution_dynamic selector-unrelated-first)
set_tests_properties(test_symbol_resolution_dynamic_selector_unrelated_first PROPERTIES
    ENVIRONMENT "PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!peak_test::Widget::func(int,double);PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_local_symbol
    COMMAND test_symbol_resolution_dynamic local)
set_tests_properties(test_symbol_resolution_dynamic_local_symbol PROPERTIES
    ENVIRONMENT "PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!peak_test::local_dynamic_target(int);PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_selector_env_snapshot
    COMMAND test_symbol_resolution_dynamic env-snapshot)
set_tests_properties(test_symbol_resolution_dynamic_selector_env_snapshot PROPERTIES
    ENVIRONMENT "PEAK_TARGET=$<TARGET_FILE:test_symbol_resolution_a>!legacy_short_unique;PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_dynamic_ambiguous
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_dynamic_ambiguity.py
            $<TARGET_FILE:test_symbol_resolution_dynamic>
            $<TARGET_FILE:test_symbol_resolution_a>
            $<TARGET_FILE:test_symbol_resolution_b>)

add_test(NAME test_symbol_resolution_dynamic_mixed_targets
    COMMAND test_symbol_resolution_dynamic mixed)
set_tests_properties(test_symbol_resolution_dynamic_mixed_targets PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:test_symbol_resolution_a>;PEAK_TARGET=peak_symbol_missing_dynamic_target,$<TARGET_FILE:test_symbol_resolution_a>!peak_test::Widget::func;PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

add_test(NAME test_symbol_resolution_startup_selectors
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_startup_selectors.py
            $<TARGET_FILE:test_symbol_resolution_dynamic>
            $<TARGET_FILE:test_symbol_resolution_a>
            $<TARGET_FILE:test_symbol_resolution_b>)

add_test(NAME test_symbol_resolution_startup_plain_c_fast_path
    COMMAND test_symbol_resolution_dynamic startup-plain-c)
set_tests_properties(test_symbol_resolution_startup_plain_c_fast_path PROPERTIES
    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:test_symbol_resolution_a>;PEAK_TARGET=peak_symbol_fixture_invoke;PEAK_ENABLE_CXX_SYMBOL_SCAN=1;PEAK_TARGET_FILE=;PEAK_TARGET_GROUP=;PEAK_HEARTBEAT_INTERVAL=0;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0")

set_tests_properties(
    test_target_resolver
    test_peak_inspect_symbols
    test_symbol_resolution_dynamic_unique
    test_symbol_resolution_dynamic_exact_instance
    test_symbol_resolution_dynamic_legacy_short
    test_symbol_resolution_dynamic_legacy_short_requires_opt_in
    test_symbol_resolution_dynamic_plain_c_fast_path
    test_symbol_resolution_dynamic_plain_c_unrelated_first
    test_symbol_resolution_dynamic_selector_unrelated_first
    test_symbol_resolution_dynamic_local_symbol
    test_symbol_resolution_dynamic_selector_env_snapshot
    test_symbol_resolution_dynamic_ambiguous
    test_symbol_resolution_dynamic_mixed_targets
    test_symbol_resolution_startup_selectors
    test_symbol_resolution_startup_plain_c_fast_path
    PROPERTIES TIMEOUT 30)
