# PEAK-Patched Frida Gum Build Path

PEAK can build with either the stock prebuilt Frida Gum devkit or a PEAK-patched
devkit exposing the PC classification and safe-point hooks described in
`docs/physical-detach-controller.md`. On Linux x86_64 and Linux Arm64 the
default `auto` provider now downloads the Frida Gum devkit, copies it to a
PEAK-patched devkit directory, and appends PEAK's Gum PC API implementation to
the archive. Other platforms keep using the stock prebuilt devkit unless a
patched devkit is selected explicitly.

## Default Provider

No extra configuration is required for the default build:

```sh
cmake -S . -B build
```

On Linux x86_64 and Linux Arm64 this produces
`frida-gum-peak-patched/libfrida-gum.a` in the build tree and validates that the
linked headers and archive expose the architecture-specific PEAK ABI.
The downloaded Frida Gum 16.5.9 archives are pinned with SHA-256 hashes in
`cmake/frida-gum-download.cmake`.

If a caller supplies `FRIDA_GUM_LIBRARIES` and `FRIDA_GUM_INCLUDE_DIRS` while
leaving `PEAK_FRIDA_GUM_PROVIDER=auto`, PEAK treats that as a caller-managed
stock-Gum build. It validates the PEAK API only when
`PEAK_REQUIRE_GUM_PEAK_API=ON` is also set; without the PEAK API, strict runtime
mutation fails closed with `missing-gum-api`. Selecting
`PEAK_FRIDA_GUM_PROVIDER=auto-patched-devkit` requires CMake to own the downloaded
devkit so it can append the overlay.

## Patched Devkit Provider

Build or unpack a PEAK-patched Frida Gum devkit outside the PEAK source tree,
then point CMake at it:

```sh
cmake -S . -B build-patched-gum \
  -DPEAK_FRIDA_GUM_PROVIDER=patched-devkit \
  -DPEAK_PATCHED_GUM_ROOT=/path/to/patched/frida-gum-devkit
```

The devkit root may contain `frida-gum.h` and `libfrida-gum.a` directly, or use
the common `include/` plus `lib/` or `lib64/` layout. If the layout is unusual,
set these directly:

```sh
cmake -S . -B build-patched-gum \
  -DPEAK_FRIDA_GUM_PROVIDER=patched-devkit \
  -DPEAK_PATCHED_GUM_INCLUDE_DIR=/path/to/include \
  -DPEAK_PATCHED_GUM_LIBRARY=/path/to/libfrida-gum.a
```

Selecting `patched-devkit` fails configuration unless the selected headers and
`libfrida-gum.a` expose a linkable PEAK PC-classification API:

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

typedef struct _GumPeakPcDiagnostics GumPeakPcDiagnostics;

gboolean gum_interceptor_peak_get_pc_diagnostics(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    GumPeakPcDiagnostics * diagnostics);

gboolean gum_interceptor_peak_get_function_patch(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    guint8 * active_patch,
    guint8 * original_prologue,
    guint * prologue_len);

guint gum_interceptor_peak_abi_fingerprint(void);
```

For custom Gum paths that are not selected through `patched-devkit`, set both
`FRIDA_GUM_LIBRARIES` and `FRIDA_GUM_INCLUDE_DIRS`. Add
`-DPEAK_REQUIRE_GUM_PEAK_API=ON` when that custom path should be validated for
the PEAK API.

## Runtime Behavior

PEAK wires the selected Gum provider into runtime capability checks. Strict
mode is the default, so PEAK refuses target Gum mutation if the selected Gum
headers do not expose the PEAK API.
`PEAK_DETACH_BACKEND=helper` forces the external ptrace helper,
`PEAK_DETACH_BACKEND=signal` forces the in-process strict signal backend, and
plain unset/`strict`/`auto` uses `auto`: helper by default, with signal selected
up front when Linux `ptrace_scope` is detected as blocking helper attachment.
Auto also falls back to signal when helper startup is unavailable or a
structured helper STOP response reports permission denied, timeout, or
unsupported. Helper protocol loss or no-response after STOP remains fail-stop
because the controller cannot prove whether target threads were already stopped.

With stock Gum, strict mode still fails closed with `missing-gum-api`. With a
patched devkit, PEAK uses a strict stop backend to hold every non-controller
Linux TID, read PCs, let the in-process controller classify those PCs through
the patched Gum API, optionally move audited trampoline PCs to Gum-provided safe
PCs, perform the physical patch write while all other threads remain stopped or
parked, and then detach/resume or release the threads. Gum bookkeeping detach is
intentionally deferred until after the strict backend releases threads, or until
final shutdown, so PEAK restores the target entry without asking
non-thread-safe Gum code to modify live patches in the dangerous window.

The signal backend is intended for ptrace-restricted environments. It reserves
an available real-time signal for PEAK, installs a protective lease handler,
guards that lease through the common dynamically linked libc signal APIs, and
sends thread-directed stop requests with hidden `rt_tgsigqueueinfo` cookies so
unrelated user signal traffic cannot satisfy a PEAK parked-thread slot. User
attempts through those libc APIs to steal, block, wait on, signalfd-use,
timer/mqueue/AIO-generate, or send the reserved signal cause PEAK to migrate its
lease to another available unblocked RT signal before forwarding the user call.
PEAK also guards dynamically linked `syscall()` calls for the common signal
syscalls that can steal, block, wait on, consume, or generate the reserved RT
signal. If PEAK cannot migrate because the signal was forced, no replacement is
available, or a mutation window is active, the user call fails with `EINVAL` and
signal-backed mutation remains fail-closed. Direct syscall instructions from
inline assembly, static code outside the preload symbol path, or JIT code are
outside this wrapper boundary, but an actual delivered reserved signal without a
valid PEAK cookie contaminates the signal backend and makes signal-backed
mutation fail closed. The backend parks threads in an
async-signal-safe atomic corridor, confirms each required PC rewrite before
release, revalidates `/proc/self/task` immediately before byte writes, and
holds PEAK's pthread-start gate until `finish_hook_mutation` releases the
mutation window. Existing-hook mutations require Gum PC diagnostics; missing
corridor metadata is a fail-closed classification error. Signal-backed physical
mutation also requires the `pthread_create` wrapper to confirm that the thread
creation gate is installed. A stolen handler, unexpected non-cookie delivery,
truly blocked reserved signal, missing arrival, unknown PC, failed rewrite,
missing pthread gate, or failed release is fail-closed; after a physical
byte/register mutation starts, failed release is fatal.

The helper is currently built on Linux x86_64 and Linux Arm64 and installed as
`bin/peak_detach_helper`. At runtime, `PEAK_DETACH_HELPER` has priority when
set. Otherwise the library tries a relocation-safe path derived from the loaded
`libpeak.so` location
(`../bin/peak_detach_helper`), a same-directory helper for local packaging, the
configured install-prefix path, and finally the configured build-tree helper
path. The build-tree fallback is last so installed libraries do not prefer stale
helpers left behind by an old build directory.

The controller launches the helper with a duplicated sanitized environment,
strips `LD_PRELOAD`, `LD_AUDIT`, and `PEAK_*`, moves the protocol socket to a
fixed descriptor, closes inherited descriptors when the kernel supports
`close_range`, and resets inherited helper state after `fork()`. The mutation
guard is a recursive pthread mutex so PEAK's own helper `fork()` cannot deadlock
against the atfork prepare handler while strict prepare already owns the guard.
The helper binds to the first target PID it serves, so a forked child cannot
reuse the parent's helper socket for a different process.

Hook-level shutdown mutations release stopped threads with `RESUME`; PEAK sends
one final helper `SHUTDOWN` after all hook teardown work is complete rather than
leaving helper teardown to descriptor close. If PEAK loses contact with the
helper after a STOP window, or if the helper reports `RELEASE_FAILED` because it
could not prove STOP cleanup released all tracees, strict mode terminates the
process instead of running with an unknown stopped-thread state. Helper writes
use `MSG_NOSIGNAL` and the helper ignores `SIGPIPE`, so controller disconnects
cannot skip cleanup by terminating the helper in the middle of a response. The
controller's fatal diagnostic write is nonblocking so a full stderr pipe cannot
hold stopped tracees indefinitely.

On Linux systems where `PR_SET_PTRACER` is unavailable (`EINVAL`/`ENOSYS`), PEAK
continues and lets the helper's actual `ptrace` operation decide whether the
stop is permitted. `EPERM`/`EACCES` from `PR_SET_PTRACER` or `ptrace` still fail
closed.
