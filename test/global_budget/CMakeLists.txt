# Focused regressions for the shared MPI budget. Protocol tests run without MPI;
# the application lifecycle tests require the enabled MPI profiler backend.
function(peak_add_budget_test name source)
    add_executable(${name} ${source} ${PROJECT_SOURCE_DIR}/src/global_budget_causal.c)
    peak_budget_configure_optional_runtime(${name})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_options(${name} PRIVATE -UNDEBUG)
    target_link_libraries(${name} PRIVATE Threads::Threads ${M_LIBRARY})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        target_sources(${name} PRIVATE
            ${PROJECT_SOURCE_DIR}/cmake/peak-gum/peak_aarch64_raw_syscall.S)
    endif()
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES TIMEOUT 20)
endfunction()

peak_add_budget_test(test_global_budget_ledger test_ledger.c)
peak_add_budget_test(test_global_budget_idle_return test_idle_return.c)
if(TARGET peak_budget_broker)
    add_library(test_budget_preload_probe SHARED preload_probe.c)
    peak_add_budget_test(test_global_budget_native_broker test_native_broker.c
        $<TARGET_FILE:peak_budget_broker> $<TARGET_FILE:peak>
        $<TARGET_FILE:test_budget_preload_probe>)
    add_dependencies(test_global_budget_native_broker test_budget_preload_probe)
    target_link_libraries(test_global_budget_native_broker PRIVATE ${CMAKE_DL_LIBS})
    target_sources(test_global_budget_native_broker PRIVATE
        ${PROJECT_SOURCE_DIR}/src/global_budget_spawn.c)
endif()

if(PEAK_ENABLE_MPI AND MPI_C_FOUND AND MPIEXEC_EXECUTABLE AND
   (PEAK_GUM_PEAK_PC_API_AVAILABLE OR PEAK_GUM_DARWIN_PATCH_API_AVAILABLE))
    add_executable(test_global_budget_mpi mpi_lifecycle.c)
    target_include_directories(test_global_budget_mpi PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_global_budget_mpi PRIVATE MPI::MPI_C ${CMAKE_DL_LIBS})
    set_target_properties(test_global_budget_mpi PROPERTIES ENABLE_EXPORTS ON)
    string(JOIN " " _budget_mpiexec_preflags ${MPIEXEC_PREFLAGS})
    foreach(scenario IN ITEMS recovery early-finalize startup-delay)
        add_test(NAME test_global_budget_mpi_${scenario}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_mpi_lifecycle.py
                --exe $<TARGET_FILE:test_global_budget_mpi> --libpeak $<TARGET_FILE:peak>
                --broker $<TARGET_FILE:peak_budget_broker>
                --mpiexec ${MPIEXEC_EXECUTABLE} --np-flag=${MPIEXEC_NUMPROC_FLAG}
                "--preflags=${_budget_mpiexec_preflags}"
                --mode ${scenario})
        set_tests_properties(test_global_budget_mpi_${scenario} PROPERTIES TIMEOUT 40 PROCESSORS 2)
    endforeach()
    add_test(NAME test_global_budget_mpi_singleton
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_mpi_lifecycle.py
            --exe $<TARGET_FILE:test_global_budget_mpi> --libpeak $<TARGET_FILE:peak>
            --broker $<TARGET_FILE:peak_budget_broker>
            --mpiexec ${MPIEXEC_EXECUTABLE} --np-flag=${MPIEXEC_NUMPROC_FLAG}
            "--preflags=${_budget_mpiexec_preflags}" --mode recovery --ranks 1)
    set_tests_properties(test_global_budget_mpi_singleton PROPERTIES TIMEOUT 40)
endif()
