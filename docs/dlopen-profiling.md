# Runtime `dlopen` Profiling

## Purpose and Contract

PEAK can profile configured CPU targets that are not present when the process
starts and appear later through `dlopen()`. The loader call itself is never
replaced. PEAK observes the real call with a Gum invocation listener, so the
application keeps its original caller, flags, `$ORIGIN`, RUNPATH, symbol scope,
and binding-mode behavior.

There are two dynamic-attach paths:

- configured targets whose exact names belong to PEAK's built-in FFTW list use
  a guarded synchronous path on Linux when `RTLD_NOLOAD` is available. For a
  matching load whose resolution, safety checks, strict prepare, and Gum attach
  succeed, PEAK installs the target listener before the intercepted `dlopen()`
  returns, so an immediate first call is visible;
- all other unresolved targets attempt admission to a bounded asynchronous
  queue drained by the existing target controller.

This split is about first-call correctness for libraries that commonly load at
runtime. It does not alter the target detach/reattach state machine. Both paths
use the controller mutation guard, listener publication rules, and ordinary
shutdown ownership. They do not use the narrow unguarded attach allowed only
during initial single-task startup.

## Activation and Target Selection

PEAK merges and deduplicates names from `PEAK_TARGET`, `PEAK_TARGET_FILE`, and
`PEAK_TARGET_GROUP` before startup resolution. The `dlopen` listener is installed
only when startup address lookup cannot find at least one requested CPU target.
A target whose address was found at startup but whose later safety check,
strict prepare, or Gum attach was rejected does not by itself arm dynamic
rediscovery. Fully startup-resolved targets therefore add no dynamic-loader
callback path.

Python and other recognized interpreter hosts require
`PEAK_PROFILE_INTERPRETERS=1`; without that opt-in PEAK does not wrap the
interpreter's `main()` or initialize this mechanism.

FFTW classification depends on the exact requested symbol name, not on which
configuration variable supplied it. For example, `PEAK_TARGET=fftw_malloc` and
the same name supplied by `PEAK_TARGET_GROUP=FFTW` are both eligible for the
synchronous FFTW path. A non-FFTW name in the same target set remains
asynchronous. The name `dlopen` itself is reserved for PEAK's support listener
and is not installed as an ordinary profiling target.

## Lifecycle

PEAK installs the inert `dlopen` listener after the initial target tables and
prerequisite support hooks exist. It then installs the remaining enabled support
hooks, including the allocator hook, before starting the target controller.
Dynamic callback admission opens only after the controller starts, and the
heartbeat thread starts afterward. This ordering prevents an on-leave callback
from publishing into uninitialized listener arrays or racing controller startup.

```text
startup
  allocate target tables and publish startup target/prerequisite hooks
  attach the process-lifetime dlopen listener
  install remaining enabled support hooks, including the allocator hook
  start the target controller
  open dynamic callback admission
  start heartbeat, if enabled

successful application dlopen
  run the real loader call with the application's caller and flags
  inspect unresolved-target hints on leave
  run eligible FFTW attach work synchronously
  publish at most one accepted canonical fallback request if work remains
  return the application's real handle unchanged

ordinary shutdown
  stop new callback admission
  perform the guarded SHUTDOWN detach of the dlopen listener
  close queue admission and wait for in-flight callbacks and drains
  discard requests that were still queued
  flush listener teardown

MPI finalizer path
  close callback and queue admission
  wait for in-flight callbacks and drains, then discard queued requests
  leave the process-lifetime listener physically pinned but logically inert

after target teardown
  release retained module handles
```

The shutdown path does not execute remaining queued attach requests. It waits
for already-running work, then releases requests that never started. If
callback or mutation safety cannot be proven, teardown fails closed and leaves
reachable state alive instead of freeing it underneath Gum.

## Application Loader Semantics

PEAK never calls the real application load on its behalf. The intercepted call
keeps its original caller, full flag set, and return value. For PEAK's
private `RTLD_NOLOAD` reference, only the application's `RTLD_LAZY` or
`RTLD_NOW` binding bit is reused; visibility and namespace flags such as
`RTLD_GLOBAL` are not replayed.

The private reference intentionally extends the loaded module's lifetime when a
hook is installed. It does not consume or transfer the application's reference.

## Fast On-Leave Classification

For an admitted successful named load, the common on-leave path reads atomic
unresolved-target hints. It does not take the general-listener lock or scan the
complete target table merely to decide whether work exists. Failed loads and
`dlopen(NULL, ...)` bypass this classification.

While built-in FFTW targets remain unresolved, PEAK probes a fixed set of public
FFTW ABI anchors on the returned handle. Handle-scoped lookup includes loaded
dependencies, so standard FFTW2 and FFTW3 precision, MPI, Fortran, and thread
DSOs are recognized without filename or SONAME rules. When FFTW is the only
unresolved scope, an unrelated `dlopen()` therefore avoids the full FFTW scan
and a second loader reference cycle. Unresolved non-FFTW work may still require
module identity and asynchronous enqueue processing.

The FFTW synchronous scope is derived from exact membership in PEAK's built-in
FFTW group. A similarly prefixed user target is not treated as an FFTW target.
Mixed `PEAK_TARGET` or `PEAK_TARGET_FILE` entries stay on the non-FFTW
asynchronous path.

PEAK borrows the application's filename argument only until the intercepted
call returns. A loader-reported module identity is preferred for retained and
queued work, with that filename as a fallback. PEAK clears errors produced by
its own internal loader and symbol lookups before a successful application call
returns. It does not preserve a pending caller-owned `dlerror()` value from
before the intercepted call.

## Synchronous FFTW Attach

On Linux with `RTLD_NOLOAD`, PEAK obtains a second, unobserved reference using
the application's original binding mode and resolves only still-unresolved FFTW
targets through that handle. Resolution uses ordinary handle-scoped `dlsym()`;
PEAK does not construct a second dependency graph or choose providers by
filename.

Resolved addresses are deduplicated and attached in bounded strict ATTACH
batches. Symbol lookup does not hold the general-listener mutex. Listener
metadata becomes visible only after Gum attach and the controller's stop-window
release both succeed.

This path has no fixed sleep and no `dlopen`-specific completion timeout. It
does use the controller's existing bounded stop and PC-classification protocol.
If that protocol returns a transient result, PEAK tries to enqueue canonical
fallback work for later controller service. If the bounded queue cannot admit
it, that fallback is dropped. Even when accepted, that exceptional call may
return before the retry attaches, so the first call after it is not guaranteed
to be observed. Fatal controller and persistent permission outcomes are
terminal rather than being retried indefinitely.

A complete non-retryable provider scan is cached by the primary loader module
when at least one listener was installed and the module is therefore retained.
Terminal failures among that provider's other symbols do not force another
scan when the same pinned module is reopened. An all-terminal scan that installs
no listener has no retained cache owner and remains uncached. No terminal
failure is marked globally resolved: a later MPI or thread extension with a
different implementation is still eligible. Scans with retryable outcomes,
including scans stopped early for fallback, are not cached.

## Exact Exported-Entry Attribution

Some FFTW distributions export allocator wrappers as one-instruction tail
jumps. Gum normally follows those redirects before attaching. Attaching to the
canonical destination would count unrelated downstream allocator/free calls as
`fftw_malloc` or `fftw_free`.

On Linux x86-64, PEAK recognizes exported `E9 rel32` and `FF 25` RIP-indirect
tail-jump entries and uses the PEAK exact-entry Gum API. Only the attach thread's
first redirect resolution is suppressed; PEAK writes a directly reachable
near jump at the exported boundary. The exact-entry override itself is
attach-time only and adds no cost beyond the normal installed profiling hook on
each call. If a directly reachable trampoline cannot be allocated, the writer
declines safely.

The default PEAK-owned Linux devkit supplies this API. A caller-provided
PEAK-patched x86 devkit is rejected at configure time unless it exports
`GUM_PEAK_EXACT_ATTACH_API_VERSION=1` and
`gum_interceptor_peak_attach_exact()`. This fail-closed capability check avoids
silently accepting the older PC-only patched ABI.

On Linux AArch64, PEAK narrowly recognizes an exported unconditional branch to
the canonical GNU ELF PLT `adrp`/`ldr`/`add`/`br` sequence. It uses Gum's public
forced-relocation option for that exact form and guards the PLT address Gum will
actually mutate. Malformed, unreadable, or noncanonical destinations retain
Gum's default checked behavior.

The selected attach plan is reused for startup attach, runtime attach, and
reattach. It changes where and how Gum writes a patch, not PEAK's hook-state
transitions.

See [PEAK-patched Frida Gum](patched-frida-gum.md) for provider selection and
the linked capability ABI. General prologue and mutation safety remain
documented in [Physical detach controller design](physical-detach-controller.md).

## Asynchronous Dynamic Targets

Non-FFTW targets and synchronous fallbacks attempt admission to a fixed-capacity
queue. Admission can fail when the queue is full or closed or when PEAK cannot
obtain the required `RTLD_NOLOAD` reference. The target controller services
pending detach/reattach work before dynamic attach work, so a busy loader
workload cannot starve the existing lifecycle state machine. A drain processes
no more than the queue length captured at the start of that controller cycle
and no more than the fixed drain budget.

A retry discovered while the controller drains its queue is placed at the tail
without waking the controller again. A later ordinary controller cadence or
independent event starts another attempt. An accepted synchronous before-return
fallback does wake the controller once after its initial enqueue. This prevents
one temporarily unsafe queued library from consuming an entire same-cycle
budget in an immediate retry loop without delaying the initial fallback
unnecessarily.

Dynamic attach retries are limited to transient `TIMEOUT` and
`CLASSIFY_FAILED` outcomes. Generic `ERROR`, `PERMISSION_DENIED`, missing Gum
capability, unsupported targets, and disabled mutation are terminal for that
queued attempt. Partial success retains a separate module reference for hooks
already installed while preserving the original request reference only when a
retry remains actionable.
There is no arbitrary per-request attempt-count or age deadline. A transient
request advances only on later ordinary controller service, and shutdown
discards any request that is still queued. This avoids a fixed timing guess and
also avoids a self-sustaining retry loop.

## Module Lifetime

Any runtime-loaded module that receives a PEAK hook through this dynamic path is
pinned with a retained `RTLD_NOLOAD` reference. The root reference also pins its
dependency closure, preventing an application `dlclose()` from unloading code
while Gum still owns a patch or PEAK still owns its listener metadata. Retained
references are released only after general-listener teardown has physically
removed dynamic hooks and Gum has flushed.

PEAK preserves the real application's `dlopen()` return value and does not
change ownership of the application's reference.

## Performance Boundaries

The `dlopen` listener is installed only when startup address lookup cannot find
at least one requested target. A target found at startup but rejected during a
later safety or attach stage does not by itself enable this path. An admitted
successful named load pays the fixed invocation callback, cancellation guard,
callback-admission gate, and two atomic unresolved-hint reads. Failed loads and
`dlopen(NULL, ...)` return before those reads. While FFTW targets remain
unresolved, the on-leave path may additionally perform at most seven
handle-scoped ABI-anchor `dlsym()` probes. The full FFTW target scan runs only
after an anchor match.

While any non-FFTW target remains unresolved, each successful named load may
create one asynchronous request. The later queue drain performs exact-name
resolution for that request; this cost is not paid when all requested targets
were already resolved at startup.

The first matching FFTW provider bears synchronous resolution and guarded
attach cost before `dlopen()` returns. That can include a strict STOP window and
one or more ATTACH batches, so it is intentionally visible as first-load
latency. There is no separate return timeout that trades first-call correctness
for a guessed delay.

| Bound | Current value |
| --- | ---: |
| Dynamic request queue capacity | 256 |
| Requests drained per controller cycle | 64 |
| Targets in one strict ATTACH batch | 64 |
| Idle controller cadence | approximately 10 ms; independent events may wake it sooner |

Exact-entry redirect selection is attach-time work. It does not add another
branch, lookup, or policy check to an already-installed target listener's hot
callback path.

## Platform and Gum Capabilities

| Build/runtime shape | Dynamic-load behavior |
| --- | --- |
| Linux x86-64, default `auto` provider | PEAK builds the patched PC API plus exact-entry overlay. Linux `RTLD_NOLOAD` enables synchronous FFTW attach; audited `E9` and `FF 25` wrappers stay attributed at their exported entries. |
| Linux AArch64, default `auto` provider | PEAK builds the patched PC API. Linux `RTLD_NOLOAD` enables synchronous FFTW attach; canonical exported `B`-to-PLT entries use the guarded forced-relocation plan. This does not promise every loader thunk is attributed at its four-byte wrapper boundary. |
| Explicit `patched-devkit` provider | Configuration always validates the PEAK Gum PC API; on Linux x86-64 it also validates the exact-entry API. |
| `auto-patched-devkit` with caller-supplied `FRIDA_GUM_*` | Configuration is rejected rather than treating an unknown caller library as the patched devkit. |
| Caller-managed Gum with the `auto` or `prebuilt` provider | Configuration does not validate the PEAK API unless `PEAK_REQUIRE_GUM_PEAK_API=ON`. Strict runtime mutation fails closed with `missing-gum-api` when the linked capability is absent; on Linux x86-64, requested validation also requires the exact-entry API. |
| Other platforms | The default provider is stock and there is no supported synchronous FFTW first-call guarantee. Dynamic fallback and safe module retention remain subject to `RTLD_NOLOAD`, Gum, and strict-mutation capabilities. |

See [PEAK-patched Frida Gum](patched-frida-gum.md) for exact CMake provider and
validation rules.

## Diagnostics

| Variable | Effect |
| --- | --- |
| `PEAK_DLOPEN_DEBUG=1` | Enable cumulative diagnostic snapshots at shutdown, release, and timeout lifecycle points; stderr output also requires `PEAK_VERBOSITY=debug`. |
| `PEAK_DLOPEN_TRACE_PATH=/path/file.csv` | Append the same lifecycle snapshots to a CSV file. |

The diagnostics include enqueue, drain, requeue, full/closed/`RTLD_NOLOAD`
drops, failed requeue, partial success, retained handles, maximum depth, current
length, capacity, and drain budget. They are cumulative for the process
lifetime and are intended for debugging rather than normal profiling output.
They are not emitted for every enqueue or drain.

CSV rows have no header and use this fixed order:

```text
event,enqueued,drained,requeued,dropped_full,dropped_closed,dropped_noload,
dropped_requeue,partial_success,retained_handles,max_depth,queue_length,
capacity,drain_budget
```

A drop counter means PEAK could not retain that dynamic request, so a still
unresolved symbol may remain uncovered. `partial_success` means at least one
listener was installed while the same scan also produced retryable work. That
retry may still be dropped if PEAK cannot preserve its request handle.

Unresolved plain C symbols do not trigger Gum's expensive global C++ demangled
scan. The asynchronous `dlopen` path uses exact-name `dlsym()` when their DSO
appears. `PEAK_ENABLE_CXX_SYMBOL_SCAN=1` restores the legacy broad startup scan
for a short C++ target name without obvious C++ spelling such as `::`, `(`,
`<`, or `operator`; it does not turn the post-load queue into a demangled C++
resolver.

## Supported Boundary

PEAK observes the public `dlopen()` entry only. `dlopen(NULL, ...)` and private
loader entries do not start target discovery. An admitted callback disables
thread cancellation from on-enter until on-leave cleanup completes, then
restores the caller's previous cancellation state.

The synchronous first-call contract begins at public `dlopen()` on-leave. It
does not cover:

- calls made by DSO constructors before the loader returns;
- `dlmopen()` namespaces;
- exact IFUNC resolver or wrapper attribution beyond the audited entry forms;
- a callback bypassed by `pthread_exit()` or a non-local jump;
- continued profiling in a multithreaded `fork()` child that does not `exec`.

A fresh child callback is rejected by a PID guard before it touches PEAK-owned
mutexes or allocations. This is fail-open protection for the application; it
does not make Gum, the loader, or PEAK generally fork-safe. Forking from inside
an already-entered `dlopen` callback is also outside the supported contract.
