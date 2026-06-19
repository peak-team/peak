add_executable(test_detach_controller
  test_detach_controller.c
  ${PROJECT_SOURCE_DIR}/src/peak_detach_controller.c
)
add_executable(test_detach_fake_helper test_detach_fake_helper.c)

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
  target_compile_definitions(test_detach_controller PRIVATE PEAK_HAVE_GUM_PEAK_PC_API=1)
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

if(TARGET peak_detach_helper)
  add_custom_command(TARGET test_detach_controller POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:peak_detach_helper>
            $<TARGET_FILE_DIR:test_detach_controller>/peak_detach_helper
  )
endif()

add_test(NAME test_detach_controller_compatibility
  COMMAND test_detach_controller compatibility
)
set_tests_properties(test_detach_controller_compatibility
  PROPERTIES
    ENVIRONMENT "PEAK_REQUIRE_SAFE_DETACH=0;PEAK_SAFE_DETACH_MODE=compatibility"
)

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
endif()

if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  add_test(NAME test_detach_controller_batch_guards
    COMMAND test_detach_controller batch-guards)
  set_tests_properties(test_detach_controller_batch_guards
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>"
      SKIP_RETURN_CODE 77)

  foreach(_fake_scenario IN ITEMS stop-permission stop-timeout bad-snapshot blocked-pc)
    add_test(NAME test_detach_controller_fake_helper_${_fake_scenario}
      COMMAND test_detach_controller fake-helper-fail-closed)
    set_tests_properties(test_detach_controller_fake_helper_${_fake_scenario}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=${_fake_scenario}")
  endforeach()

  add_test(NAME test_detach_controller_fake_helper_stop-release-failed
    COMMAND test_detach_controller fake-helper-fail-closed)
  set_tests_properties(test_detach_controller_fake_helper_stop-release-failed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=stop-release-failed"
      WILL_FAIL TRUE)

  add_test(NAME test_detach_controller_fake_helper_evacuate-error
    COMMAND test_detach_controller fake-helper-fail-closed)
  set_tests_properties(test_detach_controller_fake_helper_evacuate-error
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=evacuate-error")

  foreach(_fake_scenario IN ITEMS evacuate-release-failed)
    add_test(NAME test_detach_controller_fake_helper_${_fake_scenario}
      COMMAND test_detach_controller fake-helper-fail-closed)
    set_tests_properties(test_detach_controller_fake_helper_${_fake_scenario}
      PROPERTIES
        ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=${_fake_scenario}"
        WILL_FAIL TRUE)
  endforeach()

  add_test(NAME test_detach_controller_fake_helper_resume-release-failed
    COMMAND test_detach_controller strict-helper-empty)
  set_tests_properties(test_detach_controller_fake_helper_resume-release-failed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=resume-release-failed"
      WILL_FAIL TRUE)

  add_test(NAME test_detach_controller_fake_helper_shutdown_sequence
    COMMAND test_detach_controller fake-helper-shutdown-sequence)
  set_tests_properties(test_detach_controller_fake_helper_shutdown_sequence
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>")

  add_test(NAME test_detach_controller_fake_helper_batch_detach
    COMMAND test_detach_controller fake-helper-batch-detach)
  set_tests_properties(test_detach_controller_fake_helper_batch_detach
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_batch_detach_resume-release-failed
    COMMAND test_detach_controller fake-helper-batch-detach)
  set_tests_properties(test_detach_controller_fake_helper_batch_detach_resume-release-failed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=resume-release-failed"
      WILL_FAIL TRUE)

  add_test(NAME test_detach_controller_fake_helper_batch_abort_rollback
    COMMAND test_detach_controller fake-helper-batch-abort-rollback)
  set_tests_properties(test_detach_controller_fake_helper_batch_abort_rollback
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=evacuate-error")

  add_test(NAME test_detach_controller_fake_helper_batch_mixed
    COMMAND test_detach_controller fake-helper-batch-mixed)
  set_tests_properties(test_detach_controller_fake_helper_batch_mixed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=blocked-pc")

  add_test(NAME test_detach_controller_fake_helper_batch_missing_gum_snapshot
    COMMAND test_detach_controller fake-helper-batch-missing-gum-snapshot)
  set_tests_properties(test_detach_controller_fake_helper_batch_missing_gum_snapshot
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_batch_reattach
    COMMAND test_detach_controller fake-helper-batch-reattach)
  set_tests_properties(test_detach_controller_fake_helper_batch_reattach
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=success-zero")

  add_test(NAME test_detach_controller_fake_helper_batch_reattach_resume-release-failed
    COMMAND test_detach_controller fake-helper-batch-reattach)
  set_tests_properties(test_detach_controller_fake_helper_batch_reattach_resume-release-failed
    PROPERTIES
      ENVIRONMENT "PEAK_SAFE_DETACH_MODE=strict;PEAK_DETACH_HELPER=$<TARGET_FILE:test_detach_fake_helper>;FAKE_DETACH_HELPER_SCENARIO=resume-release-failed;FAKE_DETACH_HELPER_FAIL_RESUME_INDEX=2"
      WILL_FAIL TRUE)
endif()

add_test(NAME test_detach_controller_invalid_request
  COMMAND test_detach_controller invalid
)
set_tests_properties(test_detach_controller_invalid_request
  PROPERTIES
    ENVIRONMENT "PEAK_REQUIRE_SAFE_DETACH=0;PEAK_SAFE_DETACH_MODE=compatibility"
)
