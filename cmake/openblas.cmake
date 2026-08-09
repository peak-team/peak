macro(fetch_openblas _download_module_path _download_root)
    set(OPENBLAS_DOWNLOAD_ROOT ${_download_root})
    configure_file(
        ${_download_module_path}/openblas-download.cmake
        ${_download_root}/CMakeLists.txt
        @ONLY
        )
    unset(OPENBLAS_DOWNLOAD_ROOT)

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}" .
        WORKING_DIRECTORY
            ${_download_root}
        RESULT_VARIABLE _openblas_configure_result
        )
    if(NOT _openblas_configure_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to configure OpenBLAS download project in ${_download_root}")
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" --build .
        WORKING_DIRECTORY
            ${_download_root}
        RESULT_VARIABLE _openblas_build_result
        )
    if(NOT _openblas_build_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to build OpenBLAS download project in ${_download_root}")
    endif()

#    set (BLAS_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.so)
endmacro()
