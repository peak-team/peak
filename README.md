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
selected CPU functions, optional CUDA kernels, memory allocation activity, and
JIT-published code without requiring application recompilation. Linux uses
`LD_PRELOAD`; macOS uses `DYLD_INSERT_LIBRARIES` for baseline CPU profiling.

PEAK is designed for long-running and MPI applications where profiler overhead,
safe attach and detach behavior, and reliable final reports matter.

## Highlights

- Profile named functions instead of instrumenting the whole application.
- Inject PEAK around existing Linux and macOS applications without rebuilding
  them.
- Control profiling overhead by detaching and reattaching selected targets.
- Produce human-readable and CSV reports, including MPI-aware aggregation.
- Optionally profile CUDA kernels, memory activity, and JIT-published symbols.

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

- Linux for the full `LD_PRELOAD` runtime path, or macOS for baseline named CPU
  function profiling through `DYLD_INSERT_LIBRARIES`
- CMake 3.13 or newer
- C and C++ compilers (a Fortran compiler is needed only for
  `PEAK_ENABLE_FORTRAN_TESTS=ON`)
- POSIX threads and standard platform runtime libraries

MPI, CUDA, and OTF2 memory-trace export are optional. CUDA profiling requires
CUDA Toolkit 11.2 or newer. On Linux x86_64 and Arm64, the default `auto`
provider downloads a pinned Frida Gum devkit and applies the PEAK patch; on
macOS x86_64 and Arm64, it downloads a pinned stock devkit. Other
platforms/architectures require a caller-provided Frida Gum provider. For
controlled or offline builds, set
`PEAK_FETCH_DEPS=OFF` and provide Frida Gum through `FRIDA_GUM_LIBRARIES` and
`FRIDA_GUM_INCLUDE_DIRS`, or select a caller-provided `patched-devkit`.

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
| `PEAK_ENABLE_OTF2=ON` | Enable OTF2 memory-trace export. Default: `OFF`; CSV memory profiling remains available. |
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

The macOS path currently supports startup attachment and reporting for named
CPU functions. Linux-specific physical detach/reattach, the detach helper,
raw-syscall exec handling, CUDA profiling, and Linux signal-policy interception
are unavailable on macOS. macOS CI builds and installs PEAK on Arm64 and runs a
real `DYLD_INSERT_LIBRARIES` profiling smoke test with MPI, CUDA, and OTF2
disabled.

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
| `PEAK_ALLOW_UNSAFE_GUM_PROLOGUE` | Diagnostic override that permits known or suspected unsafe prologues. |
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
| `PEAK_CUDA_EVENT_POOL_CAPACITY` | CUDA event-pool capacity. Default: `256` events; accepts `1` through `65536`. |
| `PEAK_MEMORY_PROFILE` | Enable experimental memory allocation profiling for selected CPU targets. |
| `PEAK_MEMORY_TRACK_ALL` | Track all allocation events instead of filtering by target backtraces. |
| `PEAK_MEMLOG_PATH` | Memory CSV output prefix. Default: `./peak_memlog`. |
| `PEAK_MEMLOG_CHUNK_EVENTS` | Experimental memory-profiler fixed event capacity. A positive decimal value allocates that many slots once; when full, new events are dropped and reported at finalization. The mapping never grows or moves. |
| `PEAK_MEMLOG_OTF2_DIR` | Override the directory for memory-profile OTF2 output. |

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
  CUDA graphs may be reported as graph execution or as captured launches,
  depending on how the graph was created.
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
