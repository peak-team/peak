# Physical Detach Controller Design

## Goal

PEAK's core advantage is physical detach: once a hot target is detached, the
target application should execute the original function entry path with no PEAK
or Gum trampoline overhead. The design below keeps that property while removing
the current races around Gum detach, reattach, dynamic attach, and teardown.

The key idea is to stop treating detach as a cooperative pthread operation. PEAK
should manufacture safe detach opportunities with debugger-grade thread control
and Gum-defined safe regions.

## Implemented Split

This branch keeps two explicit runtime modes:

- Compatibility mode, the default, preserves the existing physical detach
  behavior for stock Frida Gum. Gum mutations are serialized through the PEAK
  controller, incomplete tracked-thread snapshots fail closed, and listener
  state is kept alive until Gum teardown flushes. The legacy signal pause path
  is still only a best-effort mitigation and does not see every OS thread.
- Strict mode is selected with `PEAK_REQUIRE_SAFE_DETACH=1` or
  `PEAK_SAFE_DETACH_MODE=strict` / `helper` / `debugger`. Strict mode refuses
  target-function Gum attach, detach, reattach, and shutdown detach unless PEAK
  can use the patched-Gum/helper path described below. `dlopen` replace/revert
  is also guarded because it changes a process-wide Gum patch site. With stock
  Gum, startup target hooks are skipped, already-attached target hooks remain
  attached, and PEAK logs why target mutation was skipped. Process-lifetime
  support hooks for pthread/syscall/MPI/CUDA keep explicit startup/shutdown
  ordering outside steady-state target detach. Malloc profiling still attaches
  after PEAK initialization so it does not profile PEAK setup allocations, but
  shutdown stops the target controller before malloc detach so allocator Gum
  mutations do not overlap target physical detach/reattach windows.

The signal pause path is not considered strict-safe. It cannot classify program
counters, cannot advance only audited PEAK/Gum safe regions, and cannot prove
that untracked or newly created threads are outside Gum code.

## Current Failure Mode

The existing implementation lets callbacks, heartbeat, `dlopen`, and shutdown
all interact with shared Gum state:

- callbacks may physically detach their own listener;
- heartbeat may reattach from a background thread;
- `dlopen` may publish new listeners without using the detach lock;
- shutdown frees listener-owned arrays immediately after detach;
- signal-based pausing accepts timeouts and uses a global ack semaphore.

These measures reduce some windows, but they do not prove that every thread is
outside the code and state being mutated.

## Core Invariants

1. Only one in-process controller performs steady-state target-hook lifecycle
   operations. Target-function `gum_interceptor_attach`,
   `gum_interceptor_detach`, Gum transactions, listener allocation/free, and
   dynamic target attach publication are controller-owned. `dlopen` replace and
   revert also go through the controller guard. Process-lifetime support hooks
   are allowed to use direct Gum calls only when they cannot overlap the active
   target-controller mutation window. Malloc detach is ordered after controller
   stop and before `dlopen`/Gum teardown so the memory profiler does not observe
   Gum teardown allocations.

2. Hot callbacks never call Gum lifecycle APIs. They may request detach or
   reattach by setting atomic state or enqueueing a controller request.

3. Physical detach does not require `in_flight == 0`. Active samples can be
   abandoned. Gum already keeps trampoline memory alive until its internal usage
   counter drains, so PEAK must not free listener-owned PEAK data until Gum says
   teardown is flushed.

4. Detach proceeds only when every thread is either at a safe PC or has been
   advanced to a PEAK/Gum-defined safe PC.

5. If PEAK cannot classify or safely evacuate a thread, detach is skipped or
   retried. It must not wait forever and must not detach optimistically.

## Architecture

### In-process controller

The controller is the only PEAK component allowed to mutate Gum hooks. It owns:

- hook state transitions;
- Gum transactions;
- `array_listener`, `hook_address`, and demangled-name publication;
- deferred listener destruction;
- dynamic symbol attaches after `dlopen`;
- final shutdown order.

Suggested per-hook states:

```c
typedef enum {
    PEAK_HOOK_UNRESOLVED = 0,
    PEAK_HOOK_ATTACHED,
    PEAK_HOOK_DETACH_REQUESTED,
    PEAK_HOOK_DETACHING,
    PEAK_HOOK_DETACHED,
    PEAK_HOOK_REATTACH_REQUESTED,
    PEAK_HOOK_REATTACHING,
    PEAK_HOOK_SHUTDOWN
} PeakHookState;
```

Callbacks and heartbeat only request transitions. The controller executes them.

### External helper

The external helper provides thread control and PC classification:

1. enumerate `/proc/<pid>/task`;
2. stop all threads except the controller with `PTRACE_SEIZE` plus
   `PTRACE_INTERRUPT` or an equivalent debugger-grade mechanism;
3. read each thread's registers;
4. report PC snapshots to the in-process controller, which classifies them
   against PEAK/Gum metadata;
5. redirect only threads in audited evacuation regions to known safe labels;
6. revalidate immediately before applying EVACUATE writes so any thread that
   appeared during the STOP/response window is caught before code bytes change;
7. report whether the process is safe for controller detach;
8. resume all stopped threads after ordinary mutations, or send an explicit
   `SHUTDOWN` command that detaches tracees and exits the helper for final hook
   shutdown. Helper protocol writes suppress `SIGPIPE` so a closed controller
   socket cannot kill the helper before its held-thread cleanup path runs.

The helper should not call Gum APIs directly. Gum mutation remains in-process so
it can operate on Gum's live data structures.

The current code implements this Linux x86_64 helper path behind the
patched-Gum capability check. The default Linux x86_64 CMake path now constructs
a PEAK-patched Frida Gum devkit by copying the downloaded devkit and appending
PEAK's PC API implementation to the archive. Stock Gum still reports
`missing-gum-api` in strict mode. Non-x86_64 Linux builds do not build the helper
until the register access path is implemented with the appropriate `ptrace`
regset API. When `PR_SET_PTRACER` is unavailable and returns `EINVAL`/`ENOSYS`,
PEAK falls back to the helper's real `ptrace` attempt; actual permission failures
still fail closed.

## PC Classification

For each hook, the helper/controller classify every stopped thread:

```c
typedef enum {
    PEAK_PC_SAFE = 0,
    PEAK_PC_AT_PATCH_ENTRY,
    PEAK_PC_IN_TARGET_BODY,
    PEAK_PC_IN_PEAK_EVACUATION,
    PEAK_PC_IN_GUM_EVACUATION,
    PEAK_PC_IN_GUM_CRITICAL,
    PEAK_PC_UNKNOWN
} PeakPcClass;
```

Detach rules:

- `PEAK_PC_SAFE`: safe.
- `PEAK_PC_AT_PATCH_ENTRY`: safe. The original prologue can be restored while
  all threads are stopped; the thread has not entered the trampoline yet.
- `PEAK_PC_IN_TARGET_BODY`: safe. The thread is already past the overwritten
  prologue, and detach affects future entries.
- `PEAK_PC_IN_PEAK_EVACUATION`: advance this thread to a PEAK safe label.
- `PEAK_PC_IN_GUM_EVACUATION`: advance this thread to a Gum safe label.
- `PEAK_PC_IN_GUM_CRITICAL` or `PEAK_PC_UNKNOWN`: do not detach unless Gum has
  been patched so the range has a bounded evacuation route.

The implemented overlay is intentionally conservative: it classifies the
per-function trampoline slice and shared enter/leave thunk ranges, but the helper
only redirects a stopped thread when the Gum API returns a concrete safe PC.
The currently audited direct rewrite corridor is the exact `on_enter_trampoline`
label, which redirects to the target `function_address`. Interior enter
trampoline PCs, leave/invoke trampoline PCs, and shared thunk/dispatch PCs do not
have an audited direct rewrite yet; they fail closed with `CLASSIFY_FAILED` and
are retried later. This keeps strict mode from guessing its way through Gum
internals.

## Gum Devkit Changes

The current CMake downloads a prebuilt Frida Gum devkit. On Linux x86_64 the
default `auto` provider copies that devkit to a PEAK-patched devkit directory,
appends the PEAK PC API implementation object to `libfrida-gum.a`, and appends
the public API declarations to `frida-gum.h`. A caller may also provide a patched
devkit explicitly with `PEAK_FRIDA_GUM_PROVIDER=patched-devkit`.

The selected devkit must expose:

```c
#define GUM_PEAK_PC_API_VERSION 1

typedef struct _GumPeakFunctionContext GumPeakFunctionContext;

typedef enum {
    GUM_PEAK_PC_SAFE = 0,
    GUM_PEAK_PC_AT_PATCH_ENTRY,
    GUM_PEAK_PC_IN_ENTER_TRAMPOLINE,
    GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE,
    GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE,
    GUM_PEAK_PC_IN_DISPATCH,
    GUM_PEAK_PC_UNKNOWN
} GumPeakPcState;

gboolean gum_interceptor_peak_classify_pc(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    gpointer pc,
    GumPeakFunctionContext ** ctx,
    GumPeakPcState * state);

gpointer gum_interceptor_peak_safe_pc(
    GumPeakFunctionContext * ctx,
    gpointer pc,
    GumPeakPcState state);

gboolean gum_interceptor_peak_get_function_patch(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    guint8 * active_patch,
    guint8 * original_prologue,
    guint * prologue_len);

guint gum_interceptor_peak_abi_fingerprint(void);

gboolean gum_interceptor_peak_get_pc_diagnostics(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    GumPeakPcDiagnostics * diagnostics);
```

Gum already has the key internal metadata:

- function address;
- overwritten prologue length;
- trampoline slice;
- enter, invoke, and leave trampoline addresses;
- `trampoline_usage_counter`;
- pending destroy task queue.

The PEAK-specific Gum API should turn that internal knowledge into reliable PC
classification and safe-point addresses.

## Evacuation Corridors

Unsafe threads should not be advanced through arbitrary C runtime code. They may
only be advanced through audited PEAK/Gum evacuation corridors.

Evacuation code must be:

- bounded;
- no locks;
- no malloc/free;
- no I/O or logging;
- no condition waits;
- no calls into target code;
- no calls into CUDA, MPI, GLib, or libc except audited atomic/register-level
  operations;
- independent of every other application thread.

The implemented helper can redirect a stopped thread's PC to the safe label
returned by patched Gum. That redirection is only valid for Gum states whose
safe PC was explicitly designed as an evacuation target. Future Gum patches may
replace direct redirection with a temporary breakpoint/single-step corridor when
the corridor needs to execute bounded instructions before reaching the safe
label.

The helper validates an entire instruction batch and revalidates stopped-thread
coverage before applying it. If this pre-mutation validation fails, the helper
releases held tracees and reports a retryable prepare failure. Once a byte write
or PC update has started, any helper-side failure is fatal: PEAK must not resume
threads with unknown partial code or register state.

Before a STOP window, the controller starts or validates the helper and captures
the Gum metadata needed for classification: overwritten prologue bytes,
trampoline labels, trampoline slice bounds, shared thunk bounds, and active
patch bytes. While non-controller threads are stopped, classification is pure
range math against that immutable snapshot. This keeps Gum/GLib table lookups
and helper `fork()` out of the most fragile part of the detach window.

## Physical Detach Sequence

For hook `H`:

```text
callback or heartbeat requests detach(H)
controller marks H DETACH_REQUESTED
controller validates helper availability and snapshots Gum metadata for H
controller asks helper to stop threads and report PCs for H
controller classifies PCs from the pre-STOP Gum snapshot
controller asks helper to redirect evacuable Gum PCs to safe labels
helper keeps all non-controller threads stopped
helper reports all threads safe, or reports fail-closed

if all threads safe:
    controller marks H DETACHING
    controller abandons active PEAK samples for H
    helper writes H's original prologue bytes back to the target entry
    controller marks H DETACHED
    controller keeps PEAK listener-owned memory and Gum bookkeeping alive
    controller finishes the helper guard, resuming threads
else:
    controller keeps H DETACH_REQUESTED for retry on transient strict failures
    or returns H to ATTACHED on terminal policy failures
    controller asks helper to resume threads without Gum mutation
```

Active samples are abandoned, not waited on. This avoids the hot-loop problem
where `in_flight == 0` may never appear soon enough.

Historical callers are not part of the strict wait predicate. A thread that
called `H` earlier and is now blocked or executing unrelated code is safe as
soon as its current PC classifies outside the target entry, Gum trampoline,
shared thunk, and other audited danger ranges. This prevents stale callers from
causing a detach or reattach hang by waiting for a safe region they will never
enter again.

When several target hooks are already pending, strict mode batches retry-ready
detach and reattach requests into one helper STOP window. The controller first
validates the candidates and snapshots each hook's Gum metadata before stopping
the process. While threads are stopped, every candidate is classified
independently against the same helper PC snapshot. Safe candidates contribute
their byte-write and audited safe-PC instructions to one helper instruction
batch; unsafe or unclassified candidates keep their requested state and use the
same bounded retry/backoff path as the single-hook implementation.

Partial success is allowed only for independent candidates. The batch rejects
duplicate hook ids, duplicate function addresses, missing reattach patch
records, unsupported operations, missing Gum snapshots, and conflicting helper
instructions. If combining otherwise safe candidates would make the helper
instruction list ambiguous, the controller resumes without mutating that batch
and leaves all valid candidates retryable. Once the helper starts any
byte/register mutation, the old invariant still applies: helper failure is
fatal because PEAK cannot prove what code or register state was left behind.
Candidate collection uses a round-robin cursor so a wide pending set is not
permanently biased toward low hook indexes.

## Reattach Sequence

Reattach also mutates the target entry and must use the same external stop and
classification mechanism:

```text
controller marks H REATTACHING
controller validates helper availability and snapshots Gum metadata
controller asks helper to stop threads and report PCs
controller classifies PCs from the pre-STOP Gum snapshot and asks helper to apply Gum safe-PC redirects
helper writes H's previously recorded active Gum patch bytes back to the target entry
controller marks H ATTACHED
controller finishes the helper guard, resuming threads
```

For final shutdown detach, PEAK restores original bytes under the helper guard
and releases each hook-level stop with `RESUME`. After every hook has finished
its Gum bookkeeping teardown, PEAK sends one helper `SHUTDOWN` command. The
helper detaches any held tracees, reports status, and exits; any missing response
or failed detach is fatal in strict mode because PEAK cannot prove the target's
thread state. Gum bookkeeping detach for target hooks happens before the
per-hook helper `finish` releases stopped threads; the final Gum flush happens
after all hook-level shutdown guards have completed. This keeps Gum metadata
mutation inside the same audited stop window as the physical entry-byte restore.

Steady-state detach/reattach prepare failures that return `TIMEOUT`,
`CLASSIFY_FAILED`, or a recoverable helper `ERROR` keep the requested transition
pending with bounded exponential backoff. This avoids dropping a physical
detach opportunity because one hot thread happened to be stopped at an unsafe PC
for a single snapshot. Shutdown uses a bounded retry loop and then fails closed
by leaving listener state alive if safety still cannot be proven.

`PEAK_DETACH_TRACE_PATH` records transition rows for offline diagnosis. The base
columns are `time,hook_id,symbol,operation,result,physical,status`; strict
batching extends each row with `retry_count`, `pending_age_s`, `batch_size`,
`stop_window_us`, and `batch_id`. The first seven fields are stable; parsers
should treat the tail fields as optional diagnostics. `stop_window_us` is the
measured helper-held STOP window when available, otherwise `0`; `batch_id` is a
nonzero identifier shared by all rows emitted from one controller batch and `0`
for single-row paths. These extra fields are gathered only when tracing is
enabled and should not be treated as part of PEAK's default per-call overhead
model without separate analysis.

## Memory Lifetime

After physical detach, Gum may still have in-flight trampoline users. PEAK must
not free listener-owned PEAK arrays immediately.

Rules:

- Gum listener object lifetime follows Gum's deferred-destroy mechanism.
- PEAK-owned arrays referenced by listener callbacks must remain valid until Gum
  reports that the listener has no pending trampoline teardown. The current
  implementation uses `gum_interceptor_flush()` for shutdown teardown.
- Final shutdown sets all hooks to `PEAK_HOOK_SHUTDOWN`, restores target bytes
  physically under helper control, performs any still-needed Gum bookkeeping
  detach after helper release, flushes Gum teardown, then frees PEAK state.
- Memory-profiling teardown first atomically blocks new malloc/free tracking,
  reverts and flushes the malloc Gum patches, waits for active allocator hooks
  to leave, and only then frees tracking tables and finalizes the mmap log.

## Dynamic `dlopen`

`dlopen` must not attach directly from the replacement body. It should enqueue a
dynamic attach request:

```text
peak_dlopen replacement:
    call original dlopen
    enqueue handle for controller scan
    return handle

controller:
    resolve requested symbols from handle
    use helper stop/classification before Gum attach
    publish hook metadata only after Gum attach and helper release succeed
```

The compatibility implementation queues `dlopen` work out of the replacement
body, drains it on controller-owned paths, and retains a `RTLD_NOLOAD` handle
for any module that receives PEAK hooks. The retained handle pins the module so
application `dlclose()` cannot unload code while PEAK and Gum still reference
addresses inside it. Handles are released only after general listener teardown
has flushed.

This prevents `dlopen` from racing with heartbeat detach, reattach, and final
teardown. In compatibility mode dynamic attaches are intentionally asynchronous:
a function called immediately after `dlopen()` may run before PEAK's controller
has attached the new listener. Strict mode preserves the same queuing model but
uses the helper guard for the eventual Gum attach.

The target detach controller is serviced before dynamic attach drains in each
controller cycle so a busy `dlopen` queue cannot starve pending target
detach/reattach work. Dynamic attach remains a separate bounded queue rather
than being mixed into the target-hook batch, because attach/replace work has
different Gum metadata and publication invariants. Optional diagnostics are
available through `PEAK_DLOPEN_DEBUG` and `PEAK_DLOPEN_TRACE_PATH`, which report
enqueued, drained, requeued, queue-full drops, closed-queue drops, `RTLD_NOLOAD`
drops, failed requeue drops, partial successes, retained handles, and max queue
depth. Retryable dynamic attach requests are requeued without ending the drain
budget early, so one temporarily unsafe library does not block later queued
handles in the same controller cycle.

Final `dlopen` replacement teardown is two-phase in strict mode. PEAK first
uses the helper guarded `REVERT` operation to restore the real `dlopen` entry
bytes without freeing Gum's replacement metadata. That closes admission so hot
`dlopen` loops cannot keep entering `peak_dlopen`. PEAK then resumes threads,
waits for already-entered `peak_dlopen` replacement bodies and dynamic attach
drains to leave, and performs the Gum metadata revert under a second helper
guard. If the replacement body does not drain within the bounded shutdown
window, teardown fails closed and leaves the interceptor state alive. The start
of the `peak_dlopen` replacement body is also registered as a blocked PC range
for strict revert. If a thread is stopped in that uncounted entry window, PEAK
refuses the revert and stops subsequent teardown so callback-visible state is
not freed while `dlopen` may still be interposed.

The final `dlopen` revert prepares are retried for a short bounded window on
transient helper statuses, including temporary `ptrace` permission denial seen
under hostile concurrent stress. Persistent failure still fails closed.

Unresolved plain C symbol names do not trigger Gum's expensive global C++
demangled-symbol scan. Dynamic C symbols are instead resolved by the queued
`dlopen` path when their library appears. Set `PEAK_ENABLE_CXX_SYMBOL_SCAN=1`
to restore the legacy broad scan for short C++ target names that do not include
obvious C++ spelling such as `::`, `(`, `<`, or `operator`.

## Testing Strategy

The implementation needs stress tests that are intentionally hostile:

- many threads hot-looping on one target while heartbeat repeatedly detaches;
- many threads hot-looping while controller reattaches;
- dynamic `dlopen` while heartbeat detach is active;
- process exit while worker threads are still near target functions;
- memory profiling with forced memlog growth and allocator teardown quiescence;
- CUDA host-thread launches if CUDA is available.

The key assertions are:

- no hangs;
- no crashes;
- detached hot loops regain original-call performance;
- abandoned samples do not corrupt later accounting;
- final teardown does not free listener state before Gum flush completes.

High-thread stress runs must size PEAK's thread tracking table above the worker
count plus PEAK's own controller/helper/main threads. The default is
`online_cpu_count * 2`; set `PEAK_MAX_NUM_THREADS` higher for hostile stress
runs such as 64+ worker hot loops.

The hot-loop stress harness now has two separate modes:

- detach stress requires a strict physical detach marker and rejects crashes,
  hangs, capacity failures, unsafe teardown logs, and Gum mutation failures;
- reattach stress additionally requires the final `**` marker and a
  `PEAK_DETACH_TRACE_PATH` transition trace containing target-specific
  successful detach and reattach events with `physical=1` and `status=safe`.

The stale-caller stress harness covers threads that call the target and then
leave it before detach pressure is created. It has both parked stale callers and
stale callers spinning in unrelated non-target code. The strict test also runs
with a deliberately low `PEAK_MAX_NUM_THREADS` to ensure the helper's `/proc`
thread enumeration, not PEAK's historical per-thread accounting table, is the
strict safety gate.

`--fail-on-transition-skips` is a stricter diagnostic mode. It currently exposes
safe retry attempts in very hot 64-thread reattach loops where the helper stops
threads at PCs that cannot be proven safe. Those retries are fail-closed and
eventual reattach succeeds in the stress runs, but the no-skip diagnostic remains
an opportunity target for a future audited single-step/breakpoint evacuation
corridor.

The deterministic fake-helper controller tests cover batched detach, batched
reattach, and mixed safe/unsafe batches. The mixed case verifies that one unsafe
candidate remains retryable while another independent candidate completes in the
same STOP window. The helper failure tests still cover permission denial,
timeout, malformed snapshots, evacuation errors, and release failures so the
fatal-after-mutation boundary stays explicit.
Additional fake-helper Gum-PC corridor tests force STOP snapshots at real
diagnostic PCs from an attached hook. They prove the exact
`on_enter_trampoline` label sends `SET_PC(function_address)` for the stopped
target-like TID and a physical entry-byte `WRITE_MEMORY` whose address, size,
and bytes match the Gum-reported original prologue. The fake helper can model
synthetic STOP TIDs as itself, the target pid, or an explicit numeric value, and
the corridor test validates `SET_PC` against the target-pid case. Unsupported
Gum PC classes send `RESUME`, do not send `EVACUATE` in the failing attempt,
leave no held mutation, and remain retryable `CLASSIFY_FAILED` prepares. The
LD_PRELOAD hot-loop controller matrix covers the same unsupported-real-PC path
for interior enter trampoline, leave trampoline, invoke trampoline, and enter
thunk PCs through the general listener: the hook stays `DETACH_REQUESTED` with a
retryable `prepare-failed,classify-failed` trace row, the first failed STOP window
logs no `EVACUATE`, and the later retry drains once the fake helper returns a
safe empty snapshot.

The hot-loop integration test has a paired-target mode that drives two exported
target symbols in the same worker loop. Its strict trace assertion requires both
targets to detach physically with `batch_size >= 2`, proving the general
controller path, not only the low-level controller API, batches real pending
hooks.

## Implementation Status

Completed in this branch:

1. Centralized target Gum lifecycle operations behind a PEAK controller API.
2. Removed direct target detach/reattach from callbacks and heartbeat.
3. Changed active samples to support abandon-on-detach.
4. Converted `dlopen` dynamic target attach into controller-queued work.
5. Added a patched-Gum provider path with linked PC API validation.
6. Added the Linux x86_64 auto-patched devkit overlay and external helper for
   stop/read-PC/evacuate/resume.
7. Wired target attach/detach/reattach/shutdown and dlopen replace/revert
   through the strict controller guard.
8. Made strict helper release failures fatal after any STOP window, added a
   `RELEASE_FAILED` helper status for unproven STOP cleanup, and sends one
   explicit helper `SHUTDOWN` after final shutdown mutations are complete.
9. Ordered malloc profiling detach after target-controller stop and before
   `dlopen`/Gum teardown, avoiding overlap with target mutations without
   profiling PEAK's own initialization or Gum teardown allocations.
10. Added allocator-hook quiescence to malloc teardown and made the cleanup gate
    atomic.
11. Made helper protocol writes SIGPIPE-safe and expanded the patched strict
    helper smoke test to stop/classify a real worker thread.
12. Added strict hot-loop, strict dynamic `dlopen`, helper self-preload, and
    strict MPI test coverage, plus a hot-loop benchmark for master/main versus
    compatibility and strict physical detach.
13. Clamped successful overhead calibration to a tiny positive floor so noisy
    negative dummy timings cannot suppress heartbeat detach in very hot loops.
14. Added deterministic patched-Gum range diagnostics in the strict controller
    test, covering function-entry, trampoline, and shared-dispatch
    classification plus safe-PC/fail-closed behavior.
15. Added fake-helper CTests that exercise the real controller `execve` path,
    environment stripping, permission-denied, timeout, malformed snapshot, and
    release-failed fail-stop paths.
16. Hardened helper lifecycle around `fork()`, recursive atfork-safe mutation
    locking, inherited file descriptors, target-PID binding, signal-mask reset,
    stale build-tree helper lookup, nonblocking fatal logging, and
    post-evacuation verification.
17. Added two-phase strict `dlopen` revert and a bounded replacement-body drain,
    plus skipped the global C++ symbol scan for unresolved plain C targets to
    avoid a large startup tax on dynamic-symbol workloads.
18. Added an explicit blocked-PC range for the `peak_dlopen` entry window,
    propagated `dlopen` teardown failure to `peak_fini()` so remaining state is
    left alive on unsafe revert, cleaned up failed idle helper shutdowns, and
    expanded fake-helper coverage for post-STOP evacuation/resume failures.
19. Kept support-hook Gum interceptor state pinned after successful physical
    revert for MPI, syscall, and exit hooks, matching the target/CUDA lifetime
    rule that trampoline/Gum metadata is not freed while late callbacks may
    still reference it.
20. Added `PEAK_ENABLE_REATTACH`, `PEAK_MAX_NUM_THREADS`, controller transition
    tracing with `PEAK_DETACH_TRACE_PATH`, high-thread detach stress, and a
    reattach-required stress test that validates both output markers and trace
    transitions.
21. Tightened dynamic `dlopen` partial retry handling so a partially successful
    attach retains a separate library handle for already-attached hooks while
    preserving the original request handle for retrying unresolved hooks.
22. Removed PEAK's historical called-thread table from the strict safety
    decision. Strict detach/reattach now lets the helper enumerate live OS
    threads and classify their current PCs, so stale callers that will never
    re-enter the target do not cause hangs.
23. Added bounded retry/backoff for transient detach/reattach prepare failures,
    plus a bounded shutdown retry loop before fail-closed listener retention.
24. Revalidated helper held-thread coverage immediately before EVACUATE writes
    and made only pre-mutation EVACUATE failures retryable. Any failure after a
    byte/register mutation starts remains fatal.
25. Added stale-caller stress coverage for parked stale threads and unrelated
    non-target spinning stale threads. Stress trace parsing now deletes
    per-sample trace files before each run and requires target-specific
    strict-physical `safe` success rows.
26. Moved target-hook shutdown Gum bookkeeping detach inside the helper-held
    mutation window and made idle helper shutdown I/O failure fail closed.
27. Added bounded retry around final strict `dlopen` revert prepare so transient
    helper/ptrace denial under high stress does not skip otherwise safe
    teardown.
28. Batched retry-ready target-hook detach/reattach requests into one strict
    STOP window, with per-candidate Gum snapshots captured before STOP,
    independent classification against one helper PC snapshot, partial success
    for provably independent safe candidates, and all-or-nothing retry when
    helper instructions conflict.
29. Extended strict transition tracing with retry count, pending age, batch
    size, and diagnostic STOP-window timing, and added optional dynamic
    `dlopen` queue diagnostics for drops, retries, partial successes, retained
    handles, and queue depth.
30. Added round-robin strict batch candidate collection, continued dynamic
    `dlopen` drains after retryable requeues, and added paired-target hot-loop
    integration coverage requiring batched physical detach trace rows.
31. Added deterministic fake-helper Gum-PC corridor coverage for real attached
    hook diagnostics: exact `on_enter_trampoline` is the only audited direct
    `SET_PC` rewrite today, and interior trampoline/dispatch PCs fail closed
    without an `EVACUATE` mutation.
32. Strengthened fake-helper EVACUATE validation to check the stopped snapshot
    TID plus exact Gum original-prologue write size/bytes, and added LD_PRELOAD
    hot-loop coverage proving unsupported real Gum PCs preserve pending detach
    state until a later retry can complete.
33. Expanded unsupported Gum-PC coverage to a general-listener matrix for
    interior enter trampoline, leave trampoline, invoke trampoline, and enter
    thunk PCs; the integrated test now checks helper command logs before and
    after retry so failed unsupported-PC attempts prove they avoided
    `EVACUATE`.

Remaining work:

1. Add CI coverage using the auto-patched Linux x86_64 devkit plus
   `PEAK_DETACH_HELPER`.
2. Add a true no-skip evacuation corridor for hot reattach loops. This likely
   needs helper-driven single-step or breakpoint advancement to an audited safe
   point; direct PC rewriting remains limited to Gum's exact enter-trampoline
   label.
3. Add a true live-RIP unsafe-PC test that forces a stopped thread's actual
   register state into each Gum trampoline/shared-thunk region, complementing
   the deterministic classifier tests that exercise those PCs directly.
4. Extend helper support to other architectures with the appropriate register
   access and PC-write APIs.
5. Add job-control/group-stop preservation tests and, if needed, a
   `PTRACE_LISTEN` path for threads that were already group-stopped before the
   helper interrupt.
6. Reduce the strict stop window further so future attach/replace operations can
   prebuild all Gum/GLib state before stopping target threads, leaving only
   audited byte/register commits inside the debugger-held window.
