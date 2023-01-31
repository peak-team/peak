# library perf 

## A simple tool to report timing of BLAS/LAPACK library.  

**To Compile:** 

``make`` 

**To Use without MPI:**  

``LD_PRELOAD=liblibperf.so  ./a.out`` 

**To Use with MPI:** 

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
-------------------------------------
     Using BLAS Perf Library
-------------------------------------

------------------ BLAS PERF ENV -------------------
BLASPERF_MKL_FAKE = -1 
BLASPERF_DEBUG = 0 
---------- BLAS Perf: function statistics ----------
function:   zscal_, count:       4, time:     0.0007
function:   daxpy_, count:    6626, time:     0.0013
function:   dcopy_, count: 1046756, time:     4.0465
function:    ddot_, count:  402064, time:     1.6348
function:  idamax_, count:    3744, time:     0.0019
function:   dscal_, count:    9453, time:     0.8196
function:   dnrm2_, count:   22779, time:     0.0035
function:   dtrsm_, count:     576, time:     1.1204
function:   dsymv_, count:    3312, time:     0.0030
function:   dgemv_, count:   22608, time:     0.0048
function:   dgemm_, count:   15408, time:    50.5629
function:    dger_, count:    3456, time:     0.0020
----------------------------------------------------
```

