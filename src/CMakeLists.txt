add_subdirectory(utils)

# Add all source files
FILE(GLOB sources_peak
    peak.c
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
)

install (TARGETS peak DESTINATION lib)
