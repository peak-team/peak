
add_executable(test_fftw test_fftw.c)

target_include_directories(test_fftw
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_fftw PRIVATE ${BLAS_LIBRARIES})

if(UNIX)
    add_library(test_fftw_runtime_provider SHARED
        test_fftw_runtime_provider.c)
    add_library(test_fftw_runtime_unrelated SHARED
        test_fftw_runtime_unrelated.c)
    add_library(test_fftw2_runtime_threads_provider SHARED
        test_fftw2_runtime_thread_provider.c)
    target_compile_definitions(test_fftw2_runtime_threads_provider PRIVATE
        PEAK_FFTW2_THREAD_SYMBOL=fftw_threads)
    add_library(test_rfftw2_runtime_threads_provider SHARED
        test_fftw2_runtime_thread_provider.c)
    target_compile_definitions(test_rfftw2_runtime_threads_provider PRIVATE
        PEAK_FFTW2_THREAD_SYMBOL=rfftw_threads)
    add_library(test_fftw_runtime_consumer SHARED
        test_fftw_runtime_consumer.c)
    target_link_libraries(test_fftw_runtime_consumer PRIVATE
        test_fftw_runtime_provider)
    add_library(test_fftw_runtime_extension SHARED
        test_fftw_runtime_extension.c)
    target_link_libraries(test_fftw_runtime_extension PRIVATE
        test_fftw_runtime_provider)
    add_executable(test_fftw_runtime_dlopen
        test_fftw_runtime_dlopen.c)
    target_include_directories(test_fftw_runtime_dlopen PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS})
    target_link_libraries(test_fftw_runtime_dlopen PRIVATE ${CMAKE_DL_LIBS})
    add_executable(test_dlopen_fork_child_pid_guard
        test_dlopen_fork_child_pid_guard.c)
    target_link_libraries(test_dlopen_fork_child_pid_guard PRIVATE
        ${CMAKE_DL_LIBS})
    add_executable(test_fftw_dlopen_cancellation
        test_fftw_dlopen_cancellation.c)
    target_link_libraries(test_fftw_dlopen_cancellation PRIVATE
        ${CMAKE_DL_LIBS}
        Threads::Threads)
endif()
