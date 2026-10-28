# Profiling an Already Running Process

The optional `peak-attach` executable attaches PEAK to an existing process and
starts a normal PEAK profiling runtime there. The initial profiling hooks are
installed once; PEAK's function timers, heartbeat policies, overhead control,
and final reporting then follow the regular runtime. Calls made before
activation are outside the profile.

The supported attach builds are Linux x86_64 and Arm64, and macOS Arm64. Build
with `PEAK_BUILD_ATTACH=ON` to install the executable and its separate
`libpeak-attach` runtime alongside PEAK. The normal preload runtime is not
reused for injection.

## Build

On Linux x86_64 or Arm64, PEAK needs its PEAK-patched Frida Gum provider and
Frida Core 17.15.3. The default dependency setup downloads pinned artifacts;
for an offline build, select a `patched-devkit` provider and point
`PEAK_PATCHED_GUM_ROOT` at that devkit, and provide Frida Core with
`FRIDA_CORE_LIBRARIES` and `FRIDA_CORE_INCLUDE_DIRS`.
Attach builds require the current PEAK staged-mutation Gum API. The default
auto-patched provider builds it from the checked-in overlay; a caller-provided
patched devkit must be rebuilt with that overlay before enabling attach.

On macOS Arm64, use PEAK's pinned stock Frida Gum devkit and Frida Core
17.15.3. The regular macOS Arm64 Gum requirements in the main README apply to
the attach runtime as well. Attach support is not available on macOS x86_64.
The macOS install includes `libpeak-attach-bootstrap.dylib` beside the full
`libpeak-attach.dylib` runtime. Frida injects this small bootstrap library; it
uses native dyld to load the full runtime and its system dependencies. Keep
`peak-attach` and both libraries together in their installed relative layout
when packaging or moving an installation.

For a default Linux build:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DPEAK_BUILD_ATTACH=ON
cmake --build build
cmake --install build
```

The same command works on macOS Arm64 when using PEAK's pinned providers.
When dependency fetching is disabled, pass the Frida variables above along
with the corresponding Gum variables documented in the main README.

## Attach

Start the application normally. In another shell, run:

```bash
PEAK_TARGET='compute_step,solver!advance' \
PEAK_HEARTBEAT_INTERVAL=0.1 \
PEAK_GLOBAL_OVERHEAD_RATIO=0.1 \
peak-attach 12345 --output ./attached-profile
```

`peak-attach` takes a PID. `--target` accepts comma-separated function
selectors and may be repeated; it overrides `PEAK_TARGET` from the invoking
shell. Selectors may use `module!symbol` to disambiguate a symbol. If
`--target` is omitted, ordinary PEAK target configuration from that shell is
used. The selector syntax and resolution behavior match the regular runtime.
`--output FILE` sets `PEAK_STATSLOG_PATH`; without it, PEAK uses its normal
report path and naming rules. `--agent PATH` overrides the installed runtime
library path; on macOS it names the full `libpeak-attach.dylib` runtime, not
the bootstrap library.

All `PEAK_*` variables from the `peak-attach` process environment are passed
to the attached runtime. This includes the usual overhead budgets, heartbeat
enablement, detach and reattach thresholds, and report settings. Configuration
is captured when the command runs; it does not change the target's environment.
PEAK's existing overhead controller may physically detach and reattach
selected hooks while the attached runtime remains resident.

Attach attempts are serialized. Protocol or configuration errors and other
preflight rejections, such as an invalid selector, unsupported exec option,
incompatible MPI state, or no requested profiling work, leave the process
eligible for a corrected retry. The claim becomes one-shot immediately before
PEAK begins runtime activation and registers process-exit handling. From that
point onward, retries are refused even if startup fails after partially
activating the runtime; its configuration and any callback-visible state must
remain resident. A process with an existing PEAK runtime cannot be attached.
Once activation is confirmed, `peak-attach` exits successfully while profiling
continues in the target. The injected runtime stays resident until normal
process exit, when PEAK writes its usual report. The command does not provide a
separate stop operation or report the target's eventual native exit status.

On Linux, Gum prepares hook metadata, trampolines, and entry bytes before PEAK
stops application threads. The stop window validates the prepared bytes and
program counters, then applies the patch; Gum finalizes the metadata after
threads resume. This lets attach use the same safe mutation path as PEAK's
overhead controller without allocating Gum metadata while application threads
are stopped.

The debugger's initial attach and runtime bootstrap briefly interrupt target
execution. A syscall already in flight may return `EINTR` if it is not
restartable. PEAK's runtime filesystem-stat guards cover later calls; they
cannot protect a syscall that was already in progress before injection.

Attach does not support `PEAK_EXEC_CHAIN` or `PEAK_EXEC_CHECKPOINT`; those
features depend on loader interposition installed before the target starts.
Automatic discovery of new dynamic libraries and the associated runtime
`dlopen` attach queue are also unavailable on macOS. Linux CPU and memory
attach have runtime coverage. CUDA and MPI attach paths currently have
build-time coverage only, not end-to-end runtime validation.

## Permissions and platform details

On Linux, the attaching user needs the existing process-tracing (ptrace)
permission to inspect and inject into the target. This commonly permits
same-user processes, subject to Yama, container, and administrator policy. If
the host denies tracing, `peak-attach` reports the injection failure; use the
permissions configured for the workload.

On macOS Arm64, live attach uses the platform's task debugging permissions.
Protected or non-debuggable targets may refuse attachment. For native attach
tests, PEAK signs the fixture with `com.apple.security.get-task-allow` and
signs the injector with `com.apple.security.cs.debugger`; the entitlement
files are
[`fixture.entitlements`](../test/attach/fixture.entitlements) and
[`debugger.entitlements`](../test/attach/debugger.entitlements). These test
signing settings describe the permissions required by the test fixture; they
are not a recommendation to weaken signing policy for production apps.

The macOS Arm64 backend installs the initial hook set while holding target
threads and uses the staged Darwin patch operation. After initialization, the
Darwin controller can stage `ATTACH` and `REPLACE` operations for new entries;
`REVERT` and `SHUTDOWN` remain unsupported. This controller support does not
provide automatic `dlopen` discovery or JIT target activation. The safety
scope for subsequent zero-overhead detach and reattach remains pthread-based;
direct Mach thread creation is not covered. Targets whose entry resolves
through a branch or thunk to another address are rejected to preserve
attribution. See
[Physical detach controller](physical-detach-controller.md) for lifecycle and
controller details.
