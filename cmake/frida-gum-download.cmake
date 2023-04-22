cmake_minimum_required(VERSION 3.5)

project(frida-gum-download NONE)

include(ExternalProject)

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# More about platform check with CMake
# https://gitlab.kitware.com/cmake/community/-/wikis/doc/tutorials/How-To-Write-Platform-Checks
message (STATUS "CMAKE_SYSTEM_NAME        = ${CMAKE_SYSTEM_NAME}")
message (STATUS "CMAKE_SYSTEM_PROCESSOR   = ${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    message (STATUS "Linux detected")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(FRIDA_ARCH_VAR "linux-x86_64")
        set(FRIDA_HASH_VAR "dd599854baa1a40c2f3b33a4929eedfaef2af9f6bfe38d5fd880231838a2024c")
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(FRIDA_ARCH_VAR "linux-arm64")
    else()
        message (ERROR "Platform not supported")
    endif()
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    message (STATUS "MacOS detected")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(FRIDA_ARCH_VAR "macos-x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(FRIDA_ARCH_VAR "macos-arm64")
    else()
        message (ERROR "Platform not supported")
    endif()
endif()
message (STATUS "FRIDA_ARCH_VAR   = ${FRIDA_ARCH_VAR}")
message (STATUS "FRIDA_HASH_VAR   = ${FRIDA_HASH_VAR}")

ExternalProject_Add(
  frida-gum
  SOURCE_DIR "@FRIDA_GUM_DOWNLOAD_ROOT@/frida-gum-src"
  URL
    https://github.com/frida/frida/releases/download/16.0.14/frida-gum-devkit-16.0.14-${FRIDA_ARCH_VAR}.tar.xz
  URL_HASH
    SHA256=${FRIDA_HASH_VAR}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  BUILD_IN_SOURCE 1
  INSTALL_COMMAND cp libfrida-gum.a frida-gum.h ${PROJECT_BINARY_DIR}
  TEST_COMMAND ""
  )