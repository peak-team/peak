# Heartbeat Mechanism and Runtime Policy

This document is the user- and developer-facing guide to the current heartbeat
implementation. It explains runtime behavior, accounting dataflow, policy, and
tuning.

The [physical detach controller](physical-detach-controller.md) is the detailed
mutation-safety design. It owns helper/signal backend semantics, PC
classification, thread stopping, pthread creation gating, and
failure-after-mutation boundaries.

This page explains how the heartbeat thread uses those mechanisms today.

## Lifecycle

`peak_init()` parses the heartbeat policy environment along with the target
configuration. If PEAK has no requested work, initialization returns before
starting listeners or the heartbeat.

For target profiling work, initialization proceeds in this order:

1. Attach pthread bookkeeping.
2. Optionally warm up the detach backend when target hooks exist and MPI was
   not detected.
3. Attach MPI and CUDA support surfaces when enabled.
4. Allocate target listener bookkeeping and attach the general listener.
5. Attach syscall support and, if needed, the dynamic `dlopen` path.
6. Set `peak_main_time = peak_second()`.
7. Publish the runtime start and stop-window accounting baseline through
   `peak_general_listener_note_runtime_start()`.
8. If `PEAK_HEARTBEAT_INTERVAL` parsed to a nonzero value, allocate
   `heartbeat_overhead`, initialize `PeakHeartbeatArgs`, set
   `heartbeat_running = true`, and start `peak_heartbeat_monitor()`.

The runtime-start call is deliberately before heartbeat creation. It records the
elapsed-time denominator start and snapshots the current detach-controller
stop-window count and wall time. Heartbeat and reporting code subtract that
baseline from later control accounting.

Baseline validity is retained for the full run. If the bounded baseline read
cannot obtain a coherent tuple, later reads remain marked invalid even when the
current tuple is readable; reattach fails closed and final validation rejects
the arm. The zero baseline still charges any pre-runtime control cost
conservatively instead of hiding it.

`peak_fini_impl()` stops the heartbeat before freezing the final report:

1. If PEAK is running from the MPI-finalize path, suspend callbacks.
2. Set `heartbeat_running = false`, signal the heartbeat condition variable,
   and join the heartbeat thread.
3. Stop/join the general detach controller under its existing bounded drain and
   MPI-finalize policy.
4. Convert `peak_main_time` from a start timestamp to elapsed seconds.
5. Freeze the final report snapshot.
6. Free `heartbeat_overhead`, then continue support-hook and target teardown
   according to MPI/finalize safety.

The heartbeat cleanup path snapshots any remaining `heartbeat_overhead` before
the array can be freed, so final reporting can still include heartbeat profiling
cost after the heartbeat thread exits. The final report boundary includes every
control window completed before the controller join. Pending requests that the
existing MPI-finalize or timeout policy does not start have no stop-window cost;
later per-hook cleanup in `peak_general_listener_dettach()` is excluded.

## Dataflow

```mermaid
flowchart TD
    A["target callbacks"] --> B["per-hook counters and times"]
    B --> C["heartbeat sample under general listener lock"]
    C --> D["profile seconds and lifetime ratios"]
    C --> E["recent profile rate"]
    F["detach controller accounting sidecar"] --> G["baseline-adjusted control wall/count"]
    D --> H["per-target detach policy"]
    D --> I["attached lifetime sum"]
    E --> J["attached recent sum"]
    I --> K["global detach pressure = max(recent sum, lifetime sum)"]
    J --> K
    G --> L["reattach risk/headroom and diagnostics"]
    D --> L
    L --> M["reattach admission and pending leases"]
    H --> N["request detach"]
    K --> N
    M --> O["request reattach"]
    N --> P["controller state machine"]
    O --> P
    P --> Q["physical mutation via helper/signal backend"]
    Q --> F
    D --> R["final report snapshot"]
    G --> R
```

## Heartbeat Samples and Denominators

Each heartbeat iteration uses `total_execution_time = now - peak_main_time`,
clamped to a tiny positive value. The same application-start denominator also
feeds final reporting after `peak_fini_impl()` converts `peak_main_time` to
elapsed seconds.

For each hook, the heartbeat samples total callback counts from all listener
thread slots. The calibrated callback cost is `peak_general_overhead`, measured
by temporarily attaching a dummy function when calibration is needed. Hook
profile seconds are:

```text
total_calls[i] * peak_general_overhead + heartbeat_overhead[i]
```

Detached and reattach-requested hooks can reuse a cached sample instead of
scanning live callback slots every heartbeat. Reattaching hooks are not reused,
so the first attached sample after a physical reattach is seeded from live
slots.

The heartbeat stores these per-hook snapshots:

- `calls_snapshot[i]`: total observed calls.
- `profile_seconds_snapshot[i]`: cumulative profile seconds for the hook.
- `lifetime_snapshot[i]`: cumulative hook profile ratio,
  `profile_seconds_snapshot[i] / total_execution_time`.
- `rate_snapshot[i]`: recent profile rate over the last heartbeat interval.
  Cached detached samples report zero recent rate for that cycle.

Recent baselines are advanced every heartbeat. When a hook is unavailable, the
baseline is reset so the next valid sample does not create a fake burst from an
unrelated old timestamp.

Control stop-window accounting has a separate denominator path. The detach
controller publishes cumulative stop-window count and wall time. The general
listener obtains a bounded, lock-free coherent snapshot and subtracts the
heartbeat/runtime baseline:

```text
control_spent_seconds =
  (accounting.stop_window_wall_ns - baseline_stop_window_wall_ns) / 1e9
stop_window_count =
  accounting.completed_stop_window_count - baseline_completed_stop_window_count
```

The raw spent ratio used by reporting and the reattach request context is:

```text
spent_ratio =
  (profile_spent_seconds + control_spent_seconds) / total_execution_time
```

Heartbeat management CPU time is tracked separately with
`CLOCK_THREAD_CPUTIME_ID` checkpoints around active policy work. It is reported
as diagnostics and is not added to detach pressure or reattach admission.

## Detach Signals

The detach side is profile-only. Completed stop-window wall time and heartbeat
management CPU do not create detach pressure.

Per-target detach uses cumulative profile lifetime:

```text
lifetime_ratio[i] > target_profile_ratio
```

The current code also requires at least two observed calls
(`PEAK_GLOBAL_DETACH_MIN_CALLS == 2`) before heartbeat detach requests are
created. This keeps one-call cold targets from being detached only because a
single expensive call crossed a ratio.

Global detach uses attached profile pressure:

```text
attached_recent_sum = sum recent_rate[i] for attached hooks
attached_lifetime_sum = sum lifetime_ratio[i] for attached hooks
profile_global_overhead = max(attached_recent_sum, attached_lifetime_sum)
```

If `profile_global_overhead` exceeds
`effective_rank_budget * peak_global_detach_factor`, attached candidates are
ranked by `max(recent_rate, lifetime_ratio)`, then by recent rate and index.
After each accepted detach request, the heartbeat debits the local projected
recent and lifetime sums and stops once the projected global overhead is at or
below `effective_rank_budget`.

The request context records the call count, lifetime ratio, global profile
pressure, elapsed time, and recent rate that caused the request. It does not
perform the mutation. The controller owns mutation ordering.

## MPI Job Budget and Callback Burst Protection

For a verified launcher world size `W`, the configured
`PEAK_GLOBAL_OVERHEAD_RATIO` is treated as a job risk budget. The effective
rank budget is `PEAK_GLOBAL_OVERHEAD_RATIO / W`. Global heartbeat detach,
reattach admission/headroom, and adaptive heartbeat error all use that same
rank budget. The per-target ratio remains a separate hook policy.

World metadata is frozen before callbacks start, using the same validated
rank/size resolver as reporting: complete PMI, PMIX, OMPI, MV2 and Intel MPI
pairs must agree; Slurm is the fallback only when no complete MPI pair exists.
Missing, malformed or contradictory metadata selects world size one with
source `unverified`. This does not independently prove MPI topology. A verified
single rank preserves the existing policy and has no burst guard.

When `W > 1`, heartbeat is enabled, and global heartbeat policy is enabled,
both generic and direct callbacks additionally check short CPU blocks. Every
hook/thread owns 24 bytes in the unused tail of its existing statistics cache
line, so hooks do not collide in a bounded cache and calls do not add a shared
atomic counter or lock. Every 64th invocation reads its lifecycle generation
and `CLOCK_THREAD_CPUTIME_ID`. The first block seeds a baseline; subsequent
blocks compare:

```text
64 * calibrated_wall_cost_per_call >
    effective_rank_budget * PEAK_GLOBAL_DETACH_FACTOR * block_thread_cpu_seconds
```

Each sample replaces the previous baseline. A long warmup cannot leave credit
for the next short block; MPI blocking and scheduler waiting do not create CPU
credit. Useful CPU computation between calls increases the block denominator,
so a function called infrequently around long computations remains attached.
The denominator includes all work done by this thread between that hook's
samples, including other hooks. It is not an aggregate measurement of every
thread's CPU time. The heartbeat continues to provide the cross-hook/rank-local
aggregate check.

The numerator is a calibrated **wall** callback cost. This is an estimated
callback-cost/active-thread-CPU risk signal, not a measurement of actual CPU
cost or MPI application slowdown. Calibration preemption can conservatively
increase the estimate. CPU clock failure or a non-increasing reading reseeds
without requesting detach; the normal heartbeat remains the fallback.

A burst request uses a dedicated bit in the existing packed lifecycle word.
One compare-and-swap publishes only against the sampled ATTACHED generation,
then wakes the controller. The callback does not patch code, scan global slots,
or call MPI. The controller treats this recent active-thread pressure alert as a reason for
conservative **rank-wide** budget protection and immediately requests detach for every currently attached target
on that rank, including targets with zero recorded calls. It does not wait to
discover whether the remaining targets become hot. This deliberately prioritizes
job overhead over function coverage: cold targets can be detached without ever
being profiled.

Before requesting this rank-wide wave, the controller cancels pending reattach
requests back to DETACHED and clears their retry/request context and future
lease. It performs these transitions under the existing controller lock, which
also excludes concurrent physical reattachment. Reattach admission, including
the API path, stays closed while a wave member is DETACH_REQUESTED or DETACHING.
The gate expires after the last member completes or abandons its retry budget;
existing spent/control headroom and cooldown then govern restoration. The wave
does not permanently disable profiling. A short-block pressure alert does not
prove that cumulative spent cost has exhausted the budget; historical headroom
may still be available.

Reattach also revalidates the control accounting tuple after acquiring the
controller lock. Completed-window count, failed-window count and wall time must
still exactly match the valid, unsaturated snapshot charged to headroom. A
mutation between the earlier budget reads and lock acquisition invalidates the
whole admission cycle, which resumes on a later heartbeat with a fresh budget.
This prevents stale pre-wave headroom from restoring hooks even when cooldown is
zero. It leaves unchanged-accounting admission and physical batch mutation
unchanged. The recorded second-round HPC rank-wide measurements precede this
additional admission check; its changed/invalid-tuple behavior is covered by
local deterministic tests.

All wave members carry `mpi-burst` as their request source and enter the existing
physical detach batch collector together. With strict batch support, up to 64
eligible targets share one prepare/finish stop window. Larger sets use multiple
batches; targets rejected by safety classification may require retry windows.
Backends without strict batch support retain their existing individual-target
mutation path, so this policy cannot promise a single stop window there. This
reduces the previous cost of discovering and stopping one hot function per
window, which can itself accumulate along an MPI critical path even after
callback overhead is reduced.

Generation changes reseed the CPU window; thread retirement clears it before
the statistics slot can be reused. An independent 64-call block therefore costs
at most two blocks before its first pressure decision, followed by controller
scheduling and safe mutation delay. This protection bypasses the heartbeat's
initial observation-time gate.

`PEAK_HEARTBEAT_INTERVAL=0` or `PEAK_ENABLE_GLOBAL_HEARTBEAT=0` disables the
burst guard. A configured zero global ratio retains zero tolerance for positive
estimated cost; it does not disable policy. Final text output identifies
`configured_job_target`, `effective_rank_target`, `world_size`, metadata
`source`, `burst_guard`, `stride`, `burst_action=rank-wide`, and batch capacity.
`burst_waves` and `burst_unsampled_targets` count waves and zero-call targets
observed by the reporting rank only (`burst_counters_scope=reporting-rank`),
including when the text report otherwise aggregates MPI ranks. They are not
job-wide counts or values taken from an independently selected overhead owner.
This follows existing text-output
selection, including aggregate-root output and `PEAK_TEXT_OUTPUT` overrides.

The sum of per-rank estimates is a more conservative MPI risk model than the
maximum rank's cumulative ratio: different ranks may incur overhead on
successive parts of the application's critical path. Budget division and short
blocks reduce that failure mode without background MPI communication. They
cannot guarantee a hard application slowdown bound. Calibration error,
communication/scheduling effects, the first 128 calls of a generation, safe
mutation latency, classification retries, and multiple batches for large target
sets remain outside such a proof. An application with a very short critical phase can still finish that
phase before physical detach completes.

## Reattach Risk Scaling

Node-local rank scaling is an additional conservative policy input only for
reattach admission. It remains in place after world-budget division to account
for node contention: control cost itself is not multiplied by world size. The same
scaled value is also emitted as a reporting diagnostic; it does not affect
detach decisions or hook state.

At heartbeat thread startup, the code reads a local MPI rank count from the
first positive integer in:

```text
MPI_LOCALNRANKS
OMPI_COMM_WORLD_LOCAL_SIZE
MV2_COMM_WORLD_LOCAL_SIZE
PMI_LOCAL_SIZE
```

If none of these are valid, the fallback is exactly `1`. That fallback is not
conservative when more than one MPI rank actually shares a node. PEAK does not
independently verify launcher topology, so the scaled estimate is conservative
only when this metadata accurately reports the local rank count.

Reattach computes:

```text
raw_control_spent_seconds = completed local stop-window wall since baseline
reattach_control_risk_seconds =
  raw_control_spent_seconds * local_mpi_ranks
reattach_spent_seconds =
  profile_spent_seconds + reattach_control_risk_seconds
reattach_risk_spent_ratio =
  reattach_spent_seconds / total_execution_time
effective_rank_budget = global_target_ratio / verified_world_size
reattach_gate_ratio =
  peak_global_reattach_factor * effective_rank_budget
```

A reattach wave starts only when `reattach_risk_spent_ratio <=
reattach_gate_ratio`. The local-rank multiplier does not feed per-target detach,
global detach, adaptive sleep, hook state, cooldown, controller state, or the
request context. With valid local-rank metadata, it is a conservative same-node
admission estimate for reattach.

If the controller accounting snapshot cannot be proven valid, reattach admission
rejects the wave for that heartbeat. A failed current read renders a finite zero
fallback; invalid baseline provenance may render the current tuple
conservatively. Both cases mark `snapshot_valid=0`, and neither renders sentinel
values as measurements.

The raw report and the risk report are intentionally separate. Raw control wall
is measured process-local wall time. With valid local-rank metadata, risk
control is a same-node exposure estimate. Neither is a synchronized MPI-wide
runtime overhead proof.

## Reattach Cooldown, Admission, and Reservations

`PEAK_ENABLE_REATTACH` defaults to enabled when unset, but reattach only runs
when the heartbeat is enabled, `PEAK_HIBERNATION_CYCLE` is nonzero, and either
per-target or global heartbeat policy is enabled.

The default reattach cooldown in the current implementation is 60 seconds:

```c
static const unsigned int peak_reattach_default_cooldown_ms = 60000;
```

`peak_general_listener_init_reattach_policy_once()` reads the cooldown before
the first eligibility check. `PEAK_REATTACH_COOLDOWN_MS=0` disables the
cooldown and is used by several focused tests.

A reattach candidate must still be:

- published;
- physically/logically detached;
- not marked `peak_need_detach`;
- in `PEAK_HOOK_DETACHED`;
- past cooldown;
- eligible under per-target policy, global policy, or both.

Per-target reattach eligibility requires the current projected profile ratio to
be at or below `target_profile_ratio`. Global reattach eligibility is present
when global heartbeat policy is enabled.

The projected profile seconds for a detached hook are conservative:

```text
projected_seconds[i] =
  max(saved_detach_profile_seconds[i],
      current_observed_profile_seconds[i])
projected_ratio[i] =
  projected_seconds[i] / total_execution_time
historical_rate[i] =
  projected_seconds[i] / total_execution_time
```

`saved_detach_profile_seconds` is recorded after successful detach from the
request context (`request_ratio * request_total_time`) or from a current/fallback
sample. This preserves the historical cost that justified detach, while still
allowing the ratio to decay as elapsed time grows.

Admission is expressed in seconds:

```text
headroom_seconds =
  effective_rank_budget * total_execution_time
  - profile_spent_seconds
  - local_mpi_ranks * raw_control_spent_seconds
  - pending_reattach_future_leases
  - local_mpi_ranks * predicted_pending_batch_stop_seconds
```

Pending reattach reservations are charged before adding new candidates. For a
pending reattach, the code uses the pending request rate when available;
otherwise it falls back to the current historical rate. The future lease uses
the current observation horizon:

```text
lease_seconds = request_rate * (hb_max_us / 1e6)
```

Stop-window prediction uses only
`peak_detach_controller_last_stop_window_us()`, scaled by local ranks for
reattach risk. Completed count/wall averages are not used as a predictive
policy input. Cumulative completed stop-window wall remains spent through the
control accounting sidecar.

The heartbeat holds the general listener lock while it snapshots pending
state, gathers detached candidates, subtracts pending reservations, sorts
eligible candidates, revalidates each candidate, and enqueues accepted reattach
requests. This prevents API/callback pending requests from crossing into the
same batch boundary without being charged.

Batch accounting uses `PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES == 64`.
The heartbeat estimates when a newly admitted request would create an
additional controller batch window and adds only that incremental predicted stop
cost to the candidate.

## Controller Boundary

The heartbeat never calls Gum lifecycle APIs. It only calls the request helpers
under the general listener lock:

- `peak_general_listener_request_detach_with_context_unlocked()`
- `peak_general_listener_request_reattach_with_context_unlocked()`

Those helpers only accept transitions that match the current state:

```text
ATTACHED -> DETACH_REQUESTED
DETACHED -> REATTACH_REQUESTED
```

Already-requested or in-progress same-direction transitions coalesce and return
success. Other states reject the request. Reattach also rejects a hook whose Gum
detach did not flush.

The controller thread is the mutation boundary. In strict-batch mode it:

1. Collects pending detach/reattach candidates.
2. Builds `PeakDetachRequest` records.
3. Calls `peak_detach_controller_prepare_hook_mutation_batch()`.
4. Moves prepared hooks to `DETACHING` or `REATTACHING`.
5. Performs only the Gum work needed for non-physical fallback paths.
6. Calls `peak_detach_controller_finish_hook_mutation_batch()`.
7. Publishes stable states and transition coverage markers.
8. Records traces and resets retry/pending context.

Retryable prepare failures keep the transition pending with exponential
backoff. The current constants are 1 ms base delay, 50 ms maximum delay, 300
maximum retries, and 30 seconds maximum pending age. Exceeding either retry
budget abandons only the unproven transition: failed detach leaves the hook
attached, and failed reattach leaves it detached.

During MPI finalization, `peak_general_listener_note_mpi_finalize_requested()`
prevents new detach/reattach requests and the controller processing path returns
without doing new work.

For detailed backend safety behavior, see
[Physical detach controller](physical-detach-controller.md).

## Stop-Window Accounting Sidecar

The detach controller maintains a sidecar:

```c
typedef struct {
    uint64_t completed_stop_window_count;
    uint64_t failed_stop_window_count;
    uint64_t stop_window_wall_ns;
} PeakDetachAccountingSnapshot;
```

The control-window interval starts immediately before the selected backend
issues STOP. A completed window ends after PEAK has proven the stopped threads
were released or cleanup completed. For helper-backed physical mutation, this
covers helper STOP, classification/evacuation, mutation, and release/cleanup;
signal and fallback paths cover the equivalent local protocol. This is a
controller disruption wall-time upper bound. It is not an exact simultaneous
all-thread pause duration, and it must not be interpreted as an MPI-wide
synchronized pause.

At control-window start, the controller stores a monotonic timestamp. At a
proven release/cleanup finish, it converts elapsed wall time to nanoseconds,
adds it to `stop_window_wall_ns`, increments
`completed_stop_window_count`, and publishes `last_stop_window_us` for the
batch-cost predictor. Completion describes the STOP-to-release control window,
not the hook mutation result: a Gum attach/detach failure after safe release is
still a completed window and remains a valid cost sample. If STOP,
classification, evacuation, or release reaches a
reportable failure, the elapsed time is still added to
`stop_window_wall_ns` and `failed_stop_window_count` is incremented. A failed
window does not overwrite the last successful predictor sample. Unrecoverable
failures that cannot prove thread release still terminate the process through
the existing fail-closed path. These counters are accounting-only and do not
change retry or cleanup behavior.

`peak_detach_controller_accounting_snapshot()` is lock-free and read-only. It
does not allocate, log, call Gum, or take the mutation guard. This makes it safe
for heartbeat and reporting to sample completed control cost without
participating in the mutation lifecycle.

The count and wall-time fields use a bounded sequence snapshot. If repeated
writer contention prevents a coherent read, the snapshot API returns `FALSE`
without modifying the caller's output. General-listener callers initialize the
output to a finite zero fallback, while reattach uses the validity result and
fails closed for that heartbeat.

The trace setting is independent. `PEAK_DETACH_TRACE_PATH` is cached by the
general listener and passed to
`peak_detach_controller_configure_trace_diagnostics()`, but the count/wall
sidecar and `last_stop_window_us` are not gated by trace diagnostics.

No-trace coverage exists in the fake-helper controller test: it unsets
`PEAK_DETACH_TRACE_PATH`, verifies `last_stop_window_us` is zero immediately
after prepare, finishes detach and reattach, and then checks that the completed
stop-window count/wall and last-window value were published. The static
lifecycle check also asserts that stop-window accounting is not trace-gated.

## Adaptive Sleep

The heartbeat sleeps once before the first sample using:

```text
clip(heartbeat_time, hb_min_us, hb_max_us)
```

After each policy cycle, sleep is adaptive. The signal is the same
profile-only `profile_global_overhead` used by global detach, not reattach
risk:

```text
global_rate =
  (profile_global_overhead - previous_profile_global_overhead) / dt
ema_global_rate =
  hb_ema_a * global_rate + (1 - hb_ema_a) * previous_ema
err =
  max(profile_global_overhead / effective_rank_budget - 1, 0)
scale =
  1 / (1 + hb_k_err * err + hb_k_rate * ema_global_rate)
sleep_us =
  clip(heartbeat_time * scale, hb_min_us, hb_max_us)
```

Timed sleep is excluded from heartbeat-management CPU accounting. The wait uses
a condition variable so shutdown can wake the heartbeat promptly.

## Reporting

Reporting uses `peak_general_listener_freeze_final_report_snapshot()` after the
heartbeat and controller threads have joined. That snapshot contains:

- baseline-adjusted stop-window count;
- baseline-adjusted failed control windows;
- accounting snapshot validity;
- elapsed seconds;
- profile seconds;
- raw control seconds;
- heartbeat management CPU seconds;
- profile ratio;
- control ratio;
- raw profile+control ratio;
- local-rank-scaled profile+control risk ratio;
- local-rank-scaled control risk ratio;
- management ratio.

Single-process and rank-local output print the `local` stop-window label.
Aggregate MPI/socket output prints per-rank maximum ratios where applicable and
uses `rank-0/local` for the root rank's local stop-window count and wall. The
stop-window report uses the same baseline-adjusted values:

```text
[peak] local control stop-window overhead: windows=N wall_seconds=S ratio=R
[peak] rank-0/local control stop-window overhead: windows=N wall_seconds=S ratio=R
```

Failed-window diagnostics are separate so the completed-window line stays
parsable:

```text
[peak] local failed control windows: windows=N snapshot_valid=0|1
[peak] aggregate failed control windows: windows=N snapshot_valid=0|1
```

MPI and socket aggregation sum the failed-window count and require a valid
final snapshot from every rank. The completed-window wall metric remains
explicitly rank-0/local because heartbeat accounting itself is not MPI
synchronized.

Transition coverage is also reported:

```text
detached_targets=N reattached_targets=N revisited_targets=N
```

`revisited_targets` marks hooks that were detached, successfully reattached,
and then accumulated additional profiled calls after the reattach request
baseline.

CSV output remains per-function profile data. The text report is where the
profile/control/risk/management split is currently visible.

## Environment Variables and Defaults

The defaults below describe the current implementation.

| Variable | Default | Effect |
| --- | ---: | --- |
| `PEAK_HEARTBEAT_INTERVAL` | `0.1` seconds | Base heartbeat interval. Parsed to microseconds. `0` disables the heartbeat thread. |
| `PEAK_HIBERNATION_CYCLE` | `50` | Reattach cadence: reattach runs every N heartbeat cycles when nonzero. |
| `PEAK_OVERHEAD_RATIO` | `0.1` | Per-target lifetime profile ratio threshold. |
| `PEAK_GLOBAL_OVERHEAD_RATIO` | `0.1` | Global profile target and reattach headroom target. |
| `PEAK_GLOBAL_DETACH_FACTOR` | `1.2` | Global detach hysteresis multiplier. |
| `PEAK_GLOBAL_REATTACH_FACTOR` | `0.85` | Reattach spent-risk gate multiplier. |
| `PEAK_ENABLE_PER_TARGET_HEARTBEAT` | `false` | Enables per-target heartbeat detach and per-target reattach eligibility. |
| `PEAK_ENABLE_GLOBAL_HEARTBEAT` | `true` | Enables global heartbeat detach and global reattach eligibility. Set `0` or `false` to disable. |
| `PEAK_ENABLE_REATTACH` | enabled when unset | Enables heartbeat reattach when other cadence/policy gates are satisfied. Set false/0 to disable. |
| `PEAK_REATTACH_COOLDOWN_MS` | `60000` | Minimum time after successful detach before heartbeat may reattach that hook. `0` disables cooldown. |
| `PEAK_HB_MIN_US` | `10000` | Lower bound for adaptive heartbeat sleep. |
| `PEAK_HB_MAX_US` | `500000` | Upper bound for adaptive heartbeat sleep and the reattach future-lease horizon. If less than min, it is raised to min. |
| `PEAK_HB_K_ERR` | `3.0` | Adaptive-sleep error gain. |
| `PEAK_HB_K_RATE` | `0.8` | Gain on the EMA of global profile-pressure growth. |
| `PEAK_HB_EMA_A` | `0.3` | Adaptive-sleep EMA weight. Invalid values outside `(0, 1]` reset to `0.3`. |
| `PEAK_MAX_NUM_THREADS` | `2 * online CPUs` | Size of per-listener thread slots used for callback counters. |
| `PEAK_COST` | `0` | Enables count-based detach using calibrated callback cost when positive. |
| `PEAK_DETACH_COUNT` | unset | Overrides count-based detach threshold when set to a positive integer. |
| `PEAK_CONTROLLER_MAX_PENDING_AGE_MS` | `30000` | Maximum pending age before abandoning a retrying transition. `0` disables age limit. |
| `PEAK_CONTROLLER_MAX_RETRY_COUNT` | `300` | Maximum retry count before abandoning a retrying transition. `0` disables count limit. |
| `PEAK_DETACH_TRACE_PATH` | unset | Enables transition trace rows. Accounting still runs when unset. |
| `PEAK_TEXT_OUTPUT` | auto | Truthy values force text output. Without it, rank-local MPI-shaped output may suppress duplicate text. |
| `PEAK_OUTPUT_AGGREGATION` | `mpi` under MPI | Selects `mpi`, `socket`, or local/rank-local aggregation. |
| `PEAK_MPI_COLLECTIVE_OUTPUT` | unset | Legacy alias used when `PEAK_OUTPUT_AGGREGATION` is unset. |
| `PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS` | `10000` | Timeout for MPI aggregation's proof-first all-rank finalize participation check. |
| `PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS` | `5000` | Timeout for MPI output reducer collectives. |
| `PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS` | `60000` | Socket no-progress timeout and root-release phase timeout. The absolute gather cap adds `5000` ms per 128-peer wave, up to `300000` ms adaptive margin; explicit values are never shortened. |
| `PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS` | `gather hard cap + 2 × socket phase timeout` | Peer-side end-to-end budget spanning gather, report publication, and confirmed release; smaller values are raised. |
| `PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS` | `180000` | Baseline post-publication gate timeout. A path that attempted socket publication automatically raises the effective timeout to at least the peer release budget plus two socket-phase margins; MPI and rank-local paths retain this baseline. |
| `MPI_LOCALNRANKS`, `OMPI_COMM_WORLD_LOCAL_SIZE`, `MV2_COMM_WORLD_LOCAL_SIZE`, `PMI_LOCAL_SIZE` | `1` fallback | Local rank count for reattach admission and diagnostic risk reporting. |

PEAK numeric controls above require complete, finite input. A present invalid
PEAK value emits one warning and uses its documented fallback.
`PEAK_MAX_NUM_THREADS=0` uses one slot and values above `4096` clamp to
`4096`; an invalid `PEAK_MEMLOG_CHUNK_EVENTS` disables memory logging. The
launcher-provided `MPI_LOCALNRANKS`, `OMPI_COMM_WORLD_LOCAL_SIZE`,
`MV2_COMM_WORLD_LOCAL_SIZE`, and `PMI_LOCAL_SIZE` values are metadata rather
than PEAK controls; missing or invalid values use the rank-count fallback
without a configuration warning. Sleep bounds must be positive; when the
resolved maximum is below the accepted minimum, the maximum falls back to the
minimum. The global detach multiplier is at least `1`, the reattach multiplier
is in `(0, 1]`, and the reattach multiplier must remain below the detach
multiplier; a conflict falls back to the reattach default.
The cooldown default is policy only: `PEAK_REATTACH_COOLDOWN_MS=60000` is not a
new safety precondition.

Several backend safety variables, such as `PEAK_SAFE_DETACH_MODE`,
`PEAK_DETACH_BACKEND`, `PEAK_DETACH_HELPER`, signal reservation controls, and
strict pthread-gate timeout controls, are documented in the
[physical detach controller guide](physical-detach-controller.md).

## MPI Limitations

The heartbeat thread performs no MPI synchronization. During the run it uses
only local process counters, local stop-window accounting, and local-rank count
metadata from environment variables. The node-local risk ratio is therefore an
admission heuristic for same-node exposure, not a proof of MPI-wide
application-observed overhead.

MPI enters the picture during teardown/reporting:

- `peak.c` requires an all-rank finalize-participation proof before using MPI
  collectives for MPI-aggregated output on the finalize path.
- MPI reducer calls use bounded nonblocking collectives. If a payload reduction
  fails after the proof, PEAK first attempts its socket fallback without further
  MPI calls when that fallback is enabled, then writes rank-local output if the
  socket path is unavailable or fails.
- Explicit socket aggregation and rank-local output publish their frozen CPU
  report before MPI teardown coordination. The strict socket path uses
  consistent launcher rank/size pairs and makes no MPI call until publication
  and socket release are finished. Finalize participation then joins report
  completion and finalizer policy in one long release gate. With explicit
  `PEAK_MPI_FINALIZE_POLICY=defer`, socket output instead uses a process-exit
  path that does not require an MPI-finalize proof.
- On the default pre-finalize path, MPI, socket, and rank-local writers all
  enter a bounded post-publication gate. Its reduction distinguishes output
  failure from protocol failure. Ranks that complete the same gate observe one
  reduced real-finalizer policy, but a local collective error or timeout cannot
  prove that every peer observed the same failure; that path is locally
  fail-closed rather than distributed failure consensus.
- Healthy runtimes other than Intel MPI 2019 normally return to the real
  `PMPI_Finalize()` after the release gate. Intel MPI 2019 skips its observed
  crash-prone finalizer by default; `PEAK_MPI_REAL_FINALIZE=1` explicitly opts
  back in. Unless `PEAK_MPI_REAL_FINALIZE=0`, `defer` calls the real finalizer
  before this compatibility policy and therefore bypasses it. Skipping the
  finalizer is non-conforming and still requires launcher-scale validation.
- An abnormal exit uses rank-local output without making teardown MPI calls.
  Missing all-rank participation rejects MPI aggregation. A local/socket report
  already published before a failed or timed-out combined gate is retained;
  PEAK then fails the MPI finalizer path closed without issuing later MPI
  calls or starting another output fallback.

PEAK does not call full teardown from a termination-signal handler. `SIGKILL`
cannot be caught, and `peak_fini()` uses locks, threads, allocators, I/O, and
optional MPI operations that are not async-signal-safe. CSV publication instead
uses a same-directory temporary file plus atomic rename: after abrupt process
termination the final pathname is absent or complete, while an interrupted
pre-rename writer may leave a temporary file. This is a process-level atomicity
guarantee, not a guarantee of an exact final snapshot under arbitrary kill,
launcher abort, node loss, or power loss.

## Testing and Diagnostics

Useful local checks for this mechanism include:

- `test_detach_lifecycle_invariants`: static checks for heartbeat accounting,
  reattach admission shape, trace caching, and controller boundaries.
- `test_detach_controller_fake_helper_trace_disabled_stop_window`: covers
  stop-window count/wall/last-window publication without
  `PEAK_DETACH_TRACE_PATH` when the patched Gum PC API is available.
- `test_detach_hotloop_batch_reattach_strict`,
  `test_detach_hotloop_reattach_cooldown_strict`, and
  `test_detach_hotloop_observed_overhead_reattach_guard_strict`: focused
  runtime checks for strict heartbeat reattach policy.
- `test_detach_milc_like_reattach_redetach_strict` and related MILC-like
  harnesses: checks for detach, reattach, redetach, and final-report parsing.
  This named test permits zero revisited targets and is mechanism coverage
  rather than a full acceptance test.

Trace rows, when enabled, are diagnostics. They append request provenance,
retry state, stop-window details, batch ids, and accounting fields. They do not
control whether stop-window accounting exists.

## Experimental cross-rank runtime budget prototype

`PEAK_MPI_GLOBAL_BUDGET=1` enables a prototype runtime coordinator while retaining
instrumentation and the existing safe detach controller. The default is off.
This first implementation requires a homogeneous C-MPI SPMD launch: **every
MPI_COMM_WORLD rank must preload the same build and enable the same option**.
The outer MPI initialization wrapper performs one additional endpoint broadcast
on the application's initialization thread. Partial-rank opt-in is unsupported
and can block in that collective; it is not a timeout-safe discovery protocol.
The wrapper preserves the application's requested/provided MPI thread levels.
After bootstrap, background coordination uses TCP only, including under FUNNELED
or SINGLE; heartbeat/controller threads do not invoke MPI.

Heartbeat publishes cumulative estimated profile cost, measured control-window
wall, management CPU and attached pressure after releasing the listener lock.
The coordinator retains spent cost and tracks issued credits. Fresh positive
credits fund callback blocks and admission; stale views return to the existing
local policy. Each rank additionally has at most 500 microseconds of estimated
unfunded callback grace for the entire session, shared by all hooks and threads.
It is not renewed by heartbeat, grants or detach/reattach. Exhaustion requests
the existing local emergency mutation; the report exposes unfunded cost and
emergency fallback counts. `grace_used_seconds` is the authorized portion and
cannot exceed 500 microseconds; `unfunded_seconds` also includes callback blocks
already in flight while emergency closing completes, which are covered by the
separate closing-cost estimate and can exceed that grace. The callback's ordinary
cumulative profile estimate includes both portions, so neither is charged twice.
In-flight mutation and coordination costs remain
estimates, so this prototype must be evaluated with application timings.

Root STOP commands carry a common wave identifier. Each rank's controller fans
out the command using its existing safe batch protocol; completed waves are
acknowledged. The root GO-to-all-DONE duration is a coordination diagnostic.
The prototype conservatively retains the sum of local control costs and does
not subtract them based on an unproven overlap assumption. Epoch/critical-path
communication models and replacement of an application's Barrier are not part
of this prototype.

Startup waits at most one second for a fresh global view, then degrades to local
control if unavailable. Shutdown drains local requests and retires the local application participant.
The helper remains active until the existing all-rank teardown proof completes;
final cohort settlement then waits at most two seconds before closing the
service. Ordinary nonfinal flushes remain bounded to 100 milliseconds. Reports expose startup readiness/time, whether the
view was ever active, last global costs/credit/wave state and shutdown
confirmation/time. A missing confirmation is not treated as a successful global
wave. Startup, coordination and shutdown timings are part of experimental
validation, not hidden outside its reported timing boundary.

The root's bootstrap/readiness span is charged once as an initial coordinator
cost; peer bootstrap durations are diagnostics and are not summed as independent
application delays. This can leave the initial pool in debt until elapsed time
generates sufficient credit. Invalid runtime accounting closes coordination
rather than publishing a zero-cost tuple; invalid/degraded counters and missing
shutdown acknowledgment remain visible in the final report.

For wire compatibility the sample field `unfunded_seconds` currently carries
the bounded authorized grace usage (`grace_used_seconds`), not the larger
diagnostic total of all unfunded callback charges.

The next experimental revision requests refill before the local credit reaches
zero. It uses observed consumption and refill latency, and root can issue STOP
before the funded tail expires. The callback still has only its bounded,
session-wide 500 microsecond unfunded grace; no network delay authorizes free
instrumentation. The original application's MPI_Init wall duration, measured on
root before PEAK activation, is included in earned runtime. Constructor time and
bootstrap cost do not create additional credit.

A rank joins coordination by consuming credit or requesting observation, not by
merely having cold hooks installed. A STOP view explicitly identifies each wave
participant. Cold ranks acknowledge without physically removing their hooks;
participants use the existing safe rank-wide batch. Actual closing costs remain
summed. The reserve grows with observed member closing costs, and an idle rank
returns unused credit once after safe detachment. Recovery admission uses the
local atomic credit balance, not the delayed root reservation diagnostic.

`PEAK_MPI_GLOBAL_TRACE=1` enables bounded controller/service event diagnostics.
Controller kinds 1/2 identify GO observation/DONE, 3 a local emergency consumed
by the controller, and 4/5 mutation batch preparation/completion. The fixed ring
reports overflow; no callback prints or allocates. Cross-node events must be
ordered by wave/request/decision causality, not subtraction of unsynchronized
monotonic clocks. `PEAK_MPI_TIMING_TRACE=1` separately reports original MPI init,
PEAK activation/bootstrap, PEAK finalization and original MPI finalize. Internal
finalization stages expose heartbeat join, controller stop, support cleanup,
report publication and release. These optional diagnostics are for causal
validation and are not overhead guarantees.

Controller event kind 6 records a successful hook restoration, including its
hook identifier, the already completed wave, and a fresh cumulative call
baseline read after physical restoration. The final exact call count for that hook must exceed the baseline to
prove that the restored hook was visited. Reattachment is admitted only after
root has acknowledged all members' completion of the current wave, and its
mutation reserve is charged before admission. The service also accepts the
legacy trace switch `PEAK_GLOBAL_BUDGET_TRACE`.

In global mode, execution launched from the calling thread of the original MPI
initializer/finalizer is tagged as a one-shot runtime utility child. Such a child
skips PEAK activation, avoiding inherited-preload profiler setup and reports in
MPI's internal utility programs. The parent environment is not modified. Normal
application exec/spawn and default-off operation retain their existing profiling
behavior. This scope follows the original MPI call context, not executable names;
MPI helper-thread ancestry is not inferred without evidence.

Recovery requests now fund both mutation and a useful observation interval. The
operating lease is the maximum of the most recent control-window cost, one
callback charge block, and the retained consumption rate multiplied by the
larger of the observation horizon and twice the observed refill latency. A full
mutation-plus-operating balance is required before restoration; partial grants
wait. Mutation is debited first and subsequent callbacks debit actual profile
cost, so the operating lease is reserved credit rather than a second profile
charge. This amortizes a paid restoration instead of repeatedly restoring with
only one callback block left. The root's separate closing reserve and the
session-wide unfunded grace are unchanged by this admission rule.

During MPI finalization a quiescent rank retires after callback suspension and
controller shutdown. Its TCP helper remains alive while the existing all-rank
MPI participation/report-release proof waits for ranks still doing application
work. Retired helpers charge their own CPU, release unused credit and acknowledge
future waves without new physical mutation. After that existing proof, support
wrappers are restored, a new final cost revision is published, and bounded
settlement stops the helper. The final `MPI global settlement:` row is distinct
from the earlier report's `MPI global budget:` snapshot and is authoritative for
shutdown confirmation. Failed or unproven participation remains unconfirmed.
The final revision also charges measured finalizer-thread CPU for PEAK report,
release and support cleanup; waiting wall time for peer application work and the
original MPI_Finalize call are excluded. A failed CPU clock is diagnosed rather
than represented as a successful measurement.

When post-init activation is selected, root's measured PEAK activation duration
is also charged once with bootstrap startup cost. Only the original MPI_Init
call earns the initial runtime offset; PEAK activation and bootstrap do not.
The optional init timing line keeps original, activation and bootstrap durations
separate for attribution.

Retired finalizer-thread CPU is sampled live through its POSIX thread CPU clock,
starting at PEAK finalization entry. The helper uses the maximum of the published
management total and the frozen heartbeat baseline plus that CPU delta. The
last counter update and live watch therefore settle the same cost once. FINAL
is distinct from retirement: only after the existing all-rank proof does each
rank publish a new FINAL revision. Closing waits for every FINAL revision and
its cohort acknowledgment, preventing root from dropping late peers' final cost
frames. The original MPI finalizer runs after this helper has stopped and is
outside this PEAK-management CPU watch.

Concurrent callback charges use the primary atomic available-credit counter;
contention is not interpreted as budget exhaustion. The finite unfunded grace
also uses monotonically issued atomic tickets rather than a retry-limited CAS.
A crossing ticket burns the remaining grace tail and requests emergency closing;
it does not return tickets or enlarge the session limit. Coherent diagnostic
snapshot contention is distinct from coordinator inactivity: callbacks use the
atomic active/freshness predicate, and a heartbeat skips admission on an
incoherent snapshot without switching to the old detach policy.

Detached recovery receives a complete observation lease and closing registration
in one funded decision, preventing unusable partial reservations from occupying
the pool across every detached rank. Active refills can still use partial
credit. A recovery request's complete target stays fixed until acknowledgment
and admission; waiting to earn that recovery budget is excluded from active
network-refill latency prediction.

Bootstrap readiness has an independent iteration cap and rejects failing or
backward monotonic readings. A local clock failure still participates in the
mandatory SPMD bootstrap before degrading, so it does not strand peers inside
that collective. Invalid startup timing does not create earned credit.

Issued, consumed and returned credit uses canonical integer nanoseconds. Root
reconciles profile cost with credited profile consumption. Actual control cost
is added to prepaid control credit still awaiting a proven settlement watermark;
management and transport CPU are then added separately. A lagging profile snapshot
therefore cannot hide management cost. Outstanding credit decreases only by
exact consumption or explicit return, never by unrelated management charges.

The control-credit watermark is captured under the controller lock only when no
mutation is requested or in progress, before the heartbeat reads actual control
accounting. A joined controller can settle canceled requests with their actual
outcome. This keeps prepaid future control work in escrow even when an earlier,
uncredited STOP already contributed a larger cumulative control cost.
