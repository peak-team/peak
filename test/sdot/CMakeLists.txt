
add_executable(test_sdot test_sdot.c)

target_include_directories(test_sdot
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_sdot PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_sdot PRIVATE ${BLAS_LIBRARIES} PUBLIC ${OpenMP_C_LIBRARIES})
else()
    target_link_libraries(test_sdot PRIVATE ${BLAS_LIBRARIES})
endif()