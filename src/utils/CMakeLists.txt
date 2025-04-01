# Add all source files
FILE(GLOB sources_utils_c
    env_parser.c
    mpi_utils.c
    utils.c
    source_target.c
)

FILE(GLOB sources_utils_cxx
    cuda_utils.cpp
)
set_source_files_properties(cuda_utils.cpp PROPERTIES LANGUAGE CUDA)

# Generate the shared library from the library sources
add_library(utils_c OBJECT ${sources_utils_c})
add_library(utils_cxx OBJECT ${sources_utils_cxx})
set_target_properties(utils_cxx PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(utils_c PUBLIC ${PROJECT_SOURCE_DIR}/include/utils)
target_include_directories(utils_cxx PUBLIC ${PROJECT_SOURCE_DIR}/include/utils)

if(CUDA_FOUND)
    target_link_libraries(utils_cxx PRIVATE ${CUDA_TOOLKIT_ROOT_DIR}/lib64/libcudart.so)
    target_link_options(utils_cxx PRIVATE -Wl,-rpath,${CUDA_TOOLKIT_ROOT_DIR}/lib64)
elseif(CUDAToolkit_FOUND)
    target_link_libraries(utils_cxx PRIVATE CUDA::cudart)
    target_link_options(utils_cxx PRIVATE -Wl,-rpath,${CUDAToolkit_LIBRARY_DIR})
endif()
