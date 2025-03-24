enable_language(CUDA)
set(CMAKE_C_COMPILER mpicc)
set(CMAKE_CXX_COMPILER mpicxx)
set(CMAKE_CUDA_COMPILER nvcc)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=-fopenmp -fPIC")

if (CMAKE_VERSION VERSION_LESS 3.17)
    find_package(CUDA)
else()
    find_package(CUDAToolkit)
endif()

if (CUDAToolkit_FOUND)
    message(STATUS "CUDA Toolkit found: ${CUDAToolkit_VERSION}")
    add_definitions(-DHAVE_CUDA)
    include_directories(${CUDA_INCLUDE_DIRS} ${CUDAToolkit_INCLUDE_DIRS})
    set_source_files_properties(src/cuda_interceptor.c PROPERTIES LANGUAGE CUDA)
elseif (CUDA_FOUND)
    message(STATUS "Legacy CUDA found: ${CUDA_VERSION}")
    add_definitions(-DHAVE_CUDA)
    include_directories(${CUDA_INCLUDE_DIRS})
    set_source_files_properties(src/cuda_interceptor.c PROPERTIES LANGUAGE CUDA)
else()
    message(WARNING "CUDA not found. Skipping CUDA-related features.")
endif()

find_package(OpenMP REQUIRED)
find_package(MPI REQUIRED)

# Executable: test_mpi
add_executable(test_mpi test_mpi.cu)
set_source_files_properties(test_mpi.cu PROPERTIES LANGUAGE CUDA)
set_target_properties(test_mpi PROPERTIES CUDA_ARCHITECTURES all)
target_link_libraries(test_mpi PRIVATE MPI::MPI_CXX ${CUDA_LIBRARIES})

# Executable: test_pthread
add_executable(test_pthread test_pthread.cu)
set_source_files_properties(test_pthread.cu PROPERTIES LANGUAGE CUDA)
set_target_properties(test_pthread PROPERTIES CUDA_ARCHITECTURES all)
target_link_libraries(test_pthread PRIVATE pthread ${CUDA_LIBRARIES})

# Executable: test_ompmpi (CUDA + OpenMP + MPI)
add_executable(test_ompmpi test_ompmpi.cu)
set_source_files_properties(test_ompmpi.cu PROPERTIES LANGUAGE CUDA)
set_target_properties(test_ompmpi PROPERTIES CUDA_ARCHITECTURES all)
target_link_libraries(test_ompmpi
    PRIVATE
        MPI::MPI_CXX
        OpenMP::OpenMP_C
        ${CUDA_LIBRARIES}
        gomp  # Needed for GOMP_critical_start
)