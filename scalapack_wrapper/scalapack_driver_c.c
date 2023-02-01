// https://netlib.org/scalapack/complex/pcdbsv.f
void pcdbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdtsv.f
void pcdtsv_ (const int *N, const int *NRHS,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgbsv.f
void pcgbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  int *IPIV,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgels.f
void pcgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgesv.f
void pcgesv_ (const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpbsv.f
void pcpbsv_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcposv.f
void pcposv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcptsv.f
void pcptsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *D,  void *E, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcheev.f
void pcheev_ (const char  *JOBZ, const char  *UPLO, const int *N, void *A, const int *IA, const int *JA, const int *DESCA,  float *W,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  void *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcheevd.f
void pcheevd_ (const char  *JOBZ, const char  *UPLO, const int *N, void *A, const int *IA, const int *JA, const int *DESCA,  float *W,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgesvx.f
void pcgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *AF, const int *IAF, const int *JAF, const int *DESCAF,  int *IPIV,  char  *EQUED,  float *R,  float *C,  void *B, const int *IB, const int *JB, const int *DESCB,  void *X, const int *IX, const int *JX, const int *DESCX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, EQUED, R, C, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcheevx.f
void pcheevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, void *A, const int *IA, const int *JA, const int *DESCA, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  int *NZ,  float *W, const float *ORFAC,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  float *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pchegvx.f
void pchegvx_ (const int *IBTYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  int *NZ,  float *W, const float *ORFAC,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  float *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO);
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcposvx.f
void pcposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *IA, const int *JA, const int *DESCA,  void *AF, const int *IAF, const int *JAF, const int *DESCAF,  char  *EQUED,  void *SR,  void *SC,  void *B, const int *IB, const int *JB, const int *DESCB,  void *X, const int *IX, const int *JX, const int *DESCX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, EQUED, SR, SC, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdbtrf.f
void pcdbtrf_ (const int *N, const int *BWL, const int *BWU,  void *A, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdbtrs.f
void pcdbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdbtrsv.f
void pcdbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdttrf.f
void pcdttrf_ (const int *N,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, DL, D, DU, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdttrs.f
void pcdttrs_ (const char  *TRANS, const int *N, const int *NRHS,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcdttrsv.f
void pcdttrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *NRHS,  void *DL,  void *D,  void *DU, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgbtrf.f
void pcgbtrf_ (const int *N, const int *BWL, const int *BWU,  void *A, const int *JA, const int *DESCA,  int *IPIV,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, IPIV, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgbtrs.f
void pcgbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  void *A, const int *JA, const int *DESCA,  int *IPIV,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgebrd.f
void pcgebrd_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  float *D,  float *E,  void *TAUQ,  void *TAUP,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, D, E, TAUQ, TAUP, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgecon.f
void pcgecon_ (const char  *NORM, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA, const float *ANORM,  float *RCOND,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgeequ.f
void pcgeequ_ (const int *M, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA,  float *R,  float *C,  float *ROWCND,  float *COLCND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgehrd.f
void pcgehrd_ (const int *N, const int *ILO, const int *IHI,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgelqf.f
void pcgelqf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgeqlf.f
void pcgeqlf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgeqpf.f
void pcgeqpf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  void *TAU,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, TAU, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgeqrf.f
void pcgeqrf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgerfs.f
void pcgerfs_ (const char  *TRANS, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const void *AF, const int *IAF, const int *JAF, const int *DESCAF, const int *IPIV, const void *B, const int *IB, const int *JB, const int *DESCB,  void *X, const int *IX, const int *JX, const int *DESCX,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgerqf.f
void pcgerqf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgetrf.f
void pcgetrf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgetri.f
void pcgetri_ (const int *N,  void *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  void *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, IPIV, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcgetrs.f
void pcgetrs_ (const char  *TRANS, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcggqrf.f
void pcggqrf_ (const int *N, const int *M, const int *P,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAUA,  void *B, const int *IB, const int *JB, const int *DESCB,  void *TAUB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcggrqf.f
void pcggrqf_ (const int *M, const int *P, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAUA,  void *B, const int *IB, const int *JB, const int *DESCB,  void *TAUB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, P, N, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pchegst.f
void pchegst_ (const int *IBTYPE, const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA, const void *B, const int *IB, const int *JB, const int *DESCB,  float *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pchetrd.f
void pchetrd_ (const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  float *D,  float *E,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, D, E, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpbtrf.f
void pcpbtrf_ (const char  *UPLO, const int *N, const int *BW,  void *A, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpbtrs.f
void pcpbtrs_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpbtrsv.f
void pcpbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BW, const int *NRHS,  void *A, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpocon.f
void pcpocon_ (const char  *UPLO, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA, const float *ANORM,  float *RCOND,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpoequ.f
void pcpoequ_ (const int *N, const void *A, const int *IA, const int *JA, const int *DESCA,  float *SR,  float *SC,  float *SCOND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, SR, SC, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcporfs.f
void pcporfs_ (const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const void *AF, const int *IAF, const int *JAF, const int *DESCAF, const void *B, const int *IB, const int *JB, const int *DESCB, const void *X, const int *IX, const int *JX, const int *DESCX,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpotrf.f
void pcpotrf_ (const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpotri.f
void pcpotri_ (const char  *UPLO, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpotrs.f
void pcpotrs_ (const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpttrf.f
void pcpttrf_ (const int *N,  void *D,  void *E, const int *JA, const int *DESCA,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpttrs.f
void pcpttrs_ (const char  *UPLO, const int *N, const int *NRHS,  void *D,  void *E, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcpttrsv.f
void pcpttrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *NRHS,  void *D,  void *E, const int *JA, const int *DESCA,  void *B, const int *IB, const int *DESCB,  void *AF, const int *LAF, void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcstebz.f
//TODO


// https://netlib.org/scalapack/complex/pcstein.f
void pcstein_ (const int *N, const float *D, const float *E, const int *M,  float *W, const int *IBLOCK, const int *ISPLIT, const float *ORFAC,  void *Z, const int *IZ, const int *JZ, const int *DESCZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  float *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, M, W, IBLOCK, ISPLIT, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pctrcon.f
void pctrcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA,  float *RCOND,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, A, IA, JA, DESCA, RCOND, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pctrrfs.f
void pctrrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA, const void *B, const int *IB, const int *JB, const int *DESCB, const void *X, const int *IX, const int *JX, const int *DESCX,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, RWORK, LRWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pctrtri.f
void pctrtri_ (const char  *UPLO, const char  *DIAG, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pctrtrs.f
void pctrtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const void *A, const int *IA, const int *JA, const int *DESCA,  void *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pctzrzf.f
void pctzrzf_ (const int *M, const int *N,  void *A, const int *IA, const int *JA, const int *DESCA,  void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunglq.f
void pcunglq_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcungql.f
void pcungql_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcungqr.f
void pcungqr_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcungrq.f
void pcungrq_ (const int *M, const int *N, const int *K,  void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmbr.f
void pcunmbr_ (const char  *VECT, const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmhr.f
void pcunmhr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *ILO, const int *IHI, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, ILO, IHI, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmlq.f
void pcunmlq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmql.f
void pcunmql_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmqr.f
void pcunmqr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmrq.f
void pcunmrq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmrz.f
void pcunmrz_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/complex/pcunmtr.f
void pcunmtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const void *A, const int *IA, const int *JA, const int *DESCA, const void *TAU,  void *C, const int *IC, const int *JC, const int *DESCC,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


