
add_executable(test_dgemm test_dgemm.f)

target_link_libraries(test_dgemm PRIVATE ${BLAS_LIBRARIES})
