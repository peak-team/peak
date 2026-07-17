[![CMake](https://img.shields.io/github/actions/workflow/status/peak-team/peak/cmake.yml?branch=main&logo=GitHub&label=cmake)](https://github.com/peak-team/peak/actions/workflows/cmake.yml)
[![MPI](https://img.shields.io/github/actions/workflow/status/peak-team/peak/mpi.yml?branch=main&logo=GitHub&label=mpi)](https://github.com/peak-team/peak/actions/workflows/mpi.yml)
[![License](https://img.shields.io/github/license/peak-team/peak)](LICENSE)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/peak-team/peak)

# PEAK (Performance Evaluation and Analysis Kit)

PEAK is an `LD_PRELOAD`-based profiler for HPC applications. It profiles
selected CPU functions, optional CUDA kernels, memory allocation activity, and
JIT-published code without requiring application recompilation.

PEAK is designed for long-running and MPI applications where profiler overhead,
safe attach and detach behavior, and reliable final reports matter.

## Highlights

- Profile named functions instead of instrumenting the whole application.
- Preload PEAK around existing Linux binaries without rebuilding them.
- Control profiling overhead by detaching and reattaching selected targets.
- Produce human-readable and CSV reports, including MPI-aware aggregation.
- Optionally profile CUDA kernels, memory activity, and JIT-published symbols.

## Quick Start

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

## Requirements

- Linux for the primary `LD_PRELOAD` runtime path
- CMake 3.9 or newer
- C, C++, and Fortran compilers
- POSIX threads and standard Linux runtime libraries

MPI and CUDA are optional. CUDA profiling requires CUDA Toolkit 11.2 or newer.
CMake validates or obtains the required Frida Gum and OTF2 dependencies during
configuration.

## Build and Install

The following commands intentionally remain compatible with CMake 3.9:

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
| `BUILD_TESTING=ON` | Build the CTest suite. `BUILD_TESTS=ON` is also accepted. |
| `PEAK_BUILD_BENCHMARKS=ON` | Build benchmark and stress targets. |
| `PEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS=ON` | Enable opt-in real-runtime JIT preload tests when testing is enabled. |

After installation:

```bash
PEAK_TARGET=my_function \
LD_PRELOAD="$HOME/.local/lib/libpeak.so" \
./target_application
```

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

## Configuration

PEAK is configured through environment variables. The tables below are a
compact index; the linked design documents describe the safety-sensitive paths
in detail.

### Targets and Reports

| Variable | Purpose |
| --- | --- |
| `PEAK_TARGET` | Comma-separated CPU symbol names. Fortran names commonly need a trailing underscore. |
| `PEAK_TARGET_GROUP` | Comma-separated built-in groups: `FFTW`, `PBLAS`, `ScaLAPACK`, `LAPACK`, and `BLAS`. |
| `PEAK_TARGET_FILE` | File containing one CPU target name per line. |
| `PEAK_PROFILE_INTERPRETERS` | Allow normally skipped interpreter processes to initialize PEAK. |
| `PEAK_ENABLE_CXX_SYMBOL_SCAN` | Force the C++ symbol-map lookup path for target matching. |
| `PEAK_STATSLOG_PATH` | CSV output prefix. Default: `./peak_statslog`. |
| `PEAK_TEXT_OUTPUT` | Force or suppress the human-readable stderr report. |
| `PEAK_VERBOSITY` | `silent`, `report`/`quiet`, `warn`, `info`, or `debug`; numeric levels `0` through `4` are also accepted. |
| `PEAK_NAME_TRUNCATE` | Truncate long function and kernel names in text output. |
| `PEAK_MAX_NUM_THREADS` | Tracked-thread capacity. Default: twice the online CPU count. |

### MPI Output

| Variable | Purpose |
| --- | --- |
| `PEAK_OUTPUT_AGGREGATION` | Final output mode: `mpi` (default), `socket`, or `local`, with documented aliases. |
| `PEAK_MPI_COLLECTIVE_OUTPUT` | Legacy aggregate-output switch; `PEAK_OUTPUT_AGGREGATION` takes precedence. |
| `PEAK_MPI_FINALIZE_POLICY` | Report during MPI finalization (`report`) or defer PEAK output until process exit (`defer`). |
| `PEAK_MPI_REAL_FINALIZE` | Force or skip the real MPI finalizer where supported; see the caveat below. |
| `PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS` | Timeout for the all-rank finalization participation check. Default: `250`. |
| `PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS` | Timeout for each MPI payload reduction. Default: `5000`. |
| `PEAK_OUTPUT_AGGREGATION_HOST` | Override the socket reducer host. |
| `PEAK_OUTPUT_AGGREGATION_PORT` | Override the socket reducer port. |
| `PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS` | Override the socket reducer timeout. |
| `PEAK_OUTPUT_AGGREGATION_TOKEN` | Override the socket reducer session token. |
| `PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK` | Enable MPI-reducer-to-socket and socket-to-rank-local fallback paths. Default: enabled. |

MPI finalization and aggregation are deliberately bounded and fail closed.
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
| `PEAK_MEMORY_PROFILE` | Enable beta memory allocation profiling for selected CPU targets. |
| `PEAK_MEMORY_TRACK_ALL` | Track all allocation events instead of filtering by target backtraces. |
| `PEAK_MEMLOG_PATH` | Memory CSV output prefix. Default: `./peak_memlog`. |
| `PEAK_MEMLOG_CHUNK_EVENTS` | Virtual-memory event-buffer growth size. Values greater than `1000` are accepted. |
| `PEAK_MEMLOG_OTF2_DIR` | Override the directory for memory-profile OTF2 output. |

### Legacy Controls

`PEAK_PAUSE_TIMEOUT` and `PEAK_SIG_CONT_TIMEOUT` configure the legacy
cooperative pause and continue path. Strict physical detach does not rely on
that path.

## Testing

Configure and run the local suite with CMake 3.9-compatible commands:

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
  the default; Intel MPI may skip the real finalizer after reporting unless
  `PEAK_MPI_REAL_FINALIZE=1` is set. Consult the detach-controller document
  before overriding finalization behavior.

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
