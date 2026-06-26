add_executable(test_detach_hotloop test_detach_hotloop.c)
target_include_directories(test_detach_hotloop
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS}
)
if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  target_compile_definitions(test_detach_hotloop PRIVATE PEAK_HAVE_GUM_PEAK_PC_API=1)
endif()
target_link_libraries(test_detach_hotloop PRIVATE Threads::Threads ${DL_LIBRARY})
if(RT_LIBRARY)
  target_link_libraries(test_detach_hotloop PRIVATE ${RT_LIBRARY})
endif()
if(MPI_C_FOUND)
  target_link_libraries(test_detach_hotloop PRIVATE MPI::MPI_C)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_detach_hotloop PRIVATE -Wl,--export-dynamic)
endif()

if(TARGET peak_detach_helper)
  add_executable(test_peak_detach_helper ${PROJECT_SOURCE_DIR}/src/peak_detach_helper.c)
  target_include_directories(test_peak_detach_helper
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )
  target_compile_definitions(test_peak_detach_helper
    PRIVATE
      PEAK_DETACH_HELPER_ENABLE_TEST_HOOKS=1
  )
endif()

add_executable(test_detach_stale_threads test_detach_stale_threads.c)
target_link_libraries(test_detach_stale_threads PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_detach_stale_threads PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_unsafe_gum_prologue test_unsafe_gum_prologue.c)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    enable_language(ASM)
    target_sources(test_unsafe_gum_prologue PRIVATE unsafe_gum_prologue_aarch64.S)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_unsafe_gum_prologue PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_unsafe_gum_prologue_policy
  test_unsafe_gum_prologue_policy.c
  ${PROJECT_SOURCE_DIR}/src/unsafe_gum_prologue.c
)
target_include_directories(test_unsafe_gum_prologue_policy
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${FRIDA_GUM_INCLUDE_DIRS}
)
target_link_libraries(test_unsafe_gum_prologue_policy
  PRIVATE
    ${FRIDA_GUM_LIBRARIES}
    Threads::Threads
    ${DL_LIBRARY}
    ${RT_LIBRARY}
    ${M_LIBRARY}
)
