# library perf 

## A simple tool to report timing of BLAS/LAPACK library.  

**To Compile:** 

``make`` 

**To Use without MPI:**  

``LD_PRELOAD=liblibperf.so  ./a.out`` 

**To Use with MPI:** Attached the tool to one MPI rank.

*intel mpirun:* 

  run: ``mpirun -configfile ./mpmd.txt``  
  
  mpmd.txt: 
  ```
  -n 1 LD_PRELOAD=liblibperf.so ./parsec.exe 
  -n 4 ./parsec.exe

  ```

*srun:*  

  run: ``srun -n 4 --multi-prog mpmd.txt``
  
  mpmd.txt:
  ```
   0 LD_PRELOAD=liblibperf.so ./parsec.exe 
   1 ./parsec.exe
   2 ./parsec.exe
   3 ./parsec.exe
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
Total runtime: 1125.264
------------------ BLAS/LAPACK Perf ENV -------------------
LIBPERF_MKL_FAKE = -1 
LIBPERF_DEBUG = 0 
---------- BLAS/LAPACK Perf: function statistics ----------
function:   dgesv_, count:       1, time:      0.002
function:  dsteqr_, count:      50, time:      0.003
function:   dcopy_, count:  313141, time:      1.965
function:   dtrmv_, count:   49245, time:      0.703
function:    ddot_, count:   63833, time:      1.357
function:   dtrmm_, count:      45, time:      0.004
function:  idamax_, count:    1490, time:      0.001
function:   dsyrk_, count:     960, time:      0.006
function:   dscal_, count:    3300, time:      0.388
function:   dsyev_, count:       4, time:      0.010
function:   dnrm2_, count:   22939, time:      0.037
function:   dtrsm_, count:     110, time:      1.748
function:   dgemv_, count:   98925, time:      0.132
function:   dgemm_, count:   18255, time:    609.383
function:  dpotrf_, count:      15, time:      0.001
function:    dger_, count:     160, time:      0.021
----------------------------------------------------


```

