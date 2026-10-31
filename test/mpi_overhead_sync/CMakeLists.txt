add_executable(test_mpi_overhead_policy test_policy.c
    ${PROJECT_SOURCE_DIR}/src/mpi_overhead_policy.c)
target_include_directories(test_mpi_overhead_policy PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_mpi_overhead_policy PRIVATE ${M_LIBRARY})
add_test(NAME test_mpi_overhead_policy COMMAND test_mpi_overhead_policy)
set_tests_properties(test_mpi_overhead_policy PROPERTIES TIMEOUT 10)
