# PEAK Heartbeat Controller

The heartbeat controller is PEAK's adaptive sampling loop for CPU target hooks.
It keeps normal entry/exit profiling active while the estimated overhead is
within budget, physically detaches hot targets when profiling would cost too
much, and periodically reattaches detached targets to refresh coverage.

Heartbeat scheduling only decides *which* hook transitions to request. The
strict detach controller still owns the physical attach, detach, and reattach
mutation and may retry or reject a request if the target process is not in a
safe state.

## Main Budgets

Two ratios control the heartbeat policy:

- `PEAK_OVERHEAD_RATIO` is the per-target profiling budget. Per-target detach
  compares one hook's estimated callback overhead plus expected transition cost
  with this budget.
- `PEAK_GLOBAL_OVERHEAD_RATIO` is the process-wide heartbeat budget. Global
  detach ranks hooks by recent attached callback rate and uses this ratio,
  together with measured transition overhead, for admission and reattach
  decisions.

Both budgets are PEAK estimates. They use calibrated callback cost and measured
physical mutation stop-window cost. They are not a hard wall-clock slowdown
guarantee for every application, because application lock contention, MPI
progress, and target cache behavior may amplify or hide the measured PEAK work.

## Heartbeat Pass Order

Each heartbeat pass starts from a snapshot of calls and timing, then refreshes
the shared transition ledger after detach reservations. It considers work in
this order:

1. Complete follow-up detaches for active reattach probes.
2. Run per-target detach decisions.
3. Refresh the shared transition ledger.
4. Run global detach decisions.
5. Refresh the shared transition ledger again.
6. Run per-target and global reattach decisions.

This order matters. Detach requests reserve transition budget before reattach
requests are considered, so a same-heartbeat reattach probe cannot spend budget
that has already been promised to detach work.

## Per-Target Heartbeat

Enable per-target scheduling with:

```bash
export PEAK_ENABLE_PER_TARGET_HEARTBEAT=1
```

For every attached hook, PEAK compares the hook's estimated callback overhead
against `PEAK_OVERHEAD_RATIO`. A hook is eligible for physical detach when its
own overhead exceeds the target budget after including the expected transition
cost.

Per-target detach is suppressed while effective global overhead is above
`PEAK_GLOBAL_OVERHEAD_RATIO * PEAK_GLOBAL_DETACH_FACTOR` and global heartbeat is
enabled. In that state the global scheduler chooses the hooks, because it can
rank all attached hooks by their contribution to process-wide overhead.

## Global Heartbeat

Enable global scheduling with:

```bash
export PEAK_ENABLE_GLOBAL_HEARTBEAT=1
```

Global heartbeat looks at the aggregate recent attached callback overhead plus
the measured physical transition overhead. It starts global detach pressure when
attached callback overhead is above `PEAK_GLOBAL_OVERHEAD_RATIO`. The
user-facing factors are:

```text
per-target detach yields above =
    PEAK_GLOBAL_OVERHEAD_RATIO * PEAK_GLOBAL_DETACH_FACTOR
global reattach threshold =
    PEAK_GLOBAL_OVERHEAD_RATIO * PEAK_GLOBAL_REATTACH_FACTOR
```

The default factors favor letting the global scheduler handle process-wide
overhead pressure and reattaching conservatively. Global detach chooses attached
hooks by recent overhead rate, batches physical mutations up to the controller
batch limit, and charges the batch stop-window cost once for the batch.

When attached callback overhead is already over budget, refusing every detach
because the transition ledger is also over budget leaves the application fully
instrumented and makes the violation worse. To avoid that trap, PEAK allows at
most one bounded `global-overhead-recovery` batch per heartbeat. That recovery
batch is only admitted when its single stop-window cost fits the hard mutation
budget and the selected batch is expected to remove more attached callback
overhead than it spends in transition overhead.

## Reattach Probes

Heartbeat reattach has two paths. A normally eligible detached hook can be
reattached when the projected callback and transition overhead remains within
the active per-target and global budgets. A hook that was detached by heartbeat
and is still expected to be hot may instead use a budgeted sampling probe.

For a probe reattach request, PEAK first checks that the reattach probe token
bucket has accumulated enough transition budget to pay for both the reattach
and the expected follow-up detach. After the probe is reattached, PEAK waits for
both a minimum call delta and a minimum lease time before scheduling the
follow-up detach. That follow-up detach still goes through the strict detach
controller.

This is the behavior expected for hot MILC-style workloads: PEAK should
periodically detach and reattach hot targets, collect sampled call/timing
coverage, and keep the estimated callback plus transition overhead bounded.

## Transition Overhead Accounting

Physical detach and reattach stop target threads briefly. PEAK measures that
stop-window time and stores an exponential moving average. Future heartbeat
decisions charge the predicted stop-window cost before scheduling more
mutations.

A reattach probe reserves budget for two transitions: the reattach itself and
the follow-up detach that may be needed once the probe has collected fresh
coverage. This prevents many-target workloads from creating a detach/reattach
storm that looks like the application is hung even though the controller is
making progress.

## Request Sources In Trace Output

When `PEAK_DETACH_TRACE_PATH` is set, the trace records the source of each
transition request. The heartbeat sources are:

- `per-target-heartbeat`
- `global-heartbeat`
- `global-overhead-recovery`

For a healthy hot-target sampling cycle, the trace should show a successful
detach, a later successful reattach, observed calls while reattached, and a
later successful detach for the same hot hook. The many-target stress checker
also writes `target-count-samples.csv` and verifies that call counts advance
between the reattach and follow-up detach timestamps.

## Recommended Hot-Target Configuration

Large threaded runs that need strict detach and adaptive sampling typically use
both heartbeat schedulers:

```bash
export PEAK_ENABLE_PER_TARGET_HEARTBEAT=1
export PEAK_ENABLE_GLOBAL_HEARTBEAT=1
export PEAK_ENABLE_REATTACH=1
export PEAK_OVERHEAD_RATIO=0.005
export PEAK_GLOBAL_OVERHEAD_RATIO=0.05
export PEAK_GLOBAL_DETACH_FACTOR=1.2
export PEAK_GLOBAL_REATTACH_FACTOR=0.95
export PEAK_HEARTBEAT_INTERVAL=0.01
```

Use lower ratios for stricter overhead control. Use a longer heartbeat interval
to reduce controller activity when the application is not dominated by very hot
targets.

## Manual Stress Test

The many-target detach storm stress tests are intentionally disabled in normal
CTest because they are long-running and designed for compute nodes. Enabling
the option registers auto, helper, and signal backend variants:

```bash
cmake -DBUILD_TESTING=ON -DPEAK_ENABLE_MANUAL_DETACH_MANY_TARGETS_STRESS=ON ..
ctest -R manual_detach_many_targets_heartbeat_storm --output-on-failure
```

The stress checker registers hundreds of targets, drives many hot targets from
many threads, requires real detach/reattach/sample/detach cycles, and verifies
that cold one-shot targets are not the reason the test passes.
