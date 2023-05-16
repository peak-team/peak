
add_executable(test_recursive test_recursive.c)

target_include_directories(test_recursive
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_recursive PRIVATE ${BLAS_LIBRARIES})