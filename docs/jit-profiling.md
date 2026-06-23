# JIT Profiling Design

## Goal

PEAK profiles function entries with Gum hooks. For JIT code, executable memory
alone is not enough to preserve that model: anonymous RX pages do not reliably
provide a function name, entry address, code size, generation, or lifetime.
JIT profiling therefore starts from runtime-provided metadata and only attaches
when PEAK can map a requested `PEAK_TARGET` name to an executable code range.

## Current Provider

The first implemented provider consumes Linux perf-map metadata. Enable it with:

```bash
PEAK_JIT_ENABLE=1
PEAK_JIT_PROVIDER=perfmap
PEAK_TARGET=jit_function_name
LD_PRELOAD=libpeak.so ./target_application
```

When JIT profiling is explicitly enabled, PEAK allows known JIT runtime
executables such as `node`/`nodejs` through its command wrapper even though those
commands are normally skipped as tool/runtime binaries.
The runtime must still publish metadata. For Node/V8, use V8 perf-map emission,
for example:

```bash
PEAK_JIT_ENABLE=1 PEAK_JIT_PROVIDER=perfmap PEAK_TARGET=peakJitV8Target \
LD_PRELOAD=libpeak.so node --perf-basic-prof app.js
```

The provider reads `/tmp/perf-<pid>.map` by default. Tests and runtimes with a
custom metadata path may set:

```bash
PEAK_JIT_MAP_PATH=/path/to/perf.map
```

Each perf-map row must be:

```text
<hex-address> <hex-size> <symbol name>
```

The address and size fields must be separated by whitespace, and the size and
symbol name must also be separated by whitespace. Malformed complete rows are
skipped so a later valid row can still be processed.

Names are matched exactly against unresolved `PEAK_TARGET` entries by default.
For V8-style optimized perf-map rows, PEAK also lets the logical JavaScript
function name match `JS:*name` and `LazyCompile:*name`. Non-optimized V8 tiers
such as `JS:~name` or `JS:^name` remain metadata-only and are not treated as
optimized entry targets. The symbol name may contain spaces; PEAK treats the
rest of the line after address and size as the name. Before attach, the
provider verifies that the full `address..address+size` range is currently
executable in `/proc/self/maps`.

Optional diagnostics:

```bash
PEAK_JIT_TRACE_PATH=/path/to/jit-trace.csv
```

Trace rows show provider enablement and each processed perf-map record with an
`attached`, `not-matched`, `not-executable`, `not-executable-retry`,
`not-executable-timeout`, `partial-record`, `attach-retry`, or `attach-failed`
result. Complete lines that exceed PEAK's bounded read buffer are skipped with
`overlong-record`; true EOF partial rows are retained as `partial-record`.
String fields are emitted as CSV fields, so names containing commas or quotes
remain parseable.

`partial-record` is the only normal result that leaves the reader offset at the
current record, because it means the runtime is still appending that line.
`not-executable-retry` and `attach-retry` are stored as pending retry records
keyed by address, size, and name while the reader continues scanning later
metadata. This prevents a stale or temporarily refused target row from hiding a
later executable generation of the same JIT function.

The provider also bounds each controller-thread drain pass. The default budget
is 1024 perf-map or pending records per pass and can be adjusted with:

```bash
PEAK_JIT_DRAIN_RECORD_BUDGET=4096
```

When the budget is exhausted, the provider reports pending work and lets the
controller loop process detach/reattach requests before resuming JIT metadata
drain.

## Controller Ownership

JIT providers do not call Gum directly. The perf-map provider runs from the
general listener controller thread and publishes a matching symbol through a
private controller-only attach bridge. That bridge:

1. requires the current thread to be the general listener controller;
2. matches a `PEAK_TARGET` slot and ignores duplicate rows for an already
   attached code address;
3. prepares the attach through the existing strict detach controller;
4. attaches Gum to the JIT entry address;
5. publishes `hook_address`, listener state, and demangled display name only
   after Gum attach succeeds.

This keeps JIT attach serialized with static symbol attach, `dlopen` dynamic
attach, detach, reattach, and shutdown.

If a later perf-map row publishes the same logical target at a new executable
address, PEAK allocates another internal hook slot with the same display name.
This lets tiered or recompiled JIT functions remain independently attachable
while CSV readers can still aggregate by the `function` column. If an MPI job
sees duplicate slot names from multiple JIT generations, different dynamic hook
counts across ranks, or different hook slot identities, PEAK avoids an invalid
collective reduction and writes rank-local output instead.

## Backend Independence

JIT discovery is independent of the strict stop backend. The provider only
parses metadata and validates executable mappings. The actual Gum attach uses
the same controller prepare/finish path as static and `dlopen` targets, so it
works with:

- default strict auto backend selection;
- strict auto fallback when the helper is unavailable;
- forced signal backend;
- helper backend where supported.

The test suite includes explicit perf-map JIT coverage for the default,
auto-no-helper, and signal paths.

This means metadata discovery can succeed while Gum attach is refused by a
strict backend that is unavailable for the current process or platform. That is
reported as `attach-failed` or `attach-retry`; it is not treated as a metadata
provider failure.

## Lifetime Boundary

Perf-map metadata is load-only. It does not provide a reliable unload, move, or
generation record. PEAK therefore treats perf-map JIT attach as valid only while
the JIT code mapping remains executable and alive through PEAK teardown. A row
whose range is not executable at drain time is ignored unless it matches a
configured target; matching non-executable rows are retried because many JITs
publish metadata just before flipping code pages to executable. The retry is
bounded by `PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS` (default: `1000`) so stale
target rows cannot starve later valid generations forever. If a runtime may move
or free code aggressively, a richer provider such as jitdump or the GDB JIT
interface is required before PEAK can safely support long-running code-cache GC,
unload, or move events with reattach.

The provider processes the perf-map file as append-only metadata and remembers
the last drained offset. If the file is truncated, the offset resets and PEAK
drains from the start; if the path is replaced by a different inode, PEAK also
resets its offset and pending retry records. During shutdown, the controller
drains until no retry is pending or the shutdown drain deadline expires so
records emitted shortly before process exit are not missed after one transient
retry. At the shutdown deadline, PEAK performs one final drain that treats
matching non-executable pending rows as timed out.

Duplicate rows for the same logical target and code address are treated as
metadata repeats and do not create another listener. If a JIT unmaps a code
object and later reuses the same virtual address for a different generation,
plain perf-map metadata cannot prove that lifetime transition. That case needs
an unload/move-aware provider before PEAK can safely detach and replace the old
listener.

## What RX Tracking Is Not

`mmap`/`mprotect` RX tracking can identify executable memory creation, but it
cannot generally recover function names or boundaries. PEAK does not infer
per-function hooks from RX pages alone. The negative JIT test keeps the provider
enabled but emits no metadata; it must record zero JIT calls.

## Test Coverage

The default test suite includes:

- a native mmap JIT fixture on Linux x86_64 and aarch64;
- perf-map positive attach and call-count validation;
- RX-only negative coverage;
- final-drain coverage for late metadata;
- final-drain retry coverage for late metadata that first hits a transient
  attach refusal;
- final-drain stale-row coverage proving shutdown drain can advance past a
  stale non-executable target row before exit;
- partial-row coverage proving append-in-progress perf-map records are retried;
- pre-exec coverage proving target rows emitted before `PROT_EXEC` are retried;
- two-generation coverage proving a later code version of the same target can
  be attached and counted;
- heartbeat-on two-generation coverage proving dynamic JIT table growth is safe
  while the heartbeat monitor is active;
- stale-then-valid coverage, including the default non-exec retry timeout,
  proving a dead non-executable target row does not starve a later executable
  generation or delay it until after the call window;
- duplicate-row coverage proving repeated metadata for the same code address is
  a no-op after the first attach;
- malformed-row coverage proving a bad complete row does not turn into a bogus
  hook and does not block a later valid row;
- overlong-row coverage proving a complete line larger than the provider buffer
  is skipped instead of being mistaken for an append-in-progress partial row;
- retry coverage proving transient attach refusals do not consume perf-map
  records;
- V8 optimized-name alias coverage for both `JS:*name` and
  `LazyCompile:*name`;
- CSV trace coverage for V8-style names containing commas and quotes;
- default, auto-no-helper, and signal backend variants;
- a Node/V8 perf-map metadata smoke test that skips cleanly if Node is absent.

The Node/V8 metadata test validates framework metadata emission. A stricter
Node/V8 PEAK preload test is available behind the CMake option
`PEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS`; it is not part of default CI because
the current strict attach backend can legitimately refuse JIT attach in some
runtime/process configurations even though metadata discovery succeeds.

## Provider Roadmap

The current implementation can profile any JIT-produced function that publishes
a usable perf-map row while its code range is executable and alive. To cover
JITs that do not emit perf maps, or JITs that require precise unload/move
lifetime semantics, PEAK needs additional metadata providers rather than RX-page
guessing. The next provider candidates are Linux `jitdump` and the GDB JIT
interface because they can carry richer generation and lifetime information than
plain perf maps.
