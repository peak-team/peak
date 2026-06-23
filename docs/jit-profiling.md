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

The provider reads `/tmp/perf-<pid>.map` by default. Tests and runtimes with a
custom metadata path may set:

```bash
PEAK_JIT_MAP_PATH=/path/to/perf.map
```

Each perf-map row must be:

```text
<hex-address> <hex-size> <symbol name>
```

Names are matched exactly against unresolved `PEAK_TARGET` entries. The symbol
name may contain spaces; PEAK treats the rest of the line after address and
size as the name. Before attach, the provider verifies that the full
`address..address+size` range is currently executable in `/proc/self/maps`.

Optional diagnostics:

```bash
PEAK_JIT_TRACE_PATH=/path/to/jit-trace.csv
```

Trace rows show provider enablement and each processed perf-map record with an
`attached`, `not-matched`, or `not-executable` result.

## Controller Ownership

JIT providers do not call Gum directly. The perf-map provider runs from the
general listener controller thread and publishes a matching symbol through a
private controller-only attach bridge. That bridge:

1. requires the current thread to be the general listener controller;
2. matches only an unresolved `PEAK_TARGET` slot;
3. prepares the attach through the existing strict detach controller;
4. attaches Gum to the JIT entry address;
5. publishes `hook_address`, listener state, and demangled display name only
   after Gum attach succeeds.

This keeps JIT attach serialized with static symbol attach, `dlopen` dynamic
attach, detach, reattach, and shutdown.

## Backend Independence

JIT discovery is independent of the strict stop backend. The provider only
parses metadata and validates executable mappings. The actual Gum attach uses
the same controller prepare/finish path as static and `dlopen` targets, so it
works with:

- default strict auto backend selection;
- compatibility/no-helper mode;
- forced signal backend;
- helper backend where supported.

The test suite includes explicit perf-map JIT coverage for the default,
compatibility, and signal paths.

## Lifetime Boundary

Perf-map metadata is load-only. It does not provide a reliable unload, move, or
generation record. PEAK therefore treats perf-map JIT attach as valid only while
the JIT code mapping remains executable and alive through PEAK teardown. A row
whose range is not executable at drain time is ignored. If a runtime may move or
free code aggressively, a richer provider such as jitdump or the GDB JIT
interface is required before PEAK can safely support long-running code-cache
GC with reattach.

The provider processes the perf-map file as append-only metadata and remembers
the last drained offset. If the file is truncated, the offset resets and PEAK
drains from the start. During shutdown, the controller performs one final
provider drain before it exits so records emitted shortly before process exit
are not missed.

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
- default, compatibility, and signal backend variants;
- a Node/V8 perf-map metadata smoke test that skips cleanly if Node is absent.

The Node/V8 test validates framework metadata emission. It does not force PEAK
to preload into `node`, because PEAK's existing command filter intentionally
skips tool/runtime binaries such as `node`.
