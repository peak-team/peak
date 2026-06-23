if(NOT DEFINED TEST_EXECUTABLE)
  message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()
if(NOT DEFINED PEAK_LIBRARY)
  message(FATAL_ERROR "PEAK_LIBRARY is required")
endif()
if(NOT DEFINED HELPER_EXECUTABLE)
  message(FATAL_ERROR "HELPER_EXECUTABLE is required")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "LD_PRELOAD=${PEAK_LIBRARY}"
          "PEAK_TARGET=peak_shutdown_preload_target"
          "PEAK_HEARTBEAT_INTERVAL=0"
          "PEAK_REQUIRE_SAFE_DETACH=1"
          "PEAK_SAFE_DETACH_MODE=strict"
          "PEAK_DETACH_BACKEND=helper"
          "PEAK_DETACH_HELPER=${HELPER_EXECUTABLE}"
          "FAKE_DETACH_HELPER_SCENARIO=shutdown-missing-response"
          "${TEST_EXECUTABLE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)

set(output "${stdout}\n${stderr}")

if(NOT "${result}" STREQUAL "0")
  message(FATAL_ERROR
          "expected preloaded shutdown fail-closed program to exit 0, got ${result}\n${output}")
endif()

if(NOT output MATCHES "peak-shutdown-preload-main-ok")
  message(FATAL_ERROR
          "preloaded program did not run target body\n${output}")
endif()

if(NOT output MATCHES "detach helper shutdown failed: .*; leaving listener state alive")
  message(FATAL_ERROR
          "expected idle helper SHUTDOWN failure to fail closed in peak_fini\n${output}")
endif()

if(NOT output MATCHES "Skipping pthread listener cleanup because general listener teardown is still reachable")
  message(FATAL_ERROR
          "expected peak_fini to skip pthread listener cleanup after fail-closed general teardown\n${output}")
endif()

if(NOT output MATCHES "Leaving general listener bookkeeping allocated for in-flight callbacks")
  message(FATAL_ERROR
          "expected peak_fini to retain general listener bookkeeping after fail-closed teardown\n${output}")
endif()

if(output MATCHES "fatal safe-detach|terminating to avoid running with unknown stopped-thread state")
  message(FATAL_ERROR
          "idle helper SHUTDOWN failure must not be fatal\n${output}")
endif()
