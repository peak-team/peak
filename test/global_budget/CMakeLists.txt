# Standalone Linux/64-bit transport tests. The parent chooses whether to enable
# this experimental module's tests; no MPI library or cluster is needed.
function(peak_add_global_budget_test name source)
  add_executable(${name} ${source} ${PROJECT_SOURCE_DIR}/src/global_budget.c)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(${name} PRIVATE -UNDEBUG)
  target_link_libraries(${name} PRIVATE Threads::Threads ${M_LIBRARY})
endfunction()
peak_add_global_budget_test(test_global_budget_protocol test_global_budget.c)
add_test(NAME test_global_budget_two_clients COMMAND test_global_budget_protocol 2)
add_test(NAME test_global_budget_four_clients COMMAND test_global_budget_protocol 4)
peak_add_global_budget_test(test_global_budget_fragment test_global_budget.c)
target_compile_definitions(test_global_budget_fragment PRIVATE PEAK_GLOBAL_BUDGET_IO_CHUNK=13)
add_test(NAME test_global_budget_fragment COMMAND test_global_budget_fragment 4)
peak_add_global_budget_test(test_global_budget_adaptive test_adaptive.c)
add_test(NAME test_global_budget_adaptive COMMAND test_global_budget_adaptive)
# These white-box tests include the implementation once to inspect protocol
# rejection paths; do not link a second transport instance.
foreach(test IN ITEMS protocol_validation same_snapshot peer_shutdown)
  add_executable(test_global_budget_${test} test_${test}.c)
  target_include_directories(test_global_budget_${test} PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(test_global_budget_${test} PRIVATE -UNDEBUG)
  target_link_libraries(test_global_budget_${test} PRIVATE Threads::Threads ${M_LIBRARY})
  add_test(NAME test_global_budget_${test} COMMAND test_global_budget_${test})
endforeach()
peak_add_global_budget_test(test_global_budget_idle test_idle_cost.c)
set_tests_properties(test_global_budget_two_clients test_global_budget_four_clients
  test_global_budget_fragment test_global_budget_adaptive
  test_global_budget_protocol_validation test_global_budget_same_snapshot test_global_budget_peer_shutdown
  PROPERTIES TIMEOUT 15)
