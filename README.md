[![CMake](https://img.shields.io/github/actions/workflow/status/peak-team/peak/cmake.yml?branch=main&logo=GitHub&label=cmake)](https://github.com/peak-team/peak/actions/workflows/cmake.yml)
[![MPI](https://img.shields.io/github/actions/workflow/status/peak-team/peak/mpi.yml?branch=main&logo=GitHub&label=mpi)](https://github.com/peak-team/peak/actions/workflows/mpi.yml)
[![License](https://img.shields.io/github/license/peak-team/peak)](LICENSE)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/peak-team/peak)

# PEAK (Performance Evaluation and Analysis Kit)

PEAK is an `LD_PRELOAD`-based profiler for HPC applications. It profiles
selected CPU functions, optional CUDA kernels, memory allocation activity, and
JIT-published code without requiring application recompilation.

PEAK is designed for long-running and MPI applications where profiler overhead,
safe attach/detach behavior, and reliable final reports matter.

## Highlights

- **Selective profiling:** profile only named targets by default.
- **Adaptive overhead control:** detach and reattach hot targets as profiling
  cost changes.
- **No application rebuild required:** preload `libpeak.so` around existing
  binaries.
- **MPI-aware reporting:** write aggregate, socket-reduced, or rank-local
  output depending on runtime safety.
- **Optional GPU, memory, and JIT profiling:** enable CUDA kernel profiling,
  memory allocation tracing, and Linux perf-map JIT metadata providers.

## Table of Contents

- [Quick Start](#quick-start)
- [Requirements](#requirements)
- [Build and Install](#build-and-install)
- [Usage Examples](#usage-examples)
- [Testing](#testing)
- [Project Layout](#project-layout)
- [Configuration](#configuration)
- [Configuration Example](#configuration-example)
- [Advanced Documentation](#advanced-documentation)
- [Notes and Caveats](#notes-and-caveats)
- [Citation](#citation)
- [Contributing](#contributing)
- [License](#license)

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

PEAK writes a human-readable report to stderr and a CSV profile log with the
default prefix `./peak_statslog`.

## Requirements

- CMake 3.9 or newer
- C, C++, and Fortran compilers
- POSIX threads and standard Linux runtime libraries such as `dl`, `rt`,
  `resolv`, and `m`
- Linux for the primary `LD_PRELOAD` runtime path used by PEAK

Optional components:

- MPI compiler/runtime for MPI-aware profiling and tests
- CUDA Toolkit 11.2 or newer for GPU kernel profiling
- Frida Gum and OTF2, which CMake can validate or fetch during configuration

## Build and Install

Build from the repository root:

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

Useful CMake options:

- `-DPEAK_ENABLE_MPI=OFF`: build without MPI support.
- `-DBUILD_CUDA_PROFILE=OFF`: build without CUDA profiling support.
- `-DBUILD_TESTING=ON`: build the test suite.
- `-DPEAK_BUILD_BENCHMARKS=ON`: build benchmark and stress targets.

After installation, preload the installed library:

```bash
PEAK_TARGET=my_function \
LD_PRELOAD="$HOME/.local/lib/libpeak.so" \
./target_application
```

## Usage Examples

Profile selected CPU targets:

```bash
PEAK_TARGET=dgemm_,dgemv_ \
LD_PRELOAD=/path/to/libpeak.so \
./target_application
```

Profile an MPI application:

```bash
mpirun -n 4 env \
  PEAK_TARGET=my_mpi_work \
  LD_PRELOAD=/path/to/libpeak.so \
  ./mpi_application
```

Reduce stderr noise while keeping the final report:

```bash
PEAK_TARGET=my_function \
PEAK_VERBOSITY=quiet \
LD_PRELOAD=/path/to/libpeak.so \
./target_application
```

Write only CSV output:

```bash
PEAK_TARGET=my_function \
PEAK_TEXT_OUTPUT=0 \
LD_PRELOAD=/path/to/libpeak.so \
./target_application
```

## Testing

Configure and run the local test suite:

```bash
mkdir -p build
cd build
cmake -DBUILD_TESTING=ON ..
cmake --build .
ctest --output-on-failure
```

MPI and CUDA coverage depends on the toolchains found during CMake
configuration.

## Project Layout

- `src/`: PEAK runtime implementation and interceptors.
- `include/`: public and internal headers shared across runtime components.
- `cmake/`: dependency discovery, Frida Gum integration, and build helpers.
- `test/`: CTest-based unit, runtime, MPI, JIT, dynamic loading, and memory
  profiling tests.
- `benchmarks/`: detach-controller benchmark and stress workloads.
- `docs/`: deeper design notes for JIT profiling, strict detach, and patched
  Frida Gum support.

## Configuration

PEAK is configured with environment variables. They are grouped below by
workflow area so long descriptions wrap naturally in Markdown.

### Common Profiling Targets

**`PEAK_TARGET`**

Comma-separated function names to profile, such as `dgemm_,dgemv_`.
When using demangled names, only the first matching symbol per name is selected.
For mangled names, PEAK hooks the exact symbol without demangling or additional
comparison. `MPI_Finalize`/`PMPI_Finalize` and `dlopen` are intentionally not
profiled because PEAK owns those wrappers for safe output and dynamic attach.

**`PEAK_TARGET_GROUP`**

Target libraries to profile, such as `BLAS,LAPACK,FFTW`. Supported groups
include `FFTW`, `PBLAS`, `ScaLAPACK`, `LAPACK`, and `BLAS`. Multiple groups can
be provided as a comma-separated list.

**`PEAK_TARGET_FILE`**

Path to a configuration file containing one target function name per line, such
as `/path/to/the/configuration/file`.

**`PEAK_PROFILE_INTERPRETERS`**

Set to `1`, `true`, `yes`, or `on` to allow language interpreters such as
Python, Lua, Perl, and Tcl to initialize PEAK. By default PEAK skips these
processes under `LD_PRELOAD` so module-system helpers and small launcher scripts
do not accidentally initialize Gum/PEAK before the target application starts.

### Reports and Output

**`PEAK_STATSLOG_PATH`**

Default: `./peak_statslog`. Path prefix for the PEAK CSV profile log. The log
columns are `function`, `count`, `per_thread`, `per_rank`, `call_max_s`,
`call_min_s`, `total_s`, `exclusive_s`, `thread_max_s`, `thread_min_s`, and
`overhead_s`.

**`PEAK_TEXT_OUTPUT`**

Controls PEAK's human-readable stderr table. By default PEAK prints the table,
except when a multi-rank MPI job falls back to rank-local output; in that case
CSV files are still written, but the table is suppressed to avoid launcher
stderr floods. Set to `1`, `true`, `yes`, or `on` to force text output, or `0`
to suppress it.

**`PEAK_VERBOSITY`**

Controls PEAK diagnostic stderr verbosity without changing final report/artifact
generation.

- Unset, `default`, or `warn`: final reports plus warnings for unsafe targets,
  aggregation fallback/failure, and conservative teardown.
- `quiet` or `report`: final reports and artifact summaries only.
- `silent` or `off`: suppress PEAK stderr output.
- `info`: add normal lifecycle decisions such as MPI finalize/output routing.
- `debug`: add per-hook retry, dynamic attach, and queue diagnostics.
- Numeric values `0` through `4`: map to `silent`, `report`, `warn`, `info`,
  and `debug`.

**`PEAK_NAME_TRUNCATE`**

Set to `TRUE` to truncate all function names and kernel names so they fit the
output table.

### MPI Output and Finalization

**`PEAK_OUTPUT_AGGREGATION`**

Controls final MPI job output aggregation. Default: `mpi`.

- `mpi`, `collective`, or truthy values select the MPI collective reducer. PEAK
  first proves every rank reached `PMPI_Finalize()` before reducing final output
  on the application-owned finalizer path. The proof is bounded and fail-closed,
  but still uses MPI progress. After proof success, the MPI reducer uses bounded
  nonblocking collective wrappers and falls back to rank-local output if they
  fail or time out.
- `socket`, `tcp`, or `interconnect` select the PEAK-owned TCP payload reducer.
  Unless `PEAK_MPI_FINALIZE_POLICY=report` is also set, PEAK lets the real MPI
  finalizer run immediately and performs socket aggregation later from process
  exit using launcher rank metadata instead of MPI collectives.
- `local`, `rank-local`, `none`, or falsey values write rank-local output.

On the default MPI `PMPI_Finalize()` report path, PEAK writes output, makes
target callbacks pass-through, keeps target profiling hooks pinned for process
exit cleanup, restores support wrappers such as `pthread_create`/`pthread_join`
and `close`, keeps the `PMPI_Finalize` replacement pinned, and returns to the
real finalizer through Gum's original-call trampoline after all-rank proof.

Intel MPI is an exception: PEAK skips the real finalizer by default after
successful report because Intel MPI 2019 has crashed in `hwloc` teardown after
PEAK output on large Frontera runs. Set `PEAK_MPI_REAL_FINALIZE=1` to force the
real finalizer, or `PEAK_MPI_REAL_FINALIZE=0` to force skipping it for
diagnostics on other MPI runtimes.

The old boolean `PEAK_MPI_COLLECTIVE_OUTPUT=1` is treated as aggregate-output
enabled and maps to the MPI reducer unless `PEAK_OUTPUT_AGGREGATION` is set.

**Socket aggregation controls**

- `PEAK_OUTPUT_AGGREGATION_HOST`
- `PEAK_OUTPUT_AGGREGATION_PORT`
- `PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS`
- `PEAK_OUTPUT_AGGREGATION_TOKEN`
- `PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK`

Optional socket aggregation controls. By default rank 0 listens on the first
Slurm node with a port derived from `SLURM_JOB_ID`; non-Slurm single-node MPI
runs fall back to `127.0.0.1`. PEAK derives a reducer session token from
Slurm/PMI metadata so port collisions cannot mix jobs. `PEAK_OUTPUT_AGGREGATION_TOKEN`
overrides that token for controlled tests.

After rank 0 writes the aggregate, peer ranks wait for a PEAK-owned release
acknowledgement on the adjacent port so process exit cannot race the reducer.
Use these variables to override root host, root port, timeout, or reducer
namespace. Socket failures fall back to rank-local CSV output by default. Set
`PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK=0` or `false` to disable that fallback.

**`PEAK_MPI_FINALIZE_POLICY`**

Controls what PEAK does when the application calls `PMPI_Finalize()`. Default:
`report`, except explicit socket aggregation defaults to deferred PEAK output.

- `report` or `finalize`: write PEAK output immediately while MPI is still
  available, suspend future target callbacks, and return to the real finalizer
  only after a bounded all-rank proof. If proof times out, PEAK writes
  rank-local output, skips the real finalizer, and treats MPI as unusable for
  the rest of PEAK teardown. PEAK intentionally does not cancel or free the
  timed-out nonblocking collective request because MPI has no portable
  cancellation path for active nonblocking collectives.
- `defer` or `continue`: call the real MPI finalizer immediately and keep PEAK
  profiling active until process exit. Final output then cannot use MPI
  collectives and should use socket or rank-local output. Use `defer` only when
  all ranks intentionally call `MPI_Finalize()` before doing substantial non-MPI
  work; it is not a subset-rank failure-recovery mode.

### Overhead Control

**`PEAK_COST`**

Upper limit of profiling cost in seconds, such as `10`. The monitoring process
detaches if total profiling cost exceeds this value. The number of detachments
is calculated by dividing the total allowed cost by the cost of a single
profiling operation.

**`PEAK_HEARTBEAT_INTERVAL`**

Heartbeat monitor interval in seconds. Default: `0.1`. Smaller intervals allow
quicker adaptation to overhead changes. Set to `0` to disable the heartbeat
monitor.

**`PEAK_HIBERNATION_CYCLE`**

How often the system checks for detach and reattach, based on heartbeat cycles.
Default: `50`. Smaller values enable faster reattachment decisions. Set to `0`
to disable reattachment entirely.

**`PEAK_ENABLE_REATTACH`**

Enables heartbeat-driven physical reattach after a target has been detached.
Default: enabled. Set to `0` or `false` to keep physically detached targets
detached until shutdown.

**`PEAK_REATTACH_COOLDOWN_MS`**

Minimum time a physically detached target must stay detached before heartbeat
may reattach it again. Default: `60000` ms. This prevents hot functions from
repeatedly re-entering profiling and thrashing detach/reattach. Set to `0` for
aggressive test or sampling runs.

**`PEAK_OVERHEAD_RATIO`**

Target profiling overhead ratio. Default: `0.1`. If actual overhead exceeds
this ratio, the monitoring process detaches to reduce overhead.

**`PEAK_MAX_NUM_THREADS`**

PEAK's internal tracked-thread capacity. Default: `online_cpu_count * 2`. Raise
this for hostile high-thread stress runs so worker threads plus PEAK
controller/helper/main threads fit in the snapshot table.

**`PEAK_HB_MIN_US`**

Adaptive heartbeat monitor minimum sleep interval in microseconds. Default:
`10000`.

**`PEAK_HB_MAX_US`**

Adaptive heartbeat monitor maximum sleep interval in microseconds. Default:
`500000`.

**`PEAK_HB_K_ERR`**

Adaptive heartbeat sensitivity to overhead target overshoot. Default: `3.0`.

**`PEAK_HB_K_RATE`**

Adaptive heartbeat sensitivity to overhead growth rate. Default: `0.8`.

**`PEAK_HB_EMA_A`**

Exponential moving average alpha for global overhead growth rate. Valid range:
`(0, 1]`. Default: `0.3`.

### Strict Detach Safety

**`PEAK_REQUIRE_SAFE_DETACH`**

Deprecated knob accepted by older scripts. Strict physical
attach/detach/reattach/shutdown safety is now the default and does not require
this variable.

**`PEAK_SAFE_DETACH_MODE`**

Selects strict detach behavior. Unset, `strict`, or `auto` use auto backend
selection. `helper` or `debugger` force the external ptrace helper. `signal`
forces the in-process strict signal backend.

**`PEAK_DETACH_BACKEND`**

Optional backend override for strict mode. `helper` forces `peak_detach_helper`;
`signal` forces the in-process signal backend. In auto strict mode PEAK tries
the helper first unless Linux `ptrace_scope` is known to block it, and falls
back to the signal backend for helper startup unavailability or structured
helper STOP permission-denied, timeout, or unsupported responses. A helper
protocol loss/no-response after STOP remains fail-stop.

**`PEAK_DETACH_SIGNAL`**

Optional strict signal backend reservation override. Use `auto` (default), an RT
signal number, `SIGRTMIN+n`, or `SIGRTMAX-n`. PEAK accepts only an available
default-disposition real-time signal, installs a protective lease handler, and
migrates away from normal user libc and dynamically linked `syscall()` attempts
to steal, block, wait on, signalfd-use, timer/mqueue/AIO-generate, or send the
reserved signal. A forced concrete signal does not migrate; collisions fail
closed.

**`PEAK_SIGNAL_RESERVE_EARLY`**

Controls constructor-time RT-signal leasing for the signal backend. `auto`
(default) keeps the strict-auto behavior. `always` leases whenever supported.
`forced-only` leases early only when `PEAK_DETACH_SIGNAL` names a concrete
signal. `never` disables early leasing. Explicit helper/debugger backend
selection does not reserve early for `PEAK_DETACH_SIGNAL=auto`; `never` is less
intrusive for helper-only runs but cannot protect against user constructors that
claim the future PEAK signal before fallback to the signal backend.

**Strict transition retry bounds**

- `PEAK_CONTROLLER_MAX_PENDING_AGE_MS`
- `PEAK_CONTROLLER_MAX_RETRY_COUNT`

Bounds retryable strict detach/reattach transitions. Defaults: `30000` ms and
`300` retries. When a transition repeatedly returns `timeout`,
`classify-failed`, or recoverable `error` past either limit, PEAK abandons only
that unproven transition and leaves the hook in its stable state: attached for a
failed detach, detached for a failed reattach. Set either value to `0` to
disable that bound.

**`PEAK_STRICT_GATE_WAIT_TIMEOUT_MS`**

Bounds how long pthread creation may wait behind the strict detach
thread-creation gate. Default: `10000` ms. PEAK parks the creator before the
real `pthread_create()` call while a mutation window is active, and the child
wrapper keeps a secondary gate before user code starts. If exceeded, PEAK lets
creation proceed and emits one warning so a backend stall cannot indefinitely
block application thread creation. Set to `0` to wait indefinitely.

**`PEAK_DETACH_TRACE_PATH`**

Optional CSV path for strict detach-controller transition evidence. When set,
PEAK appends rows with:

```text
time,hook_id,symbol,operation,result,physical,status,retry_count,pending_age_s,
batch_size,stop_window_us,batch_id,last_retry_status,failure_reason,failure_tid,
failure_pc,failure_aux,request_source,request_calls,request_ratio,
request_global_overhead,request_total_time,request_rate
```

The first seven fields are stable; later fields are diagnostics.
`request_source` records whether the pending transition came from an API
request, explicit detach count, per-target heartbeat, or global heartbeat.

### Gum Prologue Safety

**`PEAK_UNSAFE_GUM_PROLOGUE_POLICY`**

Controls PEAK's fail-closed Gum prologue guard. Default: `default`. The default
user-target policy only skips the audited x86_64 `DL`/`EDX`/`RDX` zero, `EAX`
zero, first indexed-load, immediate post-load `DL`/`EDX`/`RDX`
counter-update family that Gum 16.5.9 corrupts or crashes in tests.

`conservative` additionally skips exact entry-instruction high-register
`movabs` immediates containing x86 return-opcode bytes, exact entry `mov imm`
operands containing x86 return-opcode bytes, and Arm64 prefixes that mention
both `x16` and `x17`.

PEAK-owned support replacements may opt into stricter internal guards for
early-return wrapper prologues when skipping them does not remove core profiling
coverage; this does not broaden normal user-target skipping. Arm64 defaults to
deferring to Gum's relocator scratch-register checks instead of PEAK
pre-skipping single `x16`/`x17` uses.

**`PEAK_ALLOW_UNSAFE_GUM_PROLOGUE`**

Diagnostic override for `PEAK_UNSAFE_GUM_PROLOGUE_POLICY`. Set to `1` only when
you accept that PEAK may attach to a prologue class known or suspected to be
unsafe for the current Gum relocation path.

### Dynamic Loading and JIT

**`PEAK_DLOPEN_DEBUG`**

Set to `1`, `true`, `yes`, or `on` to enable stderr diagnostics for queued
dynamic `dlopen` attach/retry/drop/retained-handle counters.

**`PEAK_DLOPEN_TRACE_PATH`**

Optional CSV path for dynamic `dlopen` queue diagnostics. Rows are:

```text
event,enqueued,drained,requeued,dropped_full,dropped_closed,dropped_noload,
dropped_requeue,partial_success,retained_handles,max_depth
```

**`PEAK_JIT_ENABLE`**

Set to `1`, `true`, `yes`, or `on` to enable JIT metadata providers. JIT
profiling still requires matching `PEAK_TARGET` names. Explicit JIT mode also
lets known JIT runtimes such as `node`/`nodejs` initialize PEAK even though they
are normally skipped as tool commands.

**`PEAK_JIT_PROVIDER`**

Comma-separated JIT metadata providers. The current provider is `perfmap` /
`perf-map`, which consumes Linux `/tmp/perf-<pid>.map` style metadata.

**`PEAK_JIT_MAP_PATH`**

Optional perf-map path override. If unset, the perf-map provider reads
`/tmp/perf-<pid>.map`.

**`PEAK_JIT_TRACE_PATH`**

Optional CSV diagnostics for JIT provider events and perf-map records. Rows
include provider, symbol name, address, size, and result. Results include
`attached`, `not-matched`, `not-executable`, `not-executable-retry`,
`not-executable-timeout`, `partial-record`, `overlong-record`, `attach-retry`,
and `attach-failed`.

**`PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS`**

Default: `1000`. Bounds how long the perf-map provider keeps retrying a matching
target row whose code range is not executable yet before dropping that pending
retry as stale. Later perf-map rows continue to drain while the stale row is
pending.

**`PEAK_JIT_DRAIN_RECORD_BUDGET`**

Default: `1024`. Bounds how many perf-map or pending retry records one
controller-thread drain pass processes before yielding back to detach/reattach
work.

### GPU Profiling

**`PEAK_GPU_TARGET`**

Comma-separated GPU kernel names to profile, such as `kernel1,kernel2`. Matching
uses string comparison on the demangled kernel name and considers only base
kernel names. Namespaces and template parameters are excluded from matching. For
example, `void myspace::kernel1<int>(...)` matches `kernel1`.

**`PEAK_GPU_TARGET_FILE`**

Path to a configuration file containing one GPU kernel name per line, such as
`/path/to/gpu/config/file`.

**`PEAK_GPU_MONITOR_ALL`**

Set to `TRUE` to profile all GPU kernels, regardless of whether they are listed
in `PEAK_GPU_TARGET` or the configuration file. If set to `FALSE` or unset, only
the listed kernel names are monitored.

### Memory Profiling

**`PEAK_MEMORY_PROFILE`** *(Beta)*

Set to `TRUE` to enable memory allocation profiling for the specified
`PEAK_TARGET`. PEAK intercepts and records memory allocation and deallocation
events that occur during the target's execution.

**`PEAK_MEMLOG_PATH`**

Default: `./peak_memlog`. Path prefix for the memory profile CSV log. The log
columns are `timestamp (ns since start)`, `memory delta (bytes)`, `current
memory usage (bytes)`, `thread ID (tid)`, and `operation` (`1 = allocation`,
`2 = free`).

**`PEAK_MEMLOG_CHUNK_EVENTS`**

Default: 5,000,000. Initial and incremental size, in events, for the virtual
memory buffer used to store memory profiling data. PEAK allocates this buffer in
virtual memory and expands it by this amount when more space is needed.

**`PEAK_MEMORY_TRACK_ALL`**

Default: `FALSE`. Track all memory allocation events. If set to `TRUE`, the
memory profiler will not backtrace memory allocation events and will not record
events according to `PEAK_TARGET`.

### Legacy Cooperative Detach

**`PEAK_PAUSE_TIMEOUT`**

Legacy cooperative pause timeout. Strict physical detach does not rely on this
path.

**`PEAK_SIG_CONT_TIMEOUT`**

Legacy cooperative continue timeout. Strict physical detach does not rely on
this path.

## Configuration Example

```bash
export PEAK_TARGET=function1,function2
export PEAK_COST=10
export PEAK_TARGET_GROUP=BLAS,LAPACK
export PEAK_GPU_TARGET=kernel1,kernel2
export PEAK_GPU_MONITOR_ALL=TRUE
export PEAK_MEMORY_PROFILE=TRUE
export PEAK_NAME_TRUNCATE=TRUE
```

## Advanced Documentation

- [JIT profiling](docs/jit-profiling.md): provider guarantees, retry behavior,
  and runtime metadata lifetime limits.
- [Physical detach controller](docs/physical-detach-controller.md): strict
  attach/detach/reattach and shutdown safety model.
- [Patched Frida Gum](docs/patched-frida-gum.md): PEAK-specific Gum APIs and
  patched relocation support.

## Notes and Caveats

### Fortran Procedure Names

Append an underscore to lowercase Fortran procedure names. For example,
`Fortran_Procedure_Name` becomes `fortran_procedure_name_`.

### Target List Merging

`PEAK_TARGET`, `PEAK_TARGET_GROUP`, and `PEAK_TARGET_FILE` are merged into one
target list. Duplicate entries should be avoided, but PEAK handles them
automatically.

### GPU Kernel Profiling

GPU profiling includes kernel warm-up time and the CUDA initialization overhead
associated with the first kernel launch.

### CUDA Graph Profiling

CUDA graphs are profiled as a single node or function, and only execution time is
measured. If a graph is built through stream capture, PEAK may display the
individual kernel launches that occurred during capture. In that case, reported
execution time and call count may reflect capture behavior rather than the
graph's actual execution.

### JIT Profiling

PEAK does not infer function names or boundaries from anonymous executable pages.
JIT targets require runtime metadata. The default Linux test suite covers the
native perf-map provider, metadata-before-`PROT_EXEC` retry, and multiple code
generations for one target, heartbeat-on dynamic table growth, CSV-safe V8-style
names, plus stale non-executable target rows followed by valid generations; real
runtime preload tests such as Node/V8 can be
enabled with `-DPEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS=ON` when the strict attach
backend is supported for that runtime. See
[docs/jit-profiling.md](docs/jit-profiling.md) for provider guarantees and
lifetime limits.

## Citation

If you use PEAK in your research, please cite:

```
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
  pages = {677–680},
  numpages = {4},
  keywords = {application performance, profiling, system tools},
  location = {Denver, CO, USA},
  series = {SC-W '23}
}
```

## Contributing

Contributions are welcome through GitHub issues and pull requests.

Before opening a pull request:

- Build the project locally.
- Run the relevant tests with `ctest --output-on-failure` from the build
  directory.
- Update README or documentation files when user-facing behavior changes.
- Keep pull requests focused so review and CI results are easy to interpret.

## License

PEAK is distributed under the [BSD 3-Clause License](LICENSE).
