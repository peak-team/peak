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

The anti-storm token bucket applies to heartbeat-detached sampling probes and
normal reattach of hooks that heartbeat detached. Hooks detached by explicit API
or detach-count policy are still charged as pending transition budget, but they
are not throttled by the probe token bucket; that bucket exists to control
adaptive sampling churn, not to delay explicit reattach recovery.

## Per-Target Heartbeat

Enable per-target scheduling with:

```bash
export PEAK_ENABLE_PER_TARGET_HEARTBEAT=1
```

For every attached hook, PEAK compares the hook's estimated callback overhead
against `PEAK_OVERHEAD_RATIO`. A hook is eligible for physical detach when its
own overhead exceeds the target budget after including the expected transition
cost.

Per-target detach is the local safety valve. It is not suppressed merely
because global overhead is already high; that is exactly when a hot hook needs
to be removed from the attached callback set quickly. The global scheduler runs
in addition to this local path and can detach more hooks based on process-wide
pressure, but it must not be the only path that can detach a hook above
`PEAK_OVERHEAD_RATIO`. The per-target path keeps master-compatible trigger
semantics: a hook whose own callback overhead is above `PEAK_OVERHEAD_RATIO`
is requested for detach without adding a synthetic STOP-window estimate to that
per-hook threshold. The strict controller still owns admission, batching,
thread safety, and backend retry/failure handling for the physical mutation.
Global detach, reattach probes, and probe closeout continue to use the shared
stop-window and mutation-token ledgers because those paths can otherwise create
many-target churn.

## Global Heartbeat

Enable global scheduling with:

```bash
export PEAK_ENABLE_GLOBAL_HEARTBEAT=1
```

Global heartbeat looks at two pieces of cost:

- callback pressure from hooks that are currently attached;
- physical transition overhead from detach and reattach stop-windows.

Callback pressure is the larger of two ledgers: recent callback rate and
cumulative attached callback ratio. Recent rate catches a newly hot target
quickly. The cumulative ledger preserves pressure after a rank reaches an MPI
collective or setup wait; otherwise wall-clock wait time can make a profiling
burst look cheap after it has already delayed peers. Global detach projects the
recent and cumulative ledgers separately, then recomputes pressure as the larger
projected value after each candidate. A same-batch candidate may share an
already-paid stop-window, but it still must lower the active pressure ledger.

The effective global overhead is callback pressure plus transition overhead.
The transition part includes measured stop-window time plus pending reserved
transition budget. Admission for new mutations uses predicted stop-window cost
from per-hook/global exponential moving averages, with a small floor before
measurements exist. Callback pressure decides whether more hooks should be
physically detached; physical transition overhead is bounded by the shared
mutation-token ledger and recent stop-window burst shaper. Global detach
therefore starts when attached callback pressure is above
`PEAK_GLOBAL_OVERHEAD_RATIO`, not when cumulative transition debt has lowered an
artificial callback allowance. The user-facing factors are:

```text
global detach pressure starts above =
    PEAK_GLOBAL_OVERHEAD_RATIO * PEAK_GLOBAL_DETACH_FACTOR
global reattach threshold =
    PEAK_GLOBAL_OVERHEAD_RATIO * PEAK_GLOBAL_REATTACH_FACTOR
```

The default factors favor adding global detach pressure when process-wide
callback cost is high and reattaching conservatively. Global detach chooses
attached hooks by recent overhead rate, batches physical mutations up to the
controller batch limit, and charges the batch stop-window cost once for the
batch.

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

Probe admission requires the hook to have been detached by heartbeat, to stay
detached for at least the per-hook detached dwell floor, and to fit the bounded
token bucket plus the per-heartbeat admission caps. Probe tokens must
accumulate enough budget to pay for the expected callback cost during the
minimum attached sample window.
Physical stop-window cost lives in the shared heartbeat mutation bucket, which
charges the incremental cost of the actual reattach batch. The later closeout
detach is carried as pending/follow-up transition debt and still runs through
controller admission and strict safety. Both buckets refill from bounded
fractions of the global mutation budget, not from unlimited wall time, so
many-target reattach pressure cannot turn into an unbounded detach/reattach
storm. If a high-priority
detached hook is too expensive for the current token balance, PEAK preserves
the partial token balance and continues looking for an affordable probe inside
the same per-heartbeat cap; one expensive hook should not block distributed
sampling across the hot set.
That cap is deliberately smaller than the normal controller batch capacity.
Once the shared physical stop-window is admitted, PEAK opens a small probe wave
instead of reattaching dozens of hot hooks at once; this keeps the paired
closeout work bounded and prevents a many-target sampling storm.

The per-hook probe interval is dynamic:

```text
required detached dwell =
    max(configured detached dwell floor,
        adaptive per-hook detached dwell,
        minimum sample callback cost / available probe budget ratio)
```

The configured detached dwell floor defaults to 5 seconds and is controlled by
`PEAK_REATTACH_PROBE_MIN_DETACHED_MS`. When a reattach probe shows that the
target is still too hot, the heartbeat raises that hook's detached dwell up to
`PEAK_REATTACH_PROBE_MAX_DETACHED_MS`, which defaults to 60 seconds. Set the max
to `0` for no adaptive cap. This is not a maximum detached duration; it is only
the largest minimum dwell PEAK will require before another sampling probe. A
target may stay detached longer when budget or admission does not allow
reattach. When later samples are cool, the dwell decays back toward the floor.
The adaptive dwell is changed only by a valid reattached probe sample with a
positive call delta; if the sample window is empty or invalid, the current dwell
is left unchanged. `PEAK_REATTACH_COOLDOWN_MS` is still recognized as a
master-compatible hard gate and defaults to 60 seconds. A heartbeat-detached
hook is not eligible for heartbeat reattach before that gate opens; after it
opens, the adaptive probe dwell and mutation budgets still apply. Set it to `0`
only when intentionally running aggressive sampling/stress experiments. The
controller therefore remains a sampling profiler for hot detached targets: it
should reattach after the hook has been detached long enough and budget has
recovered, collect fresh calls, then detach again only if the target is still
too hot. If the measured probe sample is within the per-target overhead budget,
PEAK keeps the hook attached and releases the paired closeout debt so coverage
can continue normally. When the cool hook is carrying the probe wave's
amortized closeout reservation, PEAK first checks whether other active probes
still have zero local closeout credit. If they do, PEAK keeps the cool hook
attached with the reservation intact and retries on a later heartbeat; once the
reservation is no longer shared, the cool hook can promote and release its own
debt. This avoids releasing shared closeout debt while hot peers still depend
on it.

The probe path yields to detach recovery while attached callback overhead is
still above the global detach-pressure threshold,
`PEAK_GLOBAL_OVERHEAD_RATIO * PEAK_GLOBAL_DETACH_FACTOR`. During that state the
controller does not reserve shared mutation slots or shared stop-window tokens
for future reattach probes, because that reserve can otherwise make global
detach admission impossible even though many hot hooks remain attached. Once
attached overhead is near the configured budget, the reserve is restored and
probes continue under the same full-cost interval.

After the probe is reattached, PEAK waits for both a minimum call delta and a
minimum lease time before deciding whether the probe must close out. A cool
sample clears the active probe and keeps the hook attached unless doing so
would release closeout debt that is still shared by other active probes. A hot
or invalid sample schedules the follow-up detach. That follow-up detach is
treated as pre-reserved closeout work for budgeting, but it still goes through
normal controller admission and strict safety. An all-closeout batch may bypass
only the heartbeat burst/token pacing ledger because that cleanup was already
admitted as follow-up debt; closeout batches are capped to the same small probe
wave size, and new heartbeat mutations and mixed batches remain paced. PEAK
does not recheck the old local reservation against a newer stop-window EMA
before draining this hot closeout, because doing so can strand a hot probe
attached long after its sampling window should have closed.

This is the behavior expected for hot MILC-style workloads: PEAK should
periodically detach and reattach hot targets, collect sampled call/timing
coverage, and keep the estimated callback plus transition overhead bounded.

## Transition Overhead Accounting

Physical detach and reattach stop target threads briefly. PEAK measures that
stop-window time and stores an exponential moving average. Future heartbeat
decisions charge the predicted stop-window cost before scheduling more
mutations.

In `PEAK_DETACH_BACKEND=auto`, the helper remains the first attempted backend.
If a successful helper-backed mutation exceeds
`PEAK_AUTO_HELPER_PERF_FALLBACK_STOP_WINDOW_US` (default `5000` microseconds),
PEAK closes the helper and caches the signal backend for later auto mutations.
This keeps the helper-first correctness path, while avoiding repeated
helper-stop windows that can skew MPI-synchronized ranks before the heartbeat's
wall-clock denominators show the cost. Set the threshold to `0` to disable this
performance demotion.

A reattach probe reserves sample-exposure budget for the callback window needed
to gather fresh calls. The shared mutation bucket pays the current physical
reattach stop-window, using incremental batch cost when several probes are
admitted together. The paired closeout detach is added as pending/follow-up
transition debt instead of being required as another immediately spendable
shared token. Follow-up probe detaches run first as closeout work through
controller admission and strict safety; all-closeout batches may bypass only
the heartbeat pacing ledger because they were already budgeted as follow-up
debt, and those closeout batches are capped to the small probe wave size rather
than the full controller batch. The heartbeat also reserves a small number of
pending-controller slots for reattach work before admitting new non-closeout
detach work, so a saturated many-target detach backlog is less likely to starve
sampling coverage. These two safeguards reduce the risk that
many-target workloads create a detach/reattach storm that looks like the
application is hung even though the controller is making progress.

Those reserves are conditional. While attached callback overhead is still above
the global detach-pressure threshold, probe reattach yields the shared mutation
slots and shared stop-window tokens to global detach recovery. Follow-up probe
closeouts still run first, but new probe reattach reservations resume once
attached overhead is near the configured budget instead of waiting for a
perfectly under-target instant.

Heartbeat-sourced physical mutations are also shaped by a short recent
stop-window ledger. The controller records the real stop-window time of
successful heartbeat batches and uses that measurement to adapt the next
heartbeat reattach and probe-closeout batch size. Ordinary heartbeat detach
evacuation is deliberately allowed to use the full controller batch: once
attached callback overhead is over budget, detaching a wave of hot hooks is the
operation that reduces overhead, and charging a full stop-window to only one or
two hooks leaves the application instrumented for too long. A reattach/probe
batch that would place too much physical mutation time inside the current burst
window is deferred before it reaches the helper/signal backend. Fresh deferred
work is restored to the hook's last stable physical state instead of sitting in
the controller queue; a later heartbeat can request it again when budget
recovers. This is separate from retry cooldown: safe hooks are still sampled,
but the physical reattach/probe work is paced so it cannot concentrate into a
short stop-the-world burst.

Heartbeat threads also apply a deterministic per-rank phase offset and small
sleep jitter. This does not change the configured average heartbeat interval,
but it reduces cluster-wide synchronization where thousands of ranks otherwise
try to stop their local process at nearly the same instant.

## Failed Mutation Bounds

Heartbeat requests use stricter retry bounds than explicit API, detach-count,
or shutdown requests. The goal is to keep adaptive sampling from spending many
seconds repeatedly trying a physical mutation that is not currently safe.

The default heartbeat mutation STOP timeout is short:

```text
PEAK_HEARTBEAT_MUTATION_TIMEOUT_MS=1000
PEAK_HEARTBEAT_MUTATION_MAX_RETRY_COUNT=3
PEAK_HEARTBEAT_MUTATION_MAX_PENDING_AGE_MS=5000
PEAK_HEARTBEAT_MUTATION_FAILURE_COOLDOWN_MS=30000
```

This is an acquisition lease for STOP/classification/evacuation, not the cost
charged to the heartbeat budget. Bootstrap batch cost prediction remains capped
separately, and later admission uses measured STOP-window time. When a
heartbeat detach or reattach prepare keeps failing, PEAK abandons that
pending request quickly, leaves the hook in its last stable physical state, and
applies a per-hook cooldown before heartbeat may request another transition for
that same hook. The cooldown grows for repeated timeout/error failures up to a
bounded cap. A later successful physical transition clears the failure streak.
Pacing deferral is only for fresh requests that have not reached the backend;
once a request has attempted STOP/classification/evacuation, pacing is not
allowed to erase the retry state, and pending-age/retry-count limits determine
whether the request retries or fails closed.

These controls are not coverage policy knobs. Successful reattach probes still
need to happen for hot detached targets when budget allows. The bounds only
limit failed physical-transition attempts, so a target that is safe to mutate
can still participate in normal detach/reattach sampling.

`PEAK_DETACH_CONTROLLER_IO_TIMEOUT_MS` controls the conservative default
controller helper/signal I/O timeout for non-heartbeat requests. The default is
`10000` ms. Heartbeat requests pass their shorter timeout explicitly to the
strict detach controller and helper protocol for STOP acquisition,
classification, and audited evacuation. Once the controller has successfully
stopped target threads, releasing those threads is mandatory cleanup; PEAK uses
a fresh cleanup lease with the same heartbeat request timeout instead of
letting an expired heartbeat budget strand a mutation window or silently
widening a heartbeat attempt into the conservative 10-second controller timeout.

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
many threads, lightly sweeps the cold target set, requires real sample-backed
detach/reattach/sample/detach cycles across multiple hot hooks and time bins,
uses a short heartbeat interval to exercise the aggressive MILC-style reattach
case, and keeps estimated profile plus transition overhead below the configured
global budget.

For Frontera validation of this specific failure mode, use the project HPC
suite `many-target-stress-rotating-proof`. It runs one node with 832 synthetic
targets, 256 rotating hot targets, 56 worker threads, the default dynamic
probe limiter, and both auto/helper-first and signal backends. The acceptance
condition is not merely
completion: the checker requires ordered hot-hook detach -> reattach -> sampled
calls -> detach cycles over multiple hooks and time bins, bounded stop-window
cost, and no classify/prepare/Gum/retry-abandon failures.
