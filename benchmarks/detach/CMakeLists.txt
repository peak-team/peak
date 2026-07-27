find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(benchmark_detach_hotloop
  ${PROJECT_SOURCE_DIR}/test/detach_runtime/test_detach_hotloop.c
)
target_include_directories(benchmark_detach_hotloop
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${FRIDA_GUM_INCLUDE_DIRS}
)
if(PEAK_GUM_PEAK_PC_API_AVAILABLE AND PEAK_DETACH_HELPER_SUPPORTED)
  target_compile_definitions(benchmark_detach_hotloop PRIVATE PEAK_HAVE_GUM_PEAK_PC_API=1)
endif()
target_link_libraries(benchmark_detach_hotloop PRIVATE Threads::Threads ${DL_LIBRARY})
if(RT_LIBRARY)
  target_link_libraries(benchmark_detach_hotloop PRIVATE ${RT_LIBRARY})
endif()
if(MPI_C_FOUND)
  target_link_libraries(benchmark_detach_hotloop PRIVATE MPI::MPI_C)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_options(benchmark_detach_hotloop PRIVATE -Wl,--export-dynamic)
endif()

add_executable(benchmark_detach_stale_threads
  ${PROJECT_SOURCE_DIR}/test/detach_runtime/test_detach_stale_threads.c
)
target_link_libraries(benchmark_detach_stale_threads PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_options(benchmark_detach_stale_threads PRIVATE -Wl,--export-dynamic)
endif()

add_executable(benchmark_callback_hotpath
  benchmark_callback_hotpath.c
)
target_link_libraries(benchmark_callback_hotpath PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_options(benchmark_callback_hotpath PRIVATE -Wl,--export-dynamic)
endif()

set(PEAK_HOTPATH_PERF_MAX_SLOWDOWN "10:30,100:5,1000:1.5" CACHE STRING
  "Maximum profiled/unprofiled slowdown as target-ns:limit pairs")
set(PEAK_HOTPATH_PERF_MAX_NS_PER_CALL "10:350,100:500,1000:1600"
  CACHE STRING
  "Maximum thread-normalized callback ns/call as target-ns:limit pairs")
set(PEAK_HOTPATH_PERF_MIN_SCALING_EFFICIENCY "0.65" CACHE STRING
  "Minimum per-thread steady-state throughput scaling efficiency")
set(PEAK_HOTPATH_PERF_MIN_RELATIVE_SCALING_EFFICIENCY "0.75" CACHE STRING
  "Minimum PEAK scaling efficiency relative to the uninstrumented target")

set(PEAK_STRICT_DETACH_BENCHMARK_MIN_RATIO "0.80" CACHE STRING
  "Minimum current strict physical-detach throughput as a fraction of no-preload throughput")
set(PEAK_STRICT_DETACH_STRESS_THREADS "32" CACHE STRING
  "Thread count for strict physical-detach hot-loop stress benchmarking")
set(PEAK_STRICT_DETACH_STRESS_SAMPLES "12" CACHE STRING
  "Number of repeated strict physical-detach hot-loop benchmark samples")
set(PEAK_STRICT_DETACH_STRESS_SECONDS "1" CACHE STRING
  "Duration in seconds for each strict physical-detach hot-loop benchmark sample")

if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND
   PEAK_GUM_PEAK_PC_API_AVAILABLE AND
   TARGET peak_detach_helper)
  add_test(NAME benchmark_callback_hotpath_matrix
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_callback_hotpath_matrix.py
            --exe $<TARGET_FILE:benchmark_callback_hotpath>
            --current-libpeak $<TARGET_FILE:peak>
            --threads 1,2,8,32,64
            --target-ns 10,100,1000
            --seconds 0.5
            --samples 3
            --max-slowdown ${PEAK_HOTPATH_PERF_MAX_SLOWDOWN}
            --max-ns-per-call
              ${PEAK_HOTPATH_PERF_MAX_NS_PER_CALL}
            --min-scaling-efficiency
              ${PEAK_HOTPATH_PERF_MIN_SCALING_EFFICIENCY}
            --min-relative-scaling-efficiency
              ${PEAK_HOTPATH_PERF_MIN_RELATIVE_SCALING_EFFICIENCY})
  set_tests_properties(benchmark_callback_hotpath_matrix
    PROPERTIES
      LABELS benchmark
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 180
      FAIL_REGULAR_EXPRESSION
        "steady-state callback performance gate failed")

  add_test(NAME benchmark_detach_hotloop_strict
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_hotloop_benchmark.py
            --exe $<TARGET_FILE:benchmark_detach_hotloop>
            --current-libpeak $<TARGET_FILE:peak>
            --threads 4
            --seconds 1
            --samples 3
            --min-strict-baseline-ratio ${PEAK_STRICT_DETACH_BENCHMARK_MIN_RATIO})
  set_tests_properties(benchmark_detach_hotloop_strict
    PROPERTIES
      LABELS benchmark
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 90
      PASS_REGULAR_EXPRESSION "current_strict_physical:.*detached_runs=3/3")

  add_test(NAME benchmark_detach_hotloop_high_thread_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_hotloop_stress.py
            --exe $<TARGET_FILE:benchmark_detach_hotloop>
            --libpeak $<TARGET_FILE:peak>
            --threads ${PEAK_STRICT_DETACH_STRESS_THREADS}
            --seconds ${PEAK_STRICT_DETACH_STRESS_SECONDS}
            --samples ${PEAK_STRICT_DETACH_STRESS_SAMPLES}
            --disable-reattach
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-hotloop-detach-trace
            --fail-on-transition-skips)
  set_tests_properties(benchmark_detach_hotloop_high_thread_stress
    PROPERTIES
      LABELS "benchmark;stress"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 240
      PASS_REGULAR_EXPRESSION "hotloop_stress_ok")

  # Reattach stress exercises the physical transition mechanics rather than
  # production policy admission.  Remove the production cooldown and give the
  # short runs enough target/global budget to admit reattach after a fast
  # callback accumulates calls while the initial detach is coordinated.  The
  # profile ratio aggregates thread time, so its wall-time ratio may approach
  # the configured 64-thread capacity rather than being bounded by 1.0.
  set(PEAK_REATTACH_STRESS_RATIO "64.0")
  set(PEAK_REATTACH_STRESS_ENV
      "PEAK_REATTACH_COOLDOWN_MS=0;PEAK_GLOBAL_OVERHEAD_RATIO=${PEAK_REATTACH_STRESS_RATIO};PEAK_GLOBAL_REATTACH_FACTOR=1.0")

  add_test(NAME benchmark_detach_hotloop_reattach_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_hotloop_stress.py
            --exe $<TARGET_FILE:benchmark_detach_hotloop>
            --libpeak $<TARGET_FILE:peak>
            --threads ${PEAK_STRICT_DETACH_STRESS_THREADS}
            --seconds 3
            --samples 6
            --overhead-ratio ${PEAK_REATTACH_STRESS_RATIO}
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-hotloop-reattach-trace
            --require-reattach
            --fail-on-transition-skips)
  set_tests_properties(benchmark_detach_hotloop_reattach_stress
    PROPERTIES
      LABELS "benchmark;stress"
      ENVIRONMENT "${PEAK_REATTACH_STRESS_ENV}"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 240
      PASS_REGULAR_EXPRESSION "hotloop_stress_ok")

  add_test(NAME benchmark_detach_hotloop_signal_reattach_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_hotloop_stress.py
            --exe $<TARGET_FILE:benchmark_detach_hotloop>
            --libpeak $<TARGET_FILE:peak>
            --threads ${PEAK_STRICT_DETACH_STRESS_THREADS}
            --seconds 3
            --samples 6
            --overhead-ratio ${PEAK_REATTACH_STRESS_RATIO}
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-hotloop-signal-reattach-trace
            --require-reattach
            --detach-backend signal
            --fail-on-transition-skips
            --max-reattach-classify-failed 0)
  set_tests_properties(benchmark_detach_hotloop_signal_reattach_stress
    PROPERTIES
      LABELS "benchmark;stress"
      ENVIRONMENT "${PEAK_REATTACH_STRESS_ENV}"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 240
      PASS_REGULAR_EXPRESSION "hotloop_stress_ok")

  add_test(NAME benchmark_detach_stale_threads_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_stale_thread_stress.py
            --exe $<TARGET_FILE:benchmark_detach_stale_threads>
            --libpeak $<TARGET_FILE:peak>
            --active-threads 16
            --stale-threads 48
            --stale-calls 2048
            --active-after-stale
            --seconds 2
            --samples 6
            --stale-mode parked
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-stale-stress-trace
            --fail-on-transition-skips)
  set_tests_properties(benchmark_detach_stale_threads_stress
    PROPERTIES
      LABELS "benchmark;stress"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")

  add_test(NAME benchmark_detach_stale_threads_unrelated_spin_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_stale_thread_stress.py
            --exe $<TARGET_FILE:benchmark_detach_stale_threads>
            --libpeak $<TARGET_FILE:peak>
            --active-threads 16
            --stale-threads 48
            --stale-calls 2048
            --active-after-stale
            --seconds 2
            --samples 4
            --stale-mode unrelated-spin
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-stale-unrelated-spin-trace
            --fail-on-transition-skips)
  set_tests_properties(benchmark_detach_stale_threads_unrelated_spin_stress
    PROPERTIES
      LABELS "benchmark;stress"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")

  add_test(NAME benchmark_detach_stale_threads_reattach_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_stale_thread_stress.py
            --exe $<TARGET_FILE:benchmark_detach_stale_threads>
            --libpeak $<TARGET_FILE:peak>
            --active-threads 16
            --stale-threads 48
            --stale-calls 1
            --active-after-stale
            --seconds 3
            --stale-ready-timeout 20
            --samples 4
            --overhead-ratio ${PEAK_REATTACH_STRESS_RATIO}
            --require-reattach
            --stale-mode parked
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-stale-reattach-stress-trace
            --detach-backend signal
            --fail-on-transition-skips
            --max-reattach-classify-failed 0)
  set_tests_properties(benchmark_detach_stale_threads_reattach_stress
    PROPERTIES
      LABELS "benchmark;stress"
      ENVIRONMENT "${PEAK_REATTACH_STRESS_ENV}"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")
endif()
