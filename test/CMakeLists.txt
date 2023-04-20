include(CTest)
if(BUILD_TESTING)
    # Find or download blas only for tests
    find_package(BLAS)
    if(NOT BLAS_FOUND)
        message(STATUS "Building OpenBLAS")
        include(cmake/openblas.cmake)
        fetch_openblas(
            ${PROJECT_SOURCE_DIR}/cmake
            ${PROJECT_BINARY_DIR}/openblas
            )
        set(BLAS_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    endif()

    find_package(LAPACK)
    if(NOT LAPACK_FOUND AND NOT BLAS_FOUND)
        message(STATUS "Use OpenBLAS for LAPACK")
        set(BLAS_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
        set(LAPACK_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    elseif(NOT LAPACK_FOUND)
        message(STATUS "Building OpenBLAS for LAPACK")
        include(cmake/openblas.cmake)
        fetch_openblas(
            ${PROJECT_SOURCE_DIR}/cmake
            ${PROJECT_BINARY_DIR}/openblas
            )
        set(LAPACK_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    endif()

    include (FortranCInterface)
    list (APPEND FORTRAN_FUNCTIONS sdot)
    FortranCInterface_HEADER (include/FC.h 
        MACRO_NAMESPACE "FC_"
        SYMBOLS ${FORTRAN_FUNCTIONS}
    )

    # Add test of dgemm
    add_subdirectory(dgemm)
    add_test(NAME test_dgemm COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_dgemm 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=dgemm_,__libc_start_main,dummy;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
    )
    set_tests_properties(test_dgemm 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_ is called 100000 times .*[\r\n\t ]*__libc_start_main is called 1 times"
    )

    # Add test of sdot
    add_subdirectory(sdot)
    add_test(NAME test_sdot COMMAND ${PROJECT_BINARY_DIR}/test/sdot/test_sdot)
    set_tests_properties(test_sdot
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=sdot_,__libc_start_main,dummy;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
    )
    set_tests_properties(test_sdot
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "sdot_ is called 10000 times .*[\r\n\t ]*__libc_start_main is called 1 times"
    )
endif()