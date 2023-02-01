# library perf 

## A simple tool to report timing of common math libraries. 

**Supported libraries:**
1. BLAS/CBLAS
2. LAPACK (Fortran)
3. ScaLAPACK/PBLAS (Fortran)

LAPACKE (C), FFTW in progress.

**To Compile:** 

``make`` 

**To Use without MPI:**  

``LD_PRELOAD=liblibperf.so  ./a.out`` 

**To Use with MPI:** Attached the tool to one MPI rank.

*intel mpirun:* 

  run: ``mpirun -configfile ./mpmd.txt``  
  
  mpmd.txt: 
  ```
  -n 1 ./parsec.sh  #use a wrapper to setup LD_PRELOAD
  -n 3 ./parsec.exe
  ```
  parsec.sh:
  ```
  LD_PRELOAD=liblibperf.so 
  ./parsec.exe 
  ```

*srun:*  

  run: ``srun -n 4 --multi-prog mpmd.txt``
  
  mpmd.txt:
  ```
   0 ./parsec.sh  #use a wrapper to setup LD_PRELOAD
   1 ./parsec.exe
   2 ./parsec.exe
   3 ./parsec.exe
   ```
  parsec.sh:
  ```
  LD_PRELOAD=liblibperf.so 
  ./parsec.exe 
  ```

**Settings**
```
export LIBPERF_MKL_FAKE=1  #not in use right now
export LIBPERF_DEBUG=1    # turn this on to report timing at every library call, otherwise timing is only reported in the end. 
```

**Example Output:**

```
----------------------------------------------------
            Using BLAS/LAPACK Perf Library
----------------------------------------------------

-------------------- BLAS/LAPACK Perf ---------------------
Total runtime: 168.995
------------------ BLAS/LAPACK Perf ENV -------------------
LIBPERF_MKL_FAKE = -1 
LIBPERF_DEBUG = 0 
---------- BLAS/LAPACK Perf: function statistics ----------
function:   zscal_, count:       4, time:      0.001
function: pdormqr_, count:      52, time:      0.382
function:   dgesv_, count:      42, time:      0.017
function: pdsyevd_, count:     104, time:      1.719
function:  dsteqr_, count:     156, time:      0.010
function:   daxpy_, count:    9882, time:      0.001
function:   dcopy_, count:  270754, time:      4.167
function:   dtrmv_, count:    3224, time:      0.001
function:    ddot_, count:  141748, time:      1.043
function:   dtrmm_, count:     104, time:      0.001
function:  idamax_, count:     936, time:      0.001
function:   dsyrk_, count:    2496, time:      0.007
function:   dscal_, count:   10543, time:      0.237
function:   dsyev_, count:      90, time:      0.215
function:   dnrm2_, count:   18003, time:      0.006
function:  dsyr2k_, count:     624, time:      0.007
function:   dtrsm_, count:     312, time:      4.978
function:   dsymv_, count:   24804, time:      0.008
function: pdstedc_, count:      52, time:      0.228
function: pdormtr_, count:      52, time:      0.383
function:   dgemv_, count:   53404, time:      0.019
function:   dgemm_, count:    7123, time:     10.191
function:  dpotrf_, count:     156, time:      0.023
function:    dger_, count:    1664, time:      0.005
function: pdpotrf_, count:      52, time:      4.288
----------------------------------------------------



```

