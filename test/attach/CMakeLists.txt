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
add_test(NAME test_attach_runtime_calibration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_attach.py
        --cli $<TARGET_FILE:peak_attach> --agent $<TARGET_FILE:peak_attach_agent>
        --fixture $<TARGET_FILE:test_attach_fixture> --mode calibration)
set_tests_properties(test_attach_runtime_calibration PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_READELF)
        add_test(NAME test_attach_dynamic_tls COMMAND
            ${CMAKE_READELF} -d $<TARGET_FILE:peak_attach_agent>)
        set_tests_properties(test_attach_dynamic_tls PROPERTIES
            FAIL_REGULAR_EXPRESSION "STATIC_TLS" TIMEOUT 10)
    endif()
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(_peak_attach_readelf readelf)
    if(_peak_attach_readelf)
        add_library(test_attach_collector_targets SHARED collector_targets.c)
        target_link_options(test_attach_collector_targets PRIVATE -nostdlib)
        add_executable(test_attach_collector_fixture collector_fixture.cpp)
        target_link_libraries(test_attach_collector_fixture PRIVATE
            test_attach_collector_targets Threads::Threads)
        add_test(NAME test_attach_collector
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_collector.py
                ${PROJECT_SOURCE_DIR}/tools/peak-collect-targets
                $<TARGET_FILE:test_attach_collector_fixture>
                $<TARGET_FILE:test_attach_collector_targets>
                $<TARGET_FILE:peak_attach>
                $<TARGET_FILE:peak_attach_agent>)
        set_tests_properties(test_attach_collector PROPERTIES TIMEOUT 55 SKIP_RETURN_CODE 77)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MPI_C_FOUND AND MPIEXEC_EXECUTABLE)
    add_executable(test_attach_mpi_fixture mpi_fixture.c)
    set_target_properties(test_attach_mpi_fixture PROPERTIES ENABLE_EXPORTS TRUE)
    target_link_libraries(test_attach_mpi_fixture PRIVATE MPI::MPI_C Threads::Threads)
    foreach(_case IN ITEMS baseline partial all partial_local all_local)
        set(_local)
        if(_case STREQUAL "baseline")
            set(_rank_count 0)
        elseif(_case MATCHES "^partial")
            set(_rank_count 1)
        else()
            set(_rank_count 2)
        endif()
        if(_case MATCHES "_local$")
            set(_local --local)
        endif()
        add_test(NAME test_attach_mpi_${_case}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_mpi.py
                --cli $<TARGET_FILE:peak_attach>
                --agent $<TARGET_FILE:peak_attach_agent>
                --ranks ${_rank_count} ${_local}
                -- ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
                    ${MPIEXEC_PREFLAGS} $<TARGET_FILE:test_attach_mpi_fixture>
                    ${MPIEXEC_POSTFLAGS})
        set_tests_properties(test_attach_mpi_${_case} PROPERTIES TIMEOUT 65 PROCESSORS 2)
    endforeach()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(test_attach_module_target MODULE module_target.c)
    add_executable(test_attach_module_fixture module_fixture.c)
    target_link_libraries(test_attach_module_fixture PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME test_attach_startup_module
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_module.py
            $<TARGET_FILE:peak_attach> $<TARGET_FILE:peak_attach_agent>
            $<TARGET_FILE:test_attach_module_fixture> $<TARGET_FILE:test_attach_module_target>)
    set_tests_properties(test_attach_startup_module PROPERTIES TIMEOUT 45)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_executable(test_attach_memory_failure_fixture fixture.c unrelocatable_allocator.c)
    target_link_libraries(test_attach_memory_failure_fixture PRIVATE Threads::Threads)
    set_target_properties(test_attach_memory_failure_fixture PROPERTIES ENABLE_EXPORTS ON)
    add_test(NAME test_attach_memory_failure
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_attach.py
            --cli $<TARGET_FILE:peak_attach> --agent $<TARGET_FILE:peak_attach_agent>
            --fixture $<TARGET_FILE:test_attach_memory_failure_fixture> --mode memory_failure)
    set_tests_properties(test_attach_memory_failure PROPERTIES TIMEOUT 70)
endif()
