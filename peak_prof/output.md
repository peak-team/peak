
For PARSEC:   
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
