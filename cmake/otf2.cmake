macro(fetch_otf2 _download_module_path _download_root)
    set(OTF2_DOWNLOAD_ROOT ${_download_root})
    file(MAKE_DIRECTORY "${_download_root}")

    configure_file(
        "${_download_module_path}/otf2-download.cmake"
        "${_download_root}/CMakeLists.txt"
        @ONLY
    )
    unset(OTF2_DOWNLOAD_ROOT)

    # Configure and build the helper project (this runs ExternalProject_Add)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
        WORKING_DIRECTORY "${_download_root}"
    )
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build .
        WORKING_DIRECTORY "${_download_root}"
    )

    # Where we told ExternalProject to install OTF2
    set(OTF2_ROOT "${_download_root}/otf2-install")

    # Includes: ${OTF2_ROOT}/include/otf2/otf2.h etc.
    set(OTF2_INCLUDE_DIRS "${OTF2_ROOT}/include")

    # Library dir: prefer lib, fall back to lib64
    if (EXISTS "${OTF2_ROOT}/lib")
        set(_otf2_lib_dir "${OTF2_ROOT}/lib")
    elseif (EXISTS "${OTF2_ROOT}/lib64")
        set(_otf2_lib_dir "${OTF2_ROOT}/lib64")
    else()
        set(_otf2_lib_dir "${OTF2_ROOT}/lib")
    endif()

    find_library(OTF2_LIBRARY
        NAMES otf2
        PATHS "${_otf2_lib_dir}"
        NO_DEFAULT_PATH
    )

    if (NOT OTF2_LIBRARY)
        message(FATAL_ERROR "fetch_otf2: could not find libotf2 in ${_otf2_lib_dir}")
    endif()

    set(OTF2_LIBRARIES "${OTF2_LIBRARY}")

    # Optional: expose lib dir too
    set(OTF2_LIBRARY_DIR "${_otf2_lib_dir}")
endmacro()