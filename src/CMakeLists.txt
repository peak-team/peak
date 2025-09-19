add_subdirectory(utils)

# Add all source files
FILE(GLOB sources_peak
    peak.c
    general_listener.c
    pthread_listener.c
    syscall_interceptor.c
    dlopen_interceptor.c
)
FILE(GLOB sources_peak_mpi
    mpi_interceptor.c
)
FILE(GLOB sources_peak_cuda
    cuda_interceptor.cpp
)

if (CUDA_FOUND OR CUDAToolkit_FOUND)
add_library(cuda_interceptor_obj OBJECT ${sources_peak_cuda})
set_target_properties(cuda_interceptor_obj PROPERTIES LINKER_LANGUAGE CXX)
endif()

# Generate the shared library from the library sources
if(MPI_FOUND AND (CUDA_FOUND OR CUDAToolkit_FOUND))
    add_library(peak 
        SHARED 
            ${sources_peak}
            ${sources_peak_mpi}
	        $<TARGET_OBJECTS:cuda_interceptor_obj>
            $<TARGET_OBJECTS:utils_c>
            $<TARGET_OBJECTS:utils_cxx>  
    )
elseif (MPI_FOUND)
    add_library(peak
	SHARED
	    ${sources_peak}
	    ${sources_peak_mpi}
	    $<TARGET_OBJECTS:utils_c>
        $<TARGET_OBJECTS:utils_cxx>
    )
elseif (CUDA_FOUND OR CUDAToolkit_FOUND)
    add_library(peak
	SHARED
	    ${sources_peak}
	    $<TARGET_OBJECTS:cuda_interceptor_obj>
	    $<TARGET_OBJECTS:utils_c>
        $<TARGET_OBJECTS:utils_cxx>
    )
else()
    add_library(peak 
        SHARED 
            ${sources_peak}
            $<TARGET_OBJECTS:utils_c>  
            $<TARGET_OBJECTS:utils_cxx>
    )
endif()

target_include_directories(peak
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
        ${LIBIBERTY_INCLUDE_DIR}
)

if (CUDA_FOUND OR CUDAToolkit_FOUND)
target_include_directories(cuda_interceptor_obj
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
        ${LIBIBERTY_INCLUDE_DIR}
)
endif()

target_link_libraries(peak
    PRIVATE
        ${FRIDA_GUM_LIBRARIES}
        Threads::Threads
        ${DL_LIBRARY}
        ${RT_LIBRARY}
        ${RESOLV_LIBRARY}
        ${M_LIBRARY}
        ${LIBIBERTY_LIB}
)

if (CUDA_FOUND OR CUDAToolkit_FOUND)
target_link_libraries(cuda_interceptor_obj
    PRIVATE
        ${FRIDA_GUM_LIBRARIES}
        Threads::Threads
        ${DL_LIBRARY}
        ${RT_LIBRARY}
        ${RESOLV_LIBRARY}
        ${M_LIBRARY}
        ${LIBIBERTY_LIB}
)
endif()

if(MPI_FOUND)
    target_link_libraries(peak 
        PRIVATE 
            MPI::MPI_C
    )
endif()

if(MPI_FOUND AND (CUDA_FOUND OR CUDAToolkit_FOUND))
    target_link_libraries(cuda_interceptor_obj 
        PRIVATE 
            MPI::MPI_C
    )
endif()

if(CUDA_FOUND)
    target_link_libraries(peak PRIVATE "${CUDA_TOOLKIT_ROOT_DIR}/lib64/libcudart.a")
    target_link_libraries(peak PRIVATE "${CUDA_TOOLKIT_ROOT_DIR}/lib64/libcudart.so")
    target_link_options(peak PRIVATE -Wl,-rpath,${CUDA_TOOLKIT_ROOT_DIR}/lib64)
elseif(CUDAToolkit_FOUND)
    target_link_libraries(peak PRIVATE CUDA::cudart)
    target_link_libraries(peak PRIVATE "${CUDAToolkit_LIBRARY_DIR}/libcufilt.a")
    target_link_options(peak PRIVATE -Wl,-rpath,${CUDAToolkit_LIBRARY_DIR})
endif()

install (TARGETS peak DESTINATION lib)
