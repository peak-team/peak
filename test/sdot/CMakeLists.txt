
add_executable(test_sdot test_sdot.c)

target_include_directories(test_sdot
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_sdot PRIVATE ${BLAS_LIBRARIES})