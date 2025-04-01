# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a lightweight and easy-to-use performance evaluation tool designed with HPC systems in mind. With its user-friendly interface, PEAK provides detailed performance reports on programs, allowing users to quickly identify and resolve performance bottlenecks. Whether you're optimizing code for maximum performance or conducting regular performance evaluations, PEAK is the ideal solution for anyone looking to improve the performance of their programs. 

## To Compile:

```
mkdir build
cd build
cmake --install-prefix=$HOME ..
make
``` 

## To Use: 

``LD_PRELOAD=libpeak.so ./target_application_here`` 

## Settings
```
PEAK_TARGET=dgemm_,dgemv_         # functions that will be profiled
PEAK_COST=10                      # Upper limit of profiling cost in seconds. The monitoring process will detach if the total profiling cost exceeds this value.  
                                  # The number of detachments is determined by dividing the total allowed cost by the cost of a single profiling operation.  
PEAK_TARGET_GROUP=BLAS,LAPACK,FFTW  
                                  # options include FFTW, PBLAS, ScaLAPACK, LAPACK, and BLAS for specifying target libraries for profiling
PEAK_TARGET_FILE=/path/to/the/configuration/file
                                  # list function names for profiling in the configuration file, one function name per line
PEAK_HEARTBEAT_INTERVAL=1         # Interval (in seconds) at which the heartbeat monitor runs.
                                  # This determines how frequently the system assesses whether profiling should be adjusted.

PEAK_HIBERNATION_CYCLE=10         # Determines how often the system checks whether it needs to detach and reattach, 
                                  # based on the number of heartbeat cycles.
                                  # A lower value makes the system respond more quickly to overhead changes.

PEAK_OVERHEAD_RATIO=0.05          # Target profiling overhead ratio. If the actual profiling overhead exceeds this ratio,
                                  # the monitoring process will detach to reduce overhead.

PEAK_ENABLE_REATTACH=1            # Whether to allow reattaching after detachment. If set to 1 (enabled), 
                                  # the monitoring system will attempt to reattach profiling hooks when the overhead 
                                  # drops below the target threshold.
                                  
PEAK_PAUSE_TIMEOUT=0.01           # For a thread that does not call the target function or calls it infrequently, 
                                  # this variable adjusts the maximum waiting time (in seconds) for it to respond to the pause and unpause command.

PEAK_SIG_CONT_TIMEOUT=0.01        # For a thread that does not call the target function or calls it infrequently, 
                                  # this variable adjusts the maximum waiting time (in seconds) for the continue signal.

PEAK_GPU_TARGET=kernel1,kernel2   # GPU kernels that will be profiled PEAK_GPU_TARGET_FILE=/path/to/gpu/config/file  
                                  # Path to a configuration file listing GPU kernel names for profiling (one per line).
PEAK_GPU_MONITOR_ALL=TRUE         # If set to TRUE, all GPU kernels will be profiled, 
                                  # regardless of whether they're listed in PEAK_GPU_TARGET or the config file.
                                  # If set to FALSE or unset, only the listed kernel names will be monitored.
```

## Important Notes

1. **Fortran Procedure Naming:**
Append an '\_' to lower case fortran procedure names. For example, Fortran_Procedure_Name should be fortran_procedure_name_

2. **PEAK_TARGET_CONFIG and PEAK_TARGET Behavior:**
These variables are merged, combining their items into a unified list. Duplicate entries should be avoided but will be handled automatically.

3. **GPU Kernel Profiling:**
GPU profiling includes the warm-up time of kernels and the CUDA initialization overhead associated with the first kernel launch.

## Reference
If you use PEAK in your research, please cite the following paper:

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

