cmake_minimum_required(VERSION 3.5)

project(otf2-download NONE)

include(ExternalProject)

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# You can change the version here if you ever upgrade
set(OTF2_VERSION "3.1.1")

set(OTF2_URL
    "https://zenodo.org/records/15100643/files/otf2-${OTF2_VERSION}.tar.gz")
set(OTF2_SHA256
    "5a4e013a51ac4ed794fe35c55b700cd720346fda7f33ec84c76b86a5fb880a6e")

message(STATUS "OTF2: downloading ${OTF2_URL}")
message(STATUS "OTF2: download root = @OTF2_DOWNLOAD_ROOT@")

# For safety, we only really support Unix-y environments here
if (NOT (CMAKE_SYSTEM_NAME MATCHES "Linux" OR CMAKE_SYSTEM_NAME MATCHES "Darwin"))
    message(FATAL_ERROR "OTF2 auto-download currently only supports Linux/macOS-style builds.")
endif()

ExternalProject_Add(
    otf2
    URL          "${OTF2_URL}"
    URL_HASH     "SHA256=${OTF2_SHA256}"
    PREFIX       "@OTF2_DOWNLOAD_ROOT@"
    SOURCE_DIR   "@OTF2_DOWNLOAD_ROOT@/otf2-src"
    BINARY_DIR   "@OTF2_DOWNLOAD_ROOT@/otf2-build"
    INSTALL_DIR  "@OTF2_DOWNLOAD_ROOT@"

    TLS_VERIFY           ON
    TIMEOUT              120
    INACTIVITY_TIMEOUT   60

    # Autotools-style build
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-shared --enable-static
    BUILD_COMMAND     ${CMAKE_MAKE_PROGRAM}
    INSTALL_COMMAND   ${CMAKE_MAKE_PROGRAM} install

    TEST_COMMAND ""
)
