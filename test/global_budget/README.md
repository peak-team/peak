# Experimental global-budget transport checks

This module uses one job-scoped, random-nonce TCP broker and one helper thread
per process. It never calls MPI. The MPI init integration bootstraps the address,
port and nonce on the application's initialization thread; all participating
ranks must opt in. Protocol version 2 is Linux/64-bit, homogeneous-ABI only,
with at most 64 ranks. Other platforms build inert API stubs.

Runtime cost snapshots are cumulative, finite and monotonic. The root uses its
own monotonic elapsed time. Issued credit is reserved immediately; monotonic
profile consumption transfers it from reservations to charged cost, even when
the HB snapshot has not advanced. Control consumption transfers local credit
to unsettled global escrow until proven settlement. Credits and reservations are canonical uint64
nanoseconds; the floating-point seconds fields are diagnostics. Profile cost is
max(reported profile, credited profile); spent control cost is reported actual
control. Credited control not yet covered by the controller's settled watermark
remains releasable global reservation, separate from unspent local credit. HB
management, live finalizer CPU and service CPU are added separately. The rank
actual cost keeps a monotonic high-water mark; temporary control escrow never
enters that high-water mark. This is a conservative estimate, not
measured application slowdown. Unused grants are never reclaimed merely because they
expired or a peer disconnected. The root retains max(3 ms, observed closing cost) per registered demand member
and 500 us per exposed rank for the session-wide unfunded grace; these are explicit initial
estimates, not certified upper bounds. Startup is charged once by rank zero.
All helper threads' actual thread CPU time is reported automatically, including
the root broker, and is charged once (not a second root-only surcharge).

A TCP message sequence is distinct from the HB snapshot revision. New credit
requests, consumption and atomic wave ACKs are accepted with an unchanged HB
revision. Duplicate message sequences do not spend twice. Pressure events have
another monotonically increasing generation: completing a wave while retaining
a stale positive attached count and old demand cannot immediately create a new
wave. Fresh demand after successful credit consumption can create a later wave.
The root acknowledges completion after every live member reports that wave ID;
a proven quiescent retired member can acknowledge without physical mutation.
The root GO-to-all-DONE span is diagnostic; this version still charges the sum of
local control costs and does not silently deduplicate that sum into the span.

The callback reads coordinator activity/freshness independently of the bounded
coherent diagnostic sidecar. Its primary signed available balance uses one atomic
fetch-sub claim, rolling back only insufficient claims; committed profile,
control and returned counters remain monotonic. Ordinary successful consumption
uses atomics. A new pressure event or wave completion can make one nonblocking eventfd
write, comparable to the existing controller wake. The helper waits in poll;
normal costs/consumption are coalesced to 100 ms and requests/ACKs/commands are
immediate. Peer data older than one second invalidates the coordinated view.
Malformed, nonfinal disconnected or stale peers fail closed to an inactive view; no
reconnect/reset can issue the same credit twice. Stop joins a helper whose waits
are bounded, preserving the final diagnostic view with active=false. Flush is
bounded to 100 ms; FINAL flush is bounded to 2 s and waits for all FINAL ACKs
to be written by the root. Failure is explicit when the final snapshot or
all-rank wave acknowledgement cannot be proved. A cohort becoming inactive
because another participant exited does not invalidate a previously authenticated
settlement: a surviving rank can still confirm its exact final revision during
the bounded shutdown exchange, without obtaining any new spending permission. Fork children cannot read or
spend the parent's credits; inherited known sockets are closed by child stop.

Standalone commands (all tests have alarms, and should run in bounded process
groups):

```sh
gcc -std=c11 -Wall -Wextra -Werror -Iinclude src/global_budget.c \
  test/global_budget/test_global_budget.c -pthread -lm -o /tmp/test-gb
 timeout -k 2s 15s /tmp/test-gb 2
 timeout -k 2s 15s /tmp/test-gb 4
```

CMake registers normal 2/4-client tests, forced 13-byte TCP fragmentation,
credit recovery and a second wave, malformed/monotonic/overflow/fork/expired
sidecar validation, same-HB-revision credit settlement/replay/ACK tests, and a real peer-first
shutdown/final-snapshot settlement test.
The idle-cost executable is manual performance evidence (not a timing-sensitive
pass/fail test). On the local 24-CPU host, a three-second idle experiment measured
aggregate helper CPU of about .021 s for eight ranks and .102 s for 32 ranks
with event-driven transport. The earlier 1 ms polling prototype measured about
.247 s and 1.080 s respectively. These are aggregate CPU seconds, not application
wall slowdown or cross-node performance guarantees.

The opt-in real MPI integration harness preserves the final local five-case
verification (off, funded, tight, TCP disconnect, invalid runtime accounting).
It requires an MPI-enabled PEAK build with `PEAK_ENABLE_TEST_HOOKS`, MPICH's
launcher/rank environment, eight ranks, and two OpenMP threads per rank. Build
the existing `pressure` benchmark using the instructions in
`benchmarks/mpi_rank_budget/README.md`, then run:

```sh
python3 test/global_budget/run_mpi_integration.py \
  --library build-fast-dispatch/src/libpeak.so \
  --pressure /absolute/path/to/pressure \
  --mpiexec /usr/lib64/mpich/bin/mpiexec \
  --mpicc /usr/lib64/mpich/bin/mpicc --output /tmp
```

Each invocation creates a unique output directory with copied binaries, the
compiled fault shim, SHA256 provenance, per-rank logs, and case receipts. Each
MPI process group has a 50-second limit (`--timeout` overrides it) and is killed
and reaped on completion or failure. The shim closes rank zero's actual TCP
service in the disconnect case; a separate test hook injects invalid accounting.
The checks require mathematical correctness, global startup readiness, unchanged
FUNNELED required/provided levels, positive credit in the funded case, and
coordinated STOP/DONE in the tight case. They distinguish disconnect from invalid
accounting using the reported counters. These scenario assertions can fail on a
different machine or workload duration; inspect the preserved receipts rather
than interpreting failure as a measured slowdown result. This is deliberately
standalone, not a default CTest or an overhead performance threshold.

Existing receipts can be checked without launching MPI:

```sh
python3 test/global_budget/run_mpi_integration.py --replay /path/to/output
```

The implementation uses a dynamic closing reserve: 500 microseconds per
exposed rank for the unchanged session grace, plus max(3 milliseconds, observed closing
cost) for each registered demand member. Installed cold hooks alone do not
register a member. A wave carries an immutable personal `wave_participant` flag;
cold ranks acknowledge it without physically detaching. Actual costs remain a
conservative sum; the root GO-to-DONE span does not discount that sum.

Successful credit consumption can request a refill before exhaustion. The
helper estimates recent consumption rate and request/reply latency; the callback
only reads atomics, debits credit, and rarely wakes the helper. A positive reply
acknowledges the request generation, allowing the next low-water crossing to
produce a new immediate request. The root reserves every positive grant before
sending it. Credit return uses the same atomic debit counter as consumption and
a cumulative returned counter; replay cannot return funds twice. After 100ms
without consumption, the owner helper may explicitly return unused credit above
one predictive block, once per idle interval, provided no refill is pending.
The root never reclaims credits merely because they are old. Registration is
released only after confirmed DONE with detached targets, not on idle return.

`peak_global_budget_origin` accepts only the root's measured original MPI_Init
duration as earned elapsed time; profiler bootstrap remains a separately charged
cost. No observation loan is issued. `local_credit_ns` is the instantaneous local
credit balance; `allowance_seconds` is the delayed broker reservation snapshot.

Set `PEAK_MPI_GLOBAL_TRACE=1` (or `PEAK_GLOBAL_BUDGET_TRACE`) to dump the latest
256 helper events at shutdown. `ns` is the local monotonic timestamp, `rank` is
the affected rank, and `demand`/`wave` identify protocol generations. Kinds are
`r` client request submission (amount is request creation monotonic seconds),
`R` root request reception, `G` grant delta, `g` client cumulative grant receipt,
`S` root STOP, `s` client STOP receipt (amount is participant flag), `D` all-DONE
(amount is span), and `I` owner idle return. This bounded diagnostic ring can
truncate earlier events; it is not a complete execution trace.

For the proactive-refill revision, add `--require-coordinated` to require zero
local emergency requests in both healthy cases. Keep that stronger assertion
separate when replaying first-prototype evidence: those receipts deliberately
preserve the emergency behavior that motivated the correction.

Refill requests use a cumulative absolute local credit target (`debit + desired`)
and transmit only its outstanding desired balance. A prior large recovery request
therefore does not become a permanent minimum for future small refills. The root
advances `acknowledged_demand_generation` only when the complete requested balance
is reserved; partial grants do not acknowledge funding or restart the request.
New requests and positive grants restart the owner's 100ms idle observation, and
detached ranks' recovery credits are excluded from automatic idle return. This
prevents freshly arrived recovery funds from being returned in the same helper
iteration before reattachment can use them.

Retirement is distinct from transport shutdown. After suspending user callbacks
and draining the controller, publish final cumulative costs and call
`peak_global_budget_retire()`. Retirement is irreversible: it disables local
spending and new demand, returns unused credits, and advertises `sample.retiring`.
Its helper remains connected, bills its CPU, and acknowledges later waves without
waking the stopped controller. A retired member is excluded from new physical
STOP participant sets. An already retired rank can satisfy a wave's quiescence
condition; it cannot supply DONE for another live rank. Cost snapshot revision
may stay fixed while wire sequence, transport cost and retirement ACKs advance.
Only an existing all-rank MPI participation proof (or completed original
MPI_Finalize) permits final flush/stop; early rank-zero finalization must retain
the broker while other ranks execute application work. Peer loss still degrades
the cohort even when the broker's application rank has retired.

Use `--bind-ranks` with the five-case harness to allocate disjoint pairs of
eligible logical CPUs to ranks, with explicit single-CPU OpenMP places. It
checks the application's actual per-thread affinity against that map and applies
binding before exporting LD_PRELOAD, so the affinity utility is not profiled.
Eight ranks require at least sixteen eligible logical CPUs. Without this option,
multiple ranks can inherit identical OpenMP places; retain such results as
resource-contention stress evidence rather than a disjoint-placement baseline.

Two additional opt-in functional fixtures perform checked real arithmetic:

```sh
python3 test/global_budget/run_mpi_lifecycle.py --library build-fast-dispatch/src/libpeak.so --mode skew --ranks 2
python3 test/global_budget/run_mpi_lifecycle.py --library build-fast-dispatch/src/libpeak.so --mode recovery --ranks 8
```

These commands compile the fixture, bind ranks to disjoint CPU pairs, and bound
and reap compilation/launcher process groups. Skew finalization splits a peer
communicator before rank zero enters MPI_Finalize; peers continue more than a
freshness interval of checked computation and peer-communicator collectives.
They must retain active global control and reach a confirmed final settlement.
Recovery places real uninstrumented computation between hot intervals, then
requires a post-wave restoration event with a fresh call baseline followed by a
larger exact final call count. All outputs, commands and SHA provenance remain
in unique directories. These duration-based functional intervals verify control
and restoration, not application slowdown or equal work between modes.

Final settlement uses a separate `sample.final` commitment after the existing
all-rank MPI teardown proof and support teardown. Publish its new sequence once,
then call `peak_global_budget_final_flush(2000)`. The root waits for every FINAL
before sending `cohort_finalized` ACKs. Root completion additionally requires every
such ACK frame to have been fully written. Clients require their own sequence ACK
and the cohort flag. A peer that closes after its authenticated FINAL ACK keeps
its settled ledger entry and does not break the remaining closing cohort; any
nonfinal disconnect still degrades. Ordinary `flush(100)` keeps its earlier
semantics. The deadline is bounded, and a timeout remains unconfirmed. Trace
`F` denotes all FINAL, `A` all final ACKs written, and `C` clean finalized exit.

A finalizer pthread CPU clock may be registered once after heartbeat join using
its CPU baseline captured at `peak_fini` entry and the frozen HB management total.
The helper sends management as max(latest management counter, frozen baseline +
current finalizer CPU delta), so early-finalize CPU is visible while peers still
execute. A later counter containing that same CPU does not charge it twice. The
clock is read only while the owner thread lives, before original MPI_Finalize;
read failures explicitly invalidate accounting and degrade, rather than publish
zero. FINAL does not stop the helper before its final settlement.

The retained hot consumption predictor changes only on real credit consumption.
Idle wakeups do not decay its funded RTT margin to the 100us floor. The pulse test
uses a 2ms test-only request delay and 150ms cold intervals after establishing a
funded hot predictor; every spend must succeed with no grace available. The owner
still explicitly returns surplus above its funded predictive tail. The same test
with the former idle-decay condition fails its retained-credit check.


Detached recovery is funded atomically: closing registration plus the complete
frozen lease gap must fit the earned pool, otherwise neither D nor a partial
recovery reservation is issued. Active refills may still receive partial credit.
The rotating allocation cursor permits a subset to recover when the pool cannot
fund every rank. Cold budget waiting does not update the active transport RTT.
`recovery_target_ns` is the full admission goal retained until actual attachment
or explicit cancellation, not a remaining-balance estimate.

Credit claims have an explicit defensive 4-second single-operation limit, far
above normal 64-callback blocks and control mutations. Positive Linux 32-bit TIDs
bound simultaneous failed reservations, so the signed negative marker cannot
overflow at that limit. Larger claims fail safely and increment
`rejected_spend_count`; they never enter the committed ledger. Quiescent primary
available + credited profile + credited control + returned equals exact issued
credit. Metadata seqlock contention does not mean coordinator inactivity.

Control prepayments use `try_spend_control`. The controller captures
`control_consumed_ns()` only when its mutations are quiescent and actual control
accounting is updated. An HB sample saves that settled watermark before reading
actual C. The root adds actual C to spent and unsettled prepayments to global
reservation, outside the spent high-water mark. It validates component and
watermark monotonicity and never removes pending control escrow merely because
an earlier unrelated STOP cost is already reported. Retained profile-rate prediction excludes control
prepayments. No callback receives unaccounted free execution on CAS contention.

Before polling, outstanding local FINAL/request/ACK work with no pending socket
bytes forces immediate preparation. A completed old reply whose cohort/wave or
credit state changed causes one immediate rebuild. Pending blocked bytes still
wait for POLLOUT, so this removes the lost-wakeup 100ms floor without a busy loop,
a periodic-rate increase or bypassing the FINAL barrier.

A helper acquires the request generation before reading its committed profile,
control and returned counters. Each callback commits its cost before releasing
the new generation, so a new demand cannot carry an old, falsely unspent balance.
A matching full-funding ACK completes pending demand even when the root needs no
new grant; an older ACK cannot clear a newer generation. The first active request
after real idle resets the rate sampling origin and baseline, retaining its hot
predictive margin. Real positive profile consumption updates immediately with a
1us minimum denominator, rather than waiting an additional 1ms or dividing the
first burst by the preceding cold phase. All larger allowances still consume the
root's already earned pool; the session grace remains 500us.

Shutdown publishes `running=false` before its final eventfd wake. This wake
bypasses the ordinary `poke()` running guard. Waking first permits a helper on
the same rank CPU to consume the event, reenter its 100ms normal poll, and then
leave `pthread_join` waiting for that timeout. The deterministic `stop_wake`
regression forces this interleaving and checks state/order rather than a fragile
wall-time threshold. The manual 32-client `many_final` harness binds each rank's
main/helper pair to one allowed CPU and simulates 1ms preemption after the wake.
It reports FINAL flush and stop/join separately and preserves a configurable
last-rank FINAL delay, so a join fix cannot pass by weakening the cohort barrier:

```sh
gcc -O2 -std=c11 -Wall -Wextra -Werror -Iinclude \
  test/global_budget/test_many_final.c -pthread -lm -o /tmp/many-final
timeout -k 2s 18s /tmp/many-final 32 150000
```

With the same 150ms FINAL delay, the old ac5 stop measured root flush 151.993ms
and join 100.220ms; fixed ordering measured 152.291ms and 1.097ms respectively
(the latter includes the deliberate 1ms scheduling delay). These are controlled
local scheduler-race experiments, not MPI slowdown claims.

Post-init global admission now leaves newly resolved targets physically
unpatched until a complete paid installation/observation lease is available.
On supported MPI launches this is the default for global mode when the
activation policy is unset. First installation uses the controller's ATTACH
operation and event kind 7; a genuine post-detach restoration retains kind 6.
The lifetime grace cap remains unchanged. Protocol v2 accepts an explicit
`initial_unexposed` proof from deferred, never-patched ranks. Such a rank does
not reserve grace until its first whole grant atomically funds the requested
lease, closing reserve and 500us grace. Unknown and legacy ranks reserve grace
conservatively. Exposure latches permanently on a grant or a producer's false
declaration; returning unused credit does not release it. A stale true snapshot
after a grant remains valid, but a producer's false-to-true transition fails
closed. The broker never infers this proof from zero calls or attached counts.

Controller-confirmed physical target counts override cached heartbeat counts.
Successful backend changes publish an atomic membership event; a new event is
sent urgently even if the heartbeat revision is unchanged. Once available,
this evidence cannot be overwritten by a stale heartbeat. Retirement forces
the effective count to zero. `test_physical_membership.c` also checks a real
broker issues STOP for an active member whose cached heartbeat still says zero.

`test_initial_exposure.c` covers 32 deferred versus legacy ranks, the exact
first-grant boundary and one-nanosecond shortage, stale snapshots after grants,
permanent exposure after returns, and a genuine TCP declaration rollback.

`run_mpi_lifecycle.py` additionally accepts these real-MPI modes:

- `unfunded`: compares the target's entry bytes before and after execution with
  an unfundable budget; requires native execution, zero installs and zero closes.
- `initial-once`: disables reattachment, requires paid first installation and
  real calls, then STOP without a kind-6 restoration.
- `fallback`: disconnects the coordinator and requires safe local installation,
  real calls and an unconfirmed global shutdown.

Run each with `--library /absolute/path/to/libpeak.so --mode MODE`, using the
same MPI compiler/launcher options as the existing recovery test. Omit
`--activation-policy` to test the global-mode default; explicit `immediate` and
`post-init` remain available. These checks establish behavior and coverage;
fixed-work hardware timings are still required to assess slowdown.

`test_control_escrow.c` covers completion between the HB watermark and actual
cost reads, permanent high-water double charging, cancelled paid operations,
legacy unpaid control alongside new prepayment, and rejection of old marks.
