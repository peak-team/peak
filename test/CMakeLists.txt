include(CTest)
if(BUILD_TESTING)
    # Find or download blas only for tests
    find_package(BLAS)
    if(NOT BLAS_FOUND)
        message(STATUS "Building OpenBLAS")
        include(${PROJECT_SOURCE_DIR}/cmake/openblas.cmake)
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
        include(${PROJECT_SOURCE_DIR}/cmake/openblas.cmake)
        fetch_openblas(
            ${PROJECT_SOURCE_DIR}/cmake
            ${PROJECT_BINARY_DIR}/openblas
            )
        set(LAPACK_LIBRARIES ${PROJECT_BINARY_DIR}/openblas/lib/libopenblas.a)
    endif()

    find_package(OpenMP)

    include (FortranCInterface)
    list (APPEND FORTRAN_FUNCTIONS sdot dgemm)
    FortranCInterface_HEADER (include/FC.h 
        MACRO_NAMESPACE "FC_"
        SYMBOLS ${FORTRAN_FUNCTIONS}
    )

    # Add test of dgemm
    add_subdirectory(dgemm)
    add_test(NAME test_dgemm COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_dgemm 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=dgemm_,main,dummy;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
    )
    set_tests_properties(test_dgemm 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_[ \t]+100000 times .*[\r\n\t ]*main[ \t]+1 times"
    )

    # Add test of sdot
    add_subdirectory(sdot)
    add_test(NAME test_sdot COMMAND ${PROJECT_BINARY_DIR}/test/sdot/test_sdot)
    if(OpenMP_FOUND)
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=cblas_sdot,main,my_sleep_func;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
        )
    else()
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=cblas_sdot,main,my_sleep_func;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
        )
    endif()
    set_tests_properties(test_sdot
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "cblas_sdot[ \t]+1000000 times .*[\r\n\t ]*main[ \t]+1 times .*[\r\n\t ]*my_sleep_func[ \t]+56 times.*5.6"
    )
    
    if(MPI_FOUND)
        add_subdirectory(dgemm_mpi)
        add_test(NAME test_dgemm_mpi COMMAND ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} 2 ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi)
        math(EXPR N_CPU_DIV2 "${N_CPU}/2")
        if(OpenMP_FOUND)
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
            )
        else()
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so"
            )
        endif()
        set_tests_properties(test_dgemm_mpi
            PROPERTIES 
                PASS_REGULAR_EXPRESSION "cblas_dgemm[ \t]+200 times"
        )
    endif()
endif()

