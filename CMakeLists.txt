cmake_minimum_required(VERSION 3.5)

project(peak C CXX)

# Set build type to Release by default if CMAKE_BUILD_TYPE is not given
set(default_build_type "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
endif()
message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

# Set default flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_BUILD_TYPE MATCHES "Release")
    add_compile_options("SHELL: -Wall -O3 -ffunction-sections -fdata-sections")
    add_link_options("SHELL: -static-libgcc -Wl,-z,noexecstack,--gc-sections")
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_compile_options("SHELL: -Wall -O0 -g")
endif()
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

# Find or download frida-gum
if(NOT FRIDA_GUM_LIBRARIES OR NOT FRIDA_INCLUDE_DIRS)
    message(STATUS "Fetching frida-gum")
    include(cmake/frida-gum.cmake)
    fetch_frida_gum(
        ${PROJECT_SOURCE_DIR}/cmake
        ${PROJECT_BINARY_DIR}/frida-gum
    )
    message(STATUS "FRIDA_GUM_LIBRARIES   = ${FRIDA_GUM_LIBRARIES}")
    message(STATUS "FRIDA_GUM_INCLUDE_DIRS = ${FRIDA_GUM_INCLUDE_DIRS}")
endif()


############################################################
# Create a library
############################################################

# Generate the shared library from the library sources
add_library(peak SHARED 
    src/peak.c
)

target_include_directories(peak
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
)

target_link_libraries(peak
    PRIVATE
        ${FRIDA_GUM_LIBRARIES}
)

