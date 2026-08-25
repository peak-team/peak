<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/peak-logo-horizontal-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="docs/assets/peak-logo-horizontal-light.svg">
    <img src="docs/assets/peak-logo-horizontal-light.svg"
         alt="PEAK — Performance Evaluation and Analysis Kit"
         width="720">
  </picture>
</p>

[![CMake](https://img.shields.io/github/actions/workflow/status/peak-team/peak/cmake.yml?branch=main&logo=GitHub&label=cmake)](https://github.com/peak-team/peak/actions/workflows/cmake.yml)
[![MPI](https://img.shields.io/github/actions/workflow/status/peak-team/peak/mpi.yml?branch=main&logo=GitHub&label=mpi)](https://github.com/peak-team/peak/actions/workflows/mpi.yml)
[![License](https://img.shields.io/github/license/peak-team/peak)](LICENSE)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/peak-team/peak)

# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a dynamic-library injection profiler for HPC applications. It profiles
selected CPU functions and, on Linux, optional CUDA kernels, memory allocation
activity, and JIT-published code without requiring application recompilation.
Linux uses `LD_PRELOAD`; macOS uses `DYLD_INSERT_LIBRARIES` for named CPU
profiling, with physical detach and reattach available on Arm64.

PEAK is designed for long-running and MPI applications where profiler overhead,
safe attach and detach behavior, and reliable final reports matter.

## Highlights

- Profile named functions instead of instrumenting the whole application.
- Inject PEAK around existing Linux and macOS applications without rebuilding
  them.
- Control profiling overhead by detaching and reattaching selected targets.
- Produce human-readable and CSV reports, including MPI-aware aggregation.
- On Linux, optionally profile CUDA kernels, memory activity, and JIT-published
  symbols.

## Quick Start

### Linux

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .

PEAK_TARGET=my_function \
LD_PRELOAD="$PWD/src/libpeak.so" \
../target_application
```

PEAK writes a report to stderr and a CSV profile log using the default prefix
`./peak_statslog`.

### macOS

Build the baseline macOS configuration, then inject the installed library into
an application you built or otherwise control:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DPEAK_ENABLE_MPI=OFF \
  -DBUILD_CUDA_PROFILE=OFF
cmake --build build
cmake --install build

PEAK_TARGET=my_function \
DYLD_INSERT_LIBRARIES="$HOME/.local/lib/libpeak.dylib" \
./target_application
```

System Integrity Protection prevents `DYLD_INSERT_LIBRARIES` from affecting
protected Apple system executables. Use PEAK with an ordinary application you
built or installed outside the protected system locations.

## Requirements

- Linux for the full `LD_PRELOAD` runtime path, or macOS for named CPU function
  profiling through `DYLD_INSERT_LIBRARIES` (including physical detach and
  reattach on Arm64)
- CMake 3.13 or newer
- C and C++ compilers (a Fortran compiler is needed only for
  `PEAK_ENABLE_FORTRAN_TESTS=ON`)
- POSIX threads and standard platform runtime libraries

MPI, CUDA, and OTF2 memory-trace export are optional. CUDA profiling requires
CUDA Toolkit 11.2 or newer. On Linux x86_64 and Arm64, the default `auto`
provider downloads a pinned Frida Gum devkit and applies the PEAK patch; on
macOS x86_64 and Arm64, it downloads a pinned stock devkit. The pinned macOS
Arm64 devkit also enables PEAK's small Darwin patch-metadata overlay. Other
platforms/architectures require a caller-provided Frida Gum provider. For
controlled or offline builds, set
`PEAK_FETCH_DEPS=OFF` and provide Frida Gum through `FRIDA_GUM_LIBRARIES` and
`FRIDA_GUM_INCLUDE_DIRS`, or select a caller-provided `patched-devkit`.

macOS Arm64 v1 requires PEAK's default downloaded, hash-pinned Frida Gum
17.15.3 devkit. A caller-provided Gum is not supported for that path: startup
target-identity checking depends on its exported Arm64 reader symbol, and
physical detach depends on the pinned private patch-metadata layout. Provider
portability is deferred to [#80](https://github.com/peak-team/peak/issues/80).

## Build and Install

On Linux x86_64 and Arm64, the default build downloads the pinned Frida Gum
devkit and applies the PEAK patch; on macOS x86_64 and Arm64, it downloads a
pinned stock devkit. Other platforms/architectures require a caller-provided
Frida Gum provider:

```bash
mkdir -p build
cd build
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  ..
cmake --build .
cmake --build . --target install
```

Common CMake options:

| Option | Purpose |
| --- | --- |
| `PEAK_ENABLE_MPI=OFF` | Build without MPI support. |
| `BUILD_CUDA_PROFILE=OFF` | Build without CUDA profiling support. |
| `PEAK_FETCH_DEPS=OFF` | Disable dependency downloads. Provide Frida Gum explicitly for controlled/offline builds. Default: `ON`. |
| `PEAK_ENABLE_OTF2=ON` | Enable Linux OTF2 memory-trace export. Default: `OFF`; Linux CSV memory profiling remains available. |
| `PEAK_USE_SYSTEM_OTF2=ON` | Search for a system OTF2 before fetching it. |
| `PEAK_ENABLE_FORTRAN_TESTS=ON` | Build the optional Fortran dgemm tests and require a Fortran compiler. |
| `BUILD_TESTING=ON` | Build the CTest suite. `BUILD_TESTS=ON` is also accepted. |
| `PEAK_BUILD_BENCHMARKS=ON` | Build benchmark and stress targets. |
| `PEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS=ON` | Enable opt-in real-runtime JIT preload tests when testing is enabled. |

After installation:

```bash
# Linux
PEAK_TARGET=my_function \
LD_PRELOAD="$HOME/.local/lib/libpeak.so" \
./target_application

# macOS
PEAK_TARGET=my_function \
DYLD_INSERT_LIBRARIES="$HOME/.local/lib/libpeak.dylib" \
./target_application
```

The macOS path supports startup attachment and reporting for named CPU
functions only while the process is still single-threaded. PEAK proves that
condition before Gum initialization and refuses the entire activation without
installing hooks if an early constructor or deferred post-MPI activation has
already made the process multithreaded. Runtime `ATTACH`, `REPLACE`, and
`REVERT` mutations fail closed as unsupported; PEAK therefore does not install
its `dlopen` listener, `dlclose` guard, ownership thread, or dynamic-attach
queue on macOS. The JIT metadata provider is also kept disabled on macOS.
`PEAK_MEMORY_PROFILE` is also rejected explicitly rather than starting a
partially supported memory profiler.

On Arm64, PEAK supports zero-overhead physical detach and reattach for
pthread-based workloads: it gates `pthread_create`, suspends the other
enumerated Mach threads, changes only the saved entry patch bytes, and resumes
them after the mutation. A second enumeration covers a pthread creator that
crossed the gate before it closed; this v1 does not claim to cover threads
created directly through Mach APIs. If a thread is observed inside the
overwritten prologue, PEAK resumes every thread and retries through the normal
controller backoff; it deliberately does not single-step threads for liveness.
Before startup attach, Apple Arm64 rejects any target entry that stock Gum would
resolve to another address. Darwin v1 has no exact-entry attach, so profiling
such a branch or thunk would silently attribute direct calls to the destination
to the requested symbol; `PEAK_ALLOW_UNSAFE_GUM_PROLOGUE` cannot override this
target-identity check.

Entry-byte mutation is eligible only when the canonical Gum context contains
exactly the requested PEAK listener and has no replacement; a context shared by
another Gum client fails closed and remains instrumented.
At process exit, Darwin stops PEAK-owned controller work and writes the final
report but leaves installed Gum hooks, listeners, and their reachable state
alive for the operating system to reclaim; it does not claim an unheld final
shutdown mutation is safe. The Linux detach helper, raw-syscall exec handling,
CUDA profiling, memory profiling, runtime dynamic attach, and Linux
signal-policy interception remain Linux-only. macOS CI builds and installs
PEAK on Arm64 and runs real `DYLD_INSERT_LIBRARIES` profiling plus startup
rejection, redirect-attribution rejection, and single- and multithreaded
physical detach/reattach lifecycle tests with MPI, CUDA, and OTF2 disabled.

On Linux x86_64 and Arm64, the detach helper is built and installed at the
configured `${CMAKE_INSTALL_BINDIR}/peak_detach_helper` path (by default,
`bin/peak_detach_helper`). Set `PEAK_DETACH_HELPER` when a package layout
requires a different helper path.

Installed CMake consumers can use `find_package(PEAK CONFIG REQUIRED)` and link
`PEAK::peak`.

## Essential Usage

Select one or more CPU functions:

```bash
PEAK_TARGET=dgemm_,dgemv_ \
LD_PRELOAD=/path/to/libpeak.so \
./target_application
```

Run under MPI:

```bash
mpirun -n 4 env \
  PEAK_TARGET=my_mpi_work \
  LD_PRELOAD=/path/to/libpeak.so \
  ./mpi_application
```

Keep the final report but suppress routine warnings:

```bash
PEAK_TARGET=my_function \
PEAK_VERBOSITY=quiet \
LD_PRELOAD=/path/to/libpeak.so \
./target_application
```

Set `PEAK_TEXT_OUTPUT=0` for per-function CSV-only output. Heartbeat
profile/control/risk diagnostics are currently text-only. `PEAK_TARGET`,
`PEAK_TARGET_GROUP`, and `PEAK_TARGET_FILE` are merged into one CPU target list.

### C++ target selectors

For C++ targets, use a full demangled signature, an exact mangled name, or a
module-qualified selector. For example:

```bash
PEAK_TARGET='/opt/lib/libfoo.so!namespace::Class::func(int,double),libfoo.so!_ZN9namespace5Class4funcEid'
```

`module!symbol` restricts resolution to matching loaded DSOs. A basename
matches every loaded DSO with that basename; a path is matched exactly or
canonically when both paths can be resolved. PEAK attaches function listeners
only at resolved function entries; arbitrary instruction offsets are not a
supported target syntax. PEAK first considers exact symbol names, then exact full
demangled signatures, then legacy C++ short names. A selector with more than
one candidate at its best matching level is not attached; PEAK prints every
candidate instead of choosing one. Reports retain the module spelling from a
module-qualified selector, for example
`/opt/lib/libfoo.so!namespace::Class::func(int, double)`, so equal signatures
from different DSOs remain distinguishable.

Use the installed inspection command before profiling when selecting an
overload or checking several DSOs:

```bash
peak inspect-symbols --module /opt/lib/libfoo.so --module /opt/lib/libbar.so 'namespace::Class::func(int,double)'
```

It prints each candidate's address, module, mangled name, and full demangled
signature. Its exit status is `0` for one candidate, `1` for no candidate or
ambiguity, and `2` for invalid input or an explicitly requested module that
cannot be loaded. A module-qualified selector implicitly loads that module for
inspection only; profiling itself never loads a DSO to resolve a target.

## Configuration

PEAK is configured through environment variables. The tables below are a
compact index; the linked design documents describe the safety-sensitive paths
in detail.

### Targets and Reports

| Variable | Purpose |
| --- | --- |
| `PEAK_TARGET` | Comma-separated CPU symbol names or C++ selectors. Fortran names commonly need a trailing underscore. Use `PEAK_TARGET_FILE` for long or complex selector lists (one complete selector per line). |
| `PEAK_TARGET_GROUP` | Comma-separated built-in groups: `FFTW`, `PBLAS`, `ScaLAPACK`, `LAPACK`, and `BLAS`. |
| `PEAK_TARGET_FILE` | File containing one complete CPU target name or C++ selector per line; this avoids comma-list quoting concerns for complex signatures. |
| `PEAK_PROFILE_INTERPRETERS` | Allow normally skipped interpreter processes to initialize PEAK. |
| `PEAK_ENABLE_CXX_SYMBOL_SCAN` | At startup, permit legacy C++ short-name matching only after an ordinary exact lookup misses. After `dlopen()`, legacy matching is available only through an absolute or slash-containing `path!symbol` selector; an unqualified C miss remains unresolved for later DSOs. |
| `PEAK_STATSLOG_PATH` | CSV output prefix. Default: `./peak_statslog`. |
| `PEAK_STATSLOG_TEMPLATE` | Complete statistics-CSV pathname template. Supports `{jobid}`, `{stepid}`, `{host}`, `{rank}`, `{pid}`, and `{session}`; for example `{jobid}/{stepid}/{host}/peak-{rank}-{pid}-{session}.csv`. Missing parent directories are created for an explicit template. A template without `.csv` is valid; an exec checkpoint appends `-execN.csv` to it, while the final report uses the literal rendered name. The default adds all identity fields plus `.csv` to the prefix. |
| `PEAK_MEMLOG_TEMPLATE` | Complete memory-log CSV pathname template; it supports the same fields. |
| `PEAK_OUTPUT_ALLOW_OVERWRITE` | Explicitly permit replacement of a completed statistics CSV. Default: disabled. |
| `PEAK_TEXT_OUTPUT` | Force or suppress the human-readable stderr report. |
| `PEAK_VERBOSITY` | `silent`, `report`/`quiet`, `warn`, `info`, or `debug`; numeric levels `0` through `4` are also accepted. |
| `PEAK_NAME_TRUNCATE` | Truncate long function and kernel names in text output. |
| `PEAK_MAX_NUM_THREADS` | Tracked-thread capacity. Default: twice the online CPU count; `0` uses one slot and values above `4096` clamp. |

Every text and stats CSV report includes a capability manifest for CPU targets,
strict mutation, CUDA, memory tracking, JIT input, dynamic DSO discovery, and
the selected output transport. Each row distinguishes `requested`, `compiled`,
`active`, `partial`, `retained`, and `failed` state. CSV rows prefixed with
`PEAK_CAPABILITY_` provide the same machine-readable fields. CUDA reports also
include stable masks for compiled, found, installed, and failed API families.
MPI and socket aggregation preserve the common installed coverage, OR
failure/degradation evidence across ranks, and mark rank-varying coverage as
partial. Report-transport `active` state records the transport that actually
published that report; a fallback marks the requested transport partial and
failed while marking the rank-local or socket fallback active. An unavailable
requested optional backend fails open: PEAK emits one warning, records the
failed capability, and continues the application and any independently
installed profiler backends.

### MPI Output

| Variable | Purpose |
| --- | --- |
| `PEAK_MPI_ACTIVATION_POLICY` | Runtime activation timing: `immediate` (default) preserves complete startup and pre-MPI profiling. `post-init` is an explicit large-scale MPI loader-safety mode that performs no Gum mutation, module pinning, controller startup, or heartbeat work until the outermost intercepted `MPI_Init`, `PMPI_Init`, `MPI_Init_thread`, or `PMPI_Init_thread` returns. It intentionally omits all earlier behavior, including a target invocation that contains MPI initialization; `defer` and `deferred` are accepted aliases. Use the default for non-MPI, MPI Sessions-only, static/hidden MPI bindings, and any runtime whose traditional C init symbols cannot be dynamically interposed. |
| `PEAK_OUTPUT_AGGREGATION` | Final output transport: `mpi` (default), `socket`, or `local`, with documented aliases. On the intercepted-finalize path, socket/local publish before MPI teardown coordination and fold participation into the long release gate; MPI aggregation remains proof-first. |
| `PEAK_MPI_COLLECTIVE_OUTPUT` | Legacy aggregate-output switch; `PEAK_OUTPUT_AGGREGATION` takes precedence. |
| `PEAK_MPI_FINALIZE_POLICY` | Report during MPI finalization (`report`, the default for every transport) or explicitly defer PEAK output until process exit (`defer`). Unless `PEAK_MPI_REAL_FINALIZE=0`, `defer` calls the real finalizer immediately and therefore bypasses the Intel MPI 2019 compatibility skip. |
| `PEAK_MPI_REAL_FINALIZE` | Override for the real MPI finalizer. Healthy non-Intel-MPI-2019 jobs enable it by default; Intel MPI 2019 skips its crash-prone finalizer unless set to `1`. Setting it to `0` also disables the immediate real-finalizer call in `defer` mode. Setting it to `1` cannot override a failed collective safety gate on the default `report` path. |
| `PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS` | Timeout for the proof-first MPI aggregation finalization-participation check. Default: `10000`. |
| `PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS` | Baseline timeout for the post-publication all-rank release gate. For socket/local output this same gate also proves finalize participation. Default: `180000`; only a path that attempted socket publication raises the effective timeout to at least the peer release budget plus two socket-phase margins (`300000` for a singleton-size default and scale-adjusted for larger jobs). |
| `PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS` | Timeout for each MPI payload reduction. Default: `5000`. |
| `PEAK_OUTPUT_AGGREGATION_HOST` | Override the socket reducer host. |
| `PEAK_OUTPUT_AGGREGATION_PORT` | Override the socket reducer port. |
| `PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS` | Socket no-progress timeout and root-release phase timeout. Default: `60000`. The absolute gather cap adds `5000` ms per 128-peer wave, with at most `300000` ms adaptive margin; explicit values are never shortened. |
| `PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS` | Peer-side end-to-end socket release budget spanning the absolute gather cap, report publication, and confirmed release. Default and minimum: gather hard cap plus two socket phase timeouts. |
| `PEAK_OUTPUT_AGGREGATION_TOKEN` | Override the deterministic socket peer-enrollment identifier for controlled tests. Root still issues an unpredictable per-report session nonce; neither value is authentication. |
| `PEAK_OUTPUT_AGGREGATION_BIND_ADDRESS` | Bind the socket reducer to this IPv4 node-local address. By default it binds only the resolved root-host address. |
| `PEAK_OUTPUT_AGGREGATION_ALLOW_BROAD_BIND` | Explicitly permit binding the reducer to all interfaces. Default: disabled. |
| `PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK` | Enable MPI-reducer-to-socket and socket-to-rank-local fallback paths. Default: enabled. |

MPI finalization and aggregation are deliberately bounded and locally
fail-closed. A rank that observes a collective error or timeout stops issuing
later MPI teardown calls.
After every rank completes its report or transport responsibility, a separate
bounded all-rank release gate propagates publication success. Healthy runtimes
then hand off to the real `PMPI_Finalize()`. Intel MPI 2019 skips that handoff by
default because its hwloc teardown can crash after PEAK instrumentation; this
compatibility path is non-conforming and must be validated with the target
launcher. A gate error or timeout permanently disables later teardown MPI
calls, even when `PEAK_MPI_REAL_FINALIZE=1` was requested.
CSV publication uses the same job/step/host/rank/PID/session identity as the
default report name, including rank-local fallback. Completed outputs are
published no-clobber by default; replacement requires the explicit
`PEAK_OUTPUT_ALLOW_OVERWRITE` opt-in.
In both CSV and text reports, `count` is the exact total call count,
`per_thread` is the ceiling over active threads, and `per_rank`/`avg/rank` is
the non-truncated arithmetic mean over all ranks represented by the report.
See [Physical detach controller](docs/physical-detach-controller.md) for the
full output and teardown behavior.

### Overhead Control

| Variable | Purpose |
| --- | --- |
| `PEAK_COST` | Profiling-cost budget in seconds used to derive a call-count detach threshold. |
| `PEAK_DETACH_COUNT` | Explicit positive call-count detach threshold; overrides the derived threshold. |
| `PEAK_HEARTBEAT_INTERVAL` | Heartbeat interval in seconds. Default: `0.1`; `0` disables the monitor. |
| `PEAK_HIBERNATION_CYCLE` | Heartbeat cycles between reattach checks. Default: `50`; `0` disables reattach checks. |
| `PEAK_OVERHEAD_RATIO` | Per-target profiling-overhead ratio. Default: `0.1`. |
| `PEAK_GLOBAL_OVERHEAD_RATIO` | Global profiling-overhead ratio. Default: `0.1`. |
| `PEAK_ENABLE_PER_TARGET_HEARTBEAT` | Enable per-target heartbeat detach decisions. |
| `PEAK_ENABLE_GLOBAL_HEARTBEAT` | Enable global heartbeat detach decisions. |
| `PEAK_GLOBAL_DETACH_FACTOR` | Global detach hysteresis factor. Default: `1.2`. |
| `PEAK_GLOBAL_REATTACH_FACTOR` | Global reattach hysteresis factor. Default: `0.85`. |
| `PEAK_ENABLE_REATTACH` | Allow physical reattach. Default: enabled, but reattach also requires a running heartbeat, a nonzero hibernation cycle, and at least one enabled heartbeat policy. |
| `PEAK_REATTACH_COOLDOWN_MS` | Minimum detached time before reattach eligibility. Default: `60000`. |
| `PEAK_HB_MIN_US`, `PEAK_HB_MAX_US` | Adaptive heartbeat sleep bounds. Defaults: `10000` and `500000` microseconds. |
| `PEAK_HB_K_ERR`, `PEAK_HB_K_RATE` | Adaptive response coefficients. Defaults: `3.0` and `0.8`. |
| `PEAK_HB_EMA_A` | Growth-rate EMA alpha in `(0, 1]`. Default: `0.3`. |

PEAK creates the heartbeat helper only when at least one CPU target is
requested. GPU-only, memory-only, and JIT-only workloads do not start it.

Numeric overhead-control values must consume the complete value and be finite.
Invalid present values emit one warning and use their documented default.
`PEAK_COST` and the overhead ratios are nonnegative; a zero ratio remains an
immediate threshold for the existing `ratio > threshold` comparison. Heartbeat
sleep bounds are positive, and a maximum below an
accepted minimum is raised to that minimum. Global hysteresis requires a
detach factor of at least `1`, a reattach factor in `(0, 1]`, and the reattach
factor strictly below the detach factor; a conflicting reattach value uses its
default. `PEAK_REATTACH_COOLDOWN_MS=60000` remains a policy default, not a
safety requirement.

For runtime behavior, accounting, and tuning, see
[Heartbeat mechanism and runtime policy](docs/heartbeat.md).

### Exec-Chain Profiling

| Variable | Purpose |
| --- | --- |
| `PEAK_EXEC_CHAIN` | Explicit startup opt-in to keep PEAK available across eligible exec and spawn children. Default: disabled. |
| `PEAK_EXEC_CHECKPOINT` | Explicit startup opt-in to write a best-effort checkpoint before direct exec calls. Default: disabled. |
| `PEAK_EXEC_PROPAGATE_PEAK_ENV` | Copy missing parent `PEAK_*` settings into a child environment. Default: enabled. |

Enabling either startup option may enter PEAK's rich exec handling, which can
inspect environments, allocate, trace, and checkpoint and is not
async-signal-safe. With both options disabled, array-based wrappers bypass
directly to primed libc functions; variadic adapters only perform their bounded
stack-based argument scan before calling the corresponding primed libc
function.

Before constructor publication on Linux x86_64/aarch64, direct-path exec APIs
issue raw kernel calls, while PATH-search APIs perform a bounded stack search
whose candidates also use raw kernel calls. Unsupported architectures preserve
native exec semantics through libc resolution in this window, so the
resolver-free async-signal-safe guarantee does not apply there.
Pre-constructor `posix_spawn*` retains libc resolution on every architecture to
preserve native spawn semantics and is not async-signal-safe.

See [Exec-chain profiling](docs/exec-chain.md) for supported API families,
child-environment precedence, fork safety, and limitations.

### Detach and Safety

| Variable | Purpose |
| --- | --- |
| `PEAK_SAFE_DETACH_MODE` | Select strict automatic, helper, or signal behavior. |
| `PEAK_DETACH_BACKEND` | Override strict backend selection with `helper` or `signal`. |
| `PEAK_DETACH_HELPER` | Path to an executable `peak_detach_helper`. |
| `PEAK_DETACH_SIGNAL` | Reserve `auto`, a real-time signal number, `SIGRTMIN+n`, or `SIGRTMAX-n` for the signal backend. |
| `PEAK_SIGNAL_RESERVE_EARLY` | Constructor-time signal reservation policy: `auto`, `always`, `forced-only`, or `never`. |
| `PEAK_CONTROLLER_MAX_PENDING_AGE_MS` | Maximum retry age for a pending transition. Default: `30000`; `0` disables this bound. |
| `PEAK_CONTROLLER_MAX_RETRY_COUNT` | Maximum retry count for a pending transition. Default: `300`; `0` disables this bound. |
| `PEAK_STRICT_GATE_WAIT_TIMEOUT_MS` | Thread-creation gate timeout. Default: `10000`; `0` waits indefinitely. |
| `PEAK_DETACH_TRACE_PATH` | Optional CSV path for detach-controller transition evidence. |
| `PEAK_UNSAFE_GUM_PROLOGUE_POLICY` | Select the default or conservative fail-closed prologue policy. |
| `PEAK_ALLOW_UNSAFE_GUM_PROLOGUE` | Diagnostic override that permits known or suspected unsafe prologues; it does not bypass Darwin target-identity rejection. |
| `PEAK_REQUIRE_SAFE_DETACH` | Deprecated and ignored legacy knob; strict physical transition safety remains enabled. |

See [Physical detach controller](docs/physical-detach-controller.md) and
[Patched Frida Gum](docs/patched-frida-gum.md) before changing safety controls.

### Dynamic Loading and JIT

| Variable | Purpose |
| --- | --- |
| `PEAK_DLOPEN_DEBUG` | Enable cumulative dynamic-load diagnostics at shutdown/release/timeout lifecycle points; stderr output also requires `PEAK_VERBOSITY=debug`. |
| `PEAK_DLOPEN_TRACE_PATH` | Optional CSV path for those dynamic-load lifecycle snapshots. |
| `PEAK_JIT_ENABLE` | Enable JIT metadata providers; matching `PEAK_TARGET` names are still required. |
| `PEAK_JIT_PROVIDER` | Comma-separated providers. The current provider is `perfmap` / `perf-map`. |
| `PEAK_JIT_MAP_PATH` | Override the Linux `/tmp/perf-<pid>.map` path. |
| `PEAK_JIT_TRACE_PATH` | Optional CSV path for provider events and JIT records. |
| `PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS` | Retry lifetime for matching code that is not executable yet. Default: `1000`. |
| `PEAK_JIT_DRAIN_RECORD_BUDGET` | Maximum records handled by one controller drain pass. Default: `1024`. |

See [JIT profiling](docs/jit-profiling.md) for provider guarantees and code
lifetime requirements.

See [Runtime `dlopen` profiling](docs/dlopen-profiling.md) for dynamic target
discovery, FFTW first-call behavior, exact exported-entry attribution, queue
semantics, module lifetime, and supported boundaries.

### GPU and Memory

| Variable | Purpose |
| --- | --- |
| `PEAK_GPU_TARGET` | Comma-separated demangled base kernel names. |
| `PEAK_GPU_TARGET_FILE` | File containing one GPU kernel name per line. |
| `PEAK_GPU_MONITOR_ALL` | Profile every observed GPU kernel. |
| `PEAK_CUDA_EVENT_POOL_CAPACITY` | Maximum in-flight CUDA timing samples. Each fixed slot owns one start/end event pair. Default: `256`; accepts `1` through `65536`. |
| `PEAK_CUDA_FINALIZATION_TIMEOUT_MS` | Maximum time for bounded CUDA timing completion at report or detach. Default: `1000`; accepts `1` through `60000`. |
| `PEAK_MEMORY_PROFILE` | Enable experimental Linux memory allocation profiling for selected CPU targets. Rejected on macOS. |
| `PEAK_MEMORY_TRACK_ALL` | On Linux, track all allocation events instead of filtering by target backtraces. |
| `PEAK_MEMLOG_PATH` | Memory CSV output prefix. Default: `./peak_memlog`. |
| `PEAK_MEMLOG_CHUNK_EVENTS` | Experimental memory-profiler fixed export capacity. A positive decimal value reserves that many events plus at most one 64-event reservation block of slack per tracked thread; when full, new events are dropped and reported at finalization. The mapping never grows or moves. |
| `PEAK_MEMLOG_OTF2_DIR` | Override the directory for memory-profile OTF2 output. |

CUDA kernel identity state is also fixed at attach time. The primary identity
cache contains four entries per event-pool slot. A separate overflow-suppression
map is capped at the smaller of that identity capacity and 256 entries.
Configured targets displace cached non-targets when the primary cache is full;
monitor-all overflow is reported as `<identity-overflow>` instead of being
attributed to a real kernel name.

CUDA timing uses one process-wide, fixed-capacity slot pool partitioned by CUDA
context. An event pair is created and reused only in its owning context, after
the previous end event reports completion. Slot allocation, launch accounting,
and producer-to-harvester handoff are partitioned across fixed shards, so
concurrent launch wrappers do not acquire a process-wide lock or update one
shared queue position. A dedicated helper harvests a bounded number of ready
samples without making launch wrappers wait. Neither the launch path nor
finalization calls a device, stream, or context-wide synchronization API. PEAK
initializes the CUDA backend and helper only when
`PEAK_GPU_TARGET`, `PEAK_GPU_TARGET_FILE`, or `PEAK_GPU_MONITOR_ALL` explicitly
requests GPU profiling; CPU-only profiling does not load the Driver API or
create an idle CUDA helper.

When the complete typed Driver launch surface is available, PEAK times Runtime
launches at the corresponding Driver entry point and does not install a second
Runtime launch replacement. This keeps mixed Runtime/Driver coverage and exact
one-sample accounting without sending each Runtime launch through two Gum
dispatches. Driver timing records events through the Driver API, including
when the intercepted Driver launch originated in the Runtime, so the wrapper
does not re-enter the Runtime API from inside a Runtime launch. If that Driver
surface is incomplete, PEAK retains the Runtime launch replacements as the
compatibility path.

The CUDA report distinguishes launches that entered profiler accounting
(`observed`), timing samples that were successfully recorded and queued
(`accepted`), and samples whose elapsed time was harvested (`completed`). Pool
exhaustion skips only the timing sample and increments `pool_full`; the
application launch still runs. This makes `accepted` the sustainable-sampling
count when launches outpace the fixed pool and harvester. Reports also carry
`pool_high_water`, `identity_full`, `harvester_unavailable`, event
creation/record/query/elapsed failures, capture-query failures,
context-query/switch/restore failures, `unsupported_multi_device`,
`dimension_overflow`, and finalization timeout/incomplete counts through the
normal local, socket, or MPI report transport. CUDA kernel `Calls` and timing
averages describe harvested completed samples, not all application launches
when any sample was skipped or dropped.
Kernel and graph identities are also process-bounded; new graph handles beyond
four times the event-pool capacity increment `identity_full` instead of growing
the aggregate map.

Runtime and Driver capture lifecycle listeners close a sharded timing-admission
gate before a capture transition and reopen it only after capture quiescence is
proven. Launch wrappers therefore do not repeat a Driver capture query for each
target. Active capture, incomplete capture interception, or a capture-query
error keeps the gate closed and skips timing without recording events into the
captured stream. Cooperative
multi-device launch APIs are also passed through without timing because one
reusable event pair cannot safely represent multiple device contexts. These
skips are reported and do not change the application launch. The dedicated
capture lifecycle listeners cover both legacy-stream and per-thread-default
entry points. Before timing opens, PEAK validates the Driver's known capture
entry-point variants; incomplete interception or an uninstrumented returned
pointer disables CUDA timing while leaving application CUDA calls unchanged.
Driver launch timing is likewise disabled for ABI-generic redirect stubs that
cannot safely receive typed replacements. The dedicated
harvester enters CUDA's relaxed thread-local capture interaction mode before
timing admission opens, so it can query PEAK events recorded before an
unrelated concurrent global capture without invalidating that capture. The
one-time cold handshake is requested by the first matching CUDA launch; attach
and non-target launches do not initialize CUDA. Matching launches pass through
without waiting while the helper is initializing, and increment
`harvester_unavailable`. If the mode cannot be established, CUDA timing remains
disabled and launches continue to pass through unchanged.

At report or detach, PEAK stops admitting CUDA samples and lets the harvester
run until the configured monotonic deadline. If work remains, PEAK reports the
incomplete count, logs bounded context/device diagnostics, and retains the
affected event state instead of destroying an event in the wrong or unfinished
context. That retained state remains process-owned until operating-system
reclamation, so a blocked CUDA query cannot race library teardown. The
capability manifest records retention decisions made before its report
snapshot. A later physical-detach flush failure is reported by its teardown
warning but cannot retroactively change an already published manifest.

Allocation lifetimes are tracked in a lock-free pointer radix. Memory events
use sharded ordering metadata, and their process-wide `current` totals and
maximum are reconstructed after allocation hooks quiesce. Concurrent
allocation calls therefore neither acquire a process-wide tracking lock nor
update a single process-wide byte counter on the hot path. On Linux, PEAK
initializes the fixed anonymous event buffer before installing hooks, so event
writes incur neither first-write faults nor parallel-filesystem dirty-page
bookkeeping on the allocation hot path.

### Legacy Controls

`PEAK_PAUSE_TIMEOUT` and `PEAK_SIG_CONT_TIMEOUT` configure the legacy
cooperative pause and continue path. Strict physical detach does not rely on
that path.

## Testing

Configure and run the local suite with the default Frida Gum provider. On
Linux/macOS x86_64 and Arm64, it uses a pinned download; other
platforms/architectures need a caller-provided provider:

```bash
mkdir -p build
cd build
cmake -DBUILD_TESTING=ON ..
cmake --build .
ctest --output-on-failure
```

MPI, CUDA, strict-backend, and real-runtime JIT coverage depends on the
toolchains and host capabilities detected during configuration.

## Documentation

- [Heartbeat mechanism and runtime policy](docs/heartbeat.md): implementation
  behavior, overhead accounting, and tuning.
- [Exec-chain profiling](docs/exec-chain.md): child-environment behavior,
  supported exec and spawn APIs, and fork safety limits.
- [Physical detach controller](docs/physical-detach-controller.md): strict
  transition safety, MPI output, and shutdown behavior.
- [Runtime `dlopen` profiling](docs/dlopen-profiling.md): synchronous FFTW
  first-call attachment, asynchronous dynamic targets, and module lifetime.
- [JIT profiling](docs/jit-profiling.md): provider guarantees, retry behavior,
  and metadata lifetime limits.
- [Patched Frida Gum](docs/patched-frida-gum.md): PEAK-specific Gum APIs and
  patched relocation support.

## Caveats

- Lowercase Fortran procedure names commonly require a trailing underscore,
  for example `fortran_procedure_name_`.
- `PEAK_TARGET`, `PEAK_TARGET_GROUP`, and `PEAK_TARGET_FILE` are merged;
  duplicate entries are handled automatically but are best avoided.
- CUDA profiling includes first-use initialization and kernel warm-up effects.
  Launches issued while a stream is being captured are not timed; later graph
  execution outside capture can be reported as graph execution.
- JIT targets require runtime metadata; PEAK does not infer names or boundaries
  from anonymous executable pages.
- Strict detach backend availability depends on platform signal support and,
  for the helper backend, host ptrace policy.
- Heartbeat control uses process-local measurements. Local-rank scaling is a
  reattach policy input and reporting diagnostic, and is conservative only when
  launcher metadata reports the ranks per node correctly. It is not an MPI-wide
  overhead proof; measured A/B overhead remains authoritative. The current
  60-second cooldown is provisional under the linked validation standard.
- MPI output and finalization behavior is runtime-sensitive. MPI aggregation is
  the default. Reporting uses an all-rank post-publication release gate;
  non-Intel-MPI-2019 jobs normally return to the real finalizer, while Intel
  MPI 2019 uses the documented compatibility skip unless explicitly overridden.

## Citation

If you use PEAK in your research, please cite:

```bibtex
@inproceedings{10.1145/3624062.3624143,
  author = {Wang, Yinzhi and Li, Junjie},
  title = {PEAK: a Light-Weight Profiler for HPC Systems},
  year = {2023},
  isbn = {9798400707858},
  publisher = {Association for Computing Machinery},
  address = {New York, NY, USA},
  url = {https://doi.org/10.1145/3624062.3624143},
  doi = {10.1145/3624062.3624143},
  booktitle = {Proceedings of the SC '23 Workshops of The International Conference on High Performance Computing, Network, Storage, and Analysis},
  pages = {677--680},
  numpages = {4},
  keywords = {application performance, profiling, system tools},
  location = {Denver, CO, USA},
  series = {SC-W '23}
}
```

## Contributing

Contributions are welcome through GitHub issues and pull requests. Before
opening a pull request, build PEAK locally, run the relevant CTest coverage,
update user-facing documentation when behavior changes, and keep the change
focused enough to review independently.

## License

PEAK is distributed under the [BSD 3-Clause License](LICENSE).
