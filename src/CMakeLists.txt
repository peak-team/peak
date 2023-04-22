add_subdirectory(utils)

# Add all source files
FILE(GLOB sources_peak
    peak.c
    general_listener.c
    pthread_listener.c
    mpi_interceptor.c
)

# Generate the shared library from the library sources
add_library(peak 
    SHARED 
        ${sources_peak}
        $<TARGET_OBJECTS:utils>  
)

target_include_directories(peak
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
)

target_link_libraries(peak
    PRIVATE
        ${FRIDA_GUM_LIBRARIES}
        Threads::Threads
        ${DL_LIBRARY}
        ${RT_LIBRARY}
        ${RESOLV_LIBRARY}
        ${M_LIBRARY}
)

if(MPI_FOUND)
    target_link_libraries(peak 
        PRIVATE 
            MPI::MPI_C
    )
endif()


install (TARGETS peak DESTINATION lib)
