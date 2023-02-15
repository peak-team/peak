
For PARSEC:  

Single node:  

```
----------------------------------------------------
            Using Simple Perf Library
----------------------------------------------------

----------------------------- Simple Perf -------------------------------
total runtime: 188.991s, library time: 46.459s, percentage of lib: 24.6%
environmental variables:
    SIMPLEPERF_DEBUG = 0 

----------------------  function statistics (direct) --------------------
    time (in seconds) and counts of direct calls
-------------------------------------------------------------------------
group:     LAPACK, function:     dgesv_, count:       1, time:      0.079
group:      PBLAS, function:    pdtrsm_, count:       5, time:      3.263
group:       BLAS, function:     dscal_, count:       2, time:      0.037
group:  ScaLAPACK, function:   pdpotrf_, count:       5, time:      1.926
group:       BLAS, function:     dcopy_, count:    2475, time:      0.629
group:       BLAS, function:      ddot_, count:   16955, time:      0.123
group:      PBLAS, function:    pdgemm_, count:      15, time:     35.691
group:     LAPACK, function:     dsyev_, count:       4, time:      0.401
group:  ScaLAPACK, function:   pdsyevx_, count:      10, time:      4.310
                                           total library time:     46.459
-------------------------------------------------------------------------

-------------------  function statistics (exclusive) --------------------
    exclusive time (in seconds) and counts
-------------------------------------------------------------------------
group:  ScaLAPACK, function:   pdlaebz_, count:      10, time:      0.001
group:  ScaLAPACK, function:   pdormqr_, count:       5, time:      0.656
group:     LAPACK, function:     dgesv_, count:       1, time:      0.079
group:       BLAS, function:     dtrmv_, count:   49075, time:      0.214
group:       BLAS, function:    idamax_, count:     380, time:      0.015
group:      PBLAS, function:    pdtrsm_, count:     750, time:      2.093
group:  ScaLAPACK, function:   pdstebz_, count:       5, time:      0.099
group:       BLAS, function:     dscal_, count:    1882, time:      0.037
group:       BLAS, function:     dnrm2_, count:    1595, time:      0.001
group:       BLAS, function:     dtrsm_, count:     105, time:      0.224
group:  ScaLAPACK, function:   pdormtr_, count:      20, time:      0.071
group:       BLAS, function:     dgemv_, count:   97530, time:      0.041
group:  ScaLAPACK, function:   pdstein_, count:       5, time:      0.341
group:     LAPACK, function:    dpotrf_, count:      10, time:      0.055
group:       BLAS, function:     dasum_, count:     285, time:      0.002
group:  ScaLAPACK, function:   pdpotrf_, count:       5, time:      0.712
group:      PBLAS, function:    pdsyrk_, count:     745, time:      0.810
group:       BLAS, function:     dcopy_, count:  182725, time:      0.769
group:       BLAS, function:      ddot_, count:   16955, time:      0.123
group:       BLAS, function:     dtrmm_, count:      45, time:      0.004
group:  ScaLAPACK, function:  pdlapdct_, count:    4087, time:      0.108
group:       BLAS, function:     dsyrk_, count:     640, time:      0.047
group:      PBLAS, function:    pdgemm_, count:      15, time:     13.313
group:     LAPACK, function:     dsyev_, count:       4, time:      0.401
group:  ScaLAPACK, function:   pdlaecv_, count:     241, time:      0.000
group:  ScaLAPACK, function:   pdsyevx_, count:      10, time:      2.598
group:       BLAS, function:     dgemm_, count:   11495, time:     23.252
group:      PBLAS, function:   pilaenv_, count:      30, time:      0.392
group:       BLAS, function:      dger_, count:     160, time:      0.002
                                           total library time:     46.459
-------------------------------------------------------------------------

-------------------  function statistics (inclusive) --------------------
    inclusive time (in seconds) and counts
-------------------------------------------------------------------------
group:  ScaLAPACK, function:   pdlaebz_, count:      10, time:      0.109
group:  ScaLAPACK, function:   pdormqr_, count:       5, time:      0.693
group:     LAPACK, function:     dgesv_, count:       1, time:      0.079
group:       BLAS, function:     dtrmv_, count:   49075, time:      0.214
group:       BLAS, function:    idamax_, count:     380, time:      0.015
group:      PBLAS, function:    pdtrsm_, count:     750, time:      3.374
group:  ScaLAPACK, function:   pdstebz_, count:       5, time:      0.208
group:       BLAS, function:     dscal_, count:    1882, time:      0.037
group:       BLAS, function:     dnrm2_, count:    1595, time:      0.001
group:       BLAS, function:     dtrsm_, count:     105, time:      0.224
group:  ScaLAPACK, function:   pdormtr_, count:      20, time:      0.764
group:       BLAS, function:     dgemv_, count:   97530, time:      0.041
group:  ScaLAPACK, function:   pdstein_, count:       5, time:      0.359
group:     LAPACK, function:    dpotrf_, count:      10, time:      0.055
group:       BLAS, function:     dasum_, count:     285, time:      0.002
group:  ScaLAPACK, function:   pdpotrf_, count:       5, time:      1.926
group:      PBLAS, function:    pdsyrk_, count:     745, time:      1.048
group:       BLAS, function:     dcopy_, count:  182725, time:      0.769
group:       BLAS, function:      ddot_, count:   16955, time:      0.123
group:       BLAS, function:     dtrmm_, count:      45, time:      0.004
group:  ScaLAPACK, function:  pdlapdct_, count:    4087, time:      0.108
group:       BLAS, function:     dsyrk_, count:     640, time:      0.047
group:      PBLAS, function:    pdgemm_, count:      15, time:     35.691
group:     LAPACK, function:     dsyev_, count:       4, time:      0.401
group:  ScaLAPACK, function:   pdlaecv_, count:     241, time:      0.000
group:  ScaLAPACK, function:   pdsyevx_, count:      10, time:      4.310
group:       BLAS, function:     dgemm_, count:   11495, time:     23.252
group:      PBLAS, function:   pilaenv_, count:      30, time:      0.392
group:       BLAS, function:      dger_, count:     160, time:      0.002
-------------------------------------------------------------------------


``` 


Si23k 512-node: 
```
running with node= 512,  rank= 4096, thread_per_node= 7, total_cpu_core= 0
        ----------------------------------------------------
                    Using Simple Perf Library
        ----------------------------------------------------

----------------------------- Simple Perf -------------------------------
total runtime: 2047.613s, library time: 1422.032s, percentage of lib: 69.4%
environmental variables:
    SIMPLEPERF_DEBUG = 0

----------------------  function statistics (direct) --------------------
    time (in seconds) and counts of direct calls
-------------------------------------------------------------------------
group:      PBLAS, function:    pdtrsm_, count:       2, time:    195.272
group:       BLAS, function:     dscal_, count:       1, time:      0.147
group:  ScaLAPACK, function:   pdpotrf_, count:       2, time:      5.456
group:       BLAS, function:     dcopy_, count:    3193, time:      0.380
group:       BLAS, function:      ddot_, count:   21746, time:      0.298
group:      PBLAS, function:    pdgemm_, count:       6, time:   1168.785
group:     LAPACK, function:     dsyev_, count:       2, time:      0.000
group:  ScaLAPACK, function:   pdsyevx_, count:       4, time:     51.693
                                           total library time:   1422.032
-------------------------------------------------------------------------


-------------------  function statistics (exclusive) --------------------
    exclusive time (in seconds) and counts
-------------------------------------------------------------------------
group:  ScaLAPACK, function:   pdlaebz_, count:       4, time:      0.015
group:      PBLAS, function:    pdscal_, count:    1920, time:      0.010
group:      PBLAS, function:    pdnrm2_, count:    1920, time:      0.067
group:       BLAS, function:    idamax_, count:     120, time:      0.001
group:      PBLAS, function:    pdtrsm_, count:    3840, time:    184.968
group:  ScaLAPACK, function:   pdstebz_, count:       2, time:      0.551
group:       BLAS, function:     dscal_, count:    1945, time:      0.148
group:       BLAS, function:     dnrm2_, count:      30, time:      0.000
group:       BLAS, function:    dsyr2k_, count:   24282, time:      0.287
group:       BLAS, function:     dtrsm_, count:     128, time:      0.409
group:      PBLAS, function:   pdsyr2k_, count:    2556, time:      4.740
group:       BLAS, function:     dgemv_, count:    3572, time:      0.012
group:  ScaLAPACK, function:   pdstein_, count:       2, time:      0.475
group:     LAPACK, function:    dpotrf_, count:       8, time:      0.000
group:       BLAS, function:     dasum_, count:      90, time:      0.000
group:  ScaLAPACK, function:   pdpotrf_, count:       2, time:      0.567
group:      PBLAS, function:    pdtrmm_, count:    2556, time:      0.013
group:       BLAS, function:     daxpy_, count:    1824, time:      0.002
group:      PBLAS, function:    pdsyrk_, count:    3838, time:      4.546
group:       BLAS, function:     dcopy_, count:  734963, time:      0.902
group:  ScaLAPACK, function:   pdgeqrf_, count:    2564, time:      3.871
group:       BLAS, function:      ddot_, count:   21746, time:      0.298
group:       BLAS, function:     dtrmm_, count:      38, time:      0.000
group:  ScaLAPACK, function:  pdlapdct_, count:    1227, time:      0.415
group:      PBLAS, function:    pdsymm_, count:    2556, time:      4.224
group:       BLAS, function:     dsyrk_, count:    6144, time:      0.035
group:      PBLAS, function:    pdgemm_, count:    5118, time:    277.401
group:     LAPACK, function:     dsyev_, count:       2, time:      0.000
group:  ScaLAPACK, function:   pdlaecv_, count:      92, time:      0.000
group:       BLAS, function:     dsymm_, count:   24282, time:      0.213
group:  ScaLAPACK, function:   pdsyevx_, count:       4, time:     35.186
group:       BLAS, function:     dgemm_, count:  106708, time:    902.661
group:      PBLAS, function:   pilaenv_, count:   23056, time:      0.001
group:       BLAS, function:      dger_, count:    1880, time:      0.013
                                           total library time:   1422.032
-------------------------------------------------------------------------

-------------------  function statistics (inclusive) --------------------
    inclusive time (in seconds) and counts
-------------------------------------------------------------------------
group:  ScaLAPACK, function:   pdlaebz_, count:       4, time:      0.431
group:      PBLAS, function:    pdscal_, count:    1920, time:      0.010
group:      PBLAS, function:    pdnrm2_, count:    1920, time:      0.067
group:       BLAS, function:    idamax_, count:     120, time:      0.001
group:      PBLAS, function:    pdtrsm_, count:    3840, time:    195.279
group:  ScaLAPACK, function:   pdstebz_, count:       2, time:      0.981
group:       BLAS, function:     dscal_, count:    1945, time:      0.148
group:       BLAS, function:     dnrm2_, count:      30, time:      0.000
group:       BLAS, function:    dsyr2k_, count:   24282, time:      0.287
group:       BLAS, function:     dtrsm_, count:     128, time:      0.409
group:      PBLAS, function:   pdsyr2k_, count:    2556, time:      5.798
group:       BLAS, function:     dgemv_, count:    3572, time:      0.012
group:  ScaLAPACK, function:   pdstein_, count:       2, time:      0.478
group:     LAPACK, function:    dpotrf_, count:       8, time:      0.000
group:       BLAS, function:     dasum_, count:      90, time:      0.000
group:  ScaLAPACK, function:   pdpotrf_, count:       2, time:      5.456
group:      PBLAS, function:    pdtrmm_, count:    2556, time:      0.016
group:       BLAS, function:     daxpy_, count:    1824, time:      0.002
group:      PBLAS, function:    pdsyrk_, count:    3838, time:      4.882
group:       BLAS, function:     dcopy_, count:  734963, time:      0.902
group:  ScaLAPACK, function:   pdgeqrf_, count:    2564, time:      3.969
group:       BLAS, function:      ddot_, count:   21746, time:      0.298
group:       BLAS, function:     dtrmm_, count:      38, time:      0.000
group:  ScaLAPACK, function:  pdlapdct_, count:    1227, time:      0.415
group:      PBLAS, function:    pdsymm_, count:    2556, time:      5.169
group:       BLAS, function:     dsyrk_, count:    6144, time:      0.035
group:      PBLAS, function:    pdgemm_, count:    5118, time:   1168.874
group:     LAPACK, function:     dsyev_, count:       2, time:      0.000
group:  ScaLAPACK, function:   pdlaecv_, count:      92, time:      0.000
group:       BLAS, function:     dsymm_, count:   24282, time:      0.213
group:  ScaLAPACK, function:   pdsyevx_, count:       4, time:     51.693
group:       BLAS, function:     dgemm_, count:  106708, time:    902.661
group:      PBLAS, function:   pilaenv_, count:   23056, time:      0.001
group:       BLAS, function:      dger_, count:    1880, time:      0.013
-------------------------------------------------------------------------

