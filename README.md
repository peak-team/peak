# library perf  (need a better name)

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
  -n 1 ./parsec.sh >stats.log  #use a wrapper to setup LD_PRELOAD
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
   0 ./parsec.sh >stats.log  #use a wrapper to setup LD_PRELOAD
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
Total runtime: 47.686
------------------ BLAS/LAPACK Perf ENV -------------------
LIBPERF_MKL_FAKE = -1 
LIBPERF_DEBUG = 0 
---------- Library Perf: function statistics ----------
group:       BLAS, function:   zscal_, count:       2, time:      0.000
group:      PBLAS, function:   pddot_, count:    5688, time:      0.014
group:  ScaLAPACK, function: pdormqr_, count:       9, time:      2.390
group:     LAPACK, function:   dgesv_, count:       1, time:      0.004
group:  ScaLAPACK, function: pdsyevd_, count:      18, time:      2.878
group:     LAPACK, function:  dsteqr_, count:       9, time:      0.000
group:       BLAS, function:   daxpy_, count:     415, time:      0.000
group:      PBLAS, function:  pdsyrk_, count:     351, time:      4.386
group:       BLAS, function:   dcopy_, count:   61121, time:      0.100
group:      PBLAS, function:  pdscal_, count:    3240, time:      0.000
group:       BLAS, function:    ddot_, count:   16438, time:      0.033
group:      PBLAS, function:  pdnrm2_, count:     432, time:      0.005
group:       BLAS, function:  idamax_, count:     234, time:      0.000
group:      PBLAS, function:  pdtrsm_, count:     360, time:      3.730
group:      PBLAS, function:  pdsymv_, count:    2808, time:      0.182
group:      PBLAS, function:  pdgemv_, count:   16848, time:      0.019
group:      PBLAS, function:  pdgemm_, count:     261, time:      2.148
group:       BLAS, function:   dscal_, count:     586, time:      0.004
group:     LAPACK, function:   dsyev_, count:       4, time:      0.017
group:       BLAS, function:   dnrm2_, count:    1436, time:      0.000
group:       BLAS, function:   dtrsm_, count:      36, time:      0.056
group:       BLAS, function:   dsymv_, count:     207, time:      0.000
group:  ScaLAPACK, function: pdstedc_, count:       9, time:      0.039
group:      PBLAS, function: pdsyr2k_, count:     117, time:      0.057
group:  ScaLAPACK, function: pdormtr_, count:       9, time:      2.391
group:       BLAS, function:   dgemv_, count:    1413, time:      0.000
group:       BLAS, function:   dgemm_, count:     963, time:      0.959
group:     LAPACK, function:  dpotrf_, count:       9, time:      0.001
group:      PBLAS, function: pilaenv_, count:     802, time:      0.000
group:       BLAS, function:    dger_, count:     216, time:      0.000
group:  ScaLAPACK, function: pdpotrf_, count:       9, time:      5.692
group:      PBLAS, function:  pdaxpy_, count:    2808, time:      0.000
----------------------------------------------------



```

