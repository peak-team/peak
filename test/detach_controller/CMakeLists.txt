add_executable(test_detach_controller
  test_detach_controller.c
  ${PROJECT_SOURCE_DIR}/src/detach_controller.c
  ${PROJECT_SOURCE_DIR}/src/logging.c
  ${PROJECT_SOURCE_DIR}/src/signal_policy.c
  ${PROJECT_SOURCE_DIR}/src/unsafe_gum_prologue.c
  ${PROJECT_SOURCE_DIR}/src/exec_raw_syscall.c
  $<TARGET_OBJECTS:utils_c>
)
add_executable(test_detach_fake_helper test_detach_fake_helper.c)
add_executable(test_detach_shutdown_preload test_detach_shutdown_preload.c)
if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  add_executable(test_detach_listener_shutdown
    test_detach_listener_shutdown.c
    ${PROJECT_SOURCE_DIR}/src/general_listener.c
    ${PROJECT_SOURCE_DIR}/src/general_listener/report_maxima.c
    ${PROJECT_SOURCE_DIR}/src/jit_provider.c
    ${PROJECT_SOURCE_DIR}/src/logging.c
    ${PROJECT_SOURCE_DIR}/src/unsafe_gum_prologue.c
    ${PROJECT_SOURCE_DIR}/src/pthread_listener.c
    ${PROJECT_SOURCE_DIR}/src/detach_controller.c
    ${PROJECT_SOURCE_DIR}/src/signal_policy.c
    ${PROJECT_SOURCE_DIR}/src/exec_raw_syscall.c
    $<TARGET_OBJECTS:utils_c>
    $<TARGET_OBJECTS:utils_cxx>
  )
endif()

if(PEAK_EXEC_RAW_SYSCALL_SUPPORTED)
  target_sources(test_detach_controller PRIVATE
    ${PROJECT_SOURCE_DIR}/src/exec_raw_syscall_trampoline.S)
  if(TARGET test_detach_listener_shutdown)
    target_sources(test_detach_listener_shutdown PRIVATE
      ${PROJECT_SOURCE_DIR}/src/exec_raw_syscall_trampoline.S)
  endif()
endif()

target_include_directories(test_detach_fake_helper
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(test_detach_controller
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS}
)

if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  target_compile_definitions(test_detach_controller PRIVATE
    PEAK_HAVE_GUM_PEAK_PC_API=1
    PEAK_ENABLE_TEST_HOOKS=1)
endif()
target_compile_definitions(test_detach_controller PRIVATE
  PEAK_EXEC_RAW_SYSCALL_STANDALONE=1)
if(TARGET test_detach_listener_shutdown)
  target_include_directories(test_detach_listener_shutdown
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
      ${FRIDA_GUM_INCLUDE_DIRS}
  )
  target_compile_definitions(test_detach_listener_shutdown
    PRIVATE
      PEAK_HAVE_GUM_PEAK_PC_API=1
      PEAK_ENABLE_TEST_HOOKS=1
      PEAK_EXEC_RAW_SYSCALL_STANDALONE=1)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_options(test_detach_shutdown_preload PRIVATE -Wl,--export-dynamic)
endif()

target_link_libraries(test_detach_controller
  PRIVATE
    ${FRIDA_GUM_LIBRARIES}
    Threads::Threads
    ${DL_LIBRARY}
    ${RT_LIBRARY}
    ${RESOLV_LIBRARY}
    ${M_LIBRARY}
)
if(TARGET test_detach_listener_shutdown)
  target_link_libraries(test_detach_listener_shutdown
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
    target_link_libraries(test_detach_listener_shutdown
      PRIVATE
        MPI::MPI_C
    )
  endif()
  target_link_options(test_detach_listener_shutdown PRIVATE -Wl,--export-dynamic)
endif()

if(TARGET peak_detach_helper)
  add_custom_command(TARGET test_detach_controller POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:peak_detach_helper>
            $<TARGET_FILE_DIR:test_detach_controller>/peak_detach_helper
  )
endif()

add_test(NAME test_detach_controller_strict_mode
  COMMAND test_detach_controller strict
)
set_tests_properties(test_detach_controller_strict_mode
  PROPERTIES
    ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
)

add_test(NAME test_detach_controller_helper_mode
  COMMAND test_detach_controller strict
)
set_tests_properties(test_detach_controller_helper_mode
  PROPERTIES
    ENVIRONMENT "PEAK_SAFE_DETACH_MODE=helper;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
)

add_test(NAME test_detach_controller_signal_reserve_early_never
  COMMAND test_detach_controller signal-reserve-early-never
)
set_tests_properties(test_detach_controller_signal_reserve_early_never
  PROPERTIES
    ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_SIGNAL_RESERVE_EARLY=never;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
)

add_test(NAME test_detach_controller_signal_reserve_helper_auto
  COMMAND test_detach_controller signal-reserve-helper-auto
)
set_tests_properties(test_detach_controller_signal_reserve_helper_auto
  PROPERTIES
    ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_SIGNAL=auto;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
)

add_test(NAME test_detach_controller_require_env
  COMMAND test_detach_controller strict
)
set_tests_properties(test_detach_controller_require_env
  PROPERTIES
    ENVIRONMENT "PEAK_REQUIRE_SAFE_DETACH=1;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
)


if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND TARGET peak_detach_helper)
  add_test(NAME test_detach_controller_strict_helper_empty
    COMMAND test_detach_controller strict-helper-empty
  )
  set_tests_properties(test_detach_controller_strict_helper_empty
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:peak_detach_helper>"
      SKIP_RETURN_CODE 77
  )

  add_test(NAME test_detach_controller_strict_helper_default_lookup
    COMMAND test_detach_controller strict-helper-empty
  )
  set_tests_properties(test_detach_controller_strict_helper_default_lookup
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict"
      SKIP_RETURN_CODE 77
  )

  add_test(NAME test_detach_controller_strict_helper_stale_caller
    COMMAND test_detach_controller strict-helper-stale-caller
  )
  set_tests_properties(test_detach_controller_strict_helper_stale_caller
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:peak_detach_helper>"
      SKIP_RETURN_CODE 77
  )

  add_test(NAME test_detach_controller_signal_backend_empty
    COMMAND test_detach_controller strict-helper-empty
  )
  set_tests_properties(test_detach_controller_signal_backend_empty
    PROPERTIES
      ENVIRONMENT "PEAK_TARGET=strict_helper_target;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
  )

  add_test(NAME test_detach_controller_signal_backend_stale_caller
    COMMAND test_detach_controller strict-helper-stale-caller
  )
  set_tests_properties(test_detach_controller_signal_backend_stale_caller
    PROPERTIES
      ENVIRONMENT "PEAK_TARGET=strict_helper_target;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
  )

  add_test(NAME test_detach_controller_signal_backend_blocked_thread
    COMMAND test_detach_controller signal-backend-blocked-thread
  )
  set_tests_properties(test_detach_controller_signal_backend_blocked_thread
    PROPERTIES
      ENVIRONMENT "PEAK_TARGET=strict_helper_target;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
      TIMEOUT 15
  )

  add_test(NAME test_detach_controller_signal_backend_missing_thread_gate
    COMMAND test_detach_controller signal-backend-missing-thread-gate
  )
  set_tests_properties(test_detach_controller_signal_backend_missing_thread_gate
    PROPERTIES
      ENVIRONMENT "PEAK_TARGET=strict_helper_target;PEAK_SAFE_DETACH_MODE=signal;PEAK_DETACH_BACKEND=signal;PEAK_DETACH_HELPER=/no/such/peak_detach_helper"
  )

  add_test(NAME test_detach_controller_helper_backend_missing_thread_gate
    COMMAND test_detach_controller helper-backend-missing-thread-gate
  )
  set_tests_properties(test_detach_controller_helper_backend_missing_thread_gate
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:peak_detach_helper>"
      SKIP_RETURN_CODE 77
  )
endif()

if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  add_test(NAME test_detach_controller_accounting_snapshot_contention
    COMMAND test_detach_controller accounting-snapshot-contention)
  set_tests_properties(test_detach_controller_accounting_snapshot_contention
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict")

  add_test(NAME test_detach_controller_accounting_snapshot_concurrent
    COMMAND test_detach_controller accounting-snapshot-concurrent)
  set_tests_properties(test_detach_controller_accounting_snapshot_concurrent
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict"
      TIMEOUT 10)

  function(add_fatal_safe_detach_test test_name test_arg fake_scenario)
    add_test(NAME ${test_name}
      COMMAND ${CMAKE_COMMAND}
              -DTEST_EXECUTABLE=$<TARGET_FILE:test_detach_controller>
              -DTEST_ARG=${test_arg}
              -DHELPER_EXECUTABLE=$<TARGET_FILE:test_detach_fake_helper>
              -DFAKE_SCENARIO=${fake_scenario}
              ${ARGN}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_fatal_failure.cmake)
  endfunction()

  add_test(NAME test_detach_controller_batch_guards
    COMMAND test_detach_controller batch-guards)
  set_tests_properties(test_detach_controller_batch_guards
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>"
      SKIP_RETURN_CODE 77)

  add_test(NAME test_detach_controller_fake_helper_trace_disabled_stop_window
    COMMAND test_detach_controller fake-helper-trace-disabled-stop-window)
  set_tests_properties(test_detach_controller_fake_helper_trace_disabled_stop_window
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero"
      SKIP_RETURN_CODE 77)

  add_test(NAME test_detach_controller_fake_helper_gum_pc_on_enter
    COMMAND test_detach_controller fake-helper-gum-pc-corridor)
  set_tests_properties(test_detach_controller_fake_helper_gum_pc_on_enter
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_CONTROLLER_GUM_PC_CASE=on-enter"
      SKIP_RETURN_CODE 77)

  foreach(_gum_pc_case IN ITEMS on-enter-plus-one on-leave on-invoke enter-thunk)
    add_test(NAME test_detach_controller_fake_helper_gum_pc_${_gum_pc_case}
      COMMAND test_detach_controller fake-helper-gum-pc-corridor)
    set_tests_properties(test_detach_controller_fake_helper_gum_pc_${_gum_pc_case}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_CONTROLLER_GUM_PC_CASE=${_gum_pc_case}"
        SKIP_RETURN_CODE 77)
  endforeach()

  foreach(_detach_patch_pc_case IN ITEMS patch-entry patch-entry-plus-one)
    add_test(NAME test_detach_controller_fake_helper_detach_${_detach_patch_pc_case}
      COMMAND test_detach_controller fake-helper-gum-pc-corridor)
    set_tests_properties(test_detach_controller_fake_helper_detach_${_detach_patch_pc_case}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_CONTROLLER_GUM_PC_CASE=${_detach_patch_pc_case}"
        SKIP_RETURN_CODE 77)
  endforeach()

  foreach(_shutdown_pc_case IN ITEMS on-invoke patch-entry-plus-one)
    add_test(NAME test_detach_controller_fake_helper_shutdown_${_shutdown_pc_case}
      COMMAND test_detach_controller fake-helper-gum-pc-corridor)
    set_tests_properties(test_detach_controller_fake_helper_shutdown_${_shutdown_pc_case}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_CONTROLLER_GUM_PC_CASE=${_shutdown_pc_case};FAKE_DETACH_CONTROLLER_GUM_PC_OPERATION=shutdown"
        SKIP_RETURN_CODE 77)
  endforeach()


  foreach(_reattach_patch_pc_case IN ITEMS entry entry-plus-one)
    add_test(NAME test_detach_controller_fake_helper_reattach_patch_${_reattach_patch_pc_case}
      COMMAND test_detach_controller fake-helper-reattach-patch-entry)
    set_tests_properties(test_detach_controller_fake_helper_reattach_patch_${_reattach_patch_pc_case}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_CONTROLLER_REATTACH_PATCH_PC_CASE=${_reattach_patch_pc_case}"
        SKIP_RETURN_CODE 77)
  endforeach()

  foreach(_auto_fallback_scenario IN ITEMS stop-permission stop-timeout stop-unsupported)
    add_test(NAME test_detach_controller_fake_helper_auto_fallback_${_auto_fallback_scenario}
      COMMAND test_detach_controller fake-helper-auto-fallback)
    set_tests_properties(test_detach_controller_fake_helper_auto_fallback_${_auto_fallback_scenario}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=${_auto_fallback_scenario};FAKE_DETACH_HELPER_LOG=${PROJECT_BINARY_DIR}/test/detach_controller/auto-fallback-${_auto_fallback_scenario}.log"
        SKIP_RETURN_CODE 77)
  endforeach()

  add_fatal_safe_detach_test(
    test_detach_controller_fake_helper_auto_stop_missing_response_fatal
    fake-helper-fail-closed
    stop-missing-response
    -DDETACH_BACKEND=auto
    -DEXPECTED_FATAL_DETAIL=stop.response.failure)

  add_test(NAME test_detach_controller_auto_missing_helper_falls_back_signal
    COMMAND test_detach_controller fake-helper-auto-fallback)
  set_tests_properties(test_detach_controller_auto_missing_helper_falls_back_signal
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;FAKE_DETACH_HELPER_SCENARIO=stop-permission"
      SKIP_RETURN_CODE 77)

  add_test(NAME test_detach_controller_auto_ptrace_scope_uses_signal
    COMMAND test_detach_controller fake-helper-auto-fallback)
  set_tests_properties(test_detach_controller_auto_ptrace_scope_uses_signal
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_VERBOSITY=info;PEAK_TEST_PTRACE_SCOPE=2;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;FAKE_DETACH_HELPER_SCENARIO=stop-permission"
      PASS_REGULAR_EXPRESSION "auto safe detach using signal backend because ptrace_scope=2"
      SKIP_RETURN_CODE 77)

  foreach(_fake_scenario IN ITEMS stop-permission stop-timeout stop-timeout-delayed bad-snapshot duplicate-snapshot blocked-pc)
    add_test(NAME test_detach_controller_fake_helper_${_fake_scenario}
      COMMAND test_detach_controller fake-helper-fail-closed)
    set_tests_properties(test_detach_controller_fake_helper_${_fake_scenario}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=${_fake_scenario}")
  endforeach()

  add_fatal_safe_detach_test(
    test_detach_controller_fake_helper_stop-release-failed
    fake-helper-fail-closed
    stop-release-failed
    -DEXPECTED_FATAL_DETAIL=stop.cleanup.failure)
  set_tests_properties(test_detach_controller_fake_helper_stop-release-failed
    PROPERTIES
      LABELS "strict-detach-fatal")

  add_test(NAME test_detach_controller_fake_helper_evacuate-error
    COMMAND test_detach_controller fake-helper-fail-closed)
  set_tests_properties(test_detach_controller_fake_helper_evacuate-error
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=evacuate-error")

  add_fatal_safe_detach_test(
    test_detach_controller_fake_helper_evacuate-release-failed
    fake-helper-fail-closed
    evacuate-release-failed
    -DEXPECTED_FATAL_DETAIL=stop.cleanup.failure)

  add_fatal_safe_detach_test(
    test_detach_controller_fake_helper_resume-release-failed
    strict-helper-empty
    resume-release-failed
    -DEXPECTED_FATAL_DETAIL=stop.cleanup.failure)

  add_test(NAME test_detach_controller_fake_helper_shutdown_sequence
    COMMAND test_detach_controller fake-helper-shutdown-sequence)
  set_tests_properties(test_detach_controller_fake_helper_shutdown_sequence
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>")

  add_test(NAME test_detach_controller_fake_helper_batch_attach
    COMMAND test_detach_controller fake-helper-batch-attach)
  set_tests_properties(test_detach_controller_fake_helper_batch_attach
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_test(NAME test_detach_controller_fake_helper_arm64_attach_plan
      COMMAND test_detach_controller fake-helper-arm64-attach-plan)
    set_tests_properties(test_detach_controller_fake_helper_arm64_attach_plan
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>")
  endif()

  add_test(NAME test_detach_controller_fake_helper_batch_detach
    COMMAND test_detach_controller fake-helper-batch-detach)
  set_tests_properties(test_detach_controller_fake_helper_batch_detach
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  foreach(_auto_fallback_scenario IN ITEMS stop-permission stop-timeout stop-unsupported)
    add_test(NAME test_detach_controller_fake_helper_batch_auto_fallback_${_auto_fallback_scenario}
      COMMAND test_detach_controller fake-helper-batch-detach)
    set_tests_properties(test_detach_controller_fake_helper_batch_auto_fallback_${_auto_fallback_scenario}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=${_auto_fallback_scenario}"
        SKIP_RETURN_CODE 77)
  endforeach()

  add_test(NAME test_detach_controller_fake_helper_batch_auto_missing_helper_falls_back_signal
    COMMAND test_detach_controller fake-helper-batch-detach)
  set_tests_properties(test_detach_controller_fake_helper_batch_auto_missing_helper_falls_back_signal
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=auto;PEAK_DETACH_HELPER=/no/such/peak_detach_helper;FAKE_DETACH_HELPER_SCENARIO=stop-permission"
      SKIP_RETURN_CODE 77)

  add_fatal_safe_detach_test(
    test_detach_controller_fake_helper_batch_detach_resume-release-failed
    fake-helper-batch-detach
    resume-release-failed
    -DEXPECTED_FATAL_DETAIL=stop.cleanup.failure)

  add_test(NAME test_detach_controller_fake_helper_batch_abort_rollback
    COMMAND test_detach_controller fake-helper-batch-abort-rollback)
  set_tests_properties(test_detach_controller_fake_helper_batch_abort_rollback
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=evacuate-error")

  add_test(NAME test_detach_controller_fake_helper_batch_mixed
    COMMAND test_detach_controller fake-helper-batch-mixed)
  set_tests_properties(test_detach_controller_fake_helper_batch_mixed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=blocked-pc")

  add_test(NAME test_detach_controller_fake_helper_batch_missing_gum_snapshot
    COMMAND test_detach_controller fake-helper-batch-missing-gum-snapshot)
  set_tests_properties(test_detach_controller_fake_helper_batch_missing_gum_snapshot
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_listener_canonical_address
    COMMAND test_detach_controller fake-helper-listener-canonical-address)
  set_tests_properties(test_detach_controller_fake_helper_listener_canonical_address
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_listener_ambiguous_address
    COMMAND test_detach_controller fake-helper-listener-ambiguous-address)
  set_tests_properties(test_detach_controller_fake_helper_listener_ambiguous_address
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_batch_canonical_duplicate
    COMMAND test_detach_controller fake-helper-batch-canonical-duplicate)
  set_tests_properties(test_detach_controller_fake_helper_batch_canonical_duplicate
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_batch_reattach
    COMMAND test_detach_controller fake-helper-batch-reattach)
  set_tests_properties(test_detach_controller_fake_helper_batch_reattach
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_fatal_safe_detach_test(
    test_detach_controller_fake_helper_batch_reattach_resume-release-failed
    fake-helper-batch-reattach
    resume-release-failed
    -DFAIL_RESUME_INDEX=2
    -DEXPECTED_FATAL_DETAIL=stop.cleanup.failure)

  add_test(NAME test_detach_controller_general_listener_shutdown_prepare_fail_closed
    COMMAND test_detach_listener_shutdown general-listener-shutdown-prepare-fail-closed)
  set_tests_properties(test_detach_controller_general_listener_shutdown_prepare_fail_closed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero"
      PASS_REGULAR_EXPRESSION "general-listener-shutdown-prepare-fail-closed-ok")

  add_test(NAME test_detach_controller_general_listener_idle_shutdown_io_fail_closed
    COMMAND test_detach_listener_shutdown general-listener-idle-shutdown-io-fail-closed)
  set_tests_properties(test_detach_controller_general_listener_idle_shutdown_io_fail_closed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=shutdown-missing-response"
      PASS_REGULAR_EXPRESSION "general-listener-idle-shutdown-io-fail-closed-ok")

  add_test(NAME test_detach_controller_general_listener_final_freeze_after_controller_stop
    COMMAND test_detach_listener_shutdown general-listener-final-freeze-after-controller-stop)
  set_tests_properties(test_detach_controller_general_listener_final_freeze_after_controller_stop
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>"
      PASS_REGULAR_EXPRESSION "general-listener-final-freeze-after-controller-stop-ok"
      SKIP_RETURN_CODE 77)

  add_test(NAME test_detach_controller_general_listener_invalid_accounting_baseline_report
    COMMAND test_detach_listener_shutdown general-listener-invalid-accounting-baseline-report)
  set_tests_properties(test_detach_controller_general_listener_invalid_accounting_baseline_report
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_BACKEND=helper;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>"
      PASS_REGULAR_EXPRESSION "general-listener-invalid-accounting-baseline-report-ok"
      SKIP_RETURN_CODE 77)

  add_test(NAME test_detach_controller_general_listener_uint64_saturation
    COMMAND test_detach_listener_shutdown general-listener-uint64-saturation)
  set_tests_properties(test_detach_controller_general_listener_uint64_saturation
    PROPERTIES
      PASS_REGULAR_EXPRESSION "general-listener-uint64-saturation-ok")

  add_test(NAME test_detach_controller_general_listener_slurm_host_parser
    COMMAND test_detach_listener_shutdown general-listener-slurm-host-parser)
  set_tests_properties(test_detach_controller_general_listener_slurm_host_parser
    PROPERTIES
      PASS_REGULAR_EXPRESSION "general-listener-slurm-host-parser-(ok|skipped)")

  add_test(NAME test_detach_controller_preload_idle_shutdown_io_fail_closed
    COMMAND ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:test_detach_shutdown_preload>
            -DPEAK_LIBRARY=$<TARGET_FILE:peak>
            -DHELPER_EXECUTABLE=$<TARGET_FILE:test_detach_fake_helper>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_preload_shutdown_fail_closed.cmake)
endif()

add_test(NAME test_detach_controller_invalid_request
  COMMAND test_detach_controller invalid
)
set_tests_properties(test_detach_controller_invalid_request
  PROPERTIES
    ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict"
)
