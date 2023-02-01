// https://netlib.org/scalapack/complex16/pzdbsv.f
void pzdbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdtsv.f
void pzdtsv_ (const int *N, const int *NRHS,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgbsv.f
void pzgbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  int *IPIV,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgels.f
void pzgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgesv.f
void pzgesv_ (const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpbsv.f
void pzpbsv_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzposv.f
void pzposv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzptsv.f
void pzptsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *D,  void *E, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzheev.f
void pzheev_ (const char  *JOBZ, const char  *UPLO, const int *N, void *A, const int *IA, const int *JA, const int *DESCA,  double *W,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  void *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzheevd.f
void pzheevd_ (const char  *JOBZ, const char  *UPLO, const int *N, void *A, const int *IA, const int *JA, const int *DESCA,  double *W,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgesvx.f
void pzgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *AF, const int *IAF, const int *JAF, const int *DESCAF,  int *IPIV,  char  *EQUED,  double *R,  double *C,  void *B, const int *IB, const int *JB, const int *DESCB,  void *X, const int *IX, const int *JX, const int *DESCX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, EQUED, R, C, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzheevx.f
void pzheevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, void *A, const int *IA, const int *JA, const int *DESCA, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  int *NZ,  double *W, const double *ORFAC,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  double *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzhegvx.f
void pzhegvx_ (const int *IBTYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  int *NZ,  double *W, const double *ORFAC,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  double *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO);
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzposvx.f
void pzposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *AF, const int *IAF, const int *JAF, const int *DESCAF,  char  *EQUED,  void *SR,  void *SC,  void *B, const int *IB, const int *JB, const int *DESCB,  void *X, const int *IX, const int *JX, const int *DESCX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, EQUED, SR, SC, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdbtrf.f
void pzdbtrf_ (const int *N, const int *BWL, const int *BWU,  void *A, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdbtrs.f
void pzdbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdbtrsv.f
void pzdbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdttrf.f
void pzdttrf_ (const int *N,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, DL, D, DU, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdttrs.f
void pzdttrs_ (const char  *TRANS, const int *N, const int *NRHS,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzdttrsv.f
void pzdttrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *NRHS,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgbtrf.f
void pzgbtrf_ (const int *N, const int *BWL, const int *BWU,  void *A, const int *JA, const int *DESCA,  int *IPIV,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, IPIV, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgbtrs.f
void pzgbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  int *IPIV,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgebrd.f
void pzgebrd_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  double *D,  double *E,  void *TAUQ,  void *TAUP,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, D, E, TAUQ, TAUP, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgecon.f
void pzgecon_ (const char  *NORM, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA, const double *ANORM,  double *RCOND,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgeequ.f
void pzgeequ_ (const int *M, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA,  double *R,  double *C,  double *ROWCND,  double *COLCND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgehrd.f
void pzgehrd_ (const int *N, const int *ILO, const int *IHI,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgelqf.f
void pzgelqf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgeqlf.f
void pzgeqlf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgeqpf.f
void pzgeqpf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  void *TAU,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, TAU, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgeqrf.f
void pzgeqrf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgerfs.f
void pzgerfs_ (const char  *TRANS, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const void *AF, const int *IAF, const int *JAF, const int *DESCAF, const int *IPIV, const void *B, const int *IB, const int *JB, const int *DESCB,  void *X, const int *IX, const int *JX, const int *DESCX,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgerqf.f
void pzgerqf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgetrf.f
void pzgetrf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgetri.f
void pzgetri_ (const int *N,  void *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  void *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, IPIV, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzgetrs.f
void pzgetrs_ (const char  *TRANS, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzggqrf.f
void pzggqrf_ (const int *N, const int *M, const int *P,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAUA,  void *B, const int *IB, const int *JB, const int *DESCB,  void *TAUB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzggrqf.f
void pzggrqf_ (const int *M, const int *P, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAUA,  void *B, const int *IB, const int *JB, const int *DESCB,  void *TAUB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, P, N, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzhegst.f
void pzhegst_ (const int *IBTYPE, const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA, const void *B, const int *IB, const int *JB, const int *DESCB,  double *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzhetrd.f
void pzhetrd_ (const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  double *D,  double *E,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, D, E, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpbtrf.f
void pzpbtrf_ (const char  *UPLO, const int *N, const int *BW,  void *A, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpbtrs.f
void pzpbtrs_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpbtrsv.f
void pzpbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BW, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpocon.f
void pzpocon_ (const char  *UPLO, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA, const double *ANORM,  double *RCOND,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpoequ.f
void pzpoequ_ (const int *N, const void *A, const int *IA, const int *JA, const int *DESCA,  double *SR,  double *SC,  double *SCOND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, SR, SC, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzporfs.f
void pzporfs_ (const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const void *AF, const int *IAF, const int *JAF, const int *DESCAF, const void *B, const int *IB, const int *JB, const int *DESCB, const void *X, const int *IX, const int *JX, const int *DESCX,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpotrf.f
void pzpotrf_ (const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpotri.f
void pzpotri_ (const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpotrs.f
void pzpotrs_ (const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpttrf.f
void pzpttrf_ (const int *N,  void *D,  void *E, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpttrs.f
void pzpttrs_ (const char  *UPLO, const int *N, const int *NRHS,  void *D,  void *E, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzpttrsv.f
void pzpttrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *NRHS,  void *D,  void *E, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzstebz.f
//TODO


// https://netlib.org/scalapack/complex16/pzstein.f
void pzstein_ (const int *N, const double *D, const double *E, const int *M,  double *W, const int *IBLOCK, const int *ISPLIT, const double *ORFAC,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  double *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, M, W, IBLOCK, ISPLIT, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pztrcon.f
void pztrcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA,  double *RCOND,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, A, IA, JA, DESCA, RCOND, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pztrrfs.f
void pztrrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const void *B, const int *IB, const int *JB, const int *DESCB, const void *X, const int *IX, const int *JX, const int *DESCX,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pztrtri.f
void pztrtri_ (const char  *UPLO, const char  *DIAG, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pztrtrs.f
void pztrtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pztzrzf.f
void pztzrzf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunglq.f
void pzunglq_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzungql.f
void pzungql_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzungqr.f
void pzungqr_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzungrq.f
void pzungrq_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmbr.f
void pzunmbr_ (const char  *VECT, const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmhr.f
void pzunmhr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *ILO, const int *IHI, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, ILO, IHI, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmlq.f
void pzunmlq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmql.f
void pzunmql_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmqr.f
void pzunmqr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmrq.f
void pzunmrq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmrz.f
void pzunmrz_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex16/pzunmtr.f
void pzunmtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


