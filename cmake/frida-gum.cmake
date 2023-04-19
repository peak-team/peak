macro(fetch_frida_gum _download_module_path _download_root)
    set(FRIDA_GUM_DOWNLOAD_ROOT ${_download_root})
    configure_file(
        ${_download_module_path}/frida-gum-download.cmake
        ${_download_root}/CMakeLists.txt
        @ONLY
        )
    unset(FRIDA_GUM_DOWNLOAD_ROOT)

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
        WORKING_DIRECTORY
            ${_download_root}
        )
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" --build .
        WORKING_DIRECTORY
            ${_download_root}
        )

    set (FRIDA_GUM_LIBRARIES ${PROJECT_BINARY_DIR}/frida-gum/libfrida-gum.a)
    set (FRIDA_GUM_INCLUDE_DIRS ${PROJECT_BINARY_DIR}/frida-gum)
endmacro()