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
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" FRIDA_SYSTEM_PROCESSOR)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    message (STATUS "Linux detected")
    if(FRIDA_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
       FRIDA_SYSTEM_PROCESSOR STREQUAL "amd64")
        set(FRIDA_ARCH_VAR "linux-x86_64")
        set(FRIDA_HASH_VAR "f827b75f432c5f90ae57c71979e90e1c93edfa3aa3ac252b0d547f3087306f01")
    elseif(FRIDA_SYSTEM_PROCESSOR STREQUAL "arm64" OR
           FRIDA_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(FRIDA_ARCH_VAR "linux-arm64")
        set(FRIDA_HASH_VAR "b7b9f914ccb2f70c0663bfa20614d4b58fa8fc5f9e0a7786d3fb1c22113b8c61")
    else()
        message (FATAL_ERROR "Platform not supported")
    endif()
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    message (STATUS "MacOS detected")
    if(FRIDA_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
       FRIDA_SYSTEM_PROCESSOR STREQUAL "amd64")
        set(FRIDA_ARCH_VAR "macos-x86_64")
        set(FRIDA_HASH_VAR "7378f605d351a0cfd53b46c3b608f4bde383eff30b1c4c886e11006dbf08f54f")
    elseif(FRIDA_SYSTEM_PROCESSOR STREQUAL "arm64" OR
           FRIDA_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(FRIDA_ARCH_VAR "macos-arm64")
        set(FRIDA_HASH_VAR "1904bc3559e27da517289f940abd37ab9cde4cca82a78e229b386acfa5beb487")
    else()
        message (FATAL_ERROR "Platform not supported")
    endif()
endif()
if(NOT FRIDA_HASH_VAR)
    message(FATAL_ERROR
        "No verified hash is configured for frida-gum-devkit-17.15.3-${FRIDA_ARCH_VAR}.tar.xz")
endif()
message (STATUS "FRIDA_ARCH_VAR   = ${FRIDA_ARCH_VAR}")
message (STATUS "FRIDA_HASH_VAR   = ${FRIDA_HASH_VAR}")

ExternalProject_Add(
  frida-gum
  SOURCE_DIR "@FRIDA_GUM_DOWNLOAD_ROOT@/frida-gum-src"
  URL
    https://github.com/frida/frida/releases/download/17.15.3/frida-gum-devkit-17.15.3-${FRIDA_ARCH_VAR}.tar.xz
  URL_HASH
    SHA256=${FRIDA_HASH_VAR}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  BUILD_IN_SOURCE 1
  INSTALL_COMMAND cp libfrida-gum.a frida-gum.h ${PROJECT_BINARY_DIR}
  TEST_COMMAND ""
  )
