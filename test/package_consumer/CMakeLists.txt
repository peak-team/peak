cmake_minimum_required(VERSION 3.13)
project(peak_package_consumer C)

find_package(PEAK CONFIG REQUIRED)
add_executable(peak_package_consumer main.c)
target_link_libraries(peak_package_consumer PRIVATE PEAK::peak)
if(NOT DEFINED PEAK_SOURCE_DIR)
    message(FATAL_ERROR "PEAK_SOURCE_DIR is required to verify exported paths")
endif()

get_target_property(peak_include_dirs PEAK::peak INTERFACE_INCLUDE_DIRECTORIES)
string(FIND "${peak_include_dirs}" "${PEAK_SOURCE_DIR}" peak_source_dir_index)
if(NOT peak_source_dir_index EQUAL -1)
    message(FATAL_ERROR "PEAK package exports a source-tree include path")
endif()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/peak-target-path.txt"
    CONTENT "$<TARGET_FILE:PEAK::peak>\n")
add_custom_target(peak_package_target_path ALL
    DEPENDS "${CMAKE_BINARY_DIR}/peak-target-path.txt")
