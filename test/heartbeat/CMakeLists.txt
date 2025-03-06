
add_executable(test_heartbeat test_heartbeat.c)

target_include_directories(test_heartbeat
    PUBLIC 
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
if(OpenMP_FOUND)
    target_compile_options(test_heartbeat PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_heartbeat PUBLIC ${OpenMP_C_LIBRARIES})
endif()