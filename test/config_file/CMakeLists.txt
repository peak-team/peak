
add_executable(test_config_file test_config_file.f)

target_link_libraries(test_config_file PRIVATE ${BLAS_LIBRARIES})
