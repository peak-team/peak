foreach(required NM PLATFORM PEAK_LIBRARY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(PLATFORM MATCHES "Darwin")
    set(nm_args -gU)
else()
    set(nm_args -D --defined-only)
endif()

execute_process(
    COMMAND "${NM}" ${nm_args} "${PEAK_LIBRARY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 30)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR
        "failed to inspect installed PEAK dynamic symbols: ${result}\n${error}")
endif()

string(REPLACE "\n" ";" symbol_lines "${output}")
foreach(line IN LISTS symbol_lines)
    string(REGEX MATCH "[^ \t]+$" symbol "${line}")
    string(REGEX REPLACE "^_" "" symbol "${symbol}")
    if(symbol MATCHES "^(gum_|gumjs_)")
        message(FATAL_ERROR
            "installed PEAK DSO exports private Frida Gum symbol ${symbol}")
    endif()
    if(symbol MATCHES "^(OTF2_|otf2_)")
        message(FATAL_ERROR
            "installed PEAK DSO exports private OTF2 symbol ${symbol}")
    endif()
    if(symbol MATCHES
       "^(peak_.*_test_|peak_test_|pthread_listener_test_|dlopen_interceptor_test_|mpi_interceptor_test_)")
        message(FATAL_ERROR
            "installed PEAK DSO exports test hook ${symbol}")
    endif()
endforeach()

message(STATUS "peak_package_consumer_symbols_verified")
