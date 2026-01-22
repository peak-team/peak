cmake_minimum_required(VERSION 3.5)

project(otf2-download NONE)

include(ExternalProject)

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# You can change the version here if you ever upgrade
set(OTF2_VERSION "3.1.1")

set(OTF2_TARBALL_URL
    "https://perftools.pages.jsc.fz-juelich.de/cicd/otf2/tags/otf2-${OTF2_VERSION}/otf2-${OTF2_VERSION}.tar.gz"
)

message(STATUS "OTF2: downloading from ${OTF2_TARBALL_URL}")
message(STATUS "OTF2: download root = @OTF2_DOWNLOAD_ROOT@")

# For safety, we only really support Unix-y environments here
if (NOT (CMAKE_SYSTEM_NAME MATCHES "Linux" OR CMAKE_SYSTEM_NAME MATCHES "Darwin"))
    message(FATAL_ERROR "OTF2 auto-download currently only supports Linux/macOS-style builds.")
endif()

ExternalProject_Add(
    otf2
    URL          "${OTF2_TARBALL_URL}"
    PREFIX       "@OTF2_DOWNLOAD_ROOT@"
    SOURCE_DIR   "@OTF2_DOWNLOAD_ROOT@/otf2-src"
    BINARY_DIR   "@OTF2_DOWNLOAD_ROOT@/otf2-build"
    INSTALL_DIR  "@OTF2_DOWNLOAD_ROOT@/otf2-install"

    # Autotools-style build
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND     ${CMAKE_MAKE_PROGRAM}
    INSTALL_COMMAND   ${CMAKE_MAKE_PROGRAM} install

    TEST_COMMAND ""
)