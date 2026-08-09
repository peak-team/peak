cmake_minimum_required(VERSION 3.13)
project(peak_package_consumer C)

find_package(PEAK CONFIG REQUIRED)
add_executable(peak_package_consumer main.c)
target_link_libraries(peak_package_consumer PRIVATE PEAK::peak)

get_target_property(peak_include_dirs PEAK::peak INTERFACE_INCLUDE_DIRECTORIES)
foreach(include_dir IN LISTS peak_include_dirs)
    if(include_dir MATCHES "/home/iwang/git/peak")
        message(FATAL_ERROR "PEAK package exports a source-tree include path")
    endif()
endforeach()
