add_executable(test_close_interposition test_close_interposition.c)

if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
    add_executable(test_close_raw_fallback
        close_raw_fallback_test.c
        ${PROJECT_SOURCE_DIR}/src/syscall_interceptor.c)
    target_include_directories(test_close_raw_fallback
        PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_close_raw_fallback PRIVATE ${CMAKE_DL_LIBS})
endif()
