add_executable(test_detach_hotloop test_detach_hotloop.c)
target_include_directories(test_detach_hotloop
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS}
)
if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  target_compile_definitions(test_detach_hotloop PRIVATE PEAK_HAVE_GUM_PEAK_PC_API=1)
endif()
target_link_libraries(test_detach_hotloop PRIVATE Threads::Threads)
if(DL_LIBRARY)
  target_link_libraries(test_detach_hotloop PRIVATE ${DL_LIBRARY})
endif()
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
if(DL_LIBRARY)
  target_link_libraries(test_detach_stale_threads PRIVATE ${DL_LIBRARY})
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_detach_stale_threads PRIVATE -Wl,--export-dynamic)
endif()

set(PEAK_MANY_TARGET_STRESS_TARGET_COUNT "1024" CACHE STRING
  "Number of generated exported targets for test_detach_many_targets_stress")
set(PEAK_MANY_TARGET_STRESS_GENERATED
  ${CMAKE_CURRENT_BINARY_DIR}/test_detach_many_targets_generated.c)
add_custom_command(
  OUTPUT ${PEAK_MANY_TARGET_STRESS_GENERATED}
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/generate_many_targets_stress.py
          --output ${PEAK_MANY_TARGET_STRESS_GENERATED}
          --targets ${PEAK_MANY_TARGET_STRESS_TARGET_COUNT}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generate_many_targets_stress.py
          ${CMAKE_CURRENT_SOURCE_DIR}/test_detach_many_targets_stress_generated.h
  VERBATIM
)

add_executable(test_detach_many_targets_stress
  test_detach_many_targets_stress.c
  ${PEAK_MANY_TARGET_STRESS_GENERATED})
target_compile_definitions(test_detach_many_targets_stress
  PRIVATE
    PEAK_STRESS_TARGET_COUNT=${PEAK_MANY_TARGET_STRESS_TARGET_COUNT})
target_include_directories(test_detach_many_targets_stress
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_detach_many_targets_stress PRIVATE Threads::Threads)
if(DL_LIBRARY)
  target_link_libraries(test_detach_many_targets_stress PRIVATE ${DL_LIBRARY})
endif()
if(RT_LIBRARY)
  target_link_libraries(test_detach_many_targets_stress PRIVATE ${RT_LIBRARY})
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_detach_many_targets_stress PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_signal_timer_abi test_signal_timer_abi.c)
if(RT_LIBRARY)
  target_link_libraries(test_signal_timer_abi PRIVATE ${RT_LIBRARY})
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
    ${RT_LIBRARY}
    ${M_LIBRARY}
)
if(DL_LIBRARY)
  target_link_libraries(test_unsafe_gum_prologue_policy PRIVATE ${DL_LIBRARY})
endif()

add_executable(test_syscall_interceptor_policy
  test_syscall_interceptor_policy.c
)
target_include_directories(test_syscall_interceptor_policy
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
