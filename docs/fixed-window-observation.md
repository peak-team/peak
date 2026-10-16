# Optional fixed-window fixture observation

`test/detach_runtime/window_observation/window_observe.h` is a fixture-only
observer, disabled unless `PEAK_FIXED_WINDOW_OBSERVE=1` is defined. With the
default value, `WINDOW_REQUEST(fn,id,op)` and `WINDOW_DRAIN(fn,timeout)` expand
to the original calls. It changes no library source or target/reader hot loop.
Use the wrappers only from one main thread, with two live reader threads.

When enabled, initialize the two reader CPU clocks and take cut 0 before the
integer `CLOCK_MONOTONIC` window starts. Wrap the original detach, reattach and
drain calls in that window. Take cut 1 after its end, before readers exit, then
print observations after the window. Keep the original calls, arguments, fixed
round count and work amount. The observer forwards each call once and preserves
the API's errno across its clock reads. Its fixed buffer holds 128 API records;
overflow is reported, not silently treated as a complete measurement.

API records contain outer monotonic wall endpoints and inner main-thread CPU
endpoints. Reader CPU reads have monotonic brackets and explicit clock return
codes, including whether `pthread_getcpuclockid` was called. A footer supplies
the recorded count and overflow count. The fixture must separately emit:

```
WINDOW schema=1 rank=0 start_ns=... end_ns=... elapsed_ns=...
```

The existing fixed-work fixture's six work-count fields are also accepted and
retained, as one complete group: `published_calls_lo/hi`, `target_calls_lo/hi`,
`publication_lag_bound` and `lifetime_calls`. Partial work-count groups fail.

Decode **saved output**, with expected counts taken from the fixed protocol:

```
python3 test/detach_runtime/window_observation/decode_window_observation.py saved.log \
  --rank 0 --detach-count 2 --reattach-count 2 --drain-count 4
python3 test/detach_runtime/window_observation/test_decode_window_observation.py
```

The decoder requires a single complete window/footer, contiguous API indices,
the expected operation counts, successful calls/clocks, and all four reader
cuts bracketing the window. Missing fields or records, duplicate records,
uninitialized clocks, failed calls and overflow return a nonzero status and
`measurements: null`. Other ranks never fill a missing record for the selected
rank. Test fixtures exercise validation only; they are not performance evidence.

The reported API union clips serialized outer wall envelopes to the window.
CPU for a clipped API scope is unknown, not proportionally apportioned. API CPU
is an inner sample delta, not uninstrumented call cost. Reader CPU deltas cover
slightly wider sampling intervals, with the overhang brackets retained; they
are not exact-window CPU. Parallel thread CPU cannot be subtracted from wall
time. The window outside API envelopes still contains worker/reader activity,
scheduling and any explicit waits. Clock samples have no calibrated physical
error bound. Keep observation-enabled results separate from native performance.

On Linux x86-64 with the real helper enabled, CMake also builds
`test_window_observe_smoke`. It requires a real preloaded PEAK library, the real
helper path, target `window_observation_target`, heartbeat and automatic policies
disabled, manual physical detach/reattach enabled, and a host permitting the
helper's ptrace protocol. Invoke it once with an external wall deadline and
process reaping; inspect the complete exit/signal receipt and helper trace.
The executable runs two cycles, asserts physical entry-patch and public-state
transitions, and collects one worker plus two readers. Four explicit 2ms sleeps
are inside its window. It is an observability smoke, not an equal-work benchmark,
cancellation test, errno-transparency runtime test or performance qualification.
It is deliberately not registered as an automatic CTest runtime.

## Comparing existing fixed-work windows

For pair p and rank r, first compute `d[p,r] = Wcandidate[p,r] - Wmain[p,r]`.
The pair metric is the median of those same-rank differences; the six-pair
metric is the median of the six pair metrics. A selected controller-owner rank
can have a much larger difference than its pair median. Do not combine a
selected-rank result with the rank-median result as one cost decomposition.

If a candidate's same-rank whole-runtime STOP charge C covers the window's
completed mutations, recorded accounting gives
`extra outside STOP >= d[p,r] - C`. This is an accounting inequality, conditional
on coverage and nonnegative same-rank charges. A strict physical-time bound
also needs an upper error bound for C's timestamp conversion, per-window
rounding, clock errors and coverage. Printing nine decimals alone does not
supply that bound; a one-nanosecond print allowance cannot replace it.
