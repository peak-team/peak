# Shared MPI budget regressions

The standalone tests cover three runtime contracts:

- `test_global_budget_ledger`: concurrent profile/control spending and returns
  conserve issued credit; a heartbeat/control-completion race releases escrow
  without permanently charging the same control operation twice.
- `test_global_budget_idle_return`: installing a hook starts a fresh idle epoch;
  a pending mutation and physical publication cannot race an operating-credit
  return. The unchanged 100 ms idle interval still permits surplus returns.
- `test_global_budget_native_broker`: an active broker cannot grant before the
  startup fee is committed, conserves one shared bank under competing rank
  requests, remains responsive while rank zero is paused, and
  confirms final cohort replies before its owner reaps it. Real TCP cohorts
  of 1 and 65 members cover singleton admission and the former rank limit;
  the last member must receive credit and a final reply. A calibrated preload
  probe proves broker isolation on both platforms; Linux also checks its maps.

With MPI profiling and the strict mutation backend available, CTest also runs
one 80-target application fixture in three runs: singleton and two-rank
recovery, plus early rank-zero finalization. They check standard initial
installation before MPI, a real 64-call initial observation with correct enter/leave
arithmetic, selective exhaustion of the hot hook while all 79 cold hooks remain
physically installed, and paid recovery followed by new calls. Rank zero may
finalize while another rank continues using the broker. All require consumed
profile credit, correct application arithmetic and final settlement. Runtime
batch-capacity safety remains covered by the existing controller tests; initial
hooks no longer require artificial paid reattachment batches.

These are functional regressions, not performance thresholds. The MPI launcher
and profiler/broker binaries come from the current CMake build. Temporary logs
and target lists remain outside the source tree.
