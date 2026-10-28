# MPI rotating-rank budget regression

This fixed-work benchmark preserves the counterexample discussed in Codex thread
`01a16a07-395d-7460-a6f6-e9da198682bd`: each epoch profiles only one rank's thread 0,
then all ranks synchronize. The active rank rotates. A small local callback budget
can therefore coexist with a large application wall-time increase from MPI waits.
The original 32-rank, six-thread experiment measured about 0.095 seconds native
versus 4.2 seconds profiled in the main phase, while all 16,384,000 potential
profiled calls were recorded and no target transitioned. These are historical
observations on that machine, not expected portable timing thresholds.

`pressure.c`, generated target definitions, and `check.py` retain the original
integer workload. The only workload extension is optional `--main-work N` (default
1), allowing a coarse-function negative control. No sleep, timed termination,
random work, or profiler controller calls occur in the workload.

## Build

Use GCC-compatible `mpicc` and GNU binutils on x86-64. The entry-prefix checker is
specifically for x86 NOP instructions; it is not an ARM checker.

```sh
cd benchmarks/mpi_rank_budget
python3 generate.py
mpicc -O2 -g -fopenmp -fno-ipa-icf -rdynamic pressure.c -o pressure
python3 check.py ./pressure
```

Do not enable LTO, linker ICF, or compiler instrumentation. The 256 exported
functions have distinct patchable addresses and sixteen entry NOPs. `targets.txt`
selects only `pressure_000` through `pressure_015`. Functions 128 through 143 are
unprofiled arithmetic twins: the increment is the function ID modulo 128, so both
addresses perform identical useful work and state updates.

## Distributed execution

Run **inside a Slurm allocation**, normally two nodes, 32 MPI ranks, six physical
cores per rank. Use the site's supported MPI launcher and binding options; for
example, where supported:

```sh
export OMP_NUM_THREADS=6 OMP_DYNAMIC=FALSE OMP_PROC_BIND=close OMP_PLACES=cores
srun --ntasks=32 --cpus-per-task=6 --cpu-bind=cores ./pressure
```

The default parameters reproduce the original case exactly:
`--epochs 8192 --calls 2000 --warmup-calls 10000 --warmup-work 131072`.
Each of 192 threads executes 2000 calls per epoch, each doing one integer LCG
step modulo 2^64. Epoch `e` uses target ID `(e / ranks) % 16`; only rank
`e % ranks`, thread 0 uses its profiled address. All others use its native twin.
This is 3,145,728,000 main calls/steps, 16,384,000 potential profiled calls,
and 8192 main Allreduces per rank.

Warmup performs 10,000 unprofiled calls of 131,072 steps per thread. Warmup and
main wall/CPU/MPI/barrier timings are separate. Warmup is part of process lifetime
and lets profiler startup settle; it must not be included in main-phase wall time.
Every thread is checked against an independent affine-composition oracle after
each phase. Every workload Allreduce checks its integer sum. Both phases must
report `oracle_bad=0 mpi_bad=0`, and the final record must say `oracle=PASS`.

Compare the same executable and affinity in the order native, baseline, candidate,
baseline, candidate, native. Keep profiler environment, target list, and helper
binaries matched to their libraries; record library/executable hashes and the
allocation/binding metadata. The original profiler settings included:

```sh
export PEAK_TARGET_FILE="$PWD/targets.txt" PEAK_OUTPUT_AGGREGATION=mpi
export PEAK_OVERHEAD_RATIO=0.005 PEAK_GLOBAL_OVERHEAD_RATIO=0.05
export PEAK_GLOBAL_DETACH_FACTOR=1.2 PEAK_GLOBAL_REATTACH_FACTOR=0.95
export PEAK_ENABLE_PER_TARGET_HEARTBEAT=1 PEAK_ENABLE_GLOBAL_HEARTBEAT=1
export PEAK_ENABLE_REATTACH=1 PEAK_HEARTBEAT_INTERVAL=0.01
export PEAK_REATTACH_COOLDOWN_MS=100 PEAK_SAFE_DETACH_MODE=strict
export PEAK_REQUIRE_SAFE_DETACH=1 PEAK_DETACH_BACKEND=signal PEAK_MPI_REAL_FINALIZE=1
```

For each run, launch with a bounded timeout and save stdout, stderr, and the
**actual launcher exit code**. For example (replace launcher/library/helper with
site-supported choices, and use no preload/helper for native):

```sh
mkdir -p results
set +e
timeout --signal=TERM --kill-after=10s 120s srun --ntasks=32 --cpus-per-task=6 \
  env LD_PRELOAD="$BASELINE_LIBRARY" PEAK_DETACH_HELPER="$BASELINE_HELPER" \
  ./pressure > results/A0.out 2> results/A0.err
rc=$?
printf '%s\n' "$rc" > results/A0.rc
```

Repeat as `N0 A0 B0 A1 B1 N1`, using candidate library/helper for `B*`.
The timeout must cover the full process and launcher cleanup; use site-supported
Slurm cancellation if its launcher leaves a job step after termination. A timed
out or failed launch is never a passing timing result.

```sh
python3 analyze.py results > results/comparison.json
# Alternative execution-order labels, if your runs use B/C:
python3 analyze.py results --labels N0 B0 C0 B1 C1 N1
```

The positions mean native, baseline, candidate, baseline, candidate, native.
Analysis requires six stdout files and six `.rc`
receipts, validates thread/MPI oracles, exact counts, consistent configuration and
checksums, positive finite wall times, and final process elapsed time. It exits
nonzero and withholds comparisons if any run is incomplete or invalid. It reports
measured main wall rank means directly, with two-sample descriptive ratios to
native. CPU time is not wall time; MPI and release-barrier waits overlap. It never
adds independently selected per-rank maxima. Inspect PEAK stderr separately for
actual recorded calls, transitions, accounting validity, failed control windows,
and burst-budget diagnostics. Application correctness alone does not establish
profiler accounting correctness or a strict 5% application slowdown guarantee.

## Coarse negative control and local checks

Use `--main-work 262144` to do substantially more arithmetic per call. This keeps
the same routing and native twins; the oracle and `lcg_steps` scale with work.
Reduce `--epochs` and `--calls` for a bounded control, for example
`--epochs 64 --calls 128 --main-work 262144`. Keep these options identical across
the native/candidate control runs in a separate results directory. With 32 ranks,
each rank sees each of the first two targets for 128 profiled calls (64 epochs
cover two 32-rank sweeps; covering all 16 targets would require 512 epochs). This control increases useful
CPU work per call; reducing the number of calls alone would not establish low
callback density. For the six-run analyzer, repeat this configuration for all six
labels. Never compare this control's wall
time directly with the default workload as a slowdown ratio.

For local checking without installed MPI, the serial-only shim can exercise the
real OpenMP workload and oracle. **Never add `-Ismoke` to a real MPI build.**

```sh
gcc -O2 -fopenmp -fno-ipa-icf -rdynamic -Ismoke pressure.c -o /tmp/pressure-smoke
python3 check.py /tmp/pressure-smoke
OMP_NUM_THREADS=2 timeout --kill-after=2s 10s /tmp/pressure-smoke \
  --epochs 37 --calls 103 --warmup-calls 67 --warmup-work 17
python3 smoke/check_pattern.py
```

The direct simulator independently checks all 32 ranks × six threads across 529
epochs, including all 512 rank/target pairs. Serial checks establish neither
real MPI correctness nor multi-node performance.

## Synchronization and imbalance matrix

`sync_pressure.c` extends the fixed integer workload with `--sync allreduce`,
`barrier`, `bcast` (root rotates), `ring` (Sendrecv), or `iallreduce` (independent
integer work before Wait). `--layout rotating` moves the tiny profiled hot loop
across ranks; `slack` puts high-frequency profiling on rank 0 while the last rank
performs 512 arithmetic steps per native call; `critical` moves both the profiled
hot rank and the heavy 512-step computation together. Other ranks do one step.
Thus overhead on a fast rank can be hidden behind a slow native rank in `slack`,
while `critical` places it on the native bottleneck. Twins preserve exact useful
work in every configuration. No sleep is used to create imbalance.

Compile using the same flags as `pressure.c`, substituting `sync_pressure.c`.
Use all five sync values with all three layouts, identical arguments and bindings
for native/baseline/candidate arms. Start with `--epochs 2048 --calls 2000` and
adjust fixed work before freezing a distributed experiment. Existing warmup flags
apply. `rank_metric` records retain each rank's compute CPU/wall and sync elapsed;
`phase_root_wall` measures rank 0's complete phase directly. Sync elapsed includes
useful overlap for Iallreduce and is not pure waiting. Thread/barrier timing
categories overlap and must not be summed. Binding strings are gathered and
printed by rank 0 to avoid split multi-process stdout records.

`python3 sync_analyze.py RESULTS --labels N0 B0 C0 C1 B1 N1` requires actual `.rc`
receipts, complete per-rank records, successful independent arithmetic/MPI oracles,
and matching config/checksums before returning valid comparison inputs. It retains
individual rank measurements rather than combining unrelated maxima. Aggregate
profiler diagnostics and physical mutation accounting still require separate
inspection; a valid workload alone does not establish a universal slowdown cap.

`--layout aligned` instead keeps both the heavy native work and profiled hotspot
on rank 0. The complete matrix is therefore five sync modes × four layouts.
Per-rank records include exact useful `lcg_steps` and potential profiled calls;
nonblocking records split posting, independently oracle-checked overlap work,
and Wait elapsed. Use `--require-unique-affinity` in distributed analysis to
reject overlapping/multiple-CPU thread bindings; local functional tests may
retain shared bindings but cannot support a performance conclusion.

Every phase has one final MPI barrier after its selected-sync epoch loop and
before wall measurement. This uniform measurement boundary prevents rank 0 from
reporting completion before a late peer in ring/Bcast modes. Its elapsed time is
recorded separately as `completion_wait` and included in phase wall; it is not
an additional per-epoch synchronization mode. Oracle/result-validation collectives
remain outside the measured phase wall.

After collecting the distributed matrix, run
`python3 sync_summary.py results > matrix-summary.json`. The expected layout is
`results/SYNC-LAYOUT/LABEL/rank-0.out`, `LABEL/rank-RANK.err` for all 32 ranks,
and `results/SYNC-LAYOUT/LABEL.rc`. All 20 × six receipts, matching fixed work,
complete rank measurements, identical paired binding, two hosts with 192 unique
single-CPU thread bindings, and final 16-target local profiler reports are
required. Distinct CPU IDs establish distinct logical CPU bindings; proving
nonsharing of physical cores additionally requires CPU/core/socket topology
or confirmation that SMT is disabled on the allocation hardware. Helper zero-target reports are excluded; a genuine application report
with zero profiled targets remains valid. Candidate rank budget metadata must
match world size 32 and the configured five-percent job risk target.
The summary preserves each rank's measured costs and native compute imbalance,
as well as native/baseline/candidate wall samples and ratios. `valid` certifies
data completeness, independently of the list of cases exceeding five-percent
process overhead. Functional local fixtures can use explicit `--ranks 8
--threads 2 --hosts 1 --allow-shared-affinity --workload-only`; these overrides
must not be used to qualify distributed performance results.

Timing boundary: the `process` summary key retains the benchmark's
`process_seconds` field but means **main-entry-through-MPI-Finalize elapsed**.
It excludes loader/LD_PRELOAD constructors before main and launcher wall time.
The primary comparison is the measured main-phase wall including its final
completion barrier; warmup-plus-main and main-through-finalize are supplementary.

The synchronization workload also accepts `--profile-threads N` (default 1,
1 ≤ N ≤ OpenMP thread count). On the selected rank, threads 0 through N−1
call the selected targets; other threads use their arithmetic-identical native
twins. This preserves every thread's integer work and checksum. Potential
selected-call totals and per-rank counts include all N selected threads.

Final concurrent coverage uses separate `funded-concurrent-<sync>-rotating`
cases for all five synchronization modes, with `--profile-threads 6
--warmup-calls 60000 --epochs 2048 --calls 2000`. Keep their results separate
from the single-thread funded cases. The new source SHA256 is
`726dc38ff93d40255304ce7d72a88549d660376e2a1334daa7743d2300c9b0ae`;
older recorded runs retain their original workload source hash.

The activation probe builds `exec_meter.c` as an independent native executable.
Its wrapper clears `LD_PRELOAD` and `LD_AUDIT` before entering the meter. The meter
checks this condition, records `CLOCK_MONOTONIC`, sets child preload/global mode
and explicit `PEAK_MPI_ACTIVATION_POLICY=immediate` or `post-init`, then calls
`execv`. `sync_pressure.c` optionally reads `PEAK_BENCH_EXEC_BEGIN_NS` and emits
`exec_interval rank=... begin_ns=... end_ns=... seconds=...` after the original
MPI_Finalize returns. This interval includes child environment setup, exec,
loader/constructor activation and application work through MPI return. It excludes
launcher/wrapper startup and the meter's own loader and entry-proof print.
Existing phase and main-through-finalize measurements remain available.

`global_analyze.py --set activation_probe --require-final-members
--require-diagnostics` requires meter data and empty-preload/policy proof on every
rank. It reports `exec_to_finalize` alongside the existing intervals, separately
for both policies; missing meter records are errors. This probe isolates
activation placement and does not assert an overhead improvement. The meter
source SHA256 is `b679162c832e4d3ceafd8cde3ad6247cff731834ff2329a7b312c83b3639a9d8`;
the pressure source with optional meter reporting is
`6243c35a549c29456f0441acd9e1795fd759cb767988d25bcd95aaae07340fc1`.
