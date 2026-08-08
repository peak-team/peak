
add_executable(test_heartbeat test_heartbeat.c)
add_executable(test_explicit_exit test_explicit_exit.c)
add_executable(test_malloc_rollback_worker test_malloc_rollback_worker.c)

target_link_libraries(test_malloc_rollback_worker PRIVATE pthread)

target_include_directories(test_heartbeat
    PUBLIC
        ${PROJECT_BINARY_DIR}/test/include
        ${PROJECT_SOURCE_DIR}/test/include
)
target_link_libraries(test_heartbeat PRIVATE pthread)
if(OpenMP_FOUND)
    target_compile_options(test_heartbeat PUBLIC ${OpenMP_C_FLAGS})
    target_link_libraries(test_heartbeat PUBLIC ${OpenMP_C_LIBRARIES})
endif()
