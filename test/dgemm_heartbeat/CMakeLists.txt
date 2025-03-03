
add_executable(test_dgemm_heartbeat test_dgemm_heartbeat.c)

target_include_directories(test_dgemm_heartbeat
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_dgemm_heartbeat PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_dgemm_heartbeat PRIVATE ${BLAS_LIBRARIES} PUBLIC ${OpenMP_C_LIBRARIES} MPI::MPI_C)
else()
    target_link_libraries(test_dgemm_heartbeat PRIVATE ${BLAS_LIBRARIES} MPI::MPI_C)
endif()