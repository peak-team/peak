if(NOT TARGET peak_attach)
    return()
endif()
add_executable(test_attach_fixture fixture.c)
target_link_libraries(test_attach_fixture PRIVATE Threads::Threads)
set_target_properties(test_attach_fixture PROPERTIES ENABLE_EXPORTS ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_program(PEAK_TEST_CODESIGN codesign)
    if(NOT PEAK_TEST_CODESIGN)
        message(FATAL_ERROR "Native attach tests require the macOS codesign tool")
    endif()
    # Sign only build-tree test artifacts. Native Mach attach needs a debuggable
    # fixture and debugger rights on the injector.
    add_dependencies(test_attach_fixture peak_attach)
    add_custom_command(TARGET test_attach_fixture POST_BUILD
        COMMAND ${PEAK_TEST_CODESIGN} --force --sign - --timestamp=none
            --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/fixture.entitlements
            $<TARGET_FILE:test_attach_fixture>
        COMMAND ${PEAK_TEST_CODESIGN} --force --sign - --timestamp=none
            --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/debugger.entitlements
            $<TARGET_FILE:peak_attach>
        VERBATIM)
endif()

add_test(NAME test_attach_cli
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_attach.py
        --cli $<TARGET_FILE:peak_attach> --mode cli)
set(_peak_attach_injection_options)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND _peak_attach_injection_options --require-injection)
endif()
add_test(NAME test_attach_runtime
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_attach.py
        --cli $<TARGET_FILE:peak_attach> --agent $<TARGET_FILE:peak_attach_agent>
        --fixture $<TARGET_FILE:test_attach_fixture> --mode runtime
        ${_peak_attach_injection_options})
set_tests_properties(test_attach_cli PROPERTIES TIMEOUT 30)
set_tests_properties(test_attach_runtime PROPERTIES TIMEOUT 100 SKIP_RETURN_CODE 77)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME test_attach_memory
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_attach.py
            --cli $<TARGET_FILE:peak_attach> --agent $<TARGET_FILE:peak_attach_agent>
            --fixture $<TARGET_FILE:test_attach_fixture> --mode memory)
    set_tests_properties(test_attach_memory PROPERTIES TIMEOUT 100 SKIP_RETURN_CODE 77)
endif()

add_executable(test_attach_config config.c
    ${PROJECT_SOURCE_DIR}/src/attach/runtime_config.c)
target_include_directories(test_attach_config PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${FRIDA_GUM_INCLUDE_DIRS})
target_compile_definitions(test_attach_config PRIVATE PEAK_ATTACH_RUNTIME=1)
target_compile_options(test_attach_config PRIVATE
    -include ${PROJECT_SOURCE_DIR}/include/internal/attach_runtime_config.h)
get_target_property(_peak_attach_runtime_libraries peak_attach_agent LINK_LIBRARIES)
set(_peak_attach_config_libraries ${FRIDA_GUM_LIBRARIES} ${OTF2_LIBRARIES}
    ${_peak_attach_runtime_libraries})
list(REMOVE_DUPLICATES _peak_attach_config_libraries)
target_link_libraries(test_attach_config PRIVATE ${_peak_attach_config_libraries})
add_test(NAME test_attach_config COMMAND test_attach_config)
set_tests_properties(test_attach_config PROPERTIES TIMEOUT 10)

add_library(test_attach_unresponsive_agent MODULE unresponsive_agent.c)
set_target_properties(test_attach_unresponsive_agent PROPERTIES PREFIX "")
add_test(NAME test_attach_transport
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_transport.py
        --cli $<TARGET_FILE:peak_attach>
        --fixture $<TARGET_FILE:test_attach_fixture>
        --agent $<TARGET_FILE:test_attach_unresponsive_agent>)
set_tests_properties(test_attach_transport PROPERTIES TIMEOUT 60)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Compile the real Darwin overlay with a fake transaction backend. The
    # pinned Gum amalgamated header exposes Arm64 writers on every host.
    add_executable(test_attach_gum_darwin_staging
        ${PROJECT_SOURCE_DIR}/test/gum_darwin_staging_mock.c)
    target_include_directories(test_attach_gum_darwin_staging PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${FRIDA_GUM_INCLUDE_DIRS})
    target_compile_options(test_attach_gum_darwin_staging PRIVATE -UNDEBUG)
    target_link_libraries(test_attach_gum_darwin_staging PRIVATE
        ${_peak_attach_runtime_libraries})
    add_test(NAME test_attach_gum_darwin_staging
        COMMAND test_attach_gum_darwin_staging)
    set_tests_properties(test_attach_gum_darwin_staging PROPERTIES TIMEOUT 20)
    add_executable(test_attach_gum_linux_staging gum_linux_staging.c)
    target_include_directories(test_attach_gum_linux_staging PRIVATE ${FRIDA_GUM_INCLUDE_DIRS})
    target_compile_options(test_attach_gum_linux_staging PRIVATE -UNDEBUG)
    target_link_libraries(test_attach_gum_linux_staging PRIVATE ${_peak_attach_runtime_libraries})
    add_test(NAME test_attach_gum_linux_staging COMMAND test_attach_gum_linux_staging)
    set_tests_properties(test_attach_gum_linux_staging PROPERTIES TIMEOUT 20)
    add_executable(test_attach_gum_linux_staged_arm64_writer gum_linux_staged_arm64_writer.c)
    target_include_directories(test_attach_gum_linux_staged_arm64_writer PRIVATE
        ${FRIDA_GUM_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/cmake/peak-gum)
    target_compile_options(test_attach_gum_linux_staged_arm64_writer PRIVATE -UNDEBUG)
    target_link_libraries(test_attach_gum_linux_staged_arm64_writer PRIVATE ${_peak_attach_runtime_libraries})
    add_test(NAME test_attach_gum_linux_staged_arm64_writer COMMAND test_attach_gum_linux_staged_arm64_writer)
    set_tests_properties(test_attach_gum_linux_staged_arm64_writer PROPERTIES TIMEOUT 20)
endif()
