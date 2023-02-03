# Simple Perf  (needs a better name)

## A simple tool to report timing of common math libraries. 

## Supported libraries:
1. BLAS/CBLAS
2. LAPACK (Fortran)
3. ScaLAPACK/PBLAS (Fortran)

LAPACKE (C), FFTW on to-do list.

## To Compile:

``make`` 

## To Use without MPI: 

``LD_PRELOAD=libsimpleperf.so  ./a.out`` 

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
  LD_PRELOAD=libsimpleperf.so 
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
  LD_PRELOAD=libsimpleperf.so 
  ./parsec.exe 
  ```

## Settings
```
export SIMPLEPERF_MKL_FAKE=1  #not in use right now
export SIMPLEPERF_DEBUG=2    # turn this on to report timing at every library call, otherwise timing is only reported in the end. 
```

## Limitations
1. It is not able to intercept internal blas functions, e.g.  MKL's cblas_dgemm calls mkl_blas_dgemm() instead of dgemm(), and mkl_blas_dgemm is not intercepted. 
3. timing is inclusive at this point, meaning if function A calls B, the timing of B is not deducted from the timing of A.


## Example Output:

  
```
----------------------------------------------------
            Using Simple Perf Library
----------------------------------------------------

-------------------- Simple Perf ---------------------
Total runtime: 1155.030
------------------ Simple Perf ENV -------------------
SIMPLEPERF_MKL_FAKE = -1 
SIMPLEPERF_DEBUG = 0 
---------- Simple Perf: function statistics ----------
group:      PBLAS, function:   pddot_, count:   24000, time:      0.044
group:  ScaLAPACK, function: pdormqr_, count:       5, time:     17.441
group:     LAPACK, function:   dgesv_, count:       1, time:      0.054
group:       BLAS, function:   dtrmv_, count:   49245, time:      0.720
group:       BLAS, function:  idamax_, count:    1490, time:      0.001
group:      PBLAS, function:  pdtrsm_, count:     750, time:     31.103
group:       BLAS, function:   dscal_, count:    3308, time:      0.387
group:       BLAS, function:   dnrm2_, count:   22935, time:      0.037
group:       BLAS, function:   dtrsm_, count:     110, time:      1.788
group:  ScaLAPACK, function: pdstedc_, count:       5, time:      4.674
group:  ScaLAPACK, function: pdormtr_, count:       5, time:     17.640
group:       BLAS, function:   dgemv_, count:   98925, time:      0.137
group:     LAPACK, function:  dpotrf_, count:      15, time:      0.024
group:  ScaLAPACK, function: pdpotrf_, count:       5, time:      4.262
group:  ScaLAPACK, function: pdsyevd_, count:      10, time:     25.652
group:     LAPACK, function:  dsteqr_, count:      50, time:      0.004
group:      PBLAS, function:  pdsyrk_, count:     745, time:      2.875
group:       BLAS, function:   dcopy_, count:  313617, time:      2.901
group:       BLAS, function:    ddot_, count:   63833, time:      1.342
group:       BLAS, function:   dtrmm_, count:      45, time:      0.004
group:       BLAS, function:   dsyrk_, count:     960, time:      0.006
group:      PBLAS, function:  pdgemm_, count:    1505, time:    654.216
group:     LAPACK, function:   dsyev_, count:       4, time:      0.578
group:       BLAS, function:   dgemm_, count:   18264, time:    603.309
group:      PBLAS, function: pilaenv_, count:    2173, time:      0.001
group:       BLAS, function:    dger_, count:     160, time:      0.017
----------------------------------------------------

```

