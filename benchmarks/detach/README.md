# Detach and callback benchmarks

`benchmark_callback_hotpath_matrix` measures the attached, steady-state target
callback with the default 0.1 second heartbeat. Heartbeat detach policies and
the call-count detach policy are disabled so every measured target call stays
instrumented.

The matrix covers 1, 2, 8, 32, and 64 threads and calibrated target bodies of
approximately 10 ns, 100 ns, and 1 µs. It reports aggregate `ns_per_call`,
per-thread-normalized `thread_ns_per_call`, calls/second, throughput scaling,
scaling efficiency, and slowdown versus the same executable without PEAK.
The slowdown gate uses the one-thread samples; concurrent samples are gated
independently by aggregate ns/call and throughput scaling efficiency.

The CTest performance gate becomes active when the process affinity contains
at least 64 CPUs. Limits are configurable with:

- `PEAK_HOTPATH_PERF_MAX_SLOWDOWN`
- `PEAK_HOTPATH_PERF_MAX_NS_PER_CALL`
- `PEAK_HOTPATH_PERF_MIN_SCALING_EFFICIENCY`

For an explicit before/after comparison, preserve the `libpeak.so` built from
the baseline revision and run:

```sh
python3 benchmarks/detach/run_callback_hotpath_matrix.py \
  --exe build/benchmarks/detach/benchmark_callback_hotpath \
  --current-libpeak build/src/libpeak.so \
  --baseline-libpeak /path/to/baseline/libpeak.so
```
