add_subdirectory(utils)
add_compile_options(-fno-omit-frame-pointer) # add for tracing caller stack

# Add all source files
set(sources_peak
    dlopen_interceptor.c
    general_listener.c
    malloc_interceptor.c
    malloc_otf2.c
    peak.c
    detach_controller.c
    jit_provider.c
    logging.c
    signal_policy.c
    pthread_listener.c
    syscall_interceptor.c
    unsafe_gum_prologue.c
    exec_memory.c
    exec_interceptor.c
    exec_raw_syscall.c
)

# Strict dlopen teardown needs a compiler-proof entry stub.  The shared
# detach-helper predicate covers the Linux x86_64/AArch64 platforms for which
# the assembler language was enabled at the project root.
if(PEAK_DETACH_HELPER_SUPPORTED)
    list(APPEND sources_peak dlopen_entry_stub.S)
endif()

# The C dispatcher is portable. Its assembly syscall interposer is selected by
# the shared platform contract used at the project root and in platform tests.
peak_exec_append_raw_syscall_trampoline(
    sources_peak exec_raw_syscall_trampoline.S)

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

if(PEAK_DETACH_HELPER_SUPPORTED)
    add_executable(peak_detach_helper detach_helper.c)
    target_include_directories(peak_detach_helper
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )
    target_compile_definitions(peak
        PRIVATE
            PEAK_DLOPEN_ASM_ENTRY_STUB=1
            "PEAK_DEFAULT_DETACH_HELPER_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/peak_detach_helper\""
            "PEAK_INSTALL_DETACH_HELPER_PATH=\"${CMAKE_INSTALL_PREFIX}/bin/peak_detach_helper\""
    )
    add_dependencies(peak peak_detach_helper)
endif()

target_include_directories(peak
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
        ${LIBIBERTY_INCLUDE_DIR}
        ${OTF2_INCLUDE_DIRS}
)

if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
    target_compile_definitions(peak PRIVATE PEAK_HAVE_GUM_PEAK_PC_API=1)
endif()
if(BUILD_TESTS OR BUILD_TESTING)
    target_compile_definitions(peak PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
endif()

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
        ${OTF2_LIBRARIES}
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

install(TARGETS peak DESTINATION lib)
if(TARGET peak_detach_helper)
    install(TARGETS peak_detach_helper DESTINATION bin)
endif()
