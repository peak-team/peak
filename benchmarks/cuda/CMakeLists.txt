add_executable(benchmark_cuda_slot_release_scaling
  benchmark_slot_release_scaling.cpp
  ${PROJECT_SOURCE_DIR}/src/cuda_profiler_state.cpp)
target_include_directories(benchmark_cuda_slot_release_scaling PRIVATE
  ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(benchmark_cuda_slot_release_scaling PRIVATE
  Threads::Threads)

add_test(NAME benchmark_cuda_slot_release_scaling
  COMMAND benchmark_cuda_slot_release_scaling)
set_tests_properties(benchmark_cuda_slot_release_scaling PROPERTIES
  LABELS benchmark
  TIMEOUT 30
  PASS_REGULAR_EXPRESSION "cuda_slot_release_scaling_ok")
