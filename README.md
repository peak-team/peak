[![GitHub Build Status](https://img.shields.io/github/actions/workflow/status/peak-team/peak/cmake.yml?branch=main&logo=GitHub)](https://github.com/peak-team/peak/actions/workflows/cmake.yml)
[![GitHub Build Status](https://img.shields.io/github/actions/workflow/status/peak-team/peak/mpi.yml?branch=main&logo=GitHub&label=test)](https://github.com/peak-team/peak/actions/workflows/mpi.yml)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/peak-team/peak)

# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a lightweight and easy-to-use performance evaluation tool designed with HPC systems in mind. With its user-friendly interface, PEAK provides detailed performance reports on programs, allowing users to quickly identify and resolve performance bottlenecks. Whether you're optimizing code for maximum performance or conducting regular performance evaluations, PEAK is the ideal solution for anyone looking to improve the performance of their programs. 

## Features
- **Lightweight**: Profiles only user-specified targets by default, ensuring minimal impact on performance.
- **Adaptive Cost**: Adaptive profiling overhead based on user-defined limits for optimal balance between accuracy and performance
- **Ease of Use**: Requires no recompilation and supports profiling of all functions in both dynamically and statically linked libraries.
- **Comprehensive Profiling**: Supports CPU and GPU profiling with customizable targets.
- **JIT Metadata Profiling**: Can attach to JIT code published through runtime metadata, starting with Linux perf-map providers.

## Compilation

```bash
mkdir build
cd build
cmake --install-prefix=$HOME ..
make
``` 

## Usage
Profile a target application by preloading the PEAK library:
```bash
LD_PRELOAD=libpeak.so ./target_application
``` 

## Configuration
PEAK is configured via environment variables. Below are the available settings:

| Variable | Description |
| --- | --- |
| `PEAK_TARGET` | Specifies the functions to be profiled, provided as a comma-separated list (e.g., `dgemm_,dgemv_`). When using demangled names, only the first matching symbol per name is selected. For mangled names, the profiler hooks the symbol by exact match without demangling or additional comparison. `MPI_Finalize`/`PMPI_Finalize` and `dlopen` are intentionally not profiled because PEAK owns those wrappers for safe output and dynamic attach. |
| `PEAK_COST` | Defines the upper limit of profiling cost in seconds (e.g., `10`). The monitoring process detaches if the total profiling cost exceeds this value. The number of detachments is calculated by dividing the total allowed cost by the cost of a single profiling operation. |
| `PEAK_STATSLOG_PATH` | Default: `./peak_statslog`. Specifies the output file path (prefix) for the PEAK profile log (in CSV format). The log includes the following columns:`function`, `count`, `per_thread`, `per_rank`, `call_max_s`,  `call_min_s`, `total_s`, `exclusive_s`, `thread_max_s`, `thread_min_s`, `overhead_s`. |
| `PEAK_TEXT_OUTPUT` | Controls PEAK's human-readable stderr table. By default PEAK prints the table, except when a multi-rank MPI job falls back to rank-local output; in that case CSV files are still written but the table is suppressed to avoid launcher stderr floods. Set to `1`, `true`, `yes`, or `on` to force text output, or `0` to suppress it. |
| `PEAK_OUTPUT_AGGREGATION` | Controls final MPI job output aggregation (**default: `mpi`**). `mpi`, `collective`, or truthy values select the MPI collective reducer, which first proves every rank reached `PMPI_Finalize()` before reducing final output on the application-owned finalizer path. That proof is bounded and fail-closed, but it still uses MPI progress; after proof success the MPI reducer uses bounded nonblocking collective wrappers and falls back to rank-local output if they fail or time out. `socket`, `tcp`, or `interconnect` explicitly select the PEAK-owned TCP payload reducer; unless `PEAK_MPI_FINALIZE_POLICY=report` is also set, PEAK lets the real MPI finalizer run immediately and performs socket aggregation later from process exit using launcher rank metadata instead of MPI collectives. `local`, `rank-local`, `none`, or falsey values write rank-local output. On the default MPI `PMPI_Finalize()` report path, PEAK writes output, makes target callbacks pass-through, keeps target profiling hooks pinned for process exit cleanup, restores support wrappers such as `pthread_create`/`pthread_join` and `close`, restores the `PMPI_Finalize` replacement, and returns to the real finalizer after all-rank proof. Set `PEAK_MPI_REAL_FINALIZE=0` only for diagnostics if you need PEAK to skip the real finalizer after output. The old boolean `PEAK_MPI_COLLECTIVE_OUTPUT=1` is treated as aggregate-output enabled and maps to the MPI reducer unless `PEAK_OUTPUT_AGGREGATION` is set. |
| `PEAK_OUTPUT_AGGREGATION_HOST`, `PEAK_OUTPUT_AGGREGATION_PORT`, `PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS`, `PEAK_OUTPUT_AGGREGATION_TOKEN`, `PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK` | Optional controls for socket aggregation. By default rank 0 listens on the first Slurm node with a port derived from `SLURM_JOB_ID`; non-Slurm single-node MPI runs fall back to `127.0.0.1`. PEAK derives a reducer session token from Slurm/PMI metadata so port collisions cannot mix jobs; `PEAK_OUTPUT_AGGREGATION_TOKEN` overrides that token for controlled tests. After rank 0 writes the aggregate, peer ranks wait for a PEAK-owned release acknowledgement on the adjacent port so process exit cannot race the reducer. Use these variables to override root host, root port, timeout, or reducer namespace. Socket failures fall back to rank-local CSV output by default. Set `PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK=0` (or `false`) to keep socket failures from falling back. |
| `PEAK_MPI_FINALIZE_POLICY` | Controls what PEAK does when the application calls `PMPI_Finalize()` (**default: `report`**, except explicit socket aggregation defaults to deferred PEAK output). `report`/`finalize` writes PEAK output immediately while MPI is still available, suspends future target callbacks, and returns to the real finalizer only after a bounded all-rank proof. If proof times out, PEAK writes rank-local output, skips the real finalizer, and treats MPI as unusable for the rest of PEAK teardown. PEAK intentionally does not cancel or free the timed-out nonblocking collective request because MPI has no portable cancellation path for active nonblocking collectives. `defer`/`continue` calls the real MPI finalizer immediately and keeps PEAK profiling active until process exit; final output then cannot use MPI collectives and should use socket or rank-local output. Use `defer` only when all ranks intentionally call `MPI_Finalize()` before doing substantial non-MPI work; it is not a subset-rank failure-recovery mode. |
| `PEAK_TARGET_GROUP` | Specifies target libraries for profiling (e.g., `BLAS,LAPACK,FFTW`). Supported options include `FFTW`, `PBLAS`, `ScaLAPACK`, `LAPACK`, and `BLAS`. Multiple libraries can be specified in a comma-separated list. |
| `PEAK_TARGET_FILE` | Path to a configuration file listing function names for profiling, with one function name per line (e.g., `/path/to/the/configuration/file`). |
| `PEAK_UNSAFE_GUM_PROLOGUE_POLICY` | Controls PEAK's fail-closed Gum prologue guard (**default: `default`**). The default policy only skips the audited x86_64 `DL`/`EDX`/`RDX` zero, `EAX` zero, first indexed-load, immediate post-load `DL`/`EDX`/`RDX` counter-update family that Gum 16.5.9 corrupts or crashes in tests. `conservative` additionally skips exact entry-instruction high-register `movabs` immediates containing x86 return-opcode bytes, exact entry `mov imm` operands containing x86 return-opcode bytes, and Arm64 prefixes that mention both `x16` and `x17`. Arm64 defaults to deferring to Gum's relocator scratch-register checks instead of PEAK pre-skipping single `x16`/`x17` uses. |
| `PEAK_ALLOW_UNSAFE_GUM_PROLOGUE` | Diagnostic override for `PEAK_UNSAFE_GUM_PROLOGUE_POLICY`. Set to `1` only when you accept that PEAK may attach to a prologue class known or suspected to be unsafe for the current Gum relocation path. |
| `PEAK_HEARTBEAT_INTERVAL` | Sets the interval (in seconds) at which the heartbeat monitor runs to assess whether profiling adjustments are needed (**default: `0.1`**). A smaller interval allows quicker adaptation to overhead changes. If set to 0, the heartbeat monitor is disabled. |
| `PEAK_HIBERNATION_CYCLE` | Determines how often the system checks for detach and reattach, based on the number of heartbeat cycles (**default: `50`**). A smaller value enables faster reattachment decisions, while 0 disables reattachment entirely. If set to 0, reattachment is disabled, and the profiling system will not reattach after detaching due to high overhead. |
| `PEAK_ENABLE_REATTACH` | Enables heartbeat-driven physical reattach after a target has been detached (**default: enabled**). Set to `0` or `false` to keep physically detached targets detached until shutdown. |
| `PEAK_REATTACH_COOLDOWN_MS` | Minimum time a physically detached target must stay detached before heartbeat may reattach it again (**default: `60000` ms**). This prevents hot functions from repeatedly re-entering profiling and thrashing detach/reattach. Set to `0` for aggressive test or sampling runs. |
| `PEAK_OVERHEAD_RATIO` | Defines the target profiling overhead ratio (**default: `0.1`**). If the actual overhead exceeds this ratio, the monitoring process detaches to reduce overhead. |
| `PEAK_MAX_NUM_THREADS` | Sets PEAK's internal tracked-thread capacity (**default: `online_cpu_count * 2`**). Raise this for hostile high-thread stress runs so worker threads plus PEAK controller/helper/main threads fit in the snapshot table. |
| `PEAK_REQUIRE_SAFE_DETACH` | Deprecated knob accepted by older scripts. Strict physical attach/detach/reattach/shutdown safety is now the default and does not require this variable. |
| `PEAK_SAFE_DETACH_MODE` | Selects strict detach behavior. Unset/`strict`/`auto` use auto backend selection, `helper`/`debugger` force the external ptrace helper, and `signal` forces the in-process strict signal backend. |
| `PEAK_DETACH_BACKEND` | Optional backend override for strict mode. `helper` forces `peak_detach_helper`; `signal` forces the in-process signal backend. In auto strict mode PEAK tries the helper first unless Linux `ptrace_scope` is known to block it, and falls back to the signal backend for helper startup unavailability or structured helper STOP permission-denied, timeout, or unsupported responses. A helper protocol loss/no-response after STOP remains fail-stop. |
| `PEAK_DETACH_SIGNAL` | Optional strict signal backend reservation override. Use `auto` (default), an RT signal number, `SIGRTMIN+n`, or `SIGRTMAX-n`. PEAK accepts only an available default-disposition real-time signal, installs a protective lease handler, and migrates away from normal user libc and dynamically linked `syscall()` attempts to steal, block, wait on, signalfd-use, timer/mqueue/AIO-generate, or send the reserved signal. A forced concrete signal does not migrate; collisions fail closed. |
| `PEAK_SIGNAL_RESERVE_EARLY` | Controls constructor-time RT-signal leasing for the signal backend. `auto` (default) keeps the strict-auto behavior, `always` leases whenever supported, `forced-only` leases early only when `PEAK_DETACH_SIGNAL` names a concrete signal, and `never` disables early leasing. Explicit helper/debugger backend selection does not reserve early for `PEAK_DETACH_SIGNAL=auto`; `never` is less intrusive for helper-only runs but cannot protect against user constructors that claim the future PEAK signal before fallback to the signal backend. |
| `PEAK_CONTROLLER_MAX_PENDING_AGE_MS`, `PEAK_CONTROLLER_MAX_RETRY_COUNT` | Bounds retryable strict detach/reattach transitions (**defaults: `30000` ms and `300` retries**). When a transition repeatedly returns `timeout`, `classify-failed`, or recoverable `error` past either limit, PEAK abandons only that unproven transition and leaves the hook in its stable state: attached for a failed detach, detached for a failed reattach. Set either value to `0` to disable that bound. |
| `PEAK_STRICT_GATE_WAIT_TIMEOUT_MS` | Bounds how long pthread creation may wait behind the strict detach thread-creation gate (**default: `10000` ms**). PEAK parks the creator before the real `pthread_create()` call while a mutation window is active, and the child wrapper keeps a secondary gate before user code starts. If exceeded, PEAK lets creation proceed and emits one warning so a backend stall cannot indefinitely block application thread creation. Set to `0` to wait indefinitely. |
| `PEAK_DETACH_TRACE_PATH` | Optional CSV path for strict detach-controller transition evidence. When set, PEAK appends `time,hook_id,symbol,operation,result,physical,status,retry_count,pending_age_s,batch_size,stop_window_us,batch_id` rows for detach, reattach, and shutdown transitions. The first seven fields are stable; the final five fields are optional diagnostics. `stop_window_us` is the measured helper-held STOP window when available, otherwise `0`; `batch_id` is nonzero for rows emitted by one controller batch and `0` for single-row paths. |
| `PEAK_DLOPEN_DEBUG` | Enables stderr diagnostics for queued dynamic `dlopen` attach/retry/drop/retained-handle counters when set to `1`, `true`, `yes`, or `on`. |
| `PEAK_DLOPEN_TRACE_PATH` | Optional CSV path for dynamic `dlopen` queue diagnostics. Rows are `event,enqueued,drained,requeued,dropped_full,dropped_closed,dropped_noload,dropped_requeue,partial_success,retained_handles,max_depth`. |
| `PEAK_PROFILE_INTERPRETERS` | Opts language interpreters such as Python, Lua, Perl, and Tcl into PEAK startup when set to `1`, `true`, `yes`, or `on`. By default PEAK skips these processes under `LD_PRELOAD` so module-system helpers and small launcher scripts cannot accidentally initialize Gum/PEAK before the target application starts. |
| `PEAK_JIT_ENABLE` | Enables JIT metadata providers when set to `1`, `true`, `yes`, or `on`. JIT profiling still requires matching `PEAK_TARGET` names. Explicit JIT mode also lets known JIT runtimes such as `node`/`nodejs` initialize PEAK even though they are normally skipped as tool commands. |
| `PEAK_JIT_PROVIDER` | Comma-separated JIT metadata providers. The current provider is `perfmap` / `perf-map`, which consumes Linux `/tmp/perf-<pid>.map` style metadata. |
| `PEAK_JIT_MAP_PATH` | Optional perf-map path override. If unset, the perf-map provider reads `/tmp/perf-<pid>.map`. |
| `PEAK_JIT_TRACE_PATH` | Optional CSV diagnostics for JIT provider events and perf-map records. Rows include provider, symbol name, address, size, and result (`attached`, `not-matched`, `not-executable`, `not-executable-retry`, `not-executable-timeout`, `partial-record`, `overlong-record`, `attach-retry`, or `attach-failed`). |
| `PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS` | Default: `1000`. Bounds how long the perf-map provider keeps retrying a matching target row whose code range is not executable yet before dropping that pending retry as stale. Later perf-map rows continue to drain while the stale row is pending. |
| `PEAK_JIT_DRAIN_RECORD_BUDGET` | Default: `1024`. Bounds how many perf-map or pending retry records one controller-thread drain pass processes before yielding back to detach/reattach work. |
| `PEAK_HB_MIN_US` | Sets the adaptive heartbeat monitor's minimum sleep interval in microseconds (**default: `10000`**). |
| `PEAK_HB_MAX_US` | Sets the adaptive heartbeat monitor's maximum sleep interval in microseconds (**default: `500000`**). |
| `PEAK_HB_K_ERR` | Controls adaptive heartbeat sensitivity to overhead target overshoot (**default: `3.0`**). |
| `PEAK_HB_K_RATE` | Controls adaptive heartbeat sensitivity to overhead growth rate (**default: `0.8`**). |
| `PEAK_HB_EMA_A` | Sets the exponential moving average alpha for global overhead growth rate; valid range is `(0, 1]` (**default: `0.3`**). |
| `PEAK_PAUSE_TIMEOUT` | Legacy cooperative pause timeout. Strict physical detach does not rely on this path. |
| `PEAK_SIG_CONT_TIMEOUT` | Legacy cooperative continue timeout. Strict physical detach does not rely on this path. |
| `PEAK_GPU_TARGET` | Specifies GPU kernels to be profiled, provided as a comma-separated list (e.g., `kernel1,kernel2`). Matching is performed via string comparison on the demangled kernel name, considering only the base kernel names. Namespaces and template parameters are excluded from matching (e.g., `void myspace::kernel1<int>(...)` matches `kernel1`). |
| `PEAK_GPU_TARGET_FILE` | Path to a configuration file listing GPU kernel names for profiling, with one name per line (e.g., `/path/to/gpu/config/file`). |
| `PEAK_GPU_MONITOR_ALL` | When set to `TRUE`, all GPU kernels are profiled, regardless of whether they are listed in `PEAK_GPU_TARGET` or the configuration file. If set to `FALSE` or unset, only the listed kernel names are monitored. |
| `PEAK_MEMORY_PROFILE` *(Beta)* | Enables memory allocation profiling for the specified `PEAK_TARGET`. When set to `TRUE`, PEAK intercepts and records all memory allocation and deallocation events that occur during the target’s execution. |
| `PEAK_MEMLOG_PATH` | Default: `./peak_memlog`. Specifies the output file path (prefix) for the memory profile log (in CSV format). The log includes the following columns: `timestamp (ns since start)`, `memory delta (bytes)`, `current memory usage (bytes)`, `thread ID (tid)`, and `operation` (`1 = allocation`, `2 = free`). |
| `PEAK_MEMLOG_CHUNK_EVENTS` | Default: 5,000,000. Defines the initial and incremental size (in number of events) for the virtual memory buffer used to store memory profiling data. PEAK allocates this buffer in virtual memory and automatically expands it by this amount when additional space is required. |
| `PEAK_MEMORY_TRACK_ALL` | Default: `FALSE`. Track all memory allocation events. If this flag is set `TRUE`, the memory profiler will not backtrace memory allocation events, nor will it record events according to `PEAK_TARGET`. |
| `PEAK_NAME_TRUNCATE` | When set to `TRUE`, all function names and kernel names will be truncate to fit the output table. |

## Example Configuration

```bash
export PEAK_TARGET=function1,function2
export PEAK_COST=10
export PEAK_TARGET_GROUP=BLAS,LAPACK
export PEAK_GPU_TARGET=kernel1,kernel2
export PEAK_GPU_MONITOR_ALL=TRUE
export PEAK_MEMORY_PROFILE=TRUE
export PEAK_NAME_TRUNCATE=TRUE
```

## Important Notes

1. **Fortran Procedure Naming:**
Append an underscore to lowercase Fortran procedure names (e.g., `Fortran_Procedure_Name` becomes `fortran_procedure_name_`).

2. **PEAK_TARGET, PEAK_TARGET_GROUP and PEAK_TARGET_FILE Behavior:**
These variables are merged, combining their items into a single list. Duplicate entries should be avoided but will be handled automatically.

3. **GPU Kernel Profiling:**
GPU profiling includes the warm-up time of kernels and the CUDA initialization overhead associated with the first kernel launch.

4. **GPU CUDA Graph Profiling:**
GPU CUDA Graph will be profiled as a single node or function, and only the execution time will be measured. However, if the graph is built using the stream capture process, the profiler will display the individual kernel launches that occurred during the stream capture. Note that in this case, the reported execution time and call count may not be accurate, as they reflect the capture process rather than the actual graph execution.

5. **JIT Profiling:**
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

## Reference
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
Contributions are welcome! Please submit issues or pull requests on the GitHub repository.
