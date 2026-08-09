if(NOT DEFINED PEAK_SOURCE_ROOT)
    message(FATAL_ERROR "PEAK_SOURCE_ROOT is required")
endif()

file(READ "${PEAK_SOURCE_ROOT}/src/peak.c" peak_source)
foreach(required_fragment
        "pthread_cond_timedwait_relative_np"
        "CLOCK_MONOTONIC"
        "return ENOTSUP"
        "PTHREAD_CANCEL_DISABLE")
    string(FIND "${peak_source}" "${required_fragment}" fragment_offset)
    if(fragment_offset EQUAL -1)
        message(FATAL_ERROR "missing finalization wait contract: ${required_fragment}")
    endif()
endforeach()
string(FIND "${peak_source}" "CLOCK_REALTIME" realtime_offset)
if(NOT realtime_offset EQUAL -1)
    message(FATAL_ERROR "finalization wait must not use CLOCK_REALTIME")
endif()

message("peak_fini_wait_platform_contract_ok")
