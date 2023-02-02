# library perf  (need a better name)

## A simple tool to report timing of common math libraries. 

**Supported libraries:**
1. BLAS/CBLAS
2. LAPACK (Fortran)
3. ScaLAPACK/PBLAS (Fortran)

LAPACKE (C), FFTW on to-do list.

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

   **Please note: timing is inclusive at this point.**

```
----------------------------------------------------
            Using BLAS/LAPACK Perf Library
----------------------------------------------------

-------------------- BLAS/LAPACK Perf ---------------------
Total runtime: 1149.856
------------------ BLAS/LAPACK Perf ENV -------------------
LIBPERF_MKL_FAKE = -1 
LIBPERF_DEBUG = 0 
---------- Library Perf: function statistics ----------
group:      PBLAS, function:   pddot_, count:   24000, time:      0.024
group:  ScaLAPACK, function: pdormqr_, count:       5, time:     19.083
group:     LAPACK, function:   dgesv_, count:       1, time:      0.095
group:       BLAS, function:   dtrmv_, count:   49245, time:      0.670
group:       BLAS, function:  idamax_, count:    1490, time:      0.001
group:      PBLAS, function:  pdtrsm_, count:     750, time:     31.166
group:       BLAS, function:   dscal_, count:    3312, time:      0.382
group:       BLAS, function:   dnrm2_, count:   22936, time:      0.037
group:       BLAS, function:   dtrsm_, count:     110, time:      1.814
group:  ScaLAPACK, function: pdstedc_, count:       5, time:      5.290
group:  ScaLAPACK, function: pdormtr_, count:       5, time:     19.271
group:       BLAS, function:   dgemv_, count:   98925, time:      0.126
group:     LAPACK, function:  dpotrf_, count:      15, time:      0.051
group:  ScaLAPACK, function: pdpotrf_, count:       5, time:      5.205
group:  ScaLAPACK, function: pdsyevd_, count:      10, time:     27.914
group:     LAPACK, function:  dsteqr_, count:      50, time:      0.004
group:      PBLAS, function:  pdsyrk_, count:     745, time:      3.804
group:       BLAS, function:   dcopy_, count:  313087, time:      3.364
group:       BLAS, function:    ddot_, count:   63849, time:      1.935
group:       BLAS, function:   dtrmm_, count:      45, time:      0.004
group:       BLAS, function:   dsyrk_, count:     960, time:      0.006
group:      PBLAS, function:  pdgemm_, count:    1505, time:    649.565
group:     LAPACK, function:   dsyev_, count:       4, time:      0.201
group:       BLAS, function:   dgemm_, count:   18257, time:    596.370
group:      PBLAS, function: pilaenv_, count:    2172, time:      0.061
group:       BLAS, function:    dger_, count:     160, time:      0.018
----------------------------------------------------


```

