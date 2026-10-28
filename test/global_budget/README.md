# Experimental global-budget transport checks

This module uses one job-scoped, random-nonce TCP broker and one helper thread
per process. It never calls MPI. The MPI init integration bootstraps the address,
port and nonce on the application's initialization thread; all participating
ranks must opt in. This first protocol is Linux/64-bit, homogeneous-ABI only,
with at most 64 ranks. Other platforms build inert API stubs.

Runtime cost snapshots are cumulative, finite and monotonic. The root uses its
own monotonic elapsed time. Issued credit is reserved immediately; monotonic
consumption transfers it from reservations to charged cost, even when the HB
snapshot has not advanced. The accounted cost is the maximum of reported total
cost and acknowledged consumption. It therefore remains conservative when the
cost snapshot is behind credit consumption. It is an estimate, not measured
application slowdown. Unused grants are never reclaimed merely because they
expired or a peer disconnected. The root retains 3 ms per rank for control and
500 us per rank for the session-wide unfunded grace; these are explicit initial
estimates, not certified upper bounds. Startup is charged once by rank zero.
All helper threads' actual thread CPU time is reported automatically, including
the root broker, and is charged once (not a second root-only surcharge).

A TCP message sequence is distinct from the HB snapshot revision. New credit
requests, consumption and atomic wave ACKs are accepted with an unchanged HB
revision. Duplicate message sequences do not spend twice. Pressure events have
another monotonically increasing generation: completing a wave while retaining
a stale positive attached count and old demand cannot immediately create a new
wave. Fresh demand after successful credit consumption can create a later wave.
The root acknowledges completion only after every rank reports that wave ID.
The root GO-to-all-DONE span is diagnostic; this version still charges the sum of
local control costs and does not silently deduplicate that sum into the span.

The callback side reads a bounded two-attempt all-atomic sidecar and consumes
credits with at most two atomic CAS attempts. Successful consumption performs no
I/O. A new pressure event or wave completion can make one nonblocking eventfd
write, comparable to the existing controller wake. The helper waits in poll;
normal costs/consumption are coalesced to 100 ms and requests/ACKs/commands are
immediate. Peer data older than one second invalidates the coordinated view.
Malformed, disconnected or stale peers fail closed to an inactive view; no
reconnect/reset can issue the same credit twice. Stop joins a helper whose waits
are bounded, preserving the final diagnostic view with active=false. Flush is
bounded to 100 ms and explicitly returns failure when the final snapshot or
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
