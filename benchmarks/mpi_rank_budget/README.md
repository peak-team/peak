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
