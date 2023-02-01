// https://netlib.org/scalapack/single/psdbsv.f
void psdbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  float *A, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdtsv.f
void psdtsv_ (const int *N, const int *NRHS,  float *DL,  float *D,  float *DU, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgbsv.f
void psgbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  float *A, const int *JA, const int *DESCA,  int *IPIV,  float *B, const int *IB, const int *DESCB, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgels.f
void psgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  float *A, const int *IA, const int *JA, const int *DESCA,  float *B, const int *IB, const int *JB, const int *DESCB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgesv.f
void psgesv_ (const int *N, const int *NRHS,  float *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  float *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgesvd.f
void psgesvd_ (const char  *JOBU, const char  *JOBVT, const int *M, const int *N, float *A, const int *IA, const int *JA, const int *DESCA,  float *S,  float *U, const int *IU, const int *JU, const int *DESCU,  float *VT, const int *IVT, const int *JVT, const int *DESCVT,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(JOBU,JOBVT,M,N,A,IA,JA,DESCA,S,U,IU,JU,DESCU,VT,IVT,JVT,DESCVT,WORK,LWORK,INFO);
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspbsv.f
void pspbsv_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  float *A, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psposv.f
void psposv_ (const char  *UPLO, const int *N, const int *NRHS,  float *A, const int *IA, const int *JA, const int *DESCA,  float *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psptsv.f
void psptsv_ (const int *N, const int *NRHS,  float *D,  float *E, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pssyev.f
void pssyev_ (const char  *JOBZ, const char  *UPLO, const int *N, float *A, const int *IA, const int *JA, const int *DESCA,  float *W,  float *Z, const int *IZ, const int *JZ, const int *DESCZ,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pssyevd.f
void pssyevd_ (const char  *JOBZ, const char  *UPLO, const int *N, float *A, const int *IA, const int *JA, const int *DESCA,  float *W,  float *Z, const int *IZ, const int *JZ, const int *DESCZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgesvx.f
void psgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  float *A, const int *IA, const int *JA, const int *DESCA,  float *AF, const int *IAF, const int *JAF, const int *DESCAF,  int *IPIV,  char  *EQUED,  float *R,  float *C,  float *B, const int *IB, const int *JB, const int *DESCB,  float *X, const int *IX, const int *JX, const int *DESCX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, EQUED, R, C, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psposvx.f
void psposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  float *A, const int *IA, const int *JA, const int *DESCA,  float *AF, const int *IAF, const int *JAF, const int *DESCAF,  char  *EQUED,  float *SR,  float *SC,  float *B, const int *IB, const int *JB, const int *DESCB,  float *X, const int *IX, const int *JX, const int *DESCX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, EQUED, SR, SC, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pssyevx.f
void pssyevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, float *A, const int *IA, const int *JA, const int *DESCA, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  int *NZ,  float *W, const float *ORFAC,  float *Z, const int *IZ, const int *JZ, const int *DESCZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  float *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pssygvx.f
void pssygvx_ (const int *IBTYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *B, const int *IB, const int *JB, const int *DESCB, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  int *NZ,  float *W, const float *ORFAC,  float *Z, const int *IZ, const int *JZ, const int *DESCZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  float *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO);
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdbtrf.f
void psdbtrf_ (const int *N, const int *BWL, const int *BWU,  float *A, const int *JA, const int *DESCA,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdbtrs.f
void psdbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  float *A, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdbtrsv.f
void psdbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  float *A, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdttrf.f
void psdttrf_ (const int *N,  float *DL,  float *D,  float *DU, const int *JA, const int *DESCA,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, DL, D, DU, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdttrs.f
void psdttrs_ (const char  *TRANS, const int *N, const int *NRHS,  float *DL,  float *D,  float *DU, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psdttrsv.f
void psdttrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *NRHS,  float *DL,  float *D,  float *DU, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgbtrf.f
void psgbtrf_ (const int *N, const int *BWL, const int *BWU,  float *A, const int *JA, const int *DESCA,  int *IPIV,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, IPIV, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgbtrs.f
void psgbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  float *A, const int *JA, const int *DESCA,  int *IPIV,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgebrd.f
void psgebrd_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *D,  float *E,  float *TAUQ,  float *TAUP,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, D, E, TAUQ, TAUP, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgecon.f
void psgecon_ (const char  *NORM, const int *N, const float *A, const int *IA, const int *JA, const int *DESCA, const float *ANORM,  float *RCOND,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgeequ.f
void psgeequ_ (const int *M, const int *N, const float *A, const int *IA, const int *JA, const int *DESCA,  float *R,  float *C,  float *ROWCND,  float *COLCND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgehrd.f
void psgehrd_ (const int *N, const int *ILO, const int *IHI,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgelqf.f
void psgelqf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgeqlf.f
void psgeqlf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgeqpf.f
void psgeqpf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgeqrf.f
void psgeqrf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgerfs.f
void psgerfs_ (const char  *TRANS, const int *N, const int *NRHS, const float *A, const int *IA, const int *JA, const int *DESCA, const float *AF, const int *IAF, const int *JAF, const int *DESCAF, const int *IPIV, const float *B, const int *IB, const int *JB, const int *DESCB,  float *X, const int *IX, const int *JX, const int *DESCX,  float *FERR,  float *BERR,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgerqf.f
void psgerqf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgetrf.f
void psgetrf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgetri.f
void psgetri_ (const int *N,  float *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, IPIV, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psgetrs.f
void psgetrs_ (const char  *TRANS, const int *N, const int *NRHS, const float *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  float *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psggqrf.f
void psggqrf_ (const int *N, const int *M, const int *P,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAUA,  float *B, const int *IB, const int *JB, const int *DESCB,  float *TAUB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psggrqf.f
void psggrqf_ (const int *M, const int *P, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAUA,  float *B, const int *IB, const int *JB, const int *DESCB,  float *TAUB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, P, N, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pslahqr.f
void pslahqr_ (const bool *WANTT, const bool *WANTZ, const int *N, const int *ILO, const int *IHI,  float *A, const int *DESCA, float *WR, float *WI, const int *ILOZ, const int *IHIZ,  float *Z, const int *DESCZ,  float *WORK, const int *LWORK, const int *IWORK, const int *ILWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( WANTT, WANTZ, N, ILO, IHI, A, DESCA, WR, WI, ILOZ, IHIZ, Z, DESCZ, WORK, LWORK, IWORK, ILWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psorglq.f
void psorglq_ (const int *M, const int *N, const int *K,  float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psorgql.f
void psorgql_ (const int *M, const int *N, const int *K,  float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psorgqr.f
void psorgqr_ (const int *M, const int *N, const int *K,  float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psorgrq.f
void psorgrq_ (const int *M, const int *N, const int *K,  float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormbr.f
void psormbr_ (const char  *VECT, const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormhr.f
void psormhr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *ILO, const int *IHI, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, ILO, IHI, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormlq.f
void psormlq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormql.f
void psormql_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormqr.f
void psormqr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormrq.f
void psormrq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormrz.f
void psormrz_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psormtr.f
void psormtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const float *A, const int *IA, const int *JA, const int *DESCA, const float *TAU,  float *C, const int *IC, const int *JC, const int *DESCC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspbtrf.f
void pspbtrf_ (const char  *UPLO, const int *N, const int *BW,  float *A, const int *JA, const int *DESCA,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspbtrs.f
void pspbtrs_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  float *A, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspbtrsv.f
void pspbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BW, const int *NRHS,  float *A, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspocon.f
void pspocon_ (const char  *UPLO, const int *N, const float *A, const int *IA, const int *JA, const int *DESCA, const float *ANORM,  float *RCOND,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspoequ.f
void pspoequ_ (const int *N, const float *A, const int *IA, const int *JA, const int *DESCA,  float *SR,  float *SC,  float *SCOND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, SR, SC, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psporfs.f
void psporfs_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *IA, const int *JA, const int *DESCA, const float *AF, const int *IAF, const int *JAF, const int *DESCAF, const float *B, const int *IB, const int *JB, const int *DESCB, const float *X, const int *IX, const int *JX, const int *DESCX,  float *FERR,  float *BERR,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspotrf.f
void pspotrf_ (const char  *UPLO, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspotri.f
void pspotri_ (const char  *UPLO, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspotrs.f
void pspotrs_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *IA, const int *JA, const int *DESCA,  float *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspttrf.f
void pspttrf_ (const int *N,  float *D,  float *E, const int *JA, const int *DESCA,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspttrs.f
void pspttrs_ (const int *N, const int *NRHS,  float *D,  float *E, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pspttrsv.f
void pspttrsv_ (const char  *UPLO, const int *N, const int *NRHS,  float *D,  float *E, const int *JA, const int *DESCA,  float *B, const int *IB, const int *DESCB,  float *AF, const int *LAF, float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psstebz.f
// multiple routines in a single file

void psstebz_ (const int *ICTXT, const char  *RANGE, const char  *ORDER, const int *N, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL, const float *D, const float *E,  int *M,  int *NSPLIT,  float *W,  int *IBLOCK,  int *ISPLIT,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ICTXT, RANGE, ORDER, N, VL, VU, IL, IU, ABSTOL, D, E, M, NSPLIT, W, IBLOCK, ISPLIT, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}

void pslaebz_ ( const int *IJOB, const int *N, const int *MMAX, const int *MINP, const float *ABSTOL, const float *RELTOL, const float *PIVMIN, const float *D,  int *NVAL,  float *INTVL,  int *INTVLCT,  int *MOUT,  float *LSAVE, const int *IEFLAG,  int *INFO )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IJOB, N, MMAX, MINP, ABSTOL, RELTOL, PIVMIN, D, NVAL, INTVL, INTVLCT, MOUT, LSAVE, IEFLAG, INFO );
#include "function_wrapper_body2.c" 
  return;
}

void pslaecv_ (const int *IJOB,  int *KF, const int *KL,  float *INTVL,  int *INTVLCT,  int *NVAL, const float *ABSTOL, const float *RELTOL) 
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IJOB, KF, KL, INTVL, INTVLCT, NVAL, ABSTOL, RELTOL);
#include "function_wrapper_body2.c" 
  return;
}

void pslapdct_ ( const float *SIGMA, const int *N, const float *D, const float *PIVMIN,  int *COUNT)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(SIGMA, N, D, PIVMIN, COUNT);
#include "function_wrapper_body2.c" 
  return;
}




// https://netlib.org/scalapack/single/psstedc.f
void psstedc_ (const char  *COMPZ, const int *N,  float *D,  float *E,  float *Q, const int *IQ, const int *JQ, const int *DESCQ,  float *WORK,  int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Q, IQ, JQ, DESCQ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/psstein.f
void psstein_ (const int *N, const float *D, const float *E, const int *M,  float *W, const int *IBLOCK, const int *ISPLIT, const float *ORFAC,  float *Z, const int *IZ, const int *JZ, const int *DESCZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  float *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, M, W, IBLOCK, ISPLIT, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pssygst.f
void pssygst_ (const int *IBTYPE, const char  *UPLO, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA, const float *B, const int *IB, const int *JB, const int *DESCB,  float *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pssytrd.f
void pssytrd_ (const char  *UPLO, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *D,  float *E,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, D, E, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pstrcon.f
void pstrcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const float *A, const int *IA, const int *JA, const int *DESCA,  float *RCOND,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, A, IA, JA, DESCA, RCOND, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pstrrfs.f
void pstrrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const float *A, const int *IA, const int *JA, const int *DESCA, const float *B, const int *IB, const int *JB, const int *DESCB, const float *X, const int *IX, const int *JX, const int *DESCX,  float *FERR,  float *BERR,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pstrtri.f
void pstrtri_ (const char  *UPLO, const char  *DIAG, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pstrtrs.f
void pstrtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const float *A, const int *IA, const int *JA, const int *DESCA,  float *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pstzrzf.f
void pstzrzf_ (const int *M, const int *N,  float *A, const int *IA, const int *JA, const int *DESCA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


