# Shared MPI budget regressions

The standalone Linux tests cover three runtime contracts:

- `test_global_budget_ledger`: concurrent profile/control spending and returns
  conserve issued credit; a heartbeat/control-completion race releases escrow
  without permanently charging the same control operation twice.
- `test_global_budget_idle_return`: installing a hook starts a fresh idle epoch;
  a pending mutation and physical publication cannot race an operating-credit
  return. The unchanged 100 ms idle interval still permits surplus returns.
- `test_global_budget_native_broker`: an active broker cannot grant before the
  startup fee is committed, conserves one shared bank under competing rank
  requests, remains responsive while rank zero is paused, and
  confirms final cohort replies before its owner reaps it.

With MPI profiling and the strict mutation backend available, CTest also runs
one 80-target application fixture in two scenarios: paid installation through
multiple batches of at most 64 hooks, budget exhaustion and real restoration followed by
new calls; and rank zero finalizing while another rank continues to use the
broker. Both require correct application arithmetic and final settlement.

These are functional regressions, not performance thresholds. The MPI launcher
and profiler/helper binaries come from the current CMake build. Temporary logs
and target lists remain outside the source tree.
