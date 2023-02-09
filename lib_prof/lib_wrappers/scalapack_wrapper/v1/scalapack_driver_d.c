// https://netlib.org/scalapack/single/pddbsv.f
void pddbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  double *A, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddtsv.f
void pddtsv_ (const int *N, const int *NRHS,  double *DL,  double *D,  double *DU, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgbsv.f
void pdgbsv_ (const int *N, const int *BWL, const int *BWU, const int *NRHS,  double *A, const int *JA, const int *DESCA,  int *IPIV,  double *B, const int *IB, const int *DESCB, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgels.f
void pdgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  double *A, const int *IA, const int *JA, const int *DESCA,  double *B, const int *IB, const int *JB, const int *DESCB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgesv.f
void pdgesv_ (const int *N, const int *NRHS,  double *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  double *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgesvd.f
void pdgesvd_ (const char  *JOBU, const char  *JOBVT, const int *M, const int *N, double *A, const int *IA, const int *JA, const int *DESCA,  double *S,  double *U, const int *IU, const int *JU, const int *DESCU,  double *VT, const int *IVT, const int *JVT, const int *DESCVT,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(JOBU,JOBVT,M,N,A,IA,JA,DESCA,S,U,IU,JU,DESCU,VT,IVT,JVT,DESCVT,WORK,LWORK,INFO);
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpbsv.f
void pdpbsv_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  double *A, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdposv.f
void pdposv_ (const char  *UPLO, const int *N, const int *NRHS,  double *A, const int *IA, const int *JA, const int *DESCA,  double *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdptsv.f
void pdptsv_ (const int *N, const int *NRHS,  double *D,  double *E, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, JA, DESCA, B, IB, DESCB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdsyev.f
void pdsyev_ (const char  *JOBZ, const char  *UPLO, const int *N, double *A, const int *IA, const int *JA, const int *DESCA,  double *W,  double *Z, const int *IZ, const int *JZ, const int *DESCZ,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdsyevd.f
void pdsyevd_ (const char  *JOBZ, const char  *UPLO, const int *N, double *A, const int *IA, const int *JA, const int *DESCA,  double *W,  double *Z, const int *IZ, const int *JZ, const int *DESCZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, IA, JA, DESCA, W, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgesvx.f
void pdgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  double *A, const int *IA, const int *JA, const int *DESCA,  double *AF, const int *IAF, const int *JAF, const int *DESCAF,  int *IPIV,  char  *EQUED,  double *R,  double *C,  double *B, const int *IB, const int *JB, const int *DESCB,  double *X, const int *IX, const int *JX, const int *DESCX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, EQUED, R, C, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdposvx.f
void pdposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  double *A, const int *IA, const int *JA, const int *DESCA,  double *AF, const int *IAF, const int *JAF, const int *DESCAF,  char  *EQUED,  double *SR,  double *SC,  double *B, const int *IB, const int *JB, const int *DESCB,  double *X, const int *IX, const int *JX, const int *DESCX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, EQUED, SR, SC, B, IB, JB, DESCB, X, IX, JX, DESCX, RCOND, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdsyevx.f
void pdsyevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, double *A, const int *IA, const int *JA, const int *DESCA, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  int *NZ,  double *W, const double *ORFAC,  double *Z, const int *IZ, const int *JZ, const int *DESCZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  double *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdsygvx.f
void pdsygvx_ (const int *IBTYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *B, const int *IB, const int *JB, const int *DESCB, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  int *NZ,  double *W, const double *ORFAC,  double *Z, const int *IZ, const int *JZ, const int *DESCZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  double *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, JOBZ, RANGE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, VL, VU, IL, IU, ABSTOL, M, NZ, W, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO);
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddbtrf.f
void pddbtrf_ (const int *N, const int *BWL, const int *BWU,  double *A, const int *JA, const int *DESCA,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddbtrs.f
void pddbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  double *A, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddbtrsv.f
void pddbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  double *A, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddttrf.f
void pddttrf_ (const int *N,  double *DL,  double *D,  double *DU, const int *JA, const int *DESCA,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, DL, D, DU, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddttrs.f
void pddttrs_ (const char  *TRANS, const int *N, const int *NRHS,  double *DL,  double *D,  double *DU, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pddttrsv.f
void pddttrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *NRHS,  double *DL,  double *D,  double *DU, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, NRHS, DL, D, DU, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgbtrf.f
void pdgbtrf_ (const int *N, const int *BWL, const int *BWU,  double *A, const int *JA, const int *DESCA,  int *IPIV,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, BWL, BWU, A, JA, DESCA, IPIV, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgbtrs.f
void pdgbtrs_ (const char  *TRANS, const int *N, const int *BWL, const int *BWU, const int *NRHS,  double *A, const int *JA, const int *DESCA,  int *IPIV,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, BWL, BWU, NRHS, A, JA, DESCA, IPIV, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgebrd.f
void pdgebrd_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *D,  double *E,  double *TAUQ,  double *TAUP,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, D, E, TAUQ, TAUP, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgecon.f
void pdgecon_ (const char  *NORM, const int *N, const double *A, const int *IA, const int *JA, const int *DESCA, const double *ANORM,  double *RCOND,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgeequ.f
void pdgeequ_ (const int *M, const int *N, const double *A, const int *IA, const int *JA, const int *DESCA,  double *R,  double *C,  double *ROWCND,  double *COLCND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgehrd.f
void pdgehrd_ (const int *N, const int *ILO, const int *IHI,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgelqf.f
void pdgelqf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgeqlf.f
void pdgeqlf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgeqpf.f
void pdgeqpf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgeqrf.f
void pdgeqrf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgerfs.f
void pdgerfs_ (const char  *TRANS, const int *N, const int *NRHS, const double *A, const int *IA, const int *JA, const int *DESCA, const double *AF, const int *IAF, const int *JAF, const int *DESCAF, const int *IPIV, const double *B, const int *IB, const int *JB, const int *DESCB,  double *X, const int *IX, const int *JX, const int *DESCX,  double *FERR,  double *BERR,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, IPIV, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgerqf.f
void pdgerqf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgetrf.f
void pdgetrf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgetri.f
void pdgetri_ (const int *N,  double *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, IPIV, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdgetrs.f
void pdgetrs_ (const char  *TRANS, const int *N, const int *NRHS, const double *A, const int *IA, const int *JA, const int *DESCA, const int *IPIV,  double *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, IA, JA, DESCA, IPIV, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdggqrf.f
void pdggqrf_ (const int *N, const int *M, const int *P,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAUA,  double *B, const int *IB, const int *JB, const int *DESCB,  double *TAUB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdggrqf.f
void pdggrqf_ (const int *M, const int *P, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAUA,  double *B, const int *IB, const int *JB, const int *DESCB,  double *TAUB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, P, N, A, IA, JA, DESCA, TAUA, B, IB, JB, DESCB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdlahqr.f
void pdlahqr_ (const bool *WANTT, const bool *WANTZ, const int *N, const int *ILO, const int *IHI,  double *A, const int *DESCA, double *WR, double *WI, const int *ILOZ, const int *IHIZ,  double *Z, const int *DESCZ,  double *WORK, const int *LWORK, const int *IWORK, const int *ILWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( WANTT, WANTZ, N, ILO, IHI, A, DESCA, WR, WI, ILOZ, IHIZ, Z, DESCZ, WORK, LWORK, IWORK, ILWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdorglq.f
void pdorglq_ (const int *M, const int *N, const int *K,  double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdorgql.f
void pdorgql_ (const int *M, const int *N, const int *K,  double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdorgqr.f
void pdorgqr_ (const int *M, const int *N, const int *K,  double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdorgrq.f
void pdorgrq_ (const int *M, const int *N, const int *K,  double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormbr.f
void pdormbr_ (const char  *VECT, const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormhr.f
void pdormhr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *ILO, const int *IHI, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, ILO, IHI, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormlq.f
void pdormlq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormql.f
void pdormql_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormqr.f
void pdormqr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormrq.f
void pdormrq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormrz.f
void pdormrz_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdormtr.f
void pdormtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const double *A, const int *IA, const int *JA, const int *DESCA, const double *TAU,  double *C, const int *IC, const int *JC, const int *DESCC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, A, IA, JA, DESCA, TAU, C, IC, JC, DESCC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpbtrf.f
void pdpbtrf_ (const char  *UPLO, const int *N, const int *BW,  double *A, const int *JA, const int *DESCA,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, A, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpbtrs.f
void pdpbtrs_ (const char  *UPLO, const int *N, const int *BW, const int *NRHS,  double *A, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpbtrsv.f
void pdpbtrsv_ (const char  *UPLO, const char  *TRANS, const int *N, const int *BW, const int *NRHS,  double *A, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, N, BW, NRHS, A, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpocon.f
void pdpocon_ (const char  *UPLO, const int *N, const double *A, const int *IA, const int *JA, const int *DESCA, const double *ANORM,  double *RCOND,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, ANORM, RCOND, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpoequ.f
void pdpoequ_ (const int *N, const double *A, const int *IA, const int *JA, const int *DESCA,  double *SR,  double *SC,  double *SCOND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, IA, JA, DESCA, SR, SC, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdporfs.f
void pdporfs_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *IA, const int *JA, const int *DESCA, const double *AF, const int *IAF, const int *JAF, const int *DESCAF, const double *B, const int *IB, const int *JB, const int *DESCB, const double *X, const int *IX, const int *JX, const int *DESCX,  double *FERR,  double *BERR,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, AF, IAF, JAF, DESCAF, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpotrf.f
void pdpotrf_ (const char  *UPLO, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpotri.f
void pdpotri_ (const char  *UPLO, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpotrs.f
void pdpotrs_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *IA, const int *JA, const int *DESCA,  double *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpttrf.f
void pdpttrf_ (const int *N,  double *D,  double *E, const int *JA, const int *DESCA,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, JA, DESCA, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpttrs.f
void pdpttrs_ (const int *N, const int *NRHS,  double *D,  double *E, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdpttrsv.f
void pdpttrsv_ (const char  *UPLO, const int *N, const int *NRHS,  double *D,  double *E, const int *JA, const int *DESCA,  double *B, const int *IB, const int *DESCB,  double *AF, const int *LAF, double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, D, E, JA, DESCA, B, IB, DESCB, AF, LAF, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdstebz.f
// multiple routines in a single file

void pdstebz_ (const int *ICTXT, const char  *RANGE, const char  *ORDER, const int *N, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL, const double *D, const double *E,  int *M,  int *NSPLIT,  double *W,  int *IBLOCK,  int *ISPLIT,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ICTXT, RANGE, ORDER, N, VL, VU, IL, IU, ABSTOL, D, E, M, NSPLIT, W, IBLOCK, ISPLIT, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}

void pdlaebz_ ( const int *IJOB, const int *N, const int *MMAX, const int *MINP, const double *ABSTOL, const double *RELTOL, const double *PIVMIN, const double *D,  int *NVAL,  double *INTVL,  int *INTVLCT,  int *MOUT,  double *LSAVE, const int *IEFLAG,  int *INFO )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IJOB, N, MMAX, MINP, ABSTOL, RELTOL, PIVMIN, D, NVAL, INTVL, INTVLCT, MOUT, LSAVE, IEFLAG, INFO );
#include "function_wrapper_body2.c" 
  return;
}

void pdlaecv_ (const int *IJOB,  int *KF, const int *KL,  double *INTVL,  int *INTVLCT,  int *NVAL, const double *ABSTOL, const double *RELTOL) 
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IJOB, KF, KL, INTVL, INTVLCT, NVAL, ABSTOL, RELTOL);
#include "function_wrapper_body2.c" 
  return;
}

void pdlapdct_ ( const double *SIGMA, const int *N, const double *D, const double *PIVMIN,  int *COUNT)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(SIGMA, N, D, PIVMIN, COUNT);
#include "function_wrapper_body2.c" 
  return;
}




// https://netlib.org/scalapack/single/pdstedc.f
void pdstedc_ (const char  *COMPZ, const int *N,  double *D,  double *E,  double *Q, const int *IQ, const int *JQ, const int *DESCQ,  double *WORK,  int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Q, IQ, JQ, DESCQ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdstein.f
void pdstein_ (const int *N, const double *D, const double *E, const int *M,  double *W, const int *IBLOCK, const int *ISPLIT, const double *ORFAC,  double *Z, const int *IZ, const int *JZ, const int *DESCZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *IFAIL,  int *ICLUSTR,  double *GAP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, M, W, IBLOCK, ISPLIT, ORFAC, Z, IZ, JZ, DESCZ, WORK, LWORK, IWORK, LIWORK, IFAIL, ICLUSTR, GAP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdsygst.f
void pdsygst_ (const int *IBTYPE, const char  *UPLO, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA, const double *B, const int *IB, const int *JB, const int *DESCB,  double *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IBTYPE, UPLO, N, A, IA, JA, DESCA, B, IB, JB, DESCB, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdsytrd.f
void pdsytrd_ (const char  *UPLO, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *D,  double *E,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, IA, JA, DESCA, D, E, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdtrcon.f
void pdtrcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const double *A, const int *IA, const int *JA, const int *DESCA,  double *RCOND,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, A, IA, JA, DESCA, RCOND, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdtrrfs.f
void pdtrrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const double *A, const int *IA, const int *JA, const int *DESCA, const double *B, const int *IB, const int *JB, const int *DESCB, const double *X, const int *IX, const int *JX, const int *DESCX,  double *FERR,  double *BERR,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, X, IX, JX, DESCX, FERR, BERR, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdtrtri.f
void pdtrtri_ (const char  *UPLO, const char  *DIAG, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, A, IA, JA, DESCA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdtrtrs.f
void pdtrtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const double *A, const int *IA, const int *JA, const int *DESCA,  double *B, const int *IB, const int *JB, const int *DESCB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, IA, JA, DESCA, B, IB, JB, DESCB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


// https://netlib.org/scalapack/single/pdtzrzf.f
void pdtzrzf_ (const int *M, const int *N,  double *A, const int *IA, const int *JA, const int *DESCA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, IA, JA, DESCA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


