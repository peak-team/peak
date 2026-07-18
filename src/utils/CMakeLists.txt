# Keep the runtime utility boundary explicit so new or stale files cannot enter
# the PEAK library merely by appearing in this directory.
set(sources_utils_c
    env_parser.c
    mpi_utils.c
    parent_registry.c
    process_filter.c
    source_target.c
    target_config.c
    timing.c
)

set(sources_utils_cxx
    cxx_utils.cpp
)

# Generate the shared library from the library sources
add_library(utils_c OBJECT ${sources_utils_c})
add_library(utils_cxx OBJECT ${sources_utils_cxx})
set_target_properties(utils_cxx PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(utils_c PUBLIC ${PROJECT_SOURCE_DIR}/include/utils)
target_include_directories(utils_cxx PUBLIC ${PROJECT_SOURCE_DIR}/include/utils)
