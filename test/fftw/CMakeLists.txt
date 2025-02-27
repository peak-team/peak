
add_executable(test_fftw test_fftw.c)

target_include_directories(test_fftw
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_fftw PRIVATE ${BLAS_LIBRARIES})