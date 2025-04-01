# Add all source files
FILE(GLOB sources_utils_c
    env_parser.c
    mpi_utils.c
    utils.c
    source_target.c
)

FILE(GLOB sources_utils_cxx
    cxx_utils.cpp
)

# Generate the shared library from the library sources
add_library(utils_c OBJECT ${sources_utils_c})
add_library(utils_cxx OBJECT ${sources_utils_cxx})
set_target_properties(utils_cxx PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(utils_c PUBLIC ${PROJECT_SOURCE_DIR}/include/utils)
target_include_directories(utils_cxx PUBLIC ${PROJECT_SOURCE_DIR}/include/utils)
