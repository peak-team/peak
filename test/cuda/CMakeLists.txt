if (NOT BUILD_CUDA_PROFILE)
    message(STATUS "Skipping CUDA tests because BUILD_CUDA_PROFILE=OFF")
    return()
endif()

if (NOT CMAKE_CUDA_COMPILER)
    message(STATUS "Skipping CUDA tests because CUDA language is not enabled")
    return()
endif()

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=-fopenmp -fPIC")

if (CMAKE_VERSION VERSION_LESS 3.17)
    find_package(CUDA QUIET)
else()
    find_package(CUDAToolkit QUIET)
endif()

set(PEAK_CUDA_TEST_LIBRARIES)
if (CUDAToolkit_FOUND)
    message(STATUS "CUDA Toolkit found: ${CUDAToolkit_VERSION}")
    add_definitions(-DHAVE_CUDA)
    include_directories(${CUDA_INCLUDE_DIRS} ${CUDAToolkit_INCLUDE_DIRS})
    if (TARGET CUDA::cudart)
        list(APPEND PEAK_CUDA_TEST_LIBRARIES CUDA::cudart)
    endif()
elseif (CUDA_FOUND)
    message(STATUS "Legacy CUDA found: ${CUDA_VERSION}")
    add_definitions(-DHAVE_CUDA)
    include_directories(${CUDA_INCLUDE_DIRS})
    list(APPEND PEAK_CUDA_TEST_LIBRARIES ${CUDA_LIBRARIES})
else()
    message(STATUS "CUDA not found. Skipping CUDA tests.")
    return()
endif()

find_package(OpenMP QUIET)
find_package(MPI QUIET COMPONENTS CXX)

# Executable: test_pthread
add_executable(test_pthread test_pthread.cu)
set_source_files_properties(test_pthread.cu PROPERTIES LANGUAGE CUDA)
set_target_properties(test_pthread PROPERTIES CUDA_ARCHITECTURES all)
target_link_libraries(test_pthread PRIVATE pthread ${PEAK_CUDA_TEST_LIBRARIES})

# Executable: test_mpi
if (MPI_CXX_FOUND)
    add_executable(test_mpi test_mpi.cu)
    set_source_files_properties(test_mpi.cu PROPERTIES LANGUAGE CUDA)
    set_target_properties(test_mpi PROPERTIES CUDA_ARCHITECTURES all)
    target_link_libraries(test_mpi PRIVATE MPI::MPI_CXX ${PEAK_CUDA_TEST_LIBRARIES})
else()
    message(STATUS "Skipping CUDA MPI test because MPI CXX was not found")
endif()

# Executable: test_ompmpi (CUDA + OpenMP + MPI)
if (MPI_CXX_FOUND AND OpenMP_C_FOUND)
    add_executable(test_ompmpi test_ompmpi.cu)
    set_source_files_properties(test_ompmpi.cu PROPERTIES LANGUAGE CUDA)
    set_target_properties(test_ompmpi PROPERTIES CUDA_ARCHITECTURES all)
    target_link_libraries(test_ompmpi
        PRIVATE
            MPI::MPI_CXX
            OpenMP::OpenMP_C
            ${PEAK_CUDA_TEST_LIBRARIES}
            gomp  # Needed for GOMP_critical_start
    )
else()
    message(STATUS "Skipping CUDA OpenMP/MPI test because OpenMP C or MPI CXX was not found")
endif()
