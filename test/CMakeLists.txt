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

    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        set(PRELOAD_VAR "LD_PRELOAD=${PROJECT_BINARY_DIR}/src/libpeak.so")
    elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set(PRELOAD_VAR "DYLD_INSERT_LIBRARIES=${PROJECT_BINARY_DIR}/src/libpeak.dylib")
    endif()

    configure_file(${PROJECT_SOURCE_DIR}/test/data/blas_symbols.conf ${PROJECT_BINARY_DIR}/test/data/blas_symbols.conf COPYONLY)
    set(PEAK_TARGET_CONFIG_PATH ${PROJECT_BINARY_DIR}/test/data/blas_symbols.conf)

    # Add test of dgemm
    add_subdirectory(dgemm)
    add_test(NAME test_dgemm COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_dgemm 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=dgemm_,main,dummy;${PRELOAD_VAR}"
    )
    set_tests_properties(test_dgemm 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of TARGET_CONFIG
    add_test(NAME test_target_config COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_target_config 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_CONFIG=BLAS;${PRELOAD_VAR}"
    )
    set_tests_properties(test_target_config 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of config_file
    add_test(NAME test_config_file COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_config_file 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_CONFIG_ENV=${PEAK_TARGET_CONFIG_PATH};${PRELOAD_VAR}"
    )
    set_tests_properties(test_config_file 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of sdot
    add_subdirectory(sdot)
    add_test(NAME test_sdot COMMAND ${PROJECT_BINARY_DIR}/test/sdot/test_sdot)
    if(OpenMP_FOUND)
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=cblas_sdot,main;${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=cblas_sdot,main;${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_sdot
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "cblas_sdot\\|[ \t]+1000000\\|"
    )

    # Add test of sleep
    add_subdirectory(sleep)
    add_test(NAME test_sleep COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
    if(OpenMP_FOUND)
        set_tests_properties(test_sleep
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=main,my_sleep_func;${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_sleep
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=main,my_sleep_func;${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_sleep
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "my_sleep_func\\|[ \t]+1000\\|.*my_sleep_func\\|[ \t]+10\\."
    )

    # Add test of recursive
    add_subdirectory(recursive)
    add_test(NAME test_recursive COMMAND ${PROJECT_BINARY_DIR}/test/recursive/test_recursive)
    set_tests_properties(test_recursive
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=functionA,functionB;${PRELOAD_VAR}"
    )

    # Add test of fftw
    add_subdirectory(fftw)
    add_test(NAME test_fftw COMMAND ${PROJECT_BINARY_DIR}/test/fftw/test_fftw)
    set_tests_properties(test_fftw
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_CONFIG=FFTW;${PRELOAD_VAR}"
    )
    
    if(MPI_FOUND)
        add_subdirectory(dgemm_mpi)
        add_test(NAME test_dgemm_mpi COMMAND ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} 2 ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi)
        math(EXPR N_CPU_DIV2 "${N_CPU}/2")
        if(OpenMP_FOUND)
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;${PRELOAD_VAR}"
            )
        else()
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;${PRELOAD_VAR}"
            )
        endif()
        set_tests_properties(test_dgemm_mpi
            PROPERTIES 
                PASS_REGULAR_EXPRESSION "cblas_dgemm\\|[ \t]+200\\|"
        )
    endif()

   
endif()

