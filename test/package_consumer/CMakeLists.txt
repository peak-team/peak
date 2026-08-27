cmake_minimum_required(VERSION 3.13)
project(peak_package_consumer C)

set(PEAK_REQUIRED_VERSION "1" CACHE STRING
    "Version requested from the installed PEAK package")
set(PEAK_EXPECTED_VERSION "1.0.0" CACHE STRING
    "Exact installed PEAK version expected by this fixture")
set(PEAK_REQUIRED_COMPONENTS "" CACHE STRING
    "Components requested from the installed PEAK package")
if(PEAK_REQUIRED_COMPONENTS)
    find_package(PEAK ${PEAK_REQUIRED_VERSION} CONFIG REQUIRED
        COMPONENTS ${PEAK_REQUIRED_COMPONENTS})
else()
    find_package(PEAK ${PEAK_REQUIRED_VERSION} CONFIG REQUIRED)
endif()

if(NOT PEAK_VERSION VERSION_EQUAL PEAK_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Expected PEAK ${PEAK_EXPECTED_VERSION}, found ${PEAK_VERSION}")
endif()
if(NOT PEAK_PUBLIC_INTERFACE STREQUAL "PRELOAD_ONLY")
    message(FATAL_ERROR
        "Unexpected PEAK installed interface: ${PEAK_PUBLIC_INTERFACE}")
endif()
foreach(component MPI OTF2 CUDA)
    if(DEFINED PEAK_EXPECT_${component}_ENABLED AND
       NOT "${PEAK_${component}_ENABLED}" STREQUAL
           "${PEAK_EXPECT_${component}_ENABLED}")
        message(FATAL_ERROR
            "Expected PEAK_${component}_ENABLED="
            "${PEAK_EXPECT_${component}_ENABLED}, found "
            "${PEAK_${component}_ENABLED}")
    endif()
endforeach()

add_executable(peak_package_consumer main.c)
target_link_libraries(peak_package_consumer PRIVATE ${CMAKE_DL_LIBS})

add_executable(peak_package_consumer_preload preload_fixture.c)
set_target_properties(peak_package_consumer_preload PROPERTIES ENABLE_EXPORTS ON)

get_target_property(peak_include_dirs PEAK::peak INTERFACE_INCLUDE_DIRECTORIES)
if(peak_include_dirs)
    message(FATAL_ERROR
        "Preload-only PEAK target exports include paths: ${peak_include_dirs}")
endif()

get_target_property(peak_link_interface PEAK::peak INTERFACE_LINK_LIBRARIES)
if(peak_link_interface)
    message(FATAL_ERROR
        "PEAK target leaks private dependencies: ${peak_link_interface}")
endif()

get_target_property(peak_target_type PEAK::peak TYPE)
if(NOT peak_target_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "PEAK::peak is not an installed shared library")
endif()

enable_testing()
add_test(NAME peak_package_consumer_load
    COMMAND peak_package_consumer $<TARGET_FILE:PEAK::peak>)
add_test(NAME peak_package_consumer_symbols
    COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DPLATFORM=${CMAKE_SYSTEM_NAME}
        -DPEAK_LIBRARY=$<TARGET_FILE:PEAK::peak>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_dynamic_symbols.cmake)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(PEAK_PACKAGE_PRELOAD_ENV LD_PRELOAD)
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(PEAK_PACKAGE_PRELOAD_ENV DYLD_INSERT_LIBRARIES)
else()
    message(FATAL_ERROR "The PEAK preload contract is unsupported on this platform")
endif()

add_test(NAME peak_package_consumer_preload
    COMMAND ${CMAKE_COMMAND}
        -DPRELOAD_ENV=${PEAK_PACKAGE_PRELOAD_ENV}
        -DPEAK_LIBRARY=$<TARGET_FILE:PEAK::peak>
        -DFIXTURE=$<TARGET_FILE:peak_package_consumer_preload>
        -DSTATS_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-consumer-profile
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_preload_check.cmake)
set_tests_properties(
    peak_package_consumer_load
    peak_package_consumer_symbols
    peak_package_consumer_preload
    PROPERTIES TIMEOUT 60)
