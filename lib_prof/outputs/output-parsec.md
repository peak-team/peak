
For PARSEC:  


**Si23k 512-node:** 
```
running with node= 512,  rank= 4096, thread_per_node= 7, total_cpu_core= 0
        ----------------------------------------------------
                         PEAK Prof Library
        ----------------------------------------------------
for application: /scratch1/07893/junjieli/lccf/evaluation/parsec-debug/parsec.x

total runtime: 1415.943s, library time: 788.537s, percentage of lib: 55.7%

environmental variables:
    PEAKPROF_RECORD_RANK=0 
    PEAKPROF_RECORD_THRESHOLD=0.000

----------------------  function statistics (direct) --------------------
    direct call time (in seconds) and counts
-------------------------------------------------------------------------
group:      PBLAS, function:    pdgemm_, count:       6, time:    605.060
group:      PBLAS, function:    pdtrsm_, count:       2, time:    108.924
group:  ScaLAPACK, function:   pdsyevx_, count:       4, time:     63.725
group:  ScaLAPACK, function:   pdpotrf_, count:       2, time:     10.269
group:       BLAS, function:     dcopy_, count:    2443, time:      0.236
group:       BLAS, function:      ddot_, count:   20246, time:      0.177
group:     LAPACK, function:     dsyev_, count:       2, time:      0.077
group:       BLAS, function:     dscal_, count:       1, time:      0.069
------------------------------------------ total library time:    788.537
-------------------------------------------------------------------------

-------------------  function statistics (exclusive) --------------------
    exclusive call time (in seconds) and counts
-------------------------------------------------------------------------
group:       BLAS, function:     dgemm_, count:  106708, time:    387.783
group:      PBLAS, function:    pdgemm_, count:    5118, time:    223.398
group:      PBLAS, function:    pdtrsm_, count:    3840, time:    104.061
group:  ScaLAPACK, function:   pdsyevx_, count:       4, time:     44.869
group:      PBLAS, function:    pdsyrk_, count:    3838, time:      9.597
group:      PBLAS, function:   pdsyr2k_, count:    2556, time:      4.956
group:      PBLAS, function:    pdsymm_, count:    2556, time:      4.803
group:  ScaLAPACK, function:   pdgeqrf_, count:    2564, time:      4.217
group:  ScaLAPACK, function:   pdstebz_, count:       2, time:      1.011
group:       BLAS, function:     dcopy_, count:  734213, time:      0.669
group:  ScaLAPACK, function:   pdstein_, count:       2, time:      0.657
group:  ScaLAPACK, function:   pdlaebz_, count:       4, time:      0.514
group:  ScaLAPACK, function:  pdlapdct_, count:    1228, time:      0.416
group:       BLAS, function:    dsyr2k_, count:   24282, time:      0.287
group:  ScaLAPACK, function:   pdpotrf_, count:       2, time:      0.269
group:      PBLAS, function:    pdnrm2_, count:    1920, time:      0.221
group:       BLAS, function:     dsymm_, count:   24282, time:      0.205
group:       BLAS, function:     dtrsm_, count:     128, time:      0.190
group:       BLAS, function:      ddot_, count:   20246, time:      0.177
group:     LAPACK, function:     dsyev_, count:       2, time:      0.077
group:       BLAS, function:     dscal_, count:    1945, time:      0.070
group:       BLAS, function:     dsyrk_, count:    6144, time:      0.036
group:      PBLAS, function:    pdtrmm_, count:    2556, time:      0.017
group:       BLAS, function:     dgemv_, count:    3572, time:      0.013
group:       BLAS, function:      dger_, count:    1880, time:      0.012
group:     LAPACK, function:    dpotrf_, count:       8, time:      0.005
group:       BLAS, function:     daxpy_, count:    1824, time:      0.002
group:      PBLAS, function:   pilaenv_, count:   23056, time:      0.001
group:       BLAS, function:    idamax_, count:     120, time:      0.001
group:      PBLAS, function:    pdscal_, count:    1920, time:      0.001
group:       BLAS, function:     dasum_, count:      90, time:      0.000
group:       BLAS, function:     dnrm2_, count:      30, time:      0.000
group:       BLAS, function:     dtrmm_, count:      38, time:      0.000
group:  ScaLAPACK, function:   pdlaecv_, count:      91, time:      0.000
------------------------------------------ total library time:    788.537
-------------------------------------------------------------------------



