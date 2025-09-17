add_executable(test_table1 test_table1.c)

target_include_directories(test_table1
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_table1 PRIVATE ${BLAS_LIBRARIES})

add_executable(test_table2 test_table2.c)

target_include_directories(test_table2
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_table2 PRIVATE ${BLAS_LIBRARIES})

add_executable(test_table3 test_table3.c)

add_executable(test_table4 test_table4.c)

target_include_directories(test_table4
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_table4 PRIVATE ${BLAS_LIBRARIES})

add_executable(test_table5 test_table5.c)

target_include_directories(test_table5
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_table5 PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_table5 PUBLIC ${OpenMP_C_LIBRARIES})
endif()

target_link_libraries(test_table5 PRIVATE ${BLAS_LIBRARIES})

add_executable(test_table6 test_table6.c)

target_include_directories(test_table6
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_table6 PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_table6 PUBLIC ${OpenMP_C_LIBRARIES})
endif()

target_link_libraries(test_table6 PRIVATE ${BLAS_LIBRARIES})

add_executable(test_table7 test_table7.c)

target_include_directories(test_table7
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_table7 PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_table7 PUBLIC ${OpenMP_C_LIBRARIES})
endif()

target_link_libraries(test_table7 PRIVATE ${BLAS_LIBRARIES})

add_executable(test_table8_9 test_table8_9.c)

target_include_directories(test_table8_9
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)

target_link_libraries(test_table8_9 PRIVATE ${BLAS_LIBRARIES})