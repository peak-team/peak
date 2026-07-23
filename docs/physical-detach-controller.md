# Physical Detach Controller Design

## Goal

PEAK's core advantage is physical detach: once a hot target is detached, the
target application executes the original function entry path with no PEAK or Gum
trampoline overhead. This controller keeps that property while closing the races
around Gum detach, reattach, dynamic attach, JIT metadata attach, and teardown.

The key idea is to stop treating detach as a cooperative pthread operation. PEAK
manufactures safe detach opportunities with debugger-grade thread control and
Gum-defined safe regions.

## Implemented Split

PEAK now runs the strict physical-detach controller by default. After the
controller starts, it refuses target-function Gum attach, detach, reattach, and
shutdown detach unless PEAK can use patched-Gum PC classification plus a strict
stop backend. The default backend selection is `auto`: PEAK uses the external
helper unless Linux
`ptrace_scope` is already known to block helper attachment, in which case it
selects the signal backend up front. Auto also falls back to the signal backend
when helper startup is unavailable or a structured helper STOP response reports
permission denied, timeout, or unsupported. Helper protocol loss or no-response
after a STOP request is still fail-stop because the controller cannot prove
whether the helper already stopped target threads. `PEAK_SAFE_DETACH_MODE=helper`
/ `debugger` force ptrace-helper behavior; `PEAK_SAFE_DETACH_MODE=signal` or
`PEAK_DETACH_BACKEND=signal` forces the in-process signal backend. `dlopen`
listener attach/detach is also guarded because it changes a
process-wide Gum patch site. Initial startup attach has one narrow exception:
before the controller exists, PEAK may attach directly when `/proc/self/task`
confirms that the process has exactly one task. Outside that single-task startup
case, stock Gum causes startup target hooks to be skipped; already-attached
target hooks remain attached, and PEAK logs why later target mutation was
skipped. Process-lifetime support hooks keep explicit startup/shutdown
ordering outside steady-state target detach. Startup target hooks and the
process-lifetime `close`, allocator, and inert `dlopen` hooks, when enabled or
needed, are installed before the target controller starts. Runtime `dlopen`
callback admission opens only after that start, and the heartbeat thread starts
last. Shutdown stops the target controller before malloc detach so allocator
Gum mutations do not overlap target physical detach/reattach windows.

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

1. Except for the initial single-task startup attach described above, all
   target-hook mutation is serialized by the controller API and strict mutation
   guard. The startup exception ends before the controller starts and does not
   apply to detach, reattach, dynamic/JIT attach, or shutdown. The controller
   thread owns steady-state detach/reattach transitions, queued dynamic and JIT
   attaches, and deferred destruction. The
   synchronous FFTW `dlopen` path is an intentional execution-context
   exception, but not a guard exception: the application callback thread
   performs its initial ATTACH while holding the same mutation guard and
   general-listener publication lock. It
   does not introduce another detach/reattach state machine. `dlopen` listener
   attach and detach also go through the controller guard. JIT metadata
   providers publish only name/address/size candidates; the controller thread
   performs any resulting Gum attach. Process-lifetime support hooks are
   allowed to use direct Gum calls only when they cannot overlap an active
   target mutation window.
   Malloc detach is ordered after controller stop and before `dlopen`/Gum
   teardown so the memory profiler does not observe Gum teardown allocations.
   After the controller thread stops, the teardown owner performs final
   general-listener and `dlopen` shutdown through the same controller API and
   strict mutation guard.

2. Hot target-function callbacks never call Gum lifecycle APIs. They may
   request detach or reattach by setting atomic state or enqueueing a
   controller request.

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

The controller thread owns steady-state target lifecycle and queued attach
work. The controller API and strict mutation guard remain the serialization
authority for the synchronous FFTW before-return exception. The controller
thread owns:

- steady-state detach/reattach hook-state transitions;
- Gum transactions for controller-thread work;
- listener and name publication for controller-thread work;
- deferred listener destruction;
- queued dynamic symbol attaches after `dlopen` and all JIT attaches.

Final shutdown is not controller-thread work: after that thread has stopped,
the teardown owner removes the remaining hooks through the controller API and
strict mutation guard, then completes general-listener and `dlopen` teardown.

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

Target callbacks and heartbeat only request detach/reattach transitions. The
controller executes those transitions.

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
signal early for strict-auto runs by default, installs a protective lease
handler immediately, protects that lease through exported signal-policy
wrappers for common libc signal APIs, and replaces the lease handler with a
cookie-authenticated `SA_SIGINFO` stop handler when the backend is activated.
`PEAK_SIGNAL_RESERVE_EARLY=auto|always|forced-only|never` controls this
constructor-time lease. `auto` keeps the default strict-auto behavior, `always`
leases whenever the signal backend is supported, `forced-only` leases early
only when `PEAK_DETACH_SIGNAL` names a concrete RT signal, and `never` avoids
the early compatibility footprint. Explicit helper/debugger backend selection
does not reserve early for `PEAK_DETACH_SIGNAL=auto`. Disabling early leasing
loses protection against user constructors that claim a signal before helper
fallback selects the signal backend.
The controller enumerates `/proc/self/task`,
sends each non-controller TID a queued thread-directed signal with
`rt_tgsigqueueinfo`, and accepts arrival only when the handler sees the expected
hidden epoch/TID cookie. Ordinary user `kill`, `pthread_kill`, timer,
`mq_notify`, POSIX AIO, `sigqueue`, `sigwait`, `signalfd`, temporary-mask, or
signal-mask traffic cannot satisfy a PEAK stop slot. Attempts through normal
dynamically linked libc APIs to install a user handler for PEAK's reserved
signal, block it, wait on it, consume it through `signalfd`, generate it through
timer/mqueue/AIO notification, or send it cause PEAK to migrate the lease to
another available unblocked RT signal before forwarding the user call. PEAK
also guards dynamically linked `syscall()` calls for the signal-related syscalls
that can steal, block, wait on, consume, or generate the reserved RT signal,
including `rt_sigaction`, `rt_sigprocmask`, `rt_sigtimedwait`, `signalfd4`,
`timer_create`, `mq_notify`, `tgkill`, `rt_sigqueueinfo`, and
`rt_tgsigqueueinfo`. If PEAK cannot migrate because the signal was forced, all
replacement candidates are unavailable, or a mutation window is active, the
user call fails with `EINVAL` and no patch write is attempted. Truly direct
syscall instructions from inline assembly, static code outside the preload
symbol path, or JIT code remain outside this user-space wrapper boundary; if
they actually deliver PEAK's reserved signal without a valid cookie, that
delivery is treated as backend contamination and signal-backed mutation then
fails closed with `signal-unexpected-delivery`, while helper-backed mutation
remains independent. Before every signal-backed stop, PEAK revalidates that its
handler still owns the current reserved signal and that no unexpected
non-cookie delivery has contaminated the backend.
PEAK virtualizes the reserved-signal disposition for normal user introspection:
wrapped `sigaction(signum, NULL, oldact)` and wrapped
`syscall(SYS_rt_sigaction, signum, NULL, oldact, ...)` queries of the current
or transitioning PEAK signal never expose the protective or stop handler. The
libc wrapper reports a default handler; the raw `syscall()` wrapper reports the
same sanitized default action when PEAK can safely copy it to user memory, or
fails closed with `EFAULT` if the sanitized copy-out cannot be proven safe. A
later attempt to install, wait on, block, consume, generate, or send that same
signal still triggers migration or fails closed.
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
devkit directory, patches `gumelfmodule.c.o` with the online-memory ELF fallback
guard, appends the PEAK PC API implementation object to `libfrida-gum.a`, and
appends the public API declarations to `frida-gum.h`. A caller may also provide
a patched devkit explicitly with
`PEAK_FRIDA_GUM_PROVIDER=patched-devkit`.

The selected devkit must expose:

```c
#define GUM_PEAK_PC_API_VERSION 1
#define GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64 0x01171503u
#define GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64 0x02171503u

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

The diagnostics lookup treats the listener as the authoritative hook identity
when Gum cannot find a context under the requested function-address key. Some
real binaries can expose a live listener while Gum records the function context
under a canonical address that differs from the symbol address PEAK originally
stored. In that case the patched Gum overlay scans active contexts for the
listener and returns the canonical `function_address` only when exactly one
active context owns that listener. Exact-address lookup remains valid for a
shared listener because the address identifies the context; only listener-only
stale-address recovery fails closed when multiple active contexts match instead
of letting hash-table iteration choose a patch target. Physical entry-byte
writes, patch records, and target-entry PC checks use the recovered canonical
address instead of the stale request address, avoiding repeated
`gum-diagnostics-missing` retries for a hook that is actually live.

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
instructions. Duplicate detection is repeated after Gum snapshot capture using
the recovered canonical function addresses, so two stale raw requests cannot
canonicalize to the same physical patch target and be hidden by duplicate write
coalescing. If combining otherwise safe candidates would make the helper
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
for a single snapshot. To prevent large target sets from retrying forever, PEAK
also bounds steady-state pending work with `PEAK_CONTROLLER_MAX_PENDING_AGE_MS`
(default `30000`) and `PEAK_CONTROLLER_MAX_RETRY_COUNT` (default `300`). When
either bound is exceeded, only the unproven transition is abandoned:
failed detach leaves the hook attached, and failed reattach leaves it detached.
The first-pending timestamp is recorded when the detach/reattach request is
accepted and is maintained whether or not `PEAK_DETACH_TRACE_PATH` is enabled,
so controller wake timing and diagnostic tracing cannot change retry budget
behavior. The code bytes are not mutated when safety was not proven.
Shutdown uses a bounded retry loop and then fails closed by leaving listener
state alive if safety still cannot be proven.

Heartbeat-driven reattach is intentionally rate-limited separately from safety
retries. A target that was physically detached because profiling overhead was
too high remains detached for at least `PEAK_REATTACH_COOLDOWN_MS` before the
heartbeat may reattach it again. The current default is `60000` ms. This avoids
immediate detach/reattach thrashing while still allowing long-running targets
to be sampled again. Set the cooldown to `0` for aggressive resampling or
focused reattach tests. See [Heartbeat mechanism and runtime
policy](heartbeat.md) for admission behavior.

The strict pthread-create gate is also bounded by
`PEAK_STRICT_GATE_WAIT_TIMEOUT_MS` (default `10000`). The intercepted creator
waits before calling the real `pthread_create()` while PEAK is proving and
applying a mutation window, so helper mode does not see a kernel-visible newborn
task before the child has reached PEAK's wrapper. The child wrapper keeps a
secondary gate before user code starts. If a backend stall keeps the gate open
past the timeout, PEAK lets the creator/child proceed and emits one diagnostic
rather than indefinitely blocking application thread creation; set the timeout
to `0` to restore an unbounded wait.

`PEAK_DETACH_TRACE_PATH` records transition rows for offline diagnosis. The base
columns are `time,hook_id,symbol,operation,result,physical,status`; strict
batching extends each row with `retry_count`, `pending_age_s`, `batch_size`,
`stop_window_us`, `batch_id`, and `last_retry_status`. Current diagnostics also
append `failure_reason`, `failure_tid`, `failure_pc`, and `failure_aux` so
collapsed statuses such as `prepare-failed,classify-failed` can be traced back
to a concrete verifier or PC-classifier branch. Heartbeat provenance appends
`request_source`, `request_calls`, `request_ratio`,
`request_global_overhead`, `request_total_time`, and `request_rate` so a final
report marker can be joined back to the decision that requested the physical
transition. The tail also includes completed-window count, cumulative control
wall seconds and ratio, accounting snapshot validity, and failed-window count.
The first seven fields are
stable; parsers should treat every tail field as optional diagnostics and accept
older rows that stop after `batch_id`. `last_retry_status` records the most
recent retryable controller status for the hook, so a successful after-retry row
can keep `status=safe` while still reporting the pre-reset retry reason such as
`classify-failed`. `stop_window_us` is the completed controller STOP-window
sample associated with a successful scalar transition or prepared batch. A
scalar prepare-failed, retry-abandoned, or wholly unprepared batch row uses `0`
rather than reusing an older successful sample. In a partially prepared batch,
an unprepared candidate carries the same nonzero window sample and `batch_id`
as the successful candidates because its classification occurred inside that
shared window. `batch_id` is `0` for single-row paths. Retry exhaustion emits
`result=retry-abandoned` before the transition is returned to its stable state.
STOP-window accounting is always collected, including with tracing disabled;
the trace only renders the already-published diagnostic. Existing hot-callback
timing remains part of the legacy
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

## MPI Abnormal Exit

PEAK intercepts `PMPI_Finalize()` so profiler output can be written before MPI
teardown makes collectives unavailable. Error exits are different: a setup
failure may unwind only a subset of ranks, and adding PEAK collectives or
replaying the real `PMPI_Finalize()` from that state can hang or crash the MPI
job.

The MPI finalizer interposer therefore records whether the application attempted
`PMPI_Finalize()`. With the default `PEAK_MPI_FINALIZE_POLICY=report`, it
immediately makes target callbacks pass-through and freezes the CPU report on
that application-owned finalizer path. MPI aggregation remains proof-first.
Explicit socket and rank-local output instead publish the frozen report before
MPI teardown coordination, so short finalize-arrival skew cannot consume their
publication window. Finalize participation then joins report completion and
real-finalizer policy in the long post-publication release gate. Each gate is
timeout-bounded. If MPI aggregation's initial participation proof or payload
reducer fails before publication, that path may fall back to rank-local output.
If the combined local/socket gate fails after publication, PEAK retains the
already completed report responsibility and does not start another output
fallback. Either unsafe case skips the real finalizer instead of blocking
indefinitely. The gates use nonblocking MPI collectives plus bounded
`MPI_Test()` polling. This is much safer than a blocking
`MPI_Allreduce`, but it is still an MPI progress call and therefore a
best-effort proof, not a hard guarantee against every broken MPI runtime state.
If a required gate times out, PEAK deliberately treats MPI as unusable for the
rest of its teardown. It does not cancel or free the outstanding nonblocking
collective request because MPI does not provide a portable cancellation path
for active nonblocking collectives; instead, PEAK performs no further MPI calls
and relies on process exit to reclaim MPI-owned state. Suspending callbacks before
heartbeat stop, controller drain, and output prevents late hot-target samples
from enqueueing new detach/reattach work while the finalizer path is already
tearing down.

The post-publication release gate is a second bounded
`MPI_Iallreduce(MIN)` after MPI aggregation's participation proof. For
rank-local and socket output it is instead the only gate and carries finalize
participation as a third reduced value. An MPI aggregate peer contributes after
finishing its transport role, the aggregate root after atomic CSV publication
and text flush, and a rank-local writer after atomically publishing its own
file. It reduces report success and cached real-finalizer eligibility
separately. Every rank that completes the same gate observes the same minimum,
so a contributed runtime or environment mismatch disables the real finalizer
on those ranks. A zero report minimum means the protocol completed but at least
one output failed. A collective error or timeout instead permanently fails MPI
teardown closed on the observing rank and quarantines its active request and
buffers until process exit. A local timeout cannot prove that every peer
observed the same failure; a peer may complete the collective and make a
different finalizer decision. This is a bounded local fail-closed mechanism,
not distributed failure consensus.

Healthy all-rank jobs normally hand completion to the real finalizer and
launcher. Intel MPI 2019 is a compatibility exception: 1024-node validation
found its real finalizer repeatedly crashing in `hwloc_topology_destroy()`
after complete PEAK report publication, so PEAK skips that finalizer by default.
`PEAK_MPI_REAL_FINALIZE=1` explicitly opts back in; `0` disables the handoff on
other runtimes. Neither value can override a failed release protocol. Skipping
the real finalizer is non-conforming and cannot by itself guarantee clean
launcher termination. `MPI_Finalize()` is not a barrier, so PEAK does not claim
simultaneous return.

Applications where every rank intentionally calls `MPI_Finalize()` early and
then continues substantial non-MPI work should use
`PEAK_MPI_FINALIZE_POLICY=defer`. They may additionally select
`PEAK_OUTPUT_AGGREGATION=socket` for aggregate output. In `defer` mode PEAK
normally calls the real `PMPI_Finalize()` immediately from the application
finalizer, keeps target profiling active, and emits PEAK output from normal
process teardown. Explicit `PEAK_MPI_REAL_FINALIZE=0` disables that immediate
call. When the real finalizer runs, MPI is finalized before PEAK output, so the
output cannot use MPI collectives in this mode;
socket output uses launcher rank metadata, while local output writes one file per
rank. `defer` is not a subset-rank failure-recovery mode: if only some ranks
call the real MPI finalizer, the MPI runtime itself may still hang or abort.
Unless `PEAK_MPI_REAL_FINALIZE=0`, `defer` also bypasses the Intel MPI 2019
compatibility skip by calling the real finalizer before PEAK's report-path
runtime policy executes.
Do not use `defer` as Intel MPI 2019 crash containment.

The practical choice is: use default `report` with either the MPI or socket
reducer for ordinary MPI jobs that finalize at the end; use `defer` for
all-rank early-finalize programs that continue heavy non-MPI work, optionally
with socket aggregation; avoid subset-rank finalization as an application
failure path because PEAK can only fail closed around its own reporting and
cannot make a partial real MPI finalizer safe.

PEAK does not replay the real `PMPI_Finalize()` later from process teardown,
because large Intel MPI jobs can crash or hang if a profiler re-enters MPI
finalization after the application has already logically finalized. In
`report` mode PEAK keeps target profiling hooks and listener bookkeeping pinned
for process exit cleanup, restores support wrappers such as
`pthread_create`/`pthread_join` and `close`, and keeps the `PMPI_Finalize`
replacement pinned while returning through Gum's original-call trampoline.
Reverting the broader target hook set from inside the MPI finalization call path
is treated as unsafe. Process exit then reclaims the still-pinned target
interceptor state.

Because PEAK owns the finalizer wrapper, `MPI_Finalize` and `PMPI_Finalize`
targets are skipped rather than mapped to `peak_pmpi_finalize()`. Profiling the
wrapper would measure PEAK teardown code and can re-enter target accounting while
PEAK is producing final output.

`PEAK_OUTPUT_AGGREGATION` defaults to `mpi`, which uses an MPI collective reducer
after first proving that every rank observed the application's `PMPI_Finalize()`
request. `mpi`, `collective`, or truthy values select this default reducer.
`socket`, `tcp`, or `interconnect` explicitly select the PEAK-owned TCP reducer
over the job interconnect. On the intercepted-finalize path, socket and local
CPU output publish before MPI teardown coordination. Strict socket rank/size
discovery accepts only complete, mutually consistent PMI, PMIx, OpenMPI,
MVAPICH2, Intel MPI, or Slurm environment pairs and does not query MPI. The
later combined participation/publication gate still uses an MPI collective to
decide whether the real finalizer is safe. With
`PEAK_MPI_FINALIZE_POLICY=defer`, socket output runs from process teardown after
the real finalizer. `local`,
`rank-local`, `none`, or falsey values write rank-local output. The older
boolean `PEAK_MPI_COLLECTIVE_OUTPUT=1` remains a compatibility alias for
aggregate output and maps to the MPI reducer when `PEAK_OUTPUT_AGGREGATION` is
unset.
Aggregate CSVs use `base-pPID.csv`. Multi-rank local/fallback writers add the
launcher rank as `base-pPID-rRANK.csv`; if rank metadata is unavailable they use
a sanitized hostname suffix. This prevents equal process IDs on different
nodes from overwriting one another.
The CSV `count` column is the exact total call count. `per_thread` is the
ceiling over active threads, while `per_rank` is the non-truncated arithmetic
mean over every rank represented by the report. The text report labels that
same mean as `avg/rank`.
If the process exits with a nonzero status before entering `PMPI_Finalize`,
aggregation is disabled. If the application has already entered
`PMPI_Finalize()`, PEAK cannot know about a later `exit(1)` or nonzero main
return yet, so the finalize-path output decision is made from the rank/finalize
state available at that moment. If aggregation is disabled, incomplete, or the
MPI runtime is no longer initialized enough to query rank/size, PEAK avoids
aggregate output and does not add MPI collectives or a PEAK-driven MPI
finalizer.

When MPI collective output is selected and the clean-exit MPI preconditions
hold, PEAK first performs a small bounded MPI handshake to verify that every
participating rank observed the application's `PMPI_Finalize()` request.
If the collective completes with a zero minimum because any rank did not
observe finalization, all participants consistently choose rank-local output
and skip the real MPI finalizer from the subset-rank path. If one rank instead
observes a local proof error or timeout, only that process is known to fall
back and prohibit later MPI calls; PEAK cannot infer that every peer observed
the same failure.
After the proof succeeds, the MPI reducer uses bounded nonblocking
`MPI_Iallreduce`/`MPI_Ireduce` wrappers and falls back to rank-local output if a
reducer collective fails or times out. A reducer failure is treated as locally
fail-closed: PEAK leaves the active nonblocking collective owned by MPI, avoids
later MPI teardown calls on that rank, skips the real `PMPI_Finalize()` return path, and
tries the PEAK-owned socket reducer using launcher rank metadata before falling
back to rank-local output. Those wrappers still rely on `MPI_Test()` for
progress, so this reducer is appropriate for ordinary all-rank clean shutdown,
but not for environments where PEAK must avoid MPI progress entirely.
Use socket output when the report payload must avoid MPI reducer collectives
and should be published before MPI teardown coordination. Combine it with
`PEAK_MPI_FINALIZE_POLICY=defer` when PEAK must avoid teardown collectives
entirely. `PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS` controls only MPI
aggregation's initial finalize-participation proof and defaults to 10000 ms.
`PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS`
controls each MPI payload reducer collective and defaults to 5000 ms, because
large-rank payload reductions can be slower than the small fail-fast proof.
`PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS` controls the post-publication gate and
defaults to 180000 ms. For socket/local output, finalize participation is one
of that gate's reduced values. A path that attempted socket publication,
including socket-to-local fallback, automatically raises its effective gate
timeout to at least the peer release budget plus two socket-phase intervals.
The extra intervals reserve separate margin for rank-local fallback
publication and collective arrival/progress. MPI and explicitly rank-local
paths retain the configured/default baseline.
The socket payload reducer
does not use MPI reductions for the profile payload itself: rank 0 accepts a
bounded set of framed per-rank payloads, validates a Slurm/PMI-derived reducer
token plus the job's hook-slot identity, and writes one aggregate result only
after every rank has arrived. With explicit socket output and no explicit
`PEAK_MPI_FINALIZE_POLICY`, PEAK runs the socket reducer from the application
finalizer path before the bounded combined participation/release gate. That
gate determines whether PEAK may return to the real MPI finalizer. If
`PEAK_MPI_FINALIZE_POLICY=defer` is explicitly combined with
socket output, PEAK calls the real MPI finalizer immediately and runs the socket
reducer from normal process teardown without MPI collectives or an MPI finalize
proof. After rank 0 writes the aggregate, it opens a PEAK-owned
release port, sends each peer an authoritative ACK-or-fallback decision, and
waits for peer confirmation; non-root ranks wait for that decision before
leaving process teardown. Once a peer validates ACK it never downgrades to
rank-local output merely because the confirmation path later fails. On the default
pre-finalize path, the common post-publication MPI gate then covers both socket
success and every rank-local fallback writer before finalizer handoff. If the
socket reducer cannot prove complete participation, it
emits a bounded diagnostic, releases the peers it did receive, and by default
falls back to rank-local output CSV files. Set
`PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK=0` (or `false`) to keep this from
falling back and retain aggregate-disabled behavior instead.
`PEAK_OUTPUT_AGGREGATION_TOKEN` can override the reducer token for controlled
tests.

`peak_fini()` is process-single-entry: the first caller owns teardown and later
callers wait for that teardown to finish without touching Gum, MPI, or
PEAK-owned listener arrays. At process exit this avoids double-revert,
double-free, mixed MPI output decisions, and a racing thread reaching the real
`exit()` while another thread is still writing PEAK output. The first
intercepted `exit()` status wins so a later racing `exit(0)` cannot turn an
already-recorded nonzero teardown into a collective-safe clean exit.

## Dynamic `dlopen`

Runtime target discovery is documented separately in
[Runtime `dlopen` profiling](dlopen-profiling.md). The controller-relevant
invariants are:

- the real loader call is observed, not replaced;
- synchronous FFTW and queued dynamic attaches both use the existing guarded
  ATTACH operation and publish metadata only after successful release;
- target detach/reattach work is serviced before the separate bounded dynamic
  queue, so loader activity cannot change or starve the lifecycle state
  machine;
- dynamic module handles remain pinned until target teardown has flushed; and
- shutdown closes admission and drains callbacks before any reachable listener
  state is released.

The final guarded `dlopen` listener shutdown-detach prepare may retry transient
helper failures for a short bounded window. Persistent failure remains
fail-closed.

PEAK also refuses known or conservative trampoline-scratch prologue classes
before static, dynamic `dlopen`, or JIT target attach. Tiny x86_64 leaf loops
such as MILC's `f2d_4mat`/`d2f_4mat` initialize a `DL`/`EDX`/`RDX` value, zero
`EAX`, execute the first indexed load in relocated entry bytes, and immediately
update the `DL`/`EDX`/`RDX` counter used by the loop. Gum 17.15.3 may then
clobber `RDX` before jumping back to the original function body, corrupting
application data even when physical detach is disabled. GCC/local canaries
reproduce the historical byte-72 corruption for these live counter variants.
This audited live-counter family is the default PEAK-side skip policy; short
dead-RDX probes that do not update or consume the counter after the first load
remain attachable by default.

Additional Frontera canaries showed Gum can also crash while attaching to a
high-register `movabs` entry instruction whose immediate contains x86
return-opcode bytes, and to a low-register entry `mov imm` operand with the
same kind of immediate. Earlier PEAK matchers scanned too broadly and could
reject ordinary dynamic-library or BLAS setup prologues. These canaries are now
exact entry-instruction checks and are only enabled by
`PEAK_UNSAFE_GUM_PROLOGUE_POLICY=conservative`.

On Arm64, PEAK does not pre-skip single `x16`/`x17` (`ip0`/`ip1`) uses by
default. Frida's Arm64 relocator selects a trampoline scratch register after
checking whether `x16` or `x17` appears in the relocated operands, so the default
policy defers to Gum's own attach result. The conservative PEAK policy can still
skip prefixes that mention both IP registers, but this is a diagnostic
fail-closed mode rather than a proven Arm64 corruption guard.

The narrow exception is an exported target consisting of one unconditional
branch to the canonical GNU ELF Arm64 PLT sequence (`adrp`/`ldr`/`add`/`br`
through `x16`/`x17`). Gum follows the branch but its checked relocation rejects
that PLT entry because neither IP register appears free. PEAK uses Gum's public
forced-relocation option only for this exact shape; the first relocated PLT
instruction defines `x16`, so the trampoline scratch value is not consumed by
the original sequence. Initial attach, runtime attach, and reattach all select
the same option. Both the exported branch and computed PLT destination are read
with Gum's safe memory API and integer overflow checks; an unreadable, partially
unreadable cross-page, or malformed destination keeps the default relocation policy.
Other Arm64 targets retain Gum's default relocation policy. This exception
provides first-call visibility, but does not claim that every Arm64 loader thunk
is attributed at the exact four-byte exported-wrapper boundary.

PEAK's optional `close` support replacement is skipped on Linux x86 when Gum's
symbol ranges show `close` and `__close_nocancel` overlapping in either
direction, or when the alternate entry lies inside Gum 17.15.3's maximum
16-byte x86 entry redirect. Such layouts exist on older glibc: a normal short
redirect may happen not to reach the alternate entry, while Gum's wider fallback
redirect could overwrite it. The startup-only check therefore preserves
application correctness by leaving stderr protection disabled on that layout.
It adds no `close` invocation cost and does not affect user-target attachment.
The top-level `dlopen` listener and dynamic `dlopen` user targets remain governed
by the default or `conservative` user-target policy.

`PEAK_ALLOW_UNSAFE_GUM_PROLOGUE=1` is a diagnostic override, not a safety mode.

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
- skipped unsafe Gum prologues do not corrupt application data;
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
`PEAK_DETACH_TRACE_PATH` unset and verifies that completed and failed
control-window accounting remains active. A later failed attempt preserves
`last_stop_window_us` as the most recent completed predictor sample while
incrementing the separate failed-window count and total wall time.
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

A focused unsafe-prologue regression exports architecture-specific targets.
On x86_64, default-mode tests require PEAK's skip diagnostic for the audited
`DL`/`EDX`/`RDX` zero, `EAX` zero, first indexed-load, post-load counter-update
family, while override-mode tests require either deterministic corruption or a
signal crash for locally reproducible live-counter cases. Conservative-policy
tests require PEAK's skip diagnostic for the exact high-register `movabs` and
return-opcode immediate entry canaries without making them default skips.
Default-allowed tests prove ordinary `mov imm` setup, unrelated longer copy
prologues, and dead-RDX probes remain attachable. On Arm64, default tests
require PEAK not to emit its unsafe-prologue skip for `x16`-only, `x17`-only, or
both-IP-prefix canaries; the both-IP-prefix case may still be rejected by Gum
itself. Conservative Arm64 tests cover the optional PEAK-side both-IP
fail-closed guard.

## Implementation Status

Completed in this branch:

1. Centralized target Gum lifecycle operations behind a PEAK controller API.
2. Removed direct target detach/reattach from callbacks and heartbeat.
3. Changed active samples to support abandon-on-detach.
4. Converted `dlopen` dynamic target attach into controller-queued work.
5. Added a patched-Gum provider path with linked PC API validation.
6. Added the Linux x86_64 auto-patched devkit overlay and external helper for
   stop/read-PC/evacuate/resume.
7. Wired target attach/detach/reattach/shutdown and the dlopen listener lifecycle
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
17. Added a bounded `dlopen` listener-callback drain, plus skipped the global
    C++ symbol scan for unresolved plain C targets to avoid a large startup tax
    on dynamic-symbol workloads.
18. Propagated `dlopen` listener teardown failure to `peak_fini()` so remaining
    state is left alive on unsafe detach, cleaned up failed idle helper
    shutdowns, and expanded fake-helper coverage for post-STOP
    evacuation/resume failures.
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
27. Added bounded retry around final strict `dlopen` listener shutdown-detach
    prepare so transient helper/ptrace denial under high stress does not skip
    otherwise safe teardown.
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
45. Tightened signal lease robustness: signal-handler cookie authentication now
    uses only preinitialized async-safe state, signal backend support checks
    the signal-policy atomics touched from handler context, guarded user-pointer
    inspection uses kernel-mediated reads instead of maps-plus-memcpy, raw
    `rt_sigaction` read-only queries either return sanitized default state or
    fail closed before exposing PEAK handlers, and `PEAK_SIGNAL_RESERVE_EARLY`
    makes the strict-auto early lease compatibility tradeoff explicit.
46. Hardened MPI abnormal-exit teardown: PEAK now records application
    `PMPI_Finalize()` requests. The default `PEAK_MPI_FINALIZE_POLICY=report`
    makes target callbacks pass-through before heartbeat/controller teardown can
    drain or enqueue more target mutations, writes PEAK output on the
    application's own finalizer path while MPI is still initialized enough to
    query rank/size. MPI aggregation then runs a bounded all-rank finalize proof
    and a separate post-publication release gate; socket and rank-local output
    publish first and use one combined participation/publication gate. An
    initial MPI proof or reducer failure may trigger rank-local fallback. A
    post-publication gate failure retains completed output responsibility,
    marks PEAK's MPI path unusable, and skips the real finalizer instead of
    starting another fallback or blocking a subset rank. Timed-out nonblocking
    collective requests are intentionally not cancelled/freed because MPI
    provides no portable cancellation path for active nonblocking collectives;
    PEAK fails closed and avoids all later MPI calls instead.
    `PEAK_MPI_FINALIZE_POLICY=defer` normally calls the real finalizer
    immediately and leaves PEAK profiling/output until normal process teardown
    for applications where all ranks keep doing non-MPI work after
    `MPI_Finalize()`. Explicit `PEAK_MPI_REAL_FINALIZE=0` disables that
    immediate call. Socket
    aggregation selects only the report transport and follows the default
    pre-finalize ordering unless `defer` is also explicit. `defer` is documented
    as all-rank early-finalize semantics, not subset-rank failure recovery. PEAK
    does not replay the real MPI finalizer from teardown. The default MPI
    reducer is guarded by all-rank
    finalize proof, and the optional socket payload reducer uses launcher rank
    metadata, releases non-root ranks only after rank 0 has
    completed aggregate output or instructs peers to fall back to rank-local
    output, and can fall back to per-rank CSV output on reducer failure. The
    common release gate covers aggregate and local/fallback publication. Ranks
    that complete the gate observe one reduced finalizer-policy value; a local
    error or timeout may not be observed by every peer and therefore remains a
    best-effort failure path. Healthy runtimes other than Intel MPI 2019 then
    use the real `PMPI_Finalize()`; Intel MPI 2019
    skips its observed crash-prone finalizer unless explicitly opted in with
    `PEAK_MPI_REAL_FINALIZE=1`. Unless real finalization is explicitly disabled
    with `PEAK_MPI_REAL_FINALIZE=0`, `defer` bypasses that compatibility check.
    Finalizer skip, timeout, and partial-fallback paths remain explicitly best
    effort and require launcher-scale validation.
    Nonzero exit paths that reach process teardown before MPI finalization skip
    aggregate output and PEAK-driven MPI collectives/finalize, preventing
    MILC-style setup termination from turning into Intel MPI finalization
    crashes or rank hangs. Once the application has entered `PMPI_Finalize()`,
    PEAK writes output before the later exit status is knowable, makes target
    callbacks pass-through, keeps target hooks pinned, restores support
    wrappers, keeps the `PMPI_Finalize` replacement pinned, and returns to the
    real finalizer through Gum's original-call trampoline only after the
    transport's required bounded all-rank gate or gates and the all-rank policy
    decision, unless `defer` policy is selected.
    The MPI reducer uses timed nonblocking collective wrappers after its
    proof succeeds, but still depends on `MPI_Test()` progress; socket output remains
    the reducer for runs where final reporting must avoid MPI progress
    entirely. `peak_fini()` is
    single-entry per process and racing exit callers wait for the owner, so they
    cannot double-run or interrupt Gum/MPI teardown; the first intercepted exit
    status wins. Default MPI collective output uses an all-rank
    finalize-observed handshake before PEAK reductions; `PEAK_OUTPUT_AGGREGATION`
    selects `socket`, `mpi`, or `rank-local`, while
    `PEAK_MPI_COLLECTIVE_OUTPUT=0` remains a compatibility way to keep teardown
    rank-local and `PEAK_MPI_COLLECTIVE_OUTPUT=1` maps to MPI aggregation.
    Regression coverage exercises no-finalize exit, no-finalize nonzero exit,
    explicit local output, explicit MPI collective output, socket bad-host
    failure, socket token mismatch failure, socket late release-channel failure,
    socket no-fallback opt-out, finalize-defer-post-work,
    finalize-then-nonzero-exit, subset-finalize nonzero exit, default
    subset-finalize clean exit, subset-finalize handoff timeout, default socket
    pre-finalize reporting with later application work, and finalize-then-nonzero
    return MPI lifecycles.
47. Made Gum context lookup listener-canonical: if strict detach cannot find a
    Gum context under the request address but the listener is still attached,
    the patched Gum overlay scans active contexts for that listener and returns
    the context's canonical function address. The controller now uses that
    canonical address for physical patch writes, patch records, and entry-range
    decisions. Regression coverage passes a deliberately mismatched address with
    the correct listener and verifies the helper writes original bytes to the
    recovered canonical address instead of failing with
    `gum-diagnostics-missing`.
48. Closed two strict-safety edges in canonical recovery: listener-only Gum
    lookup now fails closed if the same listener is attached to multiple active
    contexts, and batched detach/reattach rejects duplicate canonical patch
    targets after snapshot capture. Regression coverage now exercises a shared
    listener attached to two functions and two stale batch requests that both
    resolve to one canonical function, preventing hash-order-dependent patch
    selection and duplicate-write coalescing from masking unsafe batches.
