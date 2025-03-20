
add_executable(test_malloc test_malloc.cpp)

target_link_libraries(test_malloc PRIVATE ${CMAKE_THREAD_LIBS_INIT})
