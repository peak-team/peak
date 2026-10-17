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
if(DEFINED DETACH_BACKEND)
  set(ENV{PEAK_DETACH_BACKEND} "${DETACH_BACKEND}")
else()
  set(ENV{PEAK_DETACH_BACKEND} "helper")
endif()
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

if(DEFINED EXPECTED_EXIT_CODE AND NOT "${result}" STREQUAL "${EXPECTED_EXIT_CODE}")
  message(FATAL_ERROR "expected exit ${EXPECTED_EXIT_CODE}, got ${result}\n${output}")
endif()

if(NOT output MATCHES "fatal safe-detach (helper failure|finish failure)")
  message(FATAL_ERROR
          "expected fatal safe-detach log for ${TEST_ARG}/${FAKE_SCENARIO}; result=${result}\n${output}")
endif()

if(DEFINED EXPECTED_FATAL_DETAIL AND NOT output MATCHES "${EXPECTED_FATAL_DETAIL}")
  message(FATAL_ERROR
          "expected fatal detail '${EXPECTED_FATAL_DETAIL}' for ${TEST_ARG}/${FAKE_SCENARIO}; result=${result}\n${output}")
endif()

# Optional ordered literal witnesses distinguish the intended forward effect
# from an earlier failure sharing the same fatal detail. Existing tests need
# not provide any witnesses and retain their previous acceptance rules.
set(witness_tail "${output}")
foreach(witness_index RANGE 1 4)
  set(witness_key "EXPECTED_WITNESS_${witness_index}")
  if(DEFINED ${witness_key} AND NOT "${${witness_key}}" STREQUAL "")
    string(FIND "${witness_tail}" "${${witness_key}}" witness_position)
    if(witness_position LESS 0)
      message(FATAL_ERROR
        "missing or out-of-order ${witness_key} '${${witness_key}}'; result=${result}\n${output}")
    endif()
    string(LENGTH "${${witness_key}}" witness_length)
    math(EXPR witness_next "${witness_position} + ${witness_length}")
    string(SUBSTRING "${witness_tail}" ${witness_next} -1 witness_tail)
  endif()
endforeach()

if(output MATCHES "FAIL:|unknown fake helper scenario|gum_interceptor_attach failed|requires PEAK_HAVE_GUM_PEAK_PC_API")
  message(FATAL_ERROR
          "unexpected harness failure while checking ${TEST_ARG}/${FAKE_SCENARIO}; result=${result}\n${output}")
endif()

if(DEFINED EXPECTED_EXIT_CODE)
  message(STATUS "verified expected fatal exit ${result}\n${output}")
endif()
