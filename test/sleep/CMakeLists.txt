
add_executable(test_sleep test_sleep.c)

target_include_directories(test_sleep
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_sleep PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_sleep PRIVATE ${BLAS_LIBRARIES} PUBLIC ${OpenMP_C_LIBRARIES})
else()
    target_link_libraries(test_sleep PRIVATE ${BLAS_LIBRARIES})
endif()