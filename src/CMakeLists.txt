add_subdirectory(utils)
add_compile_options(-fno-omit-frame-pointer) # add for tracing caller stack

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(PEAK_SIGNAL_POLICY_SOURCE signal_policy.c)
else()
    set(PEAK_SIGNAL_POLICY_SOURCE signal_policy_stub.c)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Darwin" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND
   PEAK_GUM_DARWIN_PATCH_API_AVAILABLE)
    set(PEAK_DETACH_CONTROLLER_SOURCE detach_controller_darwin.c)
    set(PEAK_DARWIN_GUM_PATCH_SOURCE gum_peak_darwin_patch_api.c)
else()
    set(PEAK_DETACH_CONTROLLER_SOURCE detach_controller.c)
    set(PEAK_DARWIN_GUM_PATCH_SOURCE)
endif()

# Add all source files
set(sources_peak
    dlopen_interceptor.c
    general_listener.c
    general_listener/attach_policy.c
    general_listener/exec_checkpoint_writer.c
    general_listener/output_identity.c
    general_listener/report_formatter.c
    general_listener/report_maxima.c
    general_listener/report_model.c
    general_listener/report_snapshot.c
    general_listener/runtime_config.c
    general_listener/socket_report_transport.c
    general_listener/target_resolver.c
    malloc_interceptor.c
    peak.c
    ${PEAK_DETACH_CONTROLLER_SOURCE}
    jit_provider.c
    ${PEAK_SIGNAL_POLICY_SOURCE}
    pthread_listener.c
    pthread_slot_registry.c
    syscall_interceptor.c
    unsafe_gum_prologue.c
    exec_memory.c
    exec_interceptor.c
    exec_raw_syscall.c
    ${PEAK_DARWIN_GUM_PATCH_SOURCE}
)

if(PEAK_ENABLE_OTF2)
    list(APPEND sources_peak malloc_otf2.c)
else()
    list(APPEND sources_peak malloc_otf2_stub.c)
endif()

# The C dispatcher is portable. Its assembly syscall interposer is selected by
# the shared platform contract used at the project root and in platform tests.
peak_exec_append_raw_syscall_trampoline(
    sources_peak exec_raw_syscall_trampoline.S)
if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    list(APPEND sources_peak
        ${PROJECT_SOURCE_DIR}/cmake/peak-gum/peak_aarch64_raw_syscall.S)
endif()

if (CUDA_FOUND OR CUDAToolkit_FOUND)
    add_library(cuda_interceptor_obj OBJECT
        cuda_interceptor.cpp
        cuda_profiler_state.cpp)
endif()

# Preserve the original source order while expressing optional modules once.
if(MPI_FOUND)
    list(APPEND sources_peak
        general_listener/mpi_report_transport.c
        mpi_teardown_guard.c
        mpi_interceptor.c)
endif()
if(CUDA_FOUND OR CUDAToolkit_FOUND)
    list(APPEND sources_peak $<TARGET_OBJECTS:cuda_interceptor_obj>)
endif()
list(APPEND sources_peak
    $<TARGET_OBJECTS:utils_c>
    $<TARGET_OBJECTS:utils_cxx>
)

add_library(peak SHARED ${sources_peak})

add_executable(peak_inspect_symbols
    peak_inspect_symbols.c
    general_listener/target_resolver.c
    utils/target_selector_syntax.c
    $<TARGET_OBJECTS:utils_cxx>)
set_target_properties(peak_inspect_symbols PROPERTIES
    OUTPUT_NAME peak
    LINKER_LANGUAGE CXX)
target_include_directories(peak_inspect_symbols PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS})
target_link_libraries(peak_inspect_symbols PRIVATE
    ${FRIDA_GUM_LIBRARIES}
    Threads::Threads
    ${DL_LIBRARY}
    ${RT_LIBRARY}
    ${RESOLV_LIBRARY}
    ${M_LIBRARY})

if(PEAK_DETACH_HELPER_SUPPORTED)
    add_executable(peak_detach_helper detach_helper.c)
    target_include_directories(peak_detach_helper
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )
    target_compile_definitions(peak
        PRIVATE
            "PEAK_DEFAULT_DETACH_HELPER_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/peak_detach_helper\""
            "PEAK_INSTALL_DETACH_HELPER_PATH=\"${CMAKE_INSTALL_FULL_BINDIR}/peak_detach_helper\""
    )
    add_dependencies(peak peak_detach_helper)
endif()

target_include_directories(peak
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${FRIDA_GUM_INCLUDE_DIRS}>
    PRIVATE
        ${FRIDA_GUM_INCLUDE_DIRS}
        ${OTF2_INCLUDE_DIRS}
)

if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
    target_compile_definitions(peak PRIVATE PEAK_HAVE_GUM_PEAK_PC_API=1)
endif()
if(PEAK_GUM_DARWIN_PATCH_API_AVAILABLE AND
   CMAKE_SYSTEM_NAME MATCHES "Darwin" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    target_compile_definitions(peak PRIVATE PEAK_HAVE_GUM_DARWIN_PATCH_API=1)
endif()
if(BUILD_TESTS OR BUILD_TESTING)
    target_compile_definitions(peak PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
    target_compile_definitions(peak PRIVATE PEAK_TARGET_RESOLVER_TESTING=1)
endif()

if (CUDA_FOUND OR CUDAToolkit_FOUND)
target_include_directories(cuda_interceptor_obj
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
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

install(TARGETS peak
    EXPORT PEAKTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS peak_inspect_symbols DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET peak_detach_helper)
    install(TARGETS peak_detach_helper DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
