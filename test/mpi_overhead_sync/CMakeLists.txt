add_executable(test_mpi_overhead_policy test_policy.c
    ${PROJECT_SOURCE_DIR}/src/mpi_overhead_policy.c)
target_include_directories(test_mpi_overhead_policy PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_mpi_overhead_policy PRIVATE ${M_LIBRARY})
add_test(NAME test_mpi_overhead_policy COMMAND test_mpi_overhead_policy)
set_tests_properties(test_mpi_overhead_policy PROPERTIES TIMEOUT 10)

if(MPI_FOUND)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(test_mpi_overhead_self_profile test_self_profile.c)
    target_link_libraries(test_mpi_overhead_self_profile PRIVATE MPI::MPI_C ${CMAKE_DL_LIBS})
    add_test(NAME test_mpi_overhead_self_profile
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_self_profile.py
            --library $<TARGET_FILE:peak> --
            ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS}
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_self_profile.py
            --rank --library $<TARGET_FILE:peak> --
            $<TARGET_FILE:test_mpi_overhead_self_profile> ${MPIEXEC_POSTFLAGS})
    set_tests_properties(test_mpi_overhead_self_profile PROPERTIES TIMEOUT 45)
endif()
