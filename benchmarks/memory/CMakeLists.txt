if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(benchmark_allocation_scaling
  benchmark_allocation_scaling.c
)
target_link_libraries(benchmark_allocation_scaling PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_options(benchmark_allocation_scaling PRIVATE -Wl,--export-dynamic)
endif()

if(TARGET peak)
  # Shared CI runners provide functional coverage only.  The Frontera suite
  # applies the strict 1--56 physical-core performance gates.
  add_test(NAME benchmark_allocation_scaling_smoke
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_allocation_scaling.py
            --exe $<TARGET_FILE:benchmark_allocation_scaling>
            --current-libpeak $<TARGET_FILE:peak>
            --threads 1,2
            --modes pair,realloc,mixed,aligned,posix
            --iterations 20000
            --samples 2
            --max-profiled-ns-per-call 10000
            --max-incremental-ns-per-call 10000
            --min-throughput-retention 0
            --verify-accounting)
  set_tests_properties(benchmark_allocation_scaling_smoke
    PROPERTIES
      LABELS benchmark
      RESOURCE_LOCK peak_memory_benchmark
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "allocation_scaling_ok")
endif()
