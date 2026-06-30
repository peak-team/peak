add_executable(test_memory test_memory.cpp)
find_package(Threads REQUIRED)
target_link_libraries(test_memory PRIVATE Threads::Threads)
