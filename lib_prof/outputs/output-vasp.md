
VASP:  


**4 nodes:** 4x56 ranks

```
        ----------------------------------------------------
                         PEAK Prof Library
        ----------------------------------------------------
for application: /scratch1/07893/junjieli/chem-job/vasp/vasp-bin/5.4.4.pl2/bin/vasp_std

recorded MPI rank: 0
total runtime: 228.507s, library time: 71.790s, percentage of lib: 31.4%

environmental variables:
    PEAKPROF_RECORD_RANK=0 
    PEAKPROF_RECORD_FUNCTION=dgemv_
    PEAKPROF_RECORD_THRESHOLD=0.000 

----------------  function statistics (direct) ---------------
    direct call time (in seconds) and counts
--------------------------------------------------------------
    |   group    |    function    |    count   |    time    |
--------------------------------------------------------------
  1 |       BLAS |         dgemv_ |   35096832 |     32.595 |
  2 |  ScaLAPACK |       pzheevx_ |        400 |     13.982 |
  3 |       BLAS |         zcopy_ |     743652 |      5.665 |
  4 |       BLAS |         dscal_ |     669880 |      5.023 |
  5 |       BLAS |         zaxpy_ |     813450 |      4.305 |
  6 |       BLAS |         daxpy_ |     415792 |      4.225 |
  7 |       BLAS |         zgemm_ |      20904 |      2.900 |
  8 |     LAPACK |         zhegv_ |      57748 |      1.244 |
  9 |  ScaLAPACK |       pzpotrf_ |        404 |      0.483 |
 10 |  ScaLAPACK |       pztrtri_ |        404 |      0.434 |
 11 |       BLAS |         zgemv_ |     488070 |      0.366 |
 12 |       BLAS |         dcopy_ |     992065 |      0.339 |
 13 |     LAPACK |         zgeev_ |          1 |      0.184 |
 14 |     LAPACK |         dgegv_ |         88 |      0.045 |
 15 |     LAPACK |        dgetrf_ |          2 |      0.000 |
 16 |     LAPACK |        dgetrs_ |          2 |      0.000 |
 17 |  ScaLAPACK |      descinit_ |          1 |      0.000 |
--------------------------------------------------------------
                             total library time:     71.790
--------------------------------------------------------------

--------------  function statistics (exclusive) --------------
    exclusive call time (in seconds) and counts
--------------------------------------------------------------
    |   group    |    function    |    count   |    time    |
--------------------------------------------------------------
  1 |       BLAS |         dgemv_ |   35096832 |     32.595 |
  2 |      PBLAS |        pzhemv_ |     116800 |      8.515 |
  3 |       BLAS |         zcopy_ |    4202820 |      5.885 |
  4 |       BLAS |         dscal_ |     673644 |      5.023 |
  5 |       BLAS |         zaxpy_ |     851850 |      4.309 |
  6 |       BLAS |         daxpy_ |     416215 |      4.225 |
  7 |       BLAS |         zgemm_ |      56620 |      2.996 |
  8 |     LAPACK |         zhegv_ |      57748 |      1.244 |
  9 |  ScaLAPACK |       pzstein_ |        400 |      1.098 |
 10 |  ScaLAPACK |       pdstebz_ |        400 |      1.021 |
 11 |  ScaLAPACK |       pzheevx_ |        400 |      0.842 |
 12 |  ScaLAPACK |       pzunmql_ |        400 |      0.616 |
 13 |      PBLAS |       pzher2k_ |       7600 |      0.607 |
 14 |      PBLAS |        pzgemv_ |     700800 |      0.498 |
 15 |       BLAS |         zgemv_ |     721270 |      0.440 |
 16 |      PBLAS |        pztrmm_ |       7676 |      0.353 |
 17 |       BLAS |         dcopy_ |     995688 |      0.340 |
 18 |  ScaLAPACK |       pzpotrf_ |        404 |      0.271 |
 19 |      PBLAS |        pzherk_ |       7676 |      0.191 |
 20 |     LAPACK |         zgeev_ |          1 |      0.184 |
 21 |      PBLAS |        pzdotc_ |     116800 |      0.088 |
 22 |  ScaLAPACK |      pdlapdct_ |      29608 |      0.071 |
 23 |      PBLAS |       pdznrm2_ |      12800 |      0.060 |
 24 |       BLAS |         zhemv_ |     122800 |      0.047 |
 25 |     LAPACK |         dgegv_ |         88 |      0.045 |
 26 |       BLAS |        zher2k_ |       7200 |      0.034 |
 27 |  ScaLAPACK |       pztrtri_ |        404 |      0.025 |
 28 |      PBLAS |        pztrsm_ |      15352 |      0.023 |
 29 |      PBLAS |        pzaxpy_ |     116800 |      0.022 |
 30 |      PBLAS |        pzscal_ |     129600 |      0.022 |
 31 |       BLAS |         ztrmm_ |       7600 |      0.020 |
 32 |  ScaLAPACK |       pzunmtr_ |       1200 |      0.019 |
 33 |      PBLAS |       pilaenv_ |     162628 |      0.013 |
 34 |  ScaLAPACK |       pdlaebz_ |        800 |      0.013 |
 35 |       BLAS |         ztrsm_ |       1212 |      0.009 |
 36 |       BLAS |         ztrmv_ |      18060 |      0.008 |
 37 |       BLAS |         zgerc_ |       6000 |      0.007 |
 38 |       BLAS |         zscal_ |      31660 |      0.005 |
 39 |       BLAS |         zher2_ |       6000 |      0.002 |
 40 |       BLAS |        idamax_ |       3764 |      0.002 |
 41 |       BLAS |         dasum_ |       2823 |      0.001 |
 42 |  ScaLAPACK |       pdlaecv_ |      13691 |      0.001 |
 43 |       BLAS |         dnrm2_ |        941 |      0.000 |
 44 |       BLAS |          ddot_ |        423 |      0.000 |
 45 |     LAPACK |        dgetrf_ |          2 |      0.000 |
 46 |     LAPACK |        dgetrs_ |          2 |      0.000 |
 47 |  ScaLAPACK |      descinit_ |          1 |      0.000 |
--------------------------------------------------------------
                             total library time:     71.790
--------------------------------------------------------------


                    dgemv_ call path statistics               
--------------------------------------------------------------
1) user->dgemv_                                      
   -------------------------------------------------------
   log10(N) |  avg(N)  | stdev(N) |   count   |    time     
   -------------------------------------------------------
       1~2  |     73.7 |     15.1 |  35096832 |     32.595  
   -------------------------------------------------------
                            total:   35096832       32.595
   -------------------------------------------------------
--------------------------------------------------------------
```
