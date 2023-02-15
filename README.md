# PEAK (Performance Evaluation and Analysis Kit)

PEAK is a lightweight and easy-to-use performance evaluation tool designed with HPC systems in mind. With its user-friendly interface, PEAK provides detailed performance reports on programs, allowing users to quickly identify and resolve performance bottlenecks. Whether you're optimizing code for maximum performance or conducting regular performance evaluations, PEAK is the ideal solution for anyone looking to improve the performance of their programs. 

## Supported Features
**Supported counters:**
1. single and double precision floating point operations as scalar, 128-bit vector, 256-bit vector, 512-bit vector: 
    FP_ARITH_INST_RETIRED_SCALAR_DOUBLE    
    FP_ARITH_INST_RETIRED_SCALAR_SINGLE        
    FP_ARITH_INST_RETIRED_128B_PACKED_DOUBLE      
    FP_ARITH_INST_RETIRED_128B_PACKED_SINGLE      
    FP_ARITH_INST_RETIRED_256B_PACKED_DOUBLE      
    FP_ARITH_INST_RETIRED_256B_PACKED_SINGLE      
    FP_ARITH_INST_RETIRED_512B_PACKED_DOUBLE      
    FP_ARITH_INST_RETIRED_512B_PACKED_SINGLE 
2. memory read and write bandwidth:    
    CAS_COUNT_RD     
    CAS_COUNT_WR  
    
**Supported libraries:**
1. BLAS/CBLAS
2. LAPACK (Fortran)
3. ScaLAPACK/PBLAS (Fortran) 

LAPACKE (C), FFTW on to-do list.

## To Compile:

``make`` 

## To Use without MPI: 

``LD_PRELOAD=peak_prof.so  ./a.out`` 

## To Use with MPI: Attached the tool to one MPI rank.

**intel mpirun:** 

  run: ``mpirun -configfile ./mpmd.txt``  
  
  mpmd.txt: 
  ```
  -n 1 ./parsec.sh >stats.log  #use a wrapper to setup LD_PRELOAD
  -n 3 ./parsec.exe
  ```
  parsec.sh:
  ```
  #!/bin/bash
  LD_PRELOAD=peak_prof.so 
  ./parsec.exe 
  ```

**srun:**  

  run: ``srun -n 4 --multi-prog mpmd.txt``
  
  mpmd.txt:
  ```
   0 ./parsec.sh >stats.log  #use a wrapper to setup LD_PRELOAD
   1 ./parsec.exe
   2 ./parsec.exe
   3 ./parsec.exe
   ```
  parsec.sh:
  ```
  #!/bin/bash
  LD_PRELOAD=peak_prof.so 
  ./parsec.exe 
  ```

## Settings
```
export PEAKPROF_DEBUG=2    # turn this on to report timing at every library call, otherwise timing is only reported in the end. 
```

## Limitations
1. It is not able to intercept internal blas functions, e.g.  MKL's cblas_dgemm calls mkl_blas_dgemm() instead of dgemm(), and mkl_blas_dgemm is not intercepted. 
2. only measures the master thread in OpenMP

## Example Output:
[PEAK Prof output for PARSEC](lib_prof/output.md)

