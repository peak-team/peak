
add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE Threads::Threads)

add_executable(test_memlog_safety test_memlog_safety.c)
target_include_directories(test_memlog_safety PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(test_memlog_safety PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_memlog_safety PRIVATE peak Threads::Threads)
