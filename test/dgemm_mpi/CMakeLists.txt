
add_executable(test_dgemm_mpi test_dgemm_mpi.c)
add_executable(test_mpi_exit_before_finalize test_mpi_exit_before_finalize.c)

target_include_directories(test_dgemm_mpi
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_dgemm_mpi PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_dgemm_mpi PRIVATE ${BLAS_LIBRARIES} PUBLIC ${OpenMP_C_LIBRARIES} MPI::MPI_C)
else()
    target_link_libraries(test_dgemm_mpi PRIVATE ${BLAS_LIBRARIES} MPI::MPI_C)
endif()
target_link_libraries(test_mpi_exit_before_finalize PRIVATE MPI::MPI_C)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_mpi_exit_before_finalize PRIVATE -Wl,--export-dynamic)
endif()
