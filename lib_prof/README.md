## To Compile:

``make`` 

## To Use: 

``LD_PRELOAD=peak_prof.so  ./a.out`` 


## Settings
```
 PEAKPROF_RECORD_RANK=0               # the rank for which result will be printed out. 
 PEAKPROF_RECORD_THRESHOLD=-0.001     # threshold for printing out timing.
```

## Limitations
1. It is not able to intercept internal blas functions, e.g.  MKL's cblas_dgemm calls mkl_blas_dgemm() instead of dgemm(), and mkl_blas_dgemm is not intercepted. 
2. Cannot intercept static libraries. Work in progress.
3. Only measures the master thread in OpenMP

## Example Output:
[PEAK Prof output for PARSEC](outputs/output-parsec.md) 
[PEAK Prof output for VASP](outputs/output-vasp.md)

