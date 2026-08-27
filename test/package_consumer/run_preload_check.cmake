foreach(required PRELOAD_ENV PEAK_LIBRARY FIXTURE STATS_PREFIX)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(GLOB old_reports "${STATS_PREFIX}*.csv" "${STATS_PREFIX}*.csv.tmp.*")
if(old_reports)
    file(REMOVE ${old_reports})
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "${PRELOAD_ENV}=${PEAK_LIBRARY}"
        "PEAK_TARGET=peak_package_consumer_target"
        "PEAK_TARGET_FILE="
        "PEAK_TARGET_GROUP="
        "PEAK_HEARTBEAT_INTERVAL=0"
        "PEAK_OUTPUT_AGGREGATION=local"
        "PEAK_STATSLOG_PATH=${STATS_PREFIX}"
        "${FIXTURE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 45)

set(output "${stdout}\n${stderr}")
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "preloaded fixture exited ${result}\n${output}")
endif()
if(NOT output MATCHES "peak_package_consumer_preload_ok")
    message(FATAL_ERROR "preloaded fixture did not execute\n${output}")
endif()
if(NOT output MATCHES "peak_package_consumer_target[*]*[ \t]*\\|[ \t]+7[ \t]*\\|")
    message(FATAL_ERROR "installed PEAK did not profile the fixture target\n${output}")
endif()

file(GLOB reports "${STATS_PREFIX}*.csv")
list(LENGTH reports report_count)
if(NOT report_count EQUAL 1)
    message(FATAL_ERROR "expected one PEAK CSV, found ${report_count}: ${reports}")
endif()
file(READ "${reports}" report)
if(NOT report MATCHES "peak_package_consumer_target")
    message(FATAL_ERROR "PEAK CSV omitted the fixture target: ${reports}")
endif()

message(STATUS "peak_package_consumer_preload_verified")
