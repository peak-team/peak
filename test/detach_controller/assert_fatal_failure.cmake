if(NOT DEFINED TEST_EXECUTABLE)
  message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED TEST_ARG)
  message(FATAL_ERROR "TEST_ARG is required")
endif()
if(NOT DEFINED HELPER_EXECUTABLE)
  message(FATAL_ERROR "HELPER_EXECUTABLE is required")
endif()
if(NOT DEFINED FAKE_SCENARIO)
  message(FATAL_ERROR "FAKE_SCENARIO is required")
endif()

set(ENV{PEAK_SAFE_DETACH_MODE} "strict")
set(ENV{PEAK_DETACH_BACKEND} "helper")
set(ENV{PEAK_DETACH_HELPER} "${HELPER_EXECUTABLE}")
set(ENV{FAKE_DETACH_HELPER_SCENARIO} "${FAKE_SCENARIO}")
if(DEFINED FAIL_RESUME_INDEX)
  set(ENV{FAKE_DETACH_HELPER_FAIL_RESUME_INDEX} "${FAIL_RESUME_INDEX}")
else()
  unset(ENV{FAKE_DETACH_HELPER_FAIL_RESUME_INDEX})
endif()

execute_process(
  COMMAND "${TEST_EXECUTABLE}" "${TEST_ARG}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)

set(output "${stdout}\n${stderr}")

if("${result}" STREQUAL "0")
  message(FATAL_ERROR
          "expected ${TEST_ARG}/${FAKE_SCENARIO} to fail, but it exited 0\n${output}")
endif()

if(NOT output MATCHES "fatal safe-detach (helper failure|finish failure)")
  message(FATAL_ERROR
          "expected fatal safe-detach log for ${TEST_ARG}/${FAKE_SCENARIO}; result=${result}\n${output}")
endif()

if(DEFINED EXPECTED_FATAL_DETAIL AND NOT output MATCHES "${EXPECTED_FATAL_DETAIL}")
  message(FATAL_ERROR
          "expected fatal detail '${EXPECTED_FATAL_DETAIL}' for ${TEST_ARG}/${FAKE_SCENARIO}; result=${result}\n${output}")
endif()

if(output MATCHES "FAIL:|unknown fake helper scenario|gum_interceptor_attach failed|requires PEAK_HAVE_GUM_PEAK_PC_API")
  message(FATAL_ERROR
          "unexpected harness failure while checking ${TEST_ARG}/${FAKE_SCENARIO}; result=${result}\n${output}")
endif()
