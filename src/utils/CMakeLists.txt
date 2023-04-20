# Add all source files
FILE(GLOB sources_utils
    env_parser.c
    utils.c
)

# Generate the shared library from the library sources
add_library(utils OBJECT ${sources_utils})

target_include_directories(utils
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include/utils
)

