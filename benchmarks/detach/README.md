# Detach and callback benchmarks

`benchmark_callback_hotpath_matrix` measures the attached, steady-state target
callback with the default 0.1 second heartbeat. Heartbeat detach policies and
the call-count detach policy are disabled so every measured target call stays
instrumented.

The matrix covers 1, 2, 8, 32, and 64 threads and calibrated target bodies of
approximately 10 ns, 100 ns, and 1 µs. It reports aggregate `ns_per_call`,
per-thread-normalized `thread_ns_per_call`, calls/second, throughput scaling,
scaling efficiency, and slowdown versus the same executable without PEAK.
Workers are pinned physical-core-first within the process affinity mask. The
single-thread slowdown and ns/call gates use the one-thread samples; concurrent
samples are gated by absolute throughput scaling efficiency and scaling
efficiency relative to the same uninstrumented target.

Absolute scaling is gated at every point up to the number of available physical
cores. Relative scaling against the equally oversubscribed uninstrumented run
is gated at all five thread counts, so ordinary CI still detects PEAK-specific
contention at 8, 32, and 64 threads. SMT or oversubscribed results are not
treated as evidence of physical-core scalability; that evidence must come from
a sufficiently large runner such as the Vista and Frontera benchmark jobs.
Limits are configurable with:

- `PEAK_HOTPATH_PERF_MAX_SLOWDOWN`
- `PEAK_HOTPATH_PERF_MAX_NS_PER_CALL`
- `PEAK_HOTPATH_PERF_MIN_SCALING_EFFICIENCY`
- `PEAK_HOTPATH_PERF_MIN_RELATIVE_SCALING_EFFICIENCY`

For an explicit before/after comparison, preserve the `libpeak.so` built from
the baseline revision and run:

```sh
python3 benchmarks/detach/run_callback_hotpath_matrix.py \
  --exe build/benchmarks/detach/benchmark_callback_hotpath \
  --current-libpeak build/src/libpeak.so \
  --baseline-libpeak /path/to/baseline/libpeak.so
```

`benchmark_entry_attach` (Linux x86-64/AArch64) reports JSON lines for cold and
1000 warm relocation queries over 129, 4097, and 65537 instruction bodies, plus
real attachment and callback checks. On AArch64 it also measures 128-branch
suffix and entry-target chains, checking both conditional paths before and after
detach. It also measures 100 and 1000 small targets
in transactions of 64, including first/last transaction times. It checks behavior
and counts without imposing hardware-dependent timing limits. Run it separately:

```sh
ctest --test-dir build -R '^benchmark_entry_attach$' -V
```

## MPI critical-path budget witness

Build the fixed-work benchmark with `-DPEAK_BUILD_BENCHMARKS=ON` and
`-DPEAK_ENABLE_MPI=ON` in an MPI-enabled PEAK build.

```sh
cmake --build build --target benchmark_mpi_critical_work
mpiexec -n 2 build/benchmarks/detach/benchmark_mpi_critical_work rotating 4 1000 64
mpiexec -n 2 build/benchmarks/detach/benchmark_mpi_critical_work common 4 1000 64
```

The arguments are schedule, even round count, useful integer iterations per
rank per round, and even hot calls per round across both ranks. Each rank
invokes its own hot target exactly `rounds × hot / 2` times and the common
sentinel once per round. In `rotating`, one rank performs all hot calls in
each round, and the owner alternates. In `common`, both ranks perform half
the hot calls in every round. Each round ends with an MPI barrier. Both
schedules execute the same total useful and target work per rank and yield
the same checksums. The benchmark uses fixed iteration counts without sleeps
or deadline-paced work.

For a native, fully attached, q5 and q10 comparison, use a quiet two-rank
placement on distinct physical cores and supply the MPI launcher and PEAK
library from the same MPI environment.

```sh
python3 benchmarks/detach/run_mpi_critical_work.py \
  --exe build/benchmarks/detach/benchmark_mpi_critical_work \
  --libpeak build/src/libpeak.so --launcher 'mpiexec -n 2' \
  --rounds 128 --base 14000000 --hot 16000 --cooldown-ms 100 --out build/mpi-critical-results
```

The runner requests the MPI active clock. Admission depends on MPI thread
support, world size, and clock qualification. To check admission separately
from the timed runs, use the same MPI environment and placement:

```sh
mkdir -p build/mpi-active-clock-qualification
mpiexec -n 2 env \
  LD_PRELOAD="$PWD/build/src/libpeak.so" \
  PEAK_TARGET=fixture_hot_a,fixture_hot_b,fixture_sentinel \
  PEAK_ENABLE_GLOBAL_HEARTBEAT=1 PEAK_ENABLE_PER_TARGET_HEARTBEAT=0 \
  PEAK_MPI_ACTIVE_CLOCK=1 PEAK_HEARTBEAT_INTERVAL=0.1 \
  PEAK_OUTPUT_AGGREGATION=local PEAK_TEXT_OUTPUT=0 PEAK_VERBOSITY=debug \
  PEAK_STATSLOG_PATH="$PWD/build/mpi-active-clock-qualification/qualification.csv" \
  build/benchmarks/detach/benchmark_mpi_critical_work common 128 14000000 16000 \
  > build/mpi-active-clock-qualification/qualification.log 2>&1
grep -E 'fixed rank=|peak mpi active clock' build/mpi-active-clock-qualification/qualification.log
```

Match each rank's PID in its `fixed rank=...` line to a `[peak mpi active clock]`
summary. On multiple hosts, include the hostname in the match using tagged
launcher output. An `unavailable` diagnostic indicates a failed clock check;
a missing summary leaves admission unconfirmed for that rank.

The runner holds these counts fixed for every budget and schedule. It reverses
the instrumented order in the second block and brackets each block with native
runs. The fully attached arm disables heartbeat detach and must record every
offered call with zero drops or lifecycle transitions. q5 and q10 use the
global heartbeat and MPI active clock with per-target heartbeat disabled.
For the MPI epoch fallback, the 5% and 10% global targets correspond to
estimated hook cost above 4.76% and 9.09% of the observed collective interval,
respectively, in each of two corroborating epochs. This is a local policy
trigger. The native brackets independently measure application slowdown.
Both budget arms enable reattach and use a 100 ms cooldown by default,
with the hibernation cycle unset. Use `--cooldown-ms` to set a different cooldown or
`--no-reattach` to inspect first-detach behavior. The native and fully
attached controls keep the same settings in either case. Each arm has a
launch timeout. For a quick functional check, pass the small counts from the
native example.

`launches.csv` gives the slowest rank's work interval, overhead relative to
the mean of its two native endpoints, and the range from the individual
endpoints. `ranks.csv` preserves timing and equal-work checksums.
`coverage.csv` records offered and actual calls per target and rank from
PEAK's rank-local CSVs. An expected target without a report row has zero
recorded calls and unknown drop and lifecycle counters, and is flagged as
missing. Known drops have a separate status. The runner also prints warnings
and records both counts in `launches.csv`. The raw reports,
commands and logs remain in the output directory. Shared runners can vary in
native timing and critical-rank behavior, so the harness checks work and
coverage without a timing assertion.
