cmake_minimum_required(VERSION 3.16)

# ----- find 'make' to run your Makefile -----
find_program(MAKE_EXE NAMES make gmake REQUIRED)

# Request both components together so CMake cannot pair one interpreter with
# another installation's development headers.  ctypes tests require the
# interpreter; development files gate only the CPython extension matrix.
find_package(Python3 COMPONENTS Interpreter Development)
if(NOT Python3_Interpreter_FOUND)
  message(FATAL_ERROR "Python3 interpreter is required for dynamic dlopen tests")
endif()

if(Python3_Development_FOUND)
  set(_BUILD_PYTHON_EXTENSIONS 1)
else()
  set(_BUILD_PYTHON_EXTENSIONS 0)
  message(STATUS
    "Python3 development headers unavailable; skipping dynamic CPython extension fixtures")
endif()

set(_PY_INCLUDE_FLAGS)
foreach(_py_include_dir IN LISTS Python3_INCLUDE_DIRS)
  list(APPEND _PY_INCLUDE_FLAGS "-I${_py_include_dir}")
endforeach()
string(JOIN " " _PY_INCLUDE_FLAGS_STR ${_PY_INCLUDE_FLAGS})
execute_process(
  COMMAND ${Python3_EXECUTABLE} -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX') or '')"
  OUTPUT_VARIABLE _PY_EXT_SUFFIX
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Environment we pass to your Makefile so it picks compilers & python
# (Change CC/AR below if you want to force GCC instead of nvc)
set(_MAKE_ENV
  CC=${CMAKE_C_COMPILER}
  AR=${CMAKE_AR}
  LD=${CMAKE_LINKER}
  PYTHON=${Python3_EXECUTABLE}
  PY_INCLUDE=${_PY_INCLUDE_FLAGS_STR}
  PY_LDFLAGS=
  PY_EXT_SUFFIX=${_PY_EXT_SUFFIX}
  BUILD_PYTHON_EXTENSIONS=${_BUILD_PYTHON_EXTENSIONS}
)

# Keep generated shared libraries, executables, objects, and Python extensions
# in the build tree so stale source-tree artifacts cannot affect test results.
set(DL_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DL_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# ----- build via Makefile: `make -f Makefile -j all` -----
add_custom_target(dynamic_lib_build ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${DL_SRC_DIR}/Makefile
          ${DL_SRC_DIR}/libA.c
          ${DL_SRC_DIR}/libB.c
          ${DL_SRC_DIR}/main.c
          ${DL_SRC_DIR}/customCpackage.c
          ${DL_SRC_DIR}/dlopen_flags_test.c
          ${DL_SRC_DIR}/concurrent_dlopen.c
          ${DL_SRC_DIR}/dlopen_mutation_guard.c
          ${DL_SRC_DIR}/dlopen_mutation_fairness.c
          ${DL_SRC_DIR}/dlopen_teardown_hot.c
          ${DL_SRC_DIR}/dlopen_replace_failure.c
          ${DL_SRC_DIR}/fork_inflight_dlopen.c
          ${DL_SRC_DIR}/nested_dlopen_outer.c
          ${DL_SRC_DIR}/nested_ifunc_outer.c
          ${DL_SRC_DIR}/resolver_nested_ifunc.c
          ${DL_SRC_DIR}/ifunc_target.c
          ${DL_SRC_DIR}/notype_target.S
          ${DL_SRC_DIR}/notype_executable_test.c
          ${DL_SRC_DIR}/startup_resolver_nested_ifunc.c
          ${DL_SRC_DIR}/startup_unloadable_target.c
          ${DL_SRC_DIR}/startup_loader.c
          ${DL_SRC_DIR}/startup_provider_retention_test.c
          ${DL_SRC_DIR}/startup_local_callable_loader.c
          ${DL_SRC_DIR}/startup_local_callable_test.c
          ${DL_SRC_DIR}/lazy_unresolved.c
          ${DL_SRC_DIR}/multiple_providers.py
          ${DL_SRC_DIR}/fork_child_dlopen.py
          ${DL_SRC_DIR}/dlopen_own_library.py
          ${DL_SRC_DIR}/import_customCpackage_variant.py
          ${DL_SRC_DIR}/test_customCpackage_script.sh
          ${DL_DIR}/
  COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile -j
  WORKING_DIRECTORY ${DL_DIR}
  COMMENT "Building dynamic_lib fixtures via Makefile"
  VERBATIM
)

if(Python3_Development_FOUND)
  # Pick the self-contained module as the canonical import name.
  add_custom_command(TARGET dynamic_lib_build POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile use_staA_staB
    WORKING_DIRECTORY ${DL_DIR}
    COMMENT "Selecting Python module variant: staA_staB (self-contained)"
    VERBATIM
  )
endif()

set(_DLOPEN_DRAIN_SCRIPT
  "import ctypes, time; lib = ctypes.CDLL('./libB.so'); deadline = time.time() + 1.0\nwhile time.time() < deadline:\n    lib.b_dynamic()"
)
add_test(NAME test_dynamic_dlopen
  COMMAND ${Python3_EXECUTABLE} -c "${_DLOPEN_DRAIN_SCRIPT}"
)

if(PEAK_GUM_PEAK_API_AVAILABLE)
add_test(NAME test_dynamic_dlopen_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libB.so'); lib.b_dynamic()"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|aarch64|arm64)$")
add_test(NAME test_dynamic_dlopen_ifunc_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libIfuncTarget.so'); lib.peak_ifunc_target()"
)
set_tests_properties(test_dynamic_dlopen_ifunc_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_ifunc_target;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_ifunc_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "peak_ifunc_target[*]*\\|[ \t]+1\\|"
)

add_test(NAME test_dynamic_nested_ifunc_constructor_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; ctypes.CDLL('./libNestedIfuncOuter.so')"
)

add_test(NAME test_dynamic_resolver_nested_dlopen_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; ctypes.CDLL('./libResolverNestedIfunc.so')"
)
set_tests_properties(test_dynamic_resolver_nested_dlopen_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_resolver_nested_ifunc,b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_resolver_nested_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)
set_tests_properties(test_dynamic_nested_ifunc_constructor_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_ifunc_target;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_nested_ifunc_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "peak_ifunc_target[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_dlopen_notype_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libNotypeTarget.so'); lib.peak_notype_target()"
)
set_tests_properties(test_dynamic_dlopen_notype_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_notype_target;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_notype_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "peak_notype_target[*]*\\|[ \t]+1\\|"
)

add_test(NAME test_startup_executable_notype_target
  COMMAND ${DL_DIR}/notype_executable_test
)
set_tests_properties(test_startup_executable_notype_target
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_notype_target;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_startup_executable_notype_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "peak_notype_target[*]*\\|[ \t]+1\\|"
)

add_test(NAME test_startup_resolver_nested_dlopen_first_call
  COMMAND ${DL_DIR}/startup_resolver_nested_ifunc_test
)
set_tests_properties(test_startup_resolver_nested_dlopen_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_startup_resolver_ifunc,b_dynamic;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_startup_resolver_nested_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "startup_resolver_nested_ifunc_ok.*peak_startup_resolver_ifunc[*]*\\|[ \t]+1\\|.*b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)

add_test(NAME test_startup_rtld_local_callable_rescan
  COMMAND ${DL_DIR}/startup_local_callable_test
)
set_tests_properties(test_startup_rtld_local_callable_rescan
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_ifunc_target,peak_notype_target;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_startup_local_callable_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "startup_local_callable_ok.*peak_ifunc_target[*]*\\|[ \t]+1\\|.*peak_notype_target[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)
endif()

set_tests_properties(test_dynamic_dlopen_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_first_call_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|"
)

add_test(NAME test_dynamic_dlopen_first_call_waits_for_completion
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libB.so'); lib.b_dynamic()"
)
set_tests_properties(test_dynamic_dlopen_first_call_waits_for_completion
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_TEST_DLOPEN_ATTACH_DELAY_MS=1500;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_completion_wait_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 10
)

add_test(NAME test_dynamic_dlopen_terminal_prepare_error_returns
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; ctypes.CDLL('./libB.so'); print('dynamic_attach_terminal_error_returned')"
)
set_tests_properties(test_dynamic_dlopen_terminal_prepare_error_returns
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_TEST_DYNAMIC_ATTACH_PREPARE_ERROR=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_terminal_prepare_error_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dynamic attach permanently failed.*dynamic attach reached a permanent failure.*dynamic_attach_terminal_error_returned"
    TIMEOUT 10
)

add_test(NAME test_dynamic_dlopen_retryable_prepare_failure_fails_open
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; ctypes.CDLL('./libB.so'); print('dynamic_attach_retryable_failure_returned')"
)
set_tests_properties(test_dynamic_dlopen_retryable_prepare_failure_fails_open
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_TEST_DYNAMIC_ATTACH_PREPARE_TIMEOUT=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_retryable_prepare_failure_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dynamic attach failed open.*safe mutation could not be established.*dynamic_attach_retryable_failure_returned"
    FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
    TIMEOUT 10
)

add_test(NAME test_dynamic_dlopen_support_wrapper_not_rediscovered
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; process = ctypes.CDLL(None); count = ctypes.c_size_t.in_dll(process, 'peak_hook_address_count'); assert count.value == 2, f'unexpected startup hook count: {count.value}'; lib = ctypes.CDLL('./libB.so'); lib.b_dynamic(); assert count.value == 2, f'support wrapper was dynamically rediscovered: {count.value}'; print('support_wrapper_dynamic_eligibility_ok')"
)
set_tests_properties(test_dynamic_dlopen_support_wrapper_not_rediscovered
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=close,b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_support_wrapper_eligibility_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "support_wrapper_dynamic_eligibility_ok.*b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_dlopen_exact_tail_entry
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libB.so'); lib.b_tail_wrapper(); lib.b_tail_impl()"
)
set_tests_properties(test_dynamic_dlopen_exact_tail_entry
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_tail_wrapper,b_tail_impl;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_exact_tail_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_tail_wrapper[*]*\\|[ \t]+1\\|.*b_tail_impl[*]*\\|[ \t]+2\\|"
)

add_test(NAME test_dynamic_nested_dlopen_constructor_first_call
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; ctypes.CDLL('./libNestedOuter.so')"
)
set_tests_properties(test_dynamic_nested_dlopen_constructor_first_call
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_nested_constructor_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_dlopen_multiple_providers
  COMMAND ${Python3_EXECUTABLE} ${DL_DIR}/multiple_providers.py
)
set_tests_properties(test_dynamic_dlopen_multiple_providers
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_multiple_providers_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|.*b_dynamic[*]*\\|[ \t]+5\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_concurrent_dlopen_requests
  COMMAND ${DL_DIR}/concurrent_dlopen_test
)

add_test(NAME test_dynamic_concurrent_dlopen_with_heartbeat
  COMMAND ${DL_DIR}/concurrent_dlopen_test
)
set_tests_properties(test_dynamic_concurrent_dlopen_with_heartbeat
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_HEARTBEAT_INTERVAL=0.001;PEAK_ENABLE_PER_TARGET_HEARTBEAT=0;PEAK_ENABLE_GLOBAL_HEARTBEAT=0;PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_concurrent_heartbeat_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+16\\|"
    TIMEOUT 30
)
set_tests_properties(test_dynamic_concurrent_dlopen_requests
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_concurrent_dlopen_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+16\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_dlopen_controller_mutation_guard
  COMMAND ${DL_DIR}/dlopen_mutation_guard_test
)
set_tests_properties(test_dynamic_dlopen_controller_mutation_guard
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_dlopen_mutation_guard_target,b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_SAFE_DETACH_MODE=compatibility;PEAK_TEST_DLOPEN_BODY_PAUSE_MATCH=./libB.so;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_mutation_guard_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dlopen_mutation_guard_ok.*peak_dlopen_mutation_guard_target[*]*\\|[ \t]+2\\|.*b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_dlopen_controller_mutation_fairness
  COMMAND ${DL_DIR}/dlopen_mutation_fairness_test
)
set_tests_properties(test_dynamic_dlopen_controller_mutation_fairness
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_dlopen_fairness_target;PEAK_PROFILE_INTERPRETERS=1;PEAK_SAFE_DETACH_MODE=compatibility;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_mutation_fairness_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dlopen_mutation_fairness_ok.*peak_dlopen_fairness_target[*]*\\|[ \t]+2\\|"
    TIMEOUT 30
)

add_test(NAME test_dynamic_fork_child_dlopen_warns_unsupported
  COMMAND ${Python3_EXECUTABLE} ${DL_DIR}/fork_child_dlopen.py
)

add_test(NAME test_dynamic_fork_inflight_dlopen_no_deadlock
  COMMAND ${DL_DIR}/fork_inflight_dlopen_test
)
set_tests_properties(test_dynamic_fork_inflight_dlopen_no_deadlock
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_TEST_DLOPEN_ATTACH_DELAY_MS=1500;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_fork_inflight_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "fork_inflight_dlopen_ok"
    TIMEOUT 10
)

add_test(NAME test_dynamic_dlopen_teardown_restores_entry_before_drain
  COMMAND ${DL_DIR}/dlopen_teardown_hot_test
)
set_tests_properties(test_dynamic_dlopen_teardown_restores_entry_before_drain
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=peak_dlopen_teardown_unresolved_target;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_TEST_DLOPEN_BODY_PAUSE_MATCH=./libB.so;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_teardown_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dlopen_teardown_entry_first_ok"
    TIMEOUT 30
)

foreach(_replace_failure IN ITEMS -1 -2)
  if(_replace_failure EQUAL -1)
    set(_replace_failure_name wrong_signature)
  else()
    set(_replace_failure_name already_replaced)
  endif()
  add_test(NAME test_dynamic_dlopen_replace_failure_${_replace_failure_name}
    COMMAND ${DL_DIR}/dlopen_replace_failure_test
  )
  set_tests_properties(
    test_dynamic_dlopen_replace_failure_${_replace_failure_name}
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=peak_dlopen_replace_failure_target,b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_SAFE_DETACH_MODE=compatibility;PEAK_TEST_DLOPEN_REPLACE_RESULT=${_replace_failure};${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_replace_failure_${_replace_failure_name}_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "replace-fast-failed-${_replace_failure}.*peak_dlopen_replace_failure_target[*]*\\|[ \t]+2\\|.*dlopen_replace_failure_cleanup_ok"
      FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\||Skipping remaining PEAK teardown"
      TIMEOUT 30
  )
endforeach()

add_test(NAME test_dynamic_dlopen_duplicate_peak_attach_preserves_ownership
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; peak = ctypes.CDLL(None); peak.dlopen_interceptor_test_replacement_installed_by_peak.restype = ctypes.c_int; peak.dlopen_interceptor_test_attach.restype = ctypes.c_int; assert peak.dlopen_interceptor_test_replacement_installed_by_peak() == 1; assert peak.dlopen_interceptor_test_attach() == -2; assert peak.dlopen_interceptor_test_replacement_installed_by_peak() == 1; library = ctypes.CDLL('./libB.so'); library.b_dynamic()"
)
set_tests_properties(
  test_dynamic_dlopen_duplicate_peak_attach_preserves_ownership
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_duplicate_peak_attach_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dlopen Gum replacement is already installed by PEAK, preserving existing ownership.*b_dynamic[*]*\\|[ \t]+1\\|"
    TIMEOUT 30
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME test_dynamic_dlopen_provider_after_startup_match
    COMMAND ${Python3_EXECUTABLE} -c
            "import ctypes; first = ctypes.CDLL('./libB.so'); first.b_dynamic(); second = ctypes.CDLL('./libBSecond.so'); second.b_dynamic()"
  )
  set_tests_properties(test_dynamic_dlopen_provider_after_startup_match
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_provider_after_startup_statslog;LD_PRELOAD=${DL_DIR}/libB.so:${PROJECT_BINARY_DIR}/src/libpeak.so"
      PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+1\\|.*b_dynamic[*]*\\|[ \t]+1\\|"
      TIMEOUT 30
  )

  add_test(NAME test_startup_provider_retained_until_hook_detach
    COMMAND ${DL_DIR}/startup_provider_retention_test
  )
  set_tests_properties(test_startup_provider_retained_until_hook_detach
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=peak_startup_unloadable_target,peak_startup_unloadable_target_second;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_startup_provider_retention_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "startup_provider_retained_ok.*peak_startup_unloadable_target[*]*\\|[ \t]+2\\|.*peak_startup_unloadable_target_second[*]*\\|[ \t]+2\\|"
      TIMEOUT 30
  )

  add_test(NAME test_startup_rtld_local_rescan_failure_isolated
    COMMAND ${DL_DIR}/startup_local_callable_test
  )
  set_tests_properties(test_startup_rtld_local_rescan_failure_isolated
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=peak_ifunc_target,peak_notype_target;PEAK_TEST_STARTUP_RESCAN_FAIL_FIRST=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_startup_rescan_failure_isolated_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "test forcing first startup provider rescan failure.*startup_local_callable_ok.*(peak_ifunc_target|peak_notype_target)[*]*\\|[ \t]+1\\|"
      TIMEOUT 30
  )
endif()
set_tests_properties(test_dynamic_fork_child_dlopen_warns_unsupported
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_fork_child_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "fork child dynamic dlopen profiling is unsupported until exec.*fork_child_dlopen_warning_count=1.*fork_child_dlopen_unsupported_ok"
    FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
    TIMEOUT 10
)

add_test(NAME test_dynamic_dlopen_controller_start_failure_no_wait
  COMMAND ${Python3_EXECUTABLE} -c
          "import ctypes; lib = ctypes.CDLL('./libB.so'); lib.b_dynamic(); print('controller_start_failure_no_wait_ok')"
)
set_tests_properties(test_dynamic_dlopen_controller_start_failure_no_wait
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_TEST_GENERAL_CONTROLLER_START_FAIL=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_controller_start_failure_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "dynamic target loading disabled because the general listener controller is unavailable.*controller_start_failure_no_wait_ok"
    FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
    TIMEOUT 10
)
endif()

if(PEAK_GUM_PEAK_API_AVAILABLE)
set_tests_properties(test_dynamic_dlopen
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
)
else()
set_tests_properties(test_dynamic_dlopen
  PROPERTIES
    WORKING_DIRECTORY ${DL_DIR}
    ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_statslog;${PRELOAD_VAR}"
    PASS_REGULAR_EXPRESSION "skipping dlopen Gum replace: missing-gum-api"
    FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
)
endif()

if(Python3_Development_FOUND)
  add_test(NAME test_dynamic_customCpackage_script
    COMMAND ${CMAKE_COMMAND} -E env
            PYTHON_BIN=${Python3_EXECUTABLE}
            ${DL_DIR}/test_customCpackage_script.sh
  )
  set_tests_properties(test_dynamic_customCpackage_script
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      PASS_REGULAR_EXPRESSION "Imported customCpackage OK"
  )
endif()

if(NOT PEAK_GUM_PEAK_API_AVAILABLE)
  add_test(NAME test_dynamic_dlopen_strict_stock_gum
    COMMAND ${Python3_EXECUTABLE} -c "${_DLOPEN_DRAIN_SCRIPT}"
  )
  set_tests_properties(test_dynamic_dlopen_strict_stock_gum
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_REQUIRE_SAFE_DETACH=1;${HEARTBEAT_OFF_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_strict_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "skipping dlopen Gum replace: missing-gum-api"
      FAIL_REGULAR_EXPRESSION "b_dynamic[*]*\\|[ \t]+[1-9][0-9]*\\|"
  )
else()
  add_test(NAME test_dynamic_dlopen_strict_patched_gum
    COMMAND ${Python3_EXECUTABLE} -c "${_DLOPEN_DRAIN_SCRIPT}"
  )
  set_tests_properties(test_dynamic_dlopen_strict_patched_gum
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_strict_statslog;${PRELOAD_VAR}"
      PASS_REGULAR_EXPRESSION "b_dynamic[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
      TIMEOUT 60
  )

  set(_DYNAMIC_STRICT_ENV
      "PEAK_TARGET=b_dynamic;PEAK_PROFILE_INTERPRETERS=1;PEAK_REQUIRE_SAFE_DETACH=1;PEAK_ENABLE_PER_TARGET_HEARTBEAT=1;PEAK_OVERHEAD_RATIO=0.000001;PEAK_HEARTBEAT_INTERVAL=0.005;PEAK_HIBERNATION_CYCLE=1;${PRELOAD_VAR}")

  foreach(_a_variant IN ITEMS dynB staB)
    add_test(NAME test_dynamic_dlopen_libA_${_a_variant}_strict_patched_gum
      COMMAND ${Python3_EXECUTABLE}
              ${DL_DIR}/dlopen_own_library.py
              --delay 0
              --which A
              --a-variant ${_a_variant}
              --duration 1.0
    )
    set_tests_properties(test_dynamic_dlopen_libA_${_a_variant}_strict_patched_gum
      PROPERTIES
        WORKING_DIRECTORY ${DL_DIR}
        ENVIRONMENT "${_DYNAMIC_STRICT_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_dlopen_libA_${_a_variant}_strict_statslog"
        PASS_REGULAR_EXPRESSION "b_dynamic[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
        TIMEOUT 60
    )
  endforeach()

  if(Python3_Development_FOUND)
    foreach(_py_variant IN ITEMS dynA_dynB dynA_staB staA_dynB staA_staB)
      add_test(NAME test_dynamic_customCpackage_${_py_variant}_strict_patched_gum
        COMMAND ${Python3_EXECUTABLE}
                ${DL_DIR}/import_customCpackage_variant.py
                customCpackage_${_py_variant}
                1.0
      )
      set_tests_properties(test_dynamic_customCpackage_${_py_variant}_strict_patched_gum
        PROPERTIES
          WORKING_DIRECTORY ${DL_DIR}
          ENVIRONMENT "${_DYNAMIC_STRICT_ENV};PEAK_STATSLOG_PATH=${CMAKE_CURRENT_BINARY_DIR}/peak_dynamic_customCpackage_${_py_variant}_strict_statslog"
          PASS_REGULAR_EXPRESSION "b_dynamic[*][*]*\\|[ \t]+[1-9][0-9]*\\|"
          TIMEOUT 60
      )
    endforeach()
  endif()
endif()

# Optional: expose a `make-clean` helper in the CMake build
add_custom_target(dynamic_lib_clean
  COMMAND ${CMAKE_COMMAND} -E env ${_MAKE_ENV} ${MAKE_EXE} -f ${DL_DIR}/Makefile clean
  WORKING_DIRECTORY ${DL_DIR}
  COMMENT "Cleaning dynamic_lib artifacts via Makefile"
  VERBATIM
)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_test(NAME test_dynamic_lib_dlopen_flags
    COMMAND ${DL_DIR}/dlopen_flags_test
  )
  set_tests_properties(test_dynamic_lib_dlopen_flags
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=b_dynamic;PEAK_HEARTBEAT_INTERVAL=0;${PRELOAD_VAR}"
  )

  add_test(NAME test_dynamic_lib_dlopen_target_no_crash
    COMMAND ${DL_DIR}/dlopen_flags_test
  )
  set_tests_properties(test_dynamic_lib_dlopen_target_no_crash
    PROPERTIES
      WORKING_DIRECTORY ${DL_DIR}
      ENVIRONMENT "PEAK_TARGET=dlopen;PEAK_HEARTBEAT_INTERVAL=0;${PRELOAD_VAR}"
      FAIL_REGULAR_EXPRESSION "Segmentation fault|SIGSEGV|Caught signal|Fatal error"
      TIMEOUT 20
  )
endif()
