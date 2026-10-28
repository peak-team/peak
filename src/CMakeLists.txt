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
    unsafe_gum_prologue.c
    exec_memory.c
    exec_interceptor.c
    exec_raw_syscall.c
    ${PEAK_DARWIN_GUM_PATCH_SOURCE}
    filesystem_stat_guard.c
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

if(PEAK_CUDA_ENABLED)
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
if(PEAK_CUDA_ENABLED)
    list(APPEND sources_peak $<TARGET_OBJECTS:cuda_interceptor_obj>)
endif()
list(APPEND sources_peak
    $<TARGET_OBJECTS:utils_c>
    $<TARGET_OBJECTS:utils_cxx>
)

add_library(peak SHARED ${sources_peak})
set_target_properties(peak PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

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
if(PEAK_ENABLE_RUNTIME_TEST_HOOKS)
    target_compile_definitions(peak PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
    target_compile_definitions(peak PRIVATE PEAK_TARGET_RESOLVER_TESTING=1)
    if(TARGET cuda_interceptor_obj)
        target_compile_definitions(cuda_interceptor_obj
            PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
    endif()
endif()

if(PEAK_CUDA_ENABLED)
target_include_directories(cuda_interceptor_obj
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${FRIDA_GUM_INCLUDE_DIRS}
        ${PEAK_CUDA_INCLUDE_DIRS}
)
target_compile_definitions(cuda_interceptor_obj PRIVATE
    $<$<BOOL:${PEAK_CUDA_HAS_RUNTIME_LAUNCH_EX}>:PEAK_CUDA_COMPILE_RUNTIME_LAUNCH_EX=1>
    $<$<BOOL:${PEAK_CUDA_HAS_DRIVER_LAUNCH_EX}>:PEAK_CUDA_COMPILE_DRIVER_LAUNCH_EX=1>)
endif()

if(PEAK_ENABLE_RUNTIME_TEST_HOOKS)
    # Internal integration tests share the test-enabled DSO's Gum runtime.
    # This artifact also exports PEAK test hooks and is never installed as a
    # production package by the package-contract jobs below.
    target_link_libraries(peak PRIVATE ${FRIDA_GUM_LIBRARIES})
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    foreach(_peak_gum_archive IN LISTS FRIDA_GUM_LIBRARIES)
        target_link_options(peak PRIVATE
            "LINKER:-load_hidden,${_peak_gum_archive}")
    endforeach()
else()
    target_link_libraries(peak PRIVATE ${FRIDA_GUM_LIBRARIES})
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        # Static archives are private implementation dependencies of this
        # preloaded DSO and must not become part of its dynamic ABI.
        target_link_options(peak PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
endif()

set(_peak_otf2_link_libraries ${OTF2_LIBRARIES})
if(NOT PEAK_ENABLE_RUNTIME_TEST_HOOKS AND
   CMAKE_SYSTEM_NAME MATCHES "Darwin")
    foreach(_peak_otf2_library IN LISTS OTF2_LIBRARIES)
        if(_peak_otf2_library MATCHES "\\.a$")
            target_link_options(peak PRIVATE
                "LINKER:-load_hidden,${_peak_otf2_library}")
            list(REMOVE_ITEM _peak_otf2_link_libraries
                "${_peak_otf2_library}")
        endif()
    endforeach()
endif()

target_link_libraries(peak
    PRIVATE
        Threads::Threads
        ${DL_LIBRARY}
        ${RT_LIBRARY}
        ${RESOLV_LIBRARY}
        ${M_LIBRARY}
        ${_peak_otf2_link_libraries}
)

if(PEAK_CUDA_ENABLED)
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

if(MPI_FOUND AND PEAK_CUDA_ENABLED)
    target_link_libraries(cuda_interceptor_obj
        PRIVATE
            MPI::MPI_C
    )
endif()

if(PEAK_CUDA_ENABLED)
    target_link_libraries(peak PRIVATE ${PEAK_CUDA_RUNTIME_LIBRARIES})
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

# The injected runtime compiles its own utility and optional module sources.
# It must never reuse preload objects with startup interposition or static TLS.
if(PEAK_BUILD_ATTACH)
    set(_peak_attach_sources)
    foreach(_source IN LISTS sources_peak)
        if(NOT _source MATCHES "^\\$<")
            list(APPEND _peak_attach_sources "${_source}")
        endif()
    endforeach()
    list(REMOVE_ITEM _peak_attach_sources signal_policy.c signal_policy_stub.c
        exec_raw_syscall_trampoline.S)
    list(APPEND _peak_attach_sources signal_policy_stub.c
        attach/agent.c
        attach/runtime_config.c
        utils/env_parser.c
        utils/mpi_utils.c
        utils/parent_registry.c
        utils/process_filter.c
        utils/source_target.c
        utils/target_selector_syntax.c
        utils/target_config.c
        utils/timing.c
        utils/cxx_utils.cpp
        logging.c)
    if(PEAK_CUDA_ENABLED)
        list(APPEND _peak_attach_sources cuda_interceptor.cpp cuda_profiler_state.cpp)
    endif()
    add_library(peak_attach_agent SHARED ${_peak_attach_sources})
    set_target_properties(peak_attach_agent PROPERTIES OUTPUT_NAME "peak-attach")
    if(TARGET peak_detach_helper)
        add_dependencies(peak_attach_agent peak_detach_helper)
    endif()
    get_target_property(_peak_includes peak INCLUDE_DIRECTORIES)
    get_target_property(_peak_definitions peak COMPILE_DEFINITIONS)
    get_target_property(_peak_libraries peak LINK_LIBRARIES)
    target_include_directories(peak_attach_agent PRIVATE ${_peak_includes}
        "${PROJECT_SOURCE_DIR}/include/utils")
    target_compile_definitions(peak_attach_agent PRIVATE PEAK_ATTACH_RUNTIME=1)
    if(_peak_definitions)
        list(REMOVE_ITEM _peak_definitions PEAK_ENABLE_TEST_HOOKS=1 PEAK_TARGET_RESOLVER_TESTING=1)
        target_compile_definitions(peak_attach_agent PRIVATE ${_peak_definitions})
    endif()
    target_compile_options(peak_attach_agent PRIVATE
        "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-include>"
        "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:${PROJECT_SOURCE_DIR}/include/internal/attach_runtime_config.h>")
    # Production preload builds put private Darwin archives in LINK_OPTIONS.
    # Add the agent's own archive dependencies before applying its visibility
    # policy instead of inheriting only the preload target's link libraries.
    set(_peak_attach_libraries ${_peak_libraries}
        ${FRIDA_GUM_LIBRARIES} ${OTF2_LIBRARIES})
    list(REMOVE_DUPLICATES _peak_attach_libraries)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        foreach(_library IN LISTS _peak_attach_libraries)
            if(_library MATCHES "\\.a$")
                target_link_options(peak_attach_agent PRIVATE "LINKER:-load_hidden,${_library}")
                list(REMOVE_ITEM _peak_attach_libraries "${_library}")
            endif()
        endforeach()
    endif()
    target_link_libraries(peak_attach_agent PRIVATE ${_peak_attach_libraries})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_peak_attach_exports "${CMAKE_CURRENT_SOURCE_DIR}/attach/exports.map")
        target_link_options(peak_attach_agent PRIVATE "LINKER:--exclude-libs,ALL"
            "LINKER:--version-script,${_peak_attach_exports}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_peak_attach_exports "${CMAKE_CURRENT_SOURCE_DIR}/attach/exports.macos")
        target_link_options(peak_attach_agent PRIVATE
            "LINKER:-exported_symbols_list,${_peak_attach_exports}")
    endif()
    set_property(TARGET peak_attach_agent APPEND PROPERTY LINK_DEPENDS "${_peak_attach_exports}")
    if(PEAK_CUDA_ENABLED)
        target_compile_definitions(peak_attach_agent PRIVATE
            $<$<BOOL:${PEAK_CUDA_HAS_RUNTIME_LAUNCH_EX}>:PEAK_CUDA_COMPILE_RUNTIME_LAUNCH_EX=1>
            $<$<BOOL:${PEAK_CUDA_HAS_DRIVER_LAUNCH_EX}>:PEAK_CUDA_COMPILE_DRIVER_LAUNCH_EX=1>)
    endif()
    add_executable(peak_attach attach/cli.c)
    set_target_properties(peak_attach PROPERTIES OUTPUT_NAME "peak-attach")
    target_include_directories(peak_attach PRIVATE ${FRIDA_CORE_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include)
    file(RELATIVE_PATH _peak_attach_agent_relative
        "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/${CMAKE_SHARED_LIBRARY_PREFIX}peak-attach${CMAKE_SHARED_LIBRARY_SUFFIX}")
    target_compile_definitions(peak_attach PRIVATE
        "PEAK_ATTACH_VERSION=\"${PROJECT_VERSION}\""
        "PEAK_ATTACH_AGENT_NAME=\"${CMAKE_SHARED_LIBRARY_PREFIX}peak-attach${CMAKE_SHARED_LIBRARY_SUFFIX}\""
        "PEAK_ATTACH_AGENT_RELATIVE_PATH=\"${_peak_attach_agent_relative}\"")
    target_link_libraries(peak_attach PRIVATE ${FRIDA_CORE_LIBRARIES}
        Threads::Threads ${DL_LIBRARY} ${RT_LIBRARY} ${RESOLV_LIBRARY} ${M_LIBRARY})
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        # Core manually maps this libSystem-only bridge; native dyld then loads
        # the full agent, including dependencies that only exist in the cache.
        add_library(peak_attach_bootstrap SHARED attach/darwin_bootstrap.c)
        set_target_properties(peak_attach_bootstrap PROPERTIES OUTPUT_NAME "peak-attach-bootstrap")
        target_include_directories(peak_attach_bootstrap PRIVATE ${PROJECT_SOURCE_DIR}/include)
        target_link_options(peak_attach_bootstrap PRIVATE
            "LINKER:-exported_symbol,_peak_attach_bootstrap_main")
        add_dependencies(peak_attach peak_attach_bootstrap)
        file(RELATIVE_PATH _peak_attach_bootstrap_relative
            "${CMAKE_INSTALL_FULL_BINDIR}"
            "${CMAKE_INSTALL_FULL_LIBDIR}/${CMAKE_SHARED_LIBRARY_PREFIX}peak-attach-bootstrap${CMAKE_SHARED_LIBRARY_SUFFIX}")
        target_compile_definitions(peak_attach PRIVATE
            "PEAK_ATTACH_BOOTSTRAP_NAME=\"${CMAKE_SHARED_LIBRARY_PREFIX}peak-attach-bootstrap${CMAKE_SHARED_LIBRARY_SUFFIX}\""
            "PEAK_ATTACH_BOOTSTRAP_RELATIVE_PATH=\"${_peak_attach_bootstrap_relative}\"")
        install(TARGETS peak_attach_bootstrap LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
        target_link_libraries(peak_attach PRIVATE bsm
            "-framework Foundation" "-framework CoreFoundation" "-framework AppKit")
    endif()
    add_dependencies(peak_attach peak_attach_agent)
    install(TARGETS peak_attach RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(TARGETS peak_attach_agent LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
