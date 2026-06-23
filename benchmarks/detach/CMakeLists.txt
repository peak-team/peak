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

  add_test(NAME benchmark_detach_hotloop_reattach_stress
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_detach_hotloop_stress.py
            --exe $<TARGET_FILE:benchmark_detach_hotloop>
            --libpeak $<TARGET_FILE:peak>
            --threads ${PEAK_STRICT_DETACH_STRESS_THREADS}
            --seconds 3
            --samples 6
            --overhead-ratio 0.01
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-hotloop-reattach-trace
            --require-reattach
            --fail-on-transition-skips)
  set_tests_properties(benchmark_detach_hotloop_reattach_stress
    PROPERTIES
      LABELS "benchmark;stress"
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
            --overhead-ratio 0.01
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-hotloop-signal-reattach-trace
            --require-reattach
            --detach-backend signal
            --fail-on-transition-skips
            --max-reattach-classify-failed 0)
  set_tests_properties(benchmark_detach_hotloop_signal_reattach_stress
    PROPERTIES
      LABELS "benchmark;stress"
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
            --overhead-ratio 0.01
            --require-reattach
            --stale-mode parked
            --trace-prefix ${PROJECT_BINARY_DIR}/benchmarks/detach/peak-stale-reattach-stress-trace
            --detach-backend signal
            --fail-on-transition-skips
            --max-reattach-classify-failed 0)
  set_tests_properties(benchmark_detach_stale_threads_reattach_stress
    PROPERTIES
      LABELS "benchmark;stress"
      RESOURCE_LOCK peak_detach_benchmark
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "stale_thread_stress_ok")
endif()
