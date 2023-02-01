# library perf 

## A simple tool to report timing of BLAS/LAPACK/ScaLAPACK library.  

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
-------------------- BLAS/LAPACK/ScaLAPACK Perf ---------------------
Total runtime: 239.246
------------------ BLAS/LAPACK/ScaLAPACK Perf ENV -------------------
LIBPERF_MKL_FAKE = -1 
LIBPERF_DEBUG = 1 
---------- BLAS/LAPACK/ScaLAPACK Perf: function statistics ----------
function:   zscal_, count:       4, time:      0.001    
function: pdormqr_, count:      49, time:      2.041    
function:   dgesv_, count:      39, time:      0.025    
function: pdsyevd_, count:      98, time:     14.162   
function:  dsteqr_, count:      49, time:      0.003    
function:   daxpy_, count:    2256, time:      0.001    
function:   dcopy_, count:  350337, time:      1.632    
function:    ddot_, count:  124920, time:      0.569    
function:  idamax_, count:    1274, time:      0.000    
function:   dscal_, count:    3215, time:      0.249    
function:   dsyev_, count:      84, time:      0.635    
function:   dnrm2_, count:    7705, time:      0.002    
function:   dtrsm_, count:     196, time:      0.686    
function:   dsymv_, count:    1127, time:      0.001    
function: pdstedc_, count:      49, time:      0.971    
function: pdormtr_, count:      49, time:      2.042    
function:   dgemv_, count:    7693, time:      0.002    
function:   dgemm_, count:    5243, time:     14.730   
function:  dpotrf_, count:      49, time:      0.018    
function:    dger_, count:    1176, time:      0.001    
function: pdpotrf_, count:      49, time:      8.283    
----------------------------------------------------



```

