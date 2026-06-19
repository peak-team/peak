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
        set(FRIDA_HASH_VAR "de155a58493e1bdbc04aff098e8861c848624a58e06998dcb2d9e04f16b8d188")
    elseif(FRIDA_SYSTEM_PROCESSOR STREQUAL "arm64" OR
           FRIDA_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(FRIDA_ARCH_VAR "linux-arm64")
        set(FRIDA_HASH_VAR "fe2f87f2e52b9ea7dfff0098a52dab9648929e02cd4e7497905465efe1ce2961")
    else()
        message (FATAL_ERROR "Platform not supported")
    endif()
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    message (STATUS "MacOS detected")
    if(FRIDA_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
       FRIDA_SYSTEM_PROCESSOR STREQUAL "amd64")
        set(FRIDA_ARCH_VAR "macos-x86_64")
        set(FRIDA_HASH_VAR "5250a804a7c496989930a97f3d47e9cff35a5fb50eaa23f226397cdf8ab6347f")
    elseif(FRIDA_SYSTEM_PROCESSOR STREQUAL "arm64" OR
           FRIDA_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(FRIDA_ARCH_VAR "macos-arm64")
        set(FRIDA_HASH_VAR "a7e42d304782210afad1a22a52f13f6cd95f27f556b7839be57de6e832071294")
    else()
        message (FATAL_ERROR "Platform not supported")
    endif()
endif()
if(NOT FRIDA_HASH_VAR)
    message(FATAL_ERROR
        "No verified hash is configured for frida-gum-devkit-16.5.9-${FRIDA_ARCH_VAR}.tar.xz")
endif()
message (STATUS "FRIDA_ARCH_VAR   = ${FRIDA_ARCH_VAR}")
message (STATUS "FRIDA_HASH_VAR   = ${FRIDA_HASH_VAR}")

ExternalProject_Add(
  frida-gum
  SOURCE_DIR "@FRIDA_GUM_DOWNLOAD_ROOT@/frida-gum-src"
  URL
    https://github.com/frida/frida/releases/download/16.5.9/frida-gum-devkit-16.5.9-${FRIDA_ARCH_VAR}.tar.xz
  URL_HASH
    SHA256=${FRIDA_HASH_VAR}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  BUILD_IN_SOURCE 1
  INSTALL_COMMAND cp libfrida-gum.a frida-gum.h ${PROJECT_BINARY_DIR}
  TEST_COMMAND ""
  )
