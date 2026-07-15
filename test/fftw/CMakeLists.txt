
add_executable(test_fftw test_fftw.c)

target_include_directories(test_fftw
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_fftw PRIVATE ${BLAS_LIBRARIES})

if(UNIX)
    find_library(PEAK_TEST_FFTW3_LIBRARY
        NAMES fftw3 libfftw3.so.3
    )

    if(PEAK_TEST_FFTW3_LIBRARY)
        add_executable(test_fftw_runtime_dlopen test_fftw_runtime_dlopen.c)
        target_link_libraries(test_fftw_runtime_dlopen PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()
