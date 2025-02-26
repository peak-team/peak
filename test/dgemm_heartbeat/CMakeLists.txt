
add_executable(test_dgemm_heartbeat test_dgemm_heartbeat.f)

target_link_libraries(test_dgemm_heartbeat PRIVATE ${BLAS_LIBRARIES})
