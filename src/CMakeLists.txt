add_subdirectory(utils)

# Add all source files
FILE(GLOB sources_peak
    peak.c
    general_listener.c
    pthread_listener.c
    syscall_interceptor.c
)
FILE(GLOB sources_peak_mpi
    mpi_interceptor.c
)
FILE(GLOB sources_peak_cuda
    cuda_interceptor.c
)

# Generate the shared library from the library sources
if(MPI_FOUND AND (CUDA_FOUND OR CUDAToolkit_FOUND))
    add_library(peak 
        SHARED 
            ${sources_peak}
            ${sources_peak_mpi}
	    ${sources_peak_cuda}
            $<TARGET_OBJECTS:utils>  
    )
elseif (MPI_FOUND)
    add_library(peak
	SHARED
	    ${sources_peak}
	    ${sources_peak_mpi}
	    $<TARGET_OBJECTS:utils>
    )
elseif (CUDA_FOUND OR CUDAToolkit_FOUND)
    add_library(peak
	SHARED
	    ${sources_peak}
	    ${sources_peak_cuda}
	    $<TARGET_OBJECTS:utils>
    )
else()
    add_library(peak 
        SHARED 
            ${sources_peak}
            $<TARGET_OBJECTS:utils>  
    )
endif()

target_include_directories(peak
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
        ${LIBIBERTY_INCLUDE_DIR}
)

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

if(MPI_FOUND)
    target_link_libraries(peak 
        PRIVATE 
            MPI::MPI_C
    )
endif()

if(CUDA_FOUND OR CUDAToolkit_FOUND)
    target_link_libraries(peak
        PRIVATE
            ${CUDA_LIBRARIES}
            ${CUDAToolkit_LIBRARY_DIR}
    )
endif()

install (TARGETS peak DESTINATION lib)
