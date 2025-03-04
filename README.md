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
 PEAK_TARGET=dgemm_,dgemv_        # functions that will be profiled
 PEAK_COST=10                     # Upper limit of profiling cost. The monitoring process will detach if the total profiling cost exceeds this value.  
                                  # The number of detachments is determined by dividing the total allowed cost by the cost of a single profiling operation.  
 PEAK_TARGET_CONFIG=BLAS,LAPACK,FFTW  
                                  # options include FFTW, PBLAS, ScaLAPACK, LAPACK, and BLAS for specifying target libraries for profiling
 PEAK_TARGET_CONFIG_ENV=/path/to/the/configuration/file
                                  # list function names for profiling in the configuration file, one function name per line
```

## Important Notes

1. **Fortran Procedure Naming:**
Append an '\_' to lower case fortran procedure names. For example, Fortran_Procedure_Name should be fortran_procedure_name_

2. **PEAK_TARGET_CONFIG and PEAK_TARGET Behavior:**
These variables are merged, combining their items into a unified list. Duplicate entries should be avoided but will be handled automatically.

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

