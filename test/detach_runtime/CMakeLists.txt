add_executable(test_detach_hotloop test_detach_hotloop.c)
add_executable(test_hook_state_independent test_hook_state_independent.c)
target_include_directories(test_hook_state_independent PRIVATE ${PROJECT_SOURCE_DIR}/include ${FRIDA_GUM_INCLUDE_DIRS})
target_link_libraries(test_hook_state_independent PRIVATE Threads::Threads ${DL_LIBRARY})
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_options(test_hook_state_independent PRIVATE -Wl,--export-dynamic)
endif()
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

add_executable(test_detach_milc_like test_detach_milc_like.c)
target_link_libraries(test_detach_milc_like PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_detach_milc_like PRIVATE -Wl,--export-dynamic)
endif()

if(TARGET peak_detach_helper)
  add_executable(test_peak_detach_helper ${PROJECT_SOURCE_DIR}/src/detach_helper.c)
  target_include_directories(test_peak_detach_helper
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )
  target_compile_definitions(test_peak_detach_helper
    PRIVATE
      PEAK_DETACH_HELPER_ENABLE_TEST_HOOKS=1
  )

  add_executable(test_detach_helper_sigchld_ignored_wrapper
    test_detach_helper_sigchld_ignored_wrapper.c)

  add_executable(test_detach_helper_ptrace_stop_wait
    test_detach_helper_ptrace_stop_wait.c)
  target_include_directories(test_detach_helper_ptrace_stop_wait
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )

  add_executable(test_detach_helper_release
    test_detach_helper_release.c)
  target_include_directories(test_detach_helper_release
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )
endif()

add_executable(test_detach_stale_threads test_detach_stale_threads.c)
target_link_libraries(test_detach_stale_threads PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_detach_stale_threads PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_fastpath_thread_exit test_fastpath_thread_exit.c)
target_include_directories(test_fastpath_thread_exit
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS}
)
target_link_libraries(test_fastpath_thread_exit
    PRIVATE Threads::Threads ${DL_LIBRARY})
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_fastpath_thread_exit PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_max_threads_parser test_max_threads_parser.c)
target_link_libraries(test_max_threads_parser PRIVATE ${DL_LIBRARY})
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_max_threads_parser PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_pthread_slot_registry
  test_pthread_slot_registry.c
  ${PROJECT_SOURCE_DIR}/src/pthread_slot_registry.c)
target_include_directories(test_pthread_slot_registry PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_pthread_slot_registry PRIVATE Threads::Threads)
target_compile_definitions(test_pthread_slot_registry PRIVATE PEAK_ENABLE_TEST_HOOKS=1)
if(PEAK_ENABLE_TSAN_TESTS)
  target_compile_options(test_pthread_slot_registry PRIVATE
    -fsanitize=thread -fno-omit-frame-pointer -O1 -g)
  target_link_options(test_pthread_slot_registry PRIVATE -fsanitize=thread)
endif()
add_test(NAME test_pthread_slot_registry COMMAND test_pthread_slot_registry)
set_tests_properties(test_pthread_slot_registry PROPERTIES
  PASS_REGULAR_EXPRESSION "pthread_slot_registry_ok")

add_executable(test_fastpath_second_listener test_fastpath_second_listener.c)
target_include_directories(test_fastpath_second_listener
  PRIVATE
    ${FRIDA_GUM_INCLUDE_DIRS}
)
target_link_libraries(test_fastpath_second_listener
  PRIVATE
    Threads::Threads
    ${DL_LIBRARY}
)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_fastpath_second_listener PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_fastpath_lifetime_handoff
  test_fastpath_lifetime_handoff.c)
target_include_directories(test_fastpath_lifetime_handoff
  PRIVATE
    ${FRIDA_GUM_INCLUDE_DIRS}
)
target_link_libraries(test_fastpath_lifetime_handoff
  PRIVATE
    Threads::Threads
    ${DL_LIBRARY}
)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_fastpath_lifetime_handoff
      PRIVATE -Wl,--export-dynamic)
endif()

add_executable(test_fastpath_nonlocal_unwind
  test_fastpath_nonlocal_unwind.c)
target_include_directories(test_fastpath_nonlocal_unwind
  PRIVATE
    ${FRIDA_GUM_INCLUDE_DIRS}
)
target_link_libraries(test_fastpath_nonlocal_unwind
  PRIVATE
    Threads::Threads
    ${DL_LIBRARY}
)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_link_options(test_fastpath_nonlocal_unwind
      PRIVATE -Wl,--export-dynamic)
endif()

add_library(cxx_throw_provider SHARED cxx_throw_provider.cpp)
add_library(cxx_throw_recursive_provider SHARED cxx_throw_recursive_provider.cpp)
add_executable(test_cxx_throw_recursive test_cxx_throw_recursive.cpp)
target_link_libraries(test_cxx_throw_recursive PRIVATE
  cxx_throw_recursive_provider Threads::Threads)
add_executable(test_cxx_throw_transparency test_cxx_throw_transparency.cpp)
target_link_libraries(test_cxx_throw_transparency PRIVATE
  cxx_throw_provider Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_unwind_frame_reentry test_unwind_frame_reentry.cpp)
    # Resolve unwind imports against the shared runtime before the project's
    # global -static-libgcc libraries. -shared-libgcc alone cannot override
    # that global driver option on GCC. Other fixture conditions stay intact.
    target_link_libraries(test_unwind_frame_reentry PRIVATE
      Threads::Threads ${DL_LIBRARY} gcc_s)
    target_compile_options(test_unwind_frame_reentry PRIVATE
      -fexceptions -fno-lto -fno-omit-frame-pointer -fno-optimize-sibling-calls)
    target_link_options(test_unwind_frame_reentry PRIVATE -Wl,--export-dynamic)
  endif()
  add_executable(test_cxx_throw_late_provider test_cxx_throw_recursive.cpp)
  target_compile_definitions(test_cxx_throw_late_provider PRIVATE
    PEAK_CXX_THROW_LATE_PROVIDER=1)
  target_link_libraries(test_cxx_throw_late_provider PRIVATE
    Threads::Threads ${DL_LIBRARY})
  add_dependencies(test_cxx_throw_late_provider cxx_throw_recursive_provider)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_cxx_throw_late_provider PRIVATE -fexceptions -fno-lto)
  endif()
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cxx_throw_provider PRIVATE -fexceptions -fno-lto)
  target_compile_options(test_cxx_throw_transparency PRIVATE -fexceptions -fno-lto)
  target_compile_options(cxx_throw_recursive_provider PRIVATE -fexceptions -fno-lto)
  target_compile_options(test_cxx_throw_recursive PRIVATE -fexceptions -fno-lto)
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

if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND
   PEAK_GUM_PEAK_EXACT_ATTACH_API_AVAILABLE)
  enable_language(ASM)
  add_executable(test_gum_exact_attach_aarch64
    test_gum_exact_attach_aarch64.c
    gum_exact_attach_aarch64.S)
  target_include_directories(test_gum_exact_attach_aarch64
    PRIVATE ${FRIDA_GUM_INCLUDE_DIRS})
  target_link_libraries(test_gum_exact_attach_aarch64
    PRIVATE
      ${FRIDA_GUM_LIBRARIES}
      Threads::Threads
      ${DL_LIBRARY}
      ${RT_LIBRARY}
      ${M_LIBRARY})
  add_test(NAME test_gum_exact_attach_aarch64
    COMMAND $<TARGET_FILE:test_gum_exact_attach_aarch64>)
  set_tests_properties(test_gum_exact_attach_aarch64
    PROPERTIES
      TIMEOUT 15
      PASS_REGULAR_EXPRESSION "gum_exact_attach_aarch64_ok")
  add_executable(test_gum_arm64_prefix_branch
    test_gum_arm64_prefix_branch.c gum_arm64_prefix_branch.S)
  target_include_directories(test_gum_arm64_prefix_branch PRIVATE ${FRIDA_GUM_INCLUDE_DIRS})
  target_link_libraries(test_gum_arm64_prefix_branch PRIVATE
    ${FRIDA_GUM_LIBRARIES} Threads::Threads ${DL_LIBRARY}
    ${RT_LIBRARY} ${RESOLV_LIBRARY} ${M_LIBRARY})
  add_test(NAME test_gum_arm64_prefix_branch COMMAND test_gum_arm64_prefix_branch)
  set_tests_properties(test_gum_arm64_prefix_branch PROPERTIES
    TIMEOUT 15 SKIP_RETURN_CODE 77
    PASS_REGULAR_EXPRESSION "gum_arm64_prefix_branch_ok")

endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  add_executable(test_gum_x86_entry_range
    test_gum_x86_entry_range.c gum_x86_entry_range.S)
  target_include_directories(test_gum_x86_entry_range PRIVATE ${FRIDA_GUM_INCLUDE_DIRS})
  target_link_libraries(test_gum_x86_entry_range PRIVATE
    ${FRIDA_GUM_LIBRARIES} Threads::Threads ${DL_LIBRARY}
    ${RT_LIBRARY} ${RESOLV_LIBRARY} ${M_LIBRARY})
  add_test(NAME test_gum_x86_entry_range COMMAND test_gum_x86_entry_range)
  set_tests_properties(test_gum_x86_entry_range PROPERTIES
    TIMEOUT 15 SKIP_RETURN_CODE 77
    PASS_REGULAR_EXPRESSION "gum_x86_entry_range_ok")
endif()
