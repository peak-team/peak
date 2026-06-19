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
    find_package(MPI)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if (CMAKE_VERSION VERSION_LESS 3.17)
        find_package(CUDA)
    else()
        find_package(CUDAToolkit)
    endif()
    if (CUDA_FOUND OR CUDAToolkit_FOUND)
        include_directories(${CUDA_INCLUDE_DIRS} ${CUDAToolkit_INCLUDE_DIRS}) 
    endif()

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

    set(HEARTBEAT_OFF_ENV "PEAK_HEARTBEAT_INTERVAL=0")
    set(HEARTBEAT_ON_ENV "PEAK_HEARTBEAT_INTERVAL=0.1")

    add_test(NAME test_gum_mutation_allowlist
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_gum_mutation_allowlist.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_gum_mutation_allowlist
        PROPERTIES
            PASS_REGULAR_EXPRESSION "gum_mutation_allowlist_ok")

    add_test(NAME test_cuda_interceptor_consistency
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_cuda_interceptor_consistency.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_cuda_interceptor_consistency
        PROPERTIES
            PASS_REGULAR_EXPRESSION "cuda_interceptor_consistency_ok")

    add_test(NAME test_detach_lifecycle_invariants
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/test/static_checks/check_detach_lifecycle_invariants.py
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_detach_lifecycle_invariants
        PROPERTIES
            PASS_REGULAR_EXPRESSION "detach_lifecycle_invariants_ok")

    if(TARGET peak_detach_helper)
        add_test(NAME test_detach_helper_self_test
            COMMAND $<TARGET_FILE:peak_detach_helper> --self-test)
        set_tests_properties(test_detach_helper_self_test
            PROPERTIES
                PASS_REGULAR_EXPRESSION "peak_detach_helper_self_test_ok")
        if(CMAKE_SYSTEM_NAME MATCHES "Linux")
            add_test(NAME test_detach_helper_self_test_preloaded
                COMMAND $<TARGET_FILE:peak_detach_helper> --self-test)
            set_tests_properties(test_detach_helper_self_test_preloaded
                PROPERTIES
                    ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:peak>;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_TARGET=peak_detach_hot_target"
                    PASS_REGULAR_EXPRESSION "peak_detach_helper_self_test_ok"
                    FAIL_REGULAR_EXPRESSION "PEAK Library|overhead calibration|Estimated overhead")
        endif()
    endif()

    configure_file(${PROJECT_SOURCE_DIR}/test/data/blas_symbols.conf ${PROJECT_BINARY_DIR}/test/data/blas_symbols.conf COPYONLY)
    set(PEAK_TARGET_CONFIG_PATH ${PROJECT_BINARY_DIR}/test/data/blas_symbols.conf)

    add_subdirectory(detach_controller)
    add_subdirectory(detach_benchmark)
    set(PEAK_STRICT_DETACH_BENCHMARK_MIN_RATIO "0.80" CACHE STRING
        "Minimum current strict physical-detach throughput as a fraction of no-preload throughput")
    set(PEAK_STRICT_DETACH_STRESS_THREADS "32" CACHE STRING
        "Thread count for strict physical-detach hot-loop stress testing")
    set(PEAK_STRICT_DETACH_STRESS_SAMPLES "12" CACHE STRING
        "Number of repeated strict physical-detach hot-loop stress samples")
    set(PEAK_STRICT_DETACH_STRESS_SECONDS "1" CACHE STRING
        "Duration in seconds for each strict physical-detach hot-loop stress sample")

    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
       PEAK_GUM_PEAK_PC_API_AVAILABLE AND
       TARGET peak_detach_helper)
        add_test(NAME test_detach_hotloop_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop --threads 4 --seconds 2)
        set_tests_properties(test_detach_hotloop_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_hot_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                TIMEOUT 20)
        add_test(NAME test_detach_hotloop_benchmark_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_hotloop_benchmark.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                    --current-libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 1
                    --samples 3
                    --min-strict-baseline-ratio ${PEAK_STRICT_DETACH_BENCHMARK_MIN_RATIO})
        set_tests_properties(test_detach_hotloop_benchmark_strict
            PROPERTIES
                TIMEOUT 90
                PASS_REGULAR_EXPRESSION "current_strict_physical:.*detached_runs=3/3")
        add_test(NAME test_detach_hotloop_high_thread_stress
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_hotloop_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads ${PEAK_STRICT_DETACH_STRESS_THREADS}
                    --seconds ${PEAK_STRICT_DETACH_STRESS_SECONDS}
                    --samples ${PEAK_STRICT_DETACH_STRESS_SAMPLES}
                    --disable-reattach
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-detach-trace)
        set_tests_properties(test_detach_hotloop_high_thread_stress
            PROPERTIES
                TIMEOUT 240
                PASS_REGULAR_EXPRESSION "hotloop_stress_ok")
        add_test(NAME test_detach_hotloop_batch_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_hotloop_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 2
                    --samples 1
                    --timeout 30
                    --paired-targets
                    --disable-reattach
                    --peak-cost 0
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-batch-trace
                    --require-detach-batch-size 2
                    --fail-on-transition-skips)
        set_tests_properties(test_detach_hotloop_batch_strict
            PROPERTIES
                TIMEOUT 45
                PASS_REGULAR_EXPRESSION "hotloop_stress_ok")
        if(TARGET test_detach_fake_helper)
            add_test(NAME test_detach_hotloop_batch_retry_preserves_pending_strict
                COMMAND ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                        --controller-batch-retry-check)
            set_tests_properties(test_detach_hotloop_batch_retry_preserves_pending_strict
                PROPERTIES
                    ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target,peak_detach_hot_target_two;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-batch-retry-trace.csv;${PRELOAD_VAR}"
                    PASS_REGULAR_EXPRESSION "controller_batch_retry_ok"
                    TIMEOUT 30)
            foreach(_unsupported_gum_pc_case IN ITEMS on-enter-plus-one on-leave on-invoke enter-thunk)
                add_test(NAME test_detach_hotloop_unsupported_gum_pc_${_unsupported_gum_pc_case}_preserves_pending_strict
                    COMMAND ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                            --controller-unsupported-gum-pc-retry-check
                            --unsupported-gum-pc-case ${_unsupported_gum_pc_case})
                set_tests_properties(test_detach_hotloop_unsupported_gum_pc_${_unsupported_gum_pc_case}_preserves_pending_strict
                    PROPERTIES
                        ENVIRONMENT "PEAK_TARGET=peak_detach_hot_target,peak_detach_hot_target_two;PEAK_MAX_NUM_THREADS=16;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_ENABLE_REATTACH=0;PEAK_COST=0;PEAK_HEARTBEAT_INTERVAL=0;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=synthetic-stop-file-once;FAKE_DETACH_HELPER_STOP_PC_FILE=${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-unsupported-gum-pc-${_unsupported_gum_pc_case}.txt;FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-unsupported-gum-pc-${_unsupported_gum_pc_case}-helper.log;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-unsupported-gum-pc-${_unsupported_gum_pc_case}-trace.csv;${PRELOAD_VAR}"
                        PASS_REGULAR_EXPRESSION "controller_unsupported_gum_pc_retry_ok"
                        SKIP_RETURN_CODE 77
                        TIMEOUT 30)
            endforeach()
        endif()
        add_test(NAME test_detach_hotloop_batch_reattach_strict
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_hotloop_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads 4
                    --seconds 3
                    --samples 1
                    --timeout 45
                    --paired-targets
                    --overhead-ratio 0.01
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-batch-reattach-trace
                    --require-detach-batch-size 2
                    --require-reattach-batch-size 2)
        set_tests_properties(test_detach_hotloop_batch_reattach_strict
            PROPERTIES
                TIMEOUT 60
                PASS_REGULAR_EXPRESSION "hotloop_stress_ok")
        add_test(NAME test_detach_hotloop_reattach_stress
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_hotloop_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_hotloop
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --threads ${PEAK_STRICT_DETACH_STRESS_THREADS}
                    --seconds 3
                    --samples 6
                    --overhead-ratio 0.01
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-hotloop-reattach-trace
                    --require-reattach)
        set_tests_properties(test_detach_hotloop_reattach_stress
            PROPERTIES
                TIMEOUT 240
                PASS_REGULAR_EXPRESSION "hotloop_stress_ok")
        add_test(NAME test_detach_stale_threads_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_stale_threads
                    --active-threads 8
                    --stale-threads 24
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2)
        set_tests_properties(test_detach_stale_threads_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=96;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests"
                TIMEOUT 40)
        add_test(NAME test_detach_stale_threads_strict_low_tracking_capacity
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_stale_threads
                    --active-threads 8
                    --stale-threads 24
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2)
        set_tests_properties(test_detach_stale_threads_strict_low_tracking_capacity
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=8;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=0;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests"
                TIMEOUT 40)
        add_test(NAME test_detach_stale_threads_reattach_strict
            COMMAND ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_stale_threads
                    --active-threads 16
                    --stale-threads 48
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 3)
        set_tests_properties(test_detach_stale_threads_reattach_strict
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=peak_detach_stale_target;PEAK_MAX_NUM_THREADS=192;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_ENABLE_REATTACH=1;PEAK_COST=0.0000001;PEAK_OVERHEAD_RATIO=0.01;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_HIBERNATION_CYCLE=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_TRACE_PATH=${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-stale-reattach-trace.csv;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "peak_detach_stale_target[*][*]\\|[ \t]+[1-9][0-9]*\\|"
                FAIL_REGULAR_EXPRESSION "not proven safe|leaving listener state alive|fatal|tracked thread snapshot exceeded|thread id .* exceeds|detach helper shutdown failed|timed out draining pending target hook detach/reattach requests|Gum .* failed"
                TIMEOUT 60)
        add_test(NAME test_detach_stale_threads_stress
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_stale_thread_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_stale_threads
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --active-threads 16
                    --stale-threads 48
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2
                    --samples 6
                    --stale-mode parked
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-stale-stress-trace)
        set_tests_properties(test_detach_stale_threads_stress
            PROPERTIES
                TIMEOUT 180
                PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")
        add_test(NAME test_detach_stale_threads_unrelated_spin_stress
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_stale_thread_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_stale_threads
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --active-threads 16
                    --stale-threads 48
                    --stale-calls 2048
                    --active-after-stale
                    --seconds 2
                    --samples 4
                    --stale-mode unrelated-spin
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-stale-unrelated-spin-trace)
        set_tests_properties(test_detach_stale_threads_unrelated_spin_stress
            PROPERTIES
                TIMEOUT 180
                PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")
        add_test(NAME test_detach_stale_threads_reattach_stress
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/test/detach_benchmark/run_detach_stale_thread_stress.py
                    --exe ${PROJECT_BINARY_DIR}/test/detach_benchmark/test_detach_stale_threads
                    --libpeak ${PROJECT_BINARY_DIR}/src/libpeak.so
                    --active-threads 16
                    --stale-threads 48
                    --stale-calls 1
                    --active-after-stale
                    --seconds 3
                    --stale-ready-timeout 20
                    --samples 4
                    --overhead-ratio 0.01
                    --require-reattach
                    --stale-mode parked
                    --trace-prefix ${PROJECT_BINARY_DIR}/test/detach_benchmark/peak-stale-reattach-stress-trace)
        set_tests_properties(test_detach_stale_threads_reattach_stress
            PROPERTIES
                TIMEOUT 180
                PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")
    endif()

    # Add test of dgemm
    add_subdirectory(dgemm)
    add_test(NAME test_dgemm COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_dgemm 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=dgemm_,main,dummy;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_dgemm 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of TARGET_CONFIG
    add_test(NAME test_target_config COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_target_config 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_GROUP=BLAS;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_target_config 
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "dgemm_\\|[ \t]+100000\\|"
    )

    # Add test of config_file
    add_test(NAME test_config_file COMMAND ${PROJECT_BINARY_DIR}/test/dgemm/test_dgemm)
    set_tests_properties(test_config_file 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_FILE=${PEAK_TARGET_CONFIG_PATH};${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
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
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=cblas_sdot,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_sdot
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=cblas_sdot,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
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
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_sleep
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_sleep
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "my_sleep_func\\|[ \t]+1000\\|.*my_sleep_func\\|[ \t]+10\\."
    )
    if(NOT PEAK_GUM_PEAK_API_AVAILABLE)
        add_test(NAME test_initial_attach_strict_stock_gum COMMAND ${PROJECT_BINARY_DIR}/test/sleep/test_sleep)
        set_tests_properties(test_initial_attach_strict_stock_gum
            PROPERTIES
                ENVIRONMENT "PEAK_TARGET=my_sleep_func;PEAK_REQUIRE_SAFE_DETACH=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_initial_attach_strict_statslog;${PRELOAD_VAR}"
                PASS_REGULAR_EXPRESSION "skipping initial Gum attach.*my_sleep_func.*missing-gum-api"
                FAIL_REGULAR_EXPRESSION "my_sleep_func\\|[ \t]+[1-9][0-9]*\\|"
        )
    endif()

    # Add test of recursive
    add_subdirectory(recursive)
    add_test(NAME test_recursive COMMAND ${PROJECT_BINARY_DIR}/test/recursive/test_recursive)
    set_tests_properties(test_recursive
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=functionA,functionB;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )

    # Add test of fftw
    add_subdirectory(fftw)
    add_test(NAME test_fftw COMMAND ${PROJECT_BINARY_DIR}/test/fftw/test_fftw)
    set_tests_properties(test_fftw
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET_CONFIG=FFTW;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    
    if(MPI_FOUND)
        add_subdirectory(dgemm_mpi)
        set(_PEAK_MPI_TEST_NPROCS 2)
        if(MPIEXEC_MAX_NUMPROCS AND MPIEXEC_MAX_NUMPROCS LESS _PEAK_MPI_TEST_NPROCS)
            set(_PEAK_MPI_TEST_NPROCS ${MPIEXEC_MAX_NUMPROCS})
        endif()
        add_test(NAME test_dgemm_mpi
            COMMAND ${MPIEXEC_EXECUTABLE}
                    ${MPIEXEC_NUMPROC_FLAG} ${_PEAK_MPI_TEST_NPROCS}
                    ${MPIEXEC_PREFLAGS}
                    ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi
                    ${MPIEXEC_POSTFLAGS})
        if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
            add_test(NAME test_dgemm_mpi_strict
                COMMAND ${MPIEXEC_EXECUTABLE}
                        ${MPIEXEC_NUMPROC_FLAG} ${_PEAK_MPI_TEST_NPROCS}
                        ${MPIEXEC_PREFLAGS}
                        ${PROJECT_BINARY_DIR}/test/dgemm_mpi/test_dgemm_mpi
                        500000
                        ${MPIEXEC_POSTFLAGS})
        endif()
        math(EXPR N_CPU_DIV2 "${N_CPU}/2")
        if(OpenMP_FOUND)
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            )
            if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
                set_tests_properties(test_dgemm_mpi_strict
                    PROPERTIES
                        ENVIRONMENT "OMP_NUM_THREADS=${N_CPU_DIV2};PEAK_TARGET=cblas_dgemm,main;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}"
                        TIMEOUT 90
                )
            endif()
        else()
            set_tests_properties(test_dgemm_mpi
                PROPERTIES 
                    ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
            )
            if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
                set_tests_properties(test_dgemm_mpi_strict
                    PROPERTIES
                        ENVIRONMENT "PEAK_TARGET=cblas_dgemm,main;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}"
                        TIMEOUT 90
                )
            endif()
        endif()
        set_tests_properties(test_dgemm_mpi
            PROPERTIES 
                PASS_REGULAR_EXPRESSION "cblas_dgemm[*]*\\|[ \t]+200\\|"
        )
        if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
            set_tests_properties(test_dgemm_mpi_strict
                PROPERTIES
                    PASS_REGULAR_EXPRESSION "cblas_dgemm[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
            )
        endif()
    endif()

    # Add test of heartbeat
    add_subdirectory(heartbeat)
    add_test(NAME test_heartbeat COMMAND ${PROJECT_BINARY_DIR}/test/heartbeat/test_heartbeat)
    if(OpenMP_FOUND)
        set_tests_properties(test_heartbeat
            PROPERTIES 
                ENVIRONMENT "OMP_NUM_THREADS=${N_CPU};PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_ON_ENV};${PRELOAD_VAR}"
        )
    else()
        set_tests_properties(test_heartbeat
            PROPERTIES 
                ENVIRONMENT "PEAK_TARGET=main,my_sleep_func;${HEARTBEAT_ON_ENV};${PRELOAD_VAR}"
        )
    endif()
    set_tests_properties(test_heartbeat
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "my_sleep_func\\|"
    )

    # Add Cuda Test cases
    if(CUDA_FOUND OR CUDAToolkit_FOUND)
        add_subdirectory(cuda)

        # add_test(NAME test_ompmpi COMMAND ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} 2 ${PROJECT_BINARY_DIR}/test/parallel_tests/test_omp_mpi)
        # set_tests_properties(test_omp_mpi PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=4;PEAK_TARGET=main;${PRELOAD_VAR}")

        # add_test(NAME test_mpi COMMAND ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} 2 ${PROJECT_BINARY_DIR}/test/parallel_tests/test_mpi)
        # set_tests_properties(test_mpi PROPERTIES ENVIRONMENT "PEAK_TARGET=main;${PRELOAD_VAR}")

        # add_test(NAME test_pthread COMMAND ${PROJECT_BINARY_DIR}/test/parallel_tests/test_pthread)
        # set_tests_properties(test_pthread PROPERTIES ENVIRONMENT "PEAK_TARGET=main;${PRELOAD_VAR}")
    endif()
    
    # Add test of dynamic library intercepting
    add_subdirectory(dynamic_lib)

    # Add test of memory allocation intercepting
    add_subdirectory(memory)
    add_test(NAME test_memory COMMAND ${PROJECT_BINARY_DIR}/test/memory/test_memory)
    set_tests_properties(test_memory 
        PROPERTIES 
            ENVIRONMENT "PEAK_TARGET=performAllocation,performFree;PEAK_MEMORY_PROFILE=TRUE;${HEARTBEAT_OFF_ENV};${PRELOAD_VAR}"
    )
    set_tests_properties(test_memory
        PROPERTIES 
            PASS_REGULAR_EXPRESSION "performAllocation*\\|[ \t]+[0-9]+\\|.*performAllocation*\\|[ \t]+[0-9]+(\\.[0-9]+)?"
            PASS_REGULAR_EXPRESSION "performFree*\\|[ \t]+[0-9]+\\|.*performFree*\\|[ \t]+[0-9]+(\\.[0-9]+)?"
            PASS_REGULAR_EXPRESSION "Max usage \\(bytes\\):[ \t]*[0-9]+"
    )

endif()
