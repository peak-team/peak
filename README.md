# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a lightweight and easy-to-use performance evaluation tool designed with HPC systems in mind. With its user-friendly interface, PEAK provides detailed performance reports on programs, allowing users to quickly identify and resolve performance bottlenecks. Whether you're optimizing code for maximum performance or conducting regular performance evaluations, PEAK is the ideal solution for anyone looking to improve the performance of their programs. 

## Features
- **Lightweight**: Profiles only user-specified targets by default, ensuring minimal impact on performance.
- **Adaptive Cost**: Adaptive profiling overhead based on user-defined limits for optimal balance between accuracy and performance
- **Ease of Use**: Requires no recompilation and supports profiling of all functions in both dynamically and statically linked libraries.
- **Comprehensive Profiling**: Supports CPU and GPU profiling with customizable targets.

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
| `PEAK_TARGET` | Specifies the functions to be profiled, provided as a comma-separated list (e.g., `dgemm_,dgemv_`). When using demangled names, only the first matching symbol per name is selected. For mangled names, the profiler hooks the symbol by exact match without demangling or additional comparison. |
| `PEAK_COST` | Defines the upper limit of profiling cost in seconds (e.g., `10`). The monitoring process detaches if the total profiling cost exceeds this value. The number of detachments is calculated by dividing the total allowed cost by the cost of a single profiling operation. |
| `PEAK_TARGET_GROUP` | Specifies target libraries for profiling (e.g., `BLAS,LAPACK,FFTW`). Supported options include `FFTW`, `PBLAS`, `ScaLAPACK`, `LAPACK`, and `BLAS`. Multiple libraries can be specified in a comma-separated list. |
| `PEAK_TARGET_FILE` | Path to a configuration file listing function names for profiling, with one function name per line (e.g., `/path/to/the/configuration/file`). |
| `PEAK_HEARTBEAT_INTERVAL` | Sets the interval (in seconds) at which the heartbeat monitor runs to assess whether profiling adjustments are needed (e.g., `1`). If set to 0, the heartbeat monitor is disabled. |
| `PEAK_HIBERNATION_CYCLE` | Determines how often the system checks for detach and reattach, based on the number of heartbeat cycles (e.g., `10`). A lower value enables faster response to overhead changes. If set to 0, reattachment is disabled, and the profiling system will not reattach after detaching due to high overhead. |
| `PEAK_OVERHEAD_RATIO` | Defines the target profiling overhead ratio (e.g., `0.05`). If the actual overhead exceeds this ratio, the monitoring process detaches to reduce overhead. |
| `PEAK_PAUSE_TIMEOUT` | Adjusts the maximum waiting time (in seconds) for a thread that does not call the target function or calls it infrequently to respond to pause and unpause commands (e.g., `0.01`). |
| `PEAK_SIG_CONT_TIMEOUT` | Adjusts the maximum waiting time (in seconds) for a thread that does not call the target function or calls it infrequently to receive the continue signal (e.g., `0.01`). |
| `PEAK_GPU_TARGET` | Specifies GPU kernels to be profiled, provided as a comma-separated list (e.g., `kernel1,kernel2`). Matching is performed via string comparison on the demangled kernel name, considering only the base kernel names. Namespaces and template parameters are excluded from matching (e.g., `void myspace::kernel1<int>(...)` matches `kernel1`). |
| `PEAK_GPU_TARGET_FILE` | Path to a configuration file listing GPU kernel names for profiling, with one name per line (e.g., `/path/to/gpu/config/file`). |
| `PEAK_GPU_MONITOR_ALL` | When set to `TRUE`, all GPU kernels are profiled, regardless of whether they are listed in `PEAK_GPU_TARGET` or the configuration file. If set to `FALSE` or unset, only the listed kernel names are monitored. |

## Example Configuration

```bash
export PEAK_TARGET=function1,function2
export PEAK_COST=10
export PEAK_TARGET_GROUP=BLAS,LAPACK
export PEAK_GPU_TARGET=kernel1,kernel2
export PEAK_GPU_MONITOR_ALL=TRUE
```

## Important Notes

1. **Fortran Procedure Naming:**
Append an underscore to lowercase Fortran procedure names (e.g., `Fortran_Procedure_Name` becomes `fortran_procedure_name_`).

2. **PEAK_TARGET, PEAK_TARGET_GROUP and PEAK_TARGET_FILE Behavior:**
These variables are merged, combining their items into a single list. Duplicate entries should be avoided but will be handled automatically.

3. **GPU Kernel Profiling:**
GPU profiling includes the warm-up time of kernels and the CUDA initialization overhead associated with the first kernel launch.

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
