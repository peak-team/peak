# Physical Detach Controller Design

## Goal

PEAK's core advantage is physical detach: once a hot target is detached, the
target application executes the original function entry path with no PEAK or Gum
trampoline overhead. This controller keeps that property while closing the races
around Gum detach, reattach, dynamic attach, and teardown.

The key idea is to stop treating detach as a cooperative pthread operation. PEAK
manufactures safe detach opportunities with debugger-grade thread control and
Gum-defined safe regions.

## Implemented Split

PEAK now runs the strict physical-detach controller by default. It refuses
target-function Gum attach, detach, reattach, and shutdown detach unless PEAK
can use patched-Gum PC classification plus a strict stop backend. The default
backend selection is `auto`: PEAK uses the external helper unless Linux
`ptrace_scope` is already known to block helper attachment, in which case it
selects the signal backend up front. Auto also falls back to the signal backend
when helper startup is unavailable or a structured helper STOP response reports
permission denied, timeout, or unsupported. Helper protocol loss or no-response
after a STOP request is still fail-stop because the controller cannot prove
whether the helper already stopped target threads. `PEAK_SAFE_DETACH_MODE=helper`
/ `debugger` force ptrace-helper behavior; `PEAK_SAFE_DETACH_MODE=signal` or
`PEAK_DETACH_BACKEND=signal` forces the in-process signal backend. `dlopen`
replace/revert is also guarded because it changes a
process-wide Gum patch site. With stock Gum, startup target hooks are skipped,
already-attached target hooks remain attached, and PEAK logs why target
mutation was skipped. Process-lifetime support hooks keep explicit startup/shutdown
  ordering outside steady-state target detach. Malloc profiling still attaches
  after PEAK initialization so it does not profile PEAK setup allocations, but
  shutdown stops the target controller before malloc detach so allocator Gum
  mutations do not overlap target physical detach/reattach windows.

The legacy cooperative signal pause path is not considered strict-safe. It
cannot classify program counters, cannot advance only audited PEAK/Gum safe
regions, and cannot prove that untracked or newly created threads are outside
Gum code. The strict signal backend described below is a separate mechanism: it
parks enumerated Linux TIDs in an async-signal-safe corridor, captures their PCs
from `ucontext_t`, classifies those PCs with the patched Gum API, performs only
audited PC rewrites and byte writes, and fails closed if any thread does not
arrive or cannot be released safely.

## Original Failure Mode

The pre-controller implementation let callbacks, heartbeat, `dlopen`, and
shutdown all interact with shared Gum state:

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

Per-hook states:

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
8. resume all stopped threads after ordinary mutations, and after final hook
   shutdown send an explicit idle `SHUTDOWN` command that tells the helper to
   exit. Helper protocol writes suppress `SIGPIPE` so a closed controller
   socket cannot kill the helper before its held-thread cleanup path runs.

The helper does not call Gum APIs directly. Gum mutation remains in-process so it
can operate on Gum's live data structures.

### Strict signal backend

On systems where ptrace is denied, PEAK can use
`PEAK_SAFE_DETACH_MODE=signal` or `PEAK_DETACH_BACKEND=signal`. This backend is
not the legacy pause mitigation. PEAK reserves a process-lifetime real-time
signal early for strict-auto runs, installs a protective lease handler
immediately, protects that lease through exported signal-policy wrappers for
common libc signal APIs, and replaces the lease handler with a
cookie-authenticated `SA_SIGINFO` stop handler when the backend is activated.
The controller enumerates `/proc/self/task`,
sends each non-controller TID a queued thread-directed signal with
`rt_tgsigqueueinfo`, and accepts arrival only when the handler sees the expected
hidden epoch/TID cookie. Ordinary user `kill`, `pthread_kill`, timer,
`sigqueue`, `sigwait`, `signalfd`, temporary-mask, or signal-mask traffic cannot
satisfy a PEAK stop slot. Attempts through normal dynamically linked libc APIs
to install a user handler for PEAK's reserved signal, block it, wait on it,
consume it through `signalfd`, generate it through a timer, or send it cause
PEAK to migrate the lease to another available unblocked RT signal before
forwarding the user call. If PEAK cannot migrate because the signal was forced,
all replacement candidates are unavailable, or a mutation window is active, the
user call fails with `EINVAL` and no patch write is attempted. Direct raw
syscalls or inline assembly are outside this user-space wrapper boundary; if
they actually deliver PEAK's reserved signal without a valid cookie, that
delivery is treated as backend contamination and signal-backed mutation then
fails closed with `signal-unexpected-delivery`, while helper-backed mutation
remains independent. Before every signal-backed stop, PEAK revalidates that its
handler still owns the current reserved signal and that no unexpected
non-cookie delivery has contaminated the backend.
If a TID has the reserved signal truly blocked, strict signal mutation fails
closed before any patch write. A short mask recheck distinguishes that condition
from the kernel's tiny handler-unwind window where the delivered signal may
still appear blocked immediately after PEAK's previous handler reports `done`.
If a TID exits after enumeration but before signal-handler arrival,
the controller deactivates that slot after verifying the TID no longer exists
instead of waiting for a safe-region arrival that can never happen. If a TID
appears in `/proc/self/task` during the initial stop verification but was not
in the first held set, the controller admits it into the same epoch, sends the
strict signal, waits for its handler to park and report a PC, and includes that
thread in the snapshot that will be classified. Threads that do arrive park in
a dependency-free atomic loop inside the signal handler. The handler records
the interrupted PC from `ucontext_t`; the controller classifies the resulting
snapshot with the same patched-Gum PC API as the helper path.

Signal evacuation shares the same stopped-thread classification path as the
helper. It supports direct `SET_PC` rewrites, `WRITE_MEMORY` commits, and a
ptrace-free `SINGLE_STEP_OUT_OF_RANGE` implementation for reattach snapshots
stopped inside the original target prologue. For that case the controller plants
a temporary architecture breakpoint at `function_address + overwritten_len`,
lets only the affected signal-held thread return from the parking handler,
catches the breakpoint in PEAK's SIGTRAP handler, parks the thread again at the
safe PC, restores the original breakpoint bytes, revalidates the stopped thread
set, and only then writes the active entry patch. PEAK installs this handler as
a chain-safe SIGTRAP handler: traps that do not match an active PEAK evacuation
epoch/TID/breakpoint are forwarded to the previously installed action. Any
timeout or failed breakpoint restore is fatal because PEAK can no longer prove
the stopped-thread state. Existing-hook mutations (`detach`, `reattach`, `shutdown`, and `revert`) require
a Gum PC diagnostic snapshot for the target hook before classification; missing
Gum corridor metadata is `CLASSIFY_FAILED`, not an implicit safe state. Before
any write, the controller validates every supported `SET_PC` target, validates
every `WRITE_MEMORY`, and re-scans `/proc/self/task` to ensure no unheld TID is
present. After PC classification has completed, newly observed unheld TIDs are
not admitted silently because their PCs were not part of the proven-safe
snapshot; those cases fail closed with reason-level diagnostics and are retried
before any byte mutation. PC rewrites are published back to each parked handler,
and release only reports success after each handler confirms the rewrite
succeeded. If a signal is masked, a thread never arrives, cleanup cannot prove
release, or a rewrite fails, strict mode fails closed; after a physical
byte/register mutation starts, release failure is fatal.

Because a new pthread can appear after a `/proc/self/task` scan, strict mutation
windows also hold a lightweight pthread creation gate. PEAK's pthread wrapper
tracks the newborn thread and waits before entering user code while a strict
physical mutation is active. The signal backend refuses to start unless the
pthread wrapper has confirmed that `pthread_create` interception is installed;
without that confirmation, signal-backed physical mutation is `UNSUPPORTED`.
This gate is held from immediately before STOP until `finish_hook_mutation`
resumes the stopped or parked threads. Direct raw `clone(2)` users outside
pthread are outside the user-space pthread gate; signal-mode STOP verification
still re-scans `/proc/self/task` before mutation and fails closed if an unheld
thread cannot be admitted and classified.

The current code implements this Linux x86_64 and Linux Arm64 helper path behind
the patched-Gum capability check. The default Linux x86_64 and Linux Arm64 CMake
paths now construct a PEAK-patched Frida Gum devkit by copying the downloaded
devkit and appending PEAK's PC API implementation to the archive. Stock Gum still
reports `missing-gum-api` in strict mode. The helper uses `PTRACE_GETREGS` /
`PTRACE_SETREGS` on x86_64 and `PTRACE_GETREGSET` / `PTRACE_SETREGSET` with
`NT_PRSTATUS` on Arm64. When `PR_SET_PTRACER` is unavailable and returns
`EINVAL`/`ENOSYS`, PEAK falls back to the helper's real `ptrace` attempt; actual
permission failures still fail closed.

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
- `PEAK_PC_AT_PATCH_ENTRY`: safe for physical detach/shutdown restore only
  when `pc == function_address`. An interior PC in the active patch range is not
  a valid continuation after restoring the original prologue, so PEAK fails
  closed instead of resuming that thread in mixed patch/prologue bytes.
- `PEAK_PC_IN_TARGET_BODY`: safe. The thread is already past the overwritten
  prologue, and detach affects future entries.
- `PEAK_PC_IN_PEAK_EVACUATION`: advance this thread to a PEAK safe label.
- `PEAK_PC_IN_GUM_EVACUATION`: advance this thread to a Gum safe label.
- `PEAK_PC_IN_GUM_CRITICAL` or `PEAK_PC_UNKNOWN`: do not detach unless Gum has
  been patched so the range has a bounded evacuation route.

The implemented overlay now treats PC disposition as operation-specific and
records the held mutation shape explicitly: whether it mutates target entry
bytes, mutates Gum metadata, or frees listener-owned state. The
`SAFE_NO_ACTION` relaxation for Gum PCs is allowed only when the candidate
mutates entry bytes and does not mutate Gum metadata or free listener state. For
physical entry-byte-only `detach` and `reattach`, stopped threads already inside
this hook's Gum enter, invoke, or leave trampoline ranges, or inside the shared
enter/leave thunk ranges reported by Gum diagnostics, are therefore safe to
leave in place. The physical path invariants are: PEAK writes only target entry
bytes, does not call Gum detach/revert, does not free listener-owned PEAK data,
does not destroy Gum context or trampoline memory, and keeps in-flight Gum
trampoline execution able to reach the old listener/context until it naturally
returns. The shared thunk ranges are not uniquely attributed to one hook, but
entry-byte-only mutation does not modify shared Gum code or free Gum metadata.
These PCs do not require `SET_PC` evacuation and should not be counted as
`CLASSIFY_FAILED`. Gum metadata mutation, shutdown, and revert paths must not
use this relaxation; they still require an explicit Gum-provided safe PC or a
retry. Shutdown also fails closed for active-patch interior PCs because Gum
metadata may be detached before stopped threads are released. Reattach remains
stricter for the target prologue: `pc ==
function_address` is accepted because no original prologue byte has executed
yet. With helper-held threads, an interior PC in the overwritten prologue emits
a pre-write `SINGLE_STEP_OUT_OF_RANGE` evacuation instruction; the helper
single-steps that thread while the original bytes are still installed, stops
again once the PC is outside the overwritten range, and only then commits the
active patch bytes. With the signal backend, the same logical instruction is
implemented with an in-process temporary breakpoint corridor instead of ptrace
single-step, so the thread advances only to the first safe PC and parks again
before the active patch bytes are written.

## Gum Devkit Changes

The current CMake downloads a prebuilt Frida Gum devkit. On Linux x86_64 and
Linux Arm64 the default `auto` provider copies that devkit to a PEAK-patched
devkit directory, appends the PEAK PC API implementation object to
`libfrida-gum.a`, and appends the public API declarations to `frida-gum.h`. A
caller may also provide a patched devkit explicitly with
`PEAK_FRIDA_GUM_PROVIDER=patched-devkit`.

The selected devkit must expose:

```c
#define GUM_PEAK_PC_API_VERSION 1
#define GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_X86_64 0x01060509u
#define GUM_PEAK_PC_ABI_FRIDA_GUM_16_5_9_LINUX_ARM64 0x02060509u

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

The PEAK-specific Gum API turns that internal knowledge into reliable PC
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
safe PC was explicitly designed as an evacuation target. Reattach prologue
evacuation uses a bounded single-step or temporary-breakpoint corridor before
entry bytes are patched.

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
controller validates strict backend availability and snapshots Gum metadata for H
controller asks the selected backend to hold threads and report PCs for H
controller classifies PCs from the pre-STOP Gum snapshot
controller asks the backend to redirect evacuable Gum PCs to safe labels
backend keeps all non-controller threads stopped or parked
backend reports all threads safe, or reports fail-closed

if all threads safe:
    controller marks H DETACHING
    controller abandons active PEAK samples for H
    selected backend writes H's original prologue bytes back to the target entry
    controller marks H DETACHED
    controller keeps PEAK listener-owned memory and Gum bookkeeping alive
    controller finishes the strict guard, resuming or releasing threads
else:
    controller keeps H DETACH_REQUESTED for retry on transient strict failures
    or returns H to ATTACHED on terminal policy failures
    controller asks the selected backend to resume/release threads without Gum mutation
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
controller asks the selected strict backend to stop threads and report PCs
controller classifies PCs from the pre-STOP Gum snapshot using operation-specific dispositions
backend evacuates any supported interior-prologue PCs before mutation
backend writes H's previously recorded active Gum patch bytes back to the target entry
controller marks H ATTACHED
controller finishes the backend guard, resuming threads
```

For final shutdown detach, PEAK restores original bytes under the helper guard
and releases each hook-level stop with `RESUME`. After every hook has finished
its Gum bookkeeping teardown, PEAK sends one helper `SHUTDOWN` command. The
helper detaches any idle tracee handles, reports status, and exits. A missing
idle `SHUTDOWN` response fails closed: public teardown returns false and leaves
listener-owned PEAK state alive so an unproven helper teardown cannot race a
late Gum trampoline callback. By contrast, any helper release/resume failure
after a STOP window or byte/register mutation is fatal in strict mode because
PEAK cannot prove the target's thread state. Gum bookkeeping detach for target
hooks happens before the per-hook helper `finish` releases stopped threads; the
final Gum flush happens after all hook-level shutdown guards have completed.
This keeps Gum metadata mutation inside the same audited stop window as the
physical entry-byte restore.

Steady-state detach/reattach prepare failures that return `TIMEOUT`,
`CLASSIFY_FAILED`, or a recoverable helper `ERROR` keep the requested transition
pending with bounded exponential backoff. This avoids dropping a physical
detach opportunity because one hot thread happened to be stopped at an unsafe PC
for a single snapshot. Shutdown uses a bounded retry loop and then fails closed
by leaving listener state alive if safety still cannot be proven.

`PEAK_DETACH_TRACE_PATH` records transition rows for offline diagnosis. The base
columns are `time,hook_id,symbol,operation,result,physical,status`; strict
batching extends each row with `retry_count`, `pending_age_s`, `batch_size`,
`stop_window_us`, `batch_id`, and `last_retry_status`. Current diagnostics also
append `failure_reason`, `failure_tid`, `failure_pc`, and `failure_aux` so
collapsed statuses such as `prepare-failed,classify-failed` can be traced back
to a concrete verifier or PC-classifier branch. The first seven fields are
stable; parsers should treat every tail field as optional diagnostics and accept
older rows that stop after `batch_id`. `last_retry_status` records the most
recent retryable controller status for the hook, so a successful after-retry row
can keep `status=safe` while still reporting the pre-reset retry reason such as
`classify-failed`. `stop_window_us` is the measured helper-held STOP window when
trace diagnostics are enabled, otherwise `0`; `batch_id` is a nonzero identifier
shared by all rows emitted from one controller batch and `0` for single-row
paths. STOP-window timing is collected only for `PEAK_DETACH_TRACE_PATH`
diagnostics. Existing hot-callback timing remains part of the legacy
overhead/profiling model and is separate from this STOP-window diagnostic path.
The hot-loop stress runner now parses these trace rows directly: manual
benchmark tests can fail on any transition skip or on configured per-operation
`classify-failed` limits, so a retry storm cannot be hidden by one eventual safe
detach/reattach success.

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
  detach before helper release, sends the final idle helper `SHUTDOWN`, flushes
  Gum teardown, then frees PEAK state. If bounded shutdown safety cannot be
  proven before Gum mutation, or if the idle helper `SHUTDOWN` cannot be proven,
  teardown fails closed and leaves callback-reachable state allocated.
- Memory-profiling teardown first atomically blocks new malloc/free tracking,
  reverts and flushes the malloc Gum patches, waits for active allocator hooks
  to leave, and only then frees tracking tables and finalizes the mmap log.

## Dynamic `dlopen`

`dlopen` must not attach directly from the replacement body. It enqueues a
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

The dynamic attach implementation queues `dlopen` work out of the replacement
body, drains it on controller-owned paths, and retains a `RTLD_NOLOAD` handle
for any module that receives PEAK hooks. The retained handle pins the module so
application `dlclose()` cannot unload code while PEAK and Gum still reference
addresses inside it. Handles are released only after general listener teardown
has flushed.

This prevents `dlopen` from racing with heartbeat detach, reattach, and final
teardown. Dynamic attaches remain intentionally asynchronous: a function called
immediately after `dlopen()` may run before PEAK's controller has attached the
new listener. Strict mode uses the selected stop backend for the eventual Gum
attach.

The target detach controller is serviced before dynamic attach drains in each
controller cycle so a busy `dlopen` queue cannot starve pending target
detach/reattach work. Dynamic attach remains a separate bounded queue rather
than being mixed into the target-hook batch, because attach/replace work has
different Gum metadata and publication invariants. Optional diagnostics are
available through `PEAK_DLOPEN_DEBUG` and `PEAK_DLOPEN_TRACE_PATH`, which report
enqueued, drained, requeued, queue-full drops, closed-queue drops, `RTLD_NOLOAD`
drops, failed requeue drops, partial successes, retained handles, and max queue
depth, plus the current queue length, fixed queue capacity, and drain budget.
Each dynamic attach drain snapshots the queue length at the start of the
controller cycle and drains at most that snapshot size and at most the fixed
budget. Retryable dynamic attach requests are requeued for a later controller
cycle, so one temporarily unsafe library cannot consume the entire same-cycle
budget by being reprocessed repeatedly. Dynamic attach prepare retries are
limited to transient `TIMEOUT`, `CLASSIFY_FAILED`, and recoverable `ERROR`
statuses; `PERMISSION_DENIED` is treated as terminal for queued dynamic attach
work so persistent ptrace policy failures cannot pin queue slots indefinitely.

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

The test and benchmark suite includes intentionally hostile cases:

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

`--fail-on-transition-skips` is a stricter diagnostic mode. The hot-loop stress
runners also accept per-operation classify-failed budgets, so helper or signal
reattach runs can require zero retry rows when the relevant prologue evacuation
corridor is expected to handle the stopped PCs. This keeps retry storms visible
even when a later transition eventually succeeds.

The deterministic fake-helper controller tests cover batched detach, batched
reattach, and mixed safe/unsafe batches. The mixed case verifies that one unsafe
candidate remains retryable while another independent candidate completes in the
same STOP window. The helper failure tests still cover permission denial,
timeout, malformed snapshots, evacuation errors, and release failures so the
fatal-after-mutation boundary stays explicit. A trace-disabled fake-helper
runtime test also runs strict detach and reattach prepare/finish with
`PEAK_DETACH_TRACE_PATH` unset and asserts `last_stop_window_us == 0`, proving
STOP-window timing is not gathered for the default runtime path.
Additional fake-helper Gum-PC corridor tests force STOP snapshots at real
diagnostic PCs from an attached hook. They prove physical entry-byte-only
`detach` writes only `WRITE_MEMORY` for exact `on_enter_trampoline`, interior
enter trampoline, leave trampoline, invoke trampoline, and shared enter-thunk
PCs. The fake helper validates the write address, size, and bytes against the
Gum-reported original prologue, and the tests assert no `SET_PC` evacuation is
needed for these in-flight Gum PCs. The LD_PRELOAD hot-loop controller matrix
covers the same real-PC classes through the general listener and now requires
immediate physical success with no same-operation `classify-failed` trace rows.
A focused reattach integration test first detaches a target, injects a one-shot
Gum-PC reattach snapshot, and verifies immediate physical reattach success. A
runtime reattach Gum-PC matrix covers the same interior enter, leave, invoke,
and shared enter-thunk cases for physical reattach. A separate
fake-helper reattach patch-entry test proves the exact target
`function_address` is accepted for reattach, while `function_address + 1` in the
original prologue emits `SINGLE_STEP_OUT_OF_RANGE,WRITE_MEMORY` and completes
physical reattach under the helper backend.

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
   explicit idle helper `SHUTDOWN` after final shutdown mutations are complete;
   an unproven idle `SHUTDOWN` fails closed by retaining listener state.
9. Ordered malloc profiling detach after target-controller stop and before
   `dlopen`/Gum teardown, avoiding overlap with target mutations without
   profiling PEAK's own initialization or Gum teardown allocations.
10. Added allocator-hook quiescence to malloc teardown and made the cleanup gate
    atomic.
11. Made helper protocol writes SIGPIPE-safe and expanded the patched strict
    helper smoke test to stop/classify a real worker thread.
12. Added strict hot-loop, strict dynamic `dlopen`, helper self-preload, and
    strict MPI test coverage, plus a hot-loop benchmark for master/main versus
    strict physical detach.
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
    hook diagnostics: physical entry-byte-only detach now treats enter, invoke,
    leave, and shared dispatch/thunk PCs as safe-no-action; runtime reattach
    matrix coverage proves the same disposition for physical reattach,
    commits only the target entry-byte `WRITE_MEMORY`, and leaves Gum metadata
    alive for in-flight trampoline users.
32. Strengthened fake-helper EVACUATE validation to check exact Gum
    original-prologue write size/bytes, and added LD_PRELOAD hot-loop coverage
    proving those real Gum PCs complete immediately without `SET_PC` or
    `CLASSIFY_FAILED` for entry-byte-only mutation.
33. Added exact reattach patch-entry coverage proving `function_address` is
    safe for reattach, and added helper prologue-step coverage for
    `function_address + 1` in the overwritten prologue. Expanded Gum-PC coverage
    to a general-listener matrix for
    interior enter trampoline, leave trampoline, invoke trampoline, and enter
    thunk PCs; the integrated tests now require immediate physical success,
    helper `EVACUATE`, and no same-operation `CLASSIFY_FAILED` row.
34. Added trace-disabled runtime coverage for fake-helper strict detach and
    reattach STOP windows, and added reattach success-after-retry trace coverage
    proving `retry_count` and `last_retry_status` survive until the success row.
35. Added helper-driven `SINGLE_STEP_OUT_OF_RANGE` evacuation for reattach
    snapshots stopped inside the overwritten original prologue. The helper
    advances those threads before entry-byte writes and the fake-helper tests
    assert the `SINGLE_STEP_OUT_OF_RANGE,WRITE_MEMORY` ordering for
    `function_address + 1`.
36. Strengthened the signal backend strictness boundary: parked threads record
    PC-rewrite success before release is reported, signal cleanup failures on
    abort paths are fatal instead of silent best-effort cleanup, and signal mode
    refuses physical mutation unless pthread creation is intercepted so newborn
    threads block outside user code during the mutation window.
37. Tightened hot-loop stress validation so benchmark/manual HPC runners can
    fail on trace-level `classify-failed`, `prepare-failed`, or `gum-failed`
    transition rows instead of accepting a run that eventually succeeds after a
    long retry storm.
38. Added ptrace-free signal-mode prologue evacuation for physical reattach: a
    temporary breakpoint advances only a thread stopped inside the original
    overwritten prologue to `function_address + overwritten_len`, parks it again
    in the signal backend, restores the temporary breakpoint bytes, revalidates
    stopped-thread coverage, and then commits the active entry patch. A CI-scale
    signal reattach trace test now requires zero reattach `CLASSIFY_FAILED`
    rows.
39. Fixed the signal stop arrival race for threads that exit after `/proc`
    enumeration but before handler arrival. The controller now deactivates
    unarrived TIDs once `tgkill(pid, tid, 0)` proves they no longer exist, so
    stale exiting callers do not force timeout retries.
40. Made the signal-mode temporary breakpoint handler chain-safe so PEAK can
    install its SIGTRAP action even when another handler already exists, while
    forwarding unrelated traps to the previous action.
41. Added reason-level strict trace diagnostics and fixed the signal stop
    `/proc` race where a TID appears after the first enumeration. Initial stop
    verification now admits the new TID into the held epoch, waits for its
    handler PC snapshot, and classifies it before mutation. Vista ARM64 full-core
    signal and auto reattach stress both passed 12 samples with zero
    classify/prepare/Gum failures.
42. Fixed the matching helper-side `/proc` race: when the helper verifies that a
    newly observed TID appeared after the first enumeration, it now keeps the
    already stopped threads held, loops back to stop only newly appeared TIDs
    under the controller's thread-creation gate, and times out rather than
    releasing/retrying an otherwise safe transition. Helper STOP failures now
    also populate trace failure diagnostics with helper status and errno. Current
    full-core validation passed with zero classify/prepare/Gum failures on
    Frontera development x86_64 (56 threads, auto and helper reattach, 12
    samples each) and Vista gg ARM64 (144 threads, auto and signal reattach, 12
    samples each).
43. Made the entry-byte-only relaxation explicit in the held mutation flags and
    tightened physical detach/shutdown at `PEAK_PC_AT_PATCH_ENTRY`: only
    `pc == function_address` is accepted, while an interior active-patch PC now
    fails closed before entry bytes or Gum metadata are changed. Deterministic
    fake-helper coverage exercises both the accepted detach entry PC and the
    rejected detach/shutdown `function_address + 1` regression cases.
44. Hardened the strict signal backend with a migratable real-time signal
    lease: PEAK now reserves an available RT signal early for strict-auto,
    installs a protective handler during the lease window, migrates away from
    normal libc attempts to steal, block, wait on, signalfd-consume,
    timer-generate, or send the reserved signal, uses hidden
    `rt_tgsigqueueinfo` cookies so application signal traffic cannot satisfy a
    parked-thread slot, revalidates handler ownership before each signal STOP,
    and fails fast when a target thread truly blocks the reserved signal or a
    non-cookie delivery contaminates the backend. Runtime coverage now includes
    forced blocked delivery, user signal-collision migration, and bad-cookie
    contamination cases while requiring successful physical detach after
    collision migration.
