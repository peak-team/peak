void dgesv_ (const int *N, const int *NRHS,  double *A, const int *LDA,  int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsgesv_ (const int *N, const int *NRHS,  double *A, const int *LDA,  int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *WORK,  float *SWORK,  int *ITER,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, LDA, IPIV, B, LDB, X, LDX, WORK, SWORK, ITER, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbsv_ (const int *N, const int *KL, const int *KU, const int *NRHS,  double *AB, const int *LDAB,  int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, KL, KU, NRHS, AB, LDAB, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgtsv_ (const int *N, const int *NRHS,  double *DL,  double *D,  double *DU,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, DL, D, DU, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dposv_ (const char  *UPLO, const int *N, const int *NRHS,  double *A, const int *LDA,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dppsv_ (const char  *UPLO, const int *N, const int *NRHS,  double *AP,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbsv_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS,  double *AB, const int *LDAB,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dptsv_ (const int *N, const int *NRHS,  double *D,  double *E,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsysv_ (const char  *UPLO, const int *N, const int *NRHS,  double *A, const int *LDA,  int *IPIV,  double *B, const int *LDB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspsv_ (const char  *UPLO, const int *N, const int *NRHS,  double *AP,  int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  double *A, const int *LDA,  double *B, const int *LDB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, LDA, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgelsd_ (const int *M, const int *N, const int *NRHS, const double *A, const int *LDA,  double *B, const int *LDB,  double *S, const double *RCOND,  int *RANK,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgglse_ (const int *M, const int *N, const int *P,  double *A, const int *LDA,  double *B, const int *LDB,  double *C,  double *D,  double *X,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, P, A, LDA, B, LDB, C, D, X, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggglm_ (const int *N, const int *M, const int *P,  double *A, const int *LDA,  double *B, const int *LDB,  double *D,  double *X,  double *Y,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, LDA, B, LDB, D, X, Y, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsyev_ (const char  *JOBZ, const char  *UPLO, const int *N,  double *A, const int *LDA,  double *W,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsyevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  double *A, const int *LDA,  double *W,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspev_ (const char  *JOBZ, const char  *UPLO, const int *N,  double *AP,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  double *AP,  double *W,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbev_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  double *AB, const int *LDAB,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbevd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  double *AB, const int *LDAB,  double *W,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstev_ (const char  *JOBZ, const int *N,  double *D,  double *E,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, N, D, E, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstevd_ (const char  *JOBZ, const int *N,  double *D,  double *E,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, N, D, E, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgees_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const int *N,  double *A, const int *LDA,  int *SDIM,  double *WR,  double *WI,  double *VS, const int *LDVS,  double *WORK, const int *LWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, N, A, LDA, SDIM, WR, WI, VS, LDVS, WORK, LWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  double *A, const int *LDA,  double *WR,  double *WI,  double *VL, const int *LDVL,  double *VR, const int *LDVR,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, WR, WI, VL, LDVL, VR, LDVR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgesvd_ (const char  *JOBU, const char  *JOBVT, const int *M, const int *N,  double *A, const int *LDA,  double *S,  double *U, const int *LDU,  double *VT, const int *LDVT,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBVT, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgesdd_ (const char  *JOBZ, const int *M, const int *N,  double *A, const int *LDA,  double *S,  double *U, const int *LDU,  double *VT, const int *LDVT,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsygv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *W,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsygvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *W,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspgv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  double *AP,  double *BP,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspgvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  double *AP,  double *BP,  double *W,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbgv_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  double *AB, const int *LDAB,  double *BB, const int *LDBB,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbgvd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  double *AB, const int *LDAB,  double *BB, const int *LDBB,  double *W,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgegs_ (const char  *JOBVSL, const char  *JOBVSR, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *VSL, const int *LDVSL,  double *VSR, const int *LDVSR,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgges_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  int *SDIM,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *VSL, const int *LDVSL,  double *VSR, const int *LDVSR,  double *WORK, const int *LWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, N, A, LDA, B, LDB, SDIM, ALPHAR, ALPHAI, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgegv_ (const char  *JOBVL, const char  *JOBVR, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *VL, const int *LDVL,  double *VR, const int *LDVR,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *VL, const int *LDVL,  double *VR, const int *LDVR,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggsvd_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *N, const int *P,  int *K,  int *L,  double *A, const int *LDA,  double *B, const int *LDB,  double *ALPHA,  double *BETA,  double *U, const int *LDU,  double *V, const int *LDV,  double *Q, const int *LDQ,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, N, P, K, L, A, LDA, B, LDB, ALPHA, BETA, U, LDU, V, LDV, Q, LDQ, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  double *A, const int *LDA,  double *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  double *R,  double *C,  double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbsvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS,  double *AB, const int *LDAB,  double *AFB, const int *LDAFB,  int *IPIV,  char  *EQUED,  double *R,  double *C,  double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgtsvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS, const double *DL, const double *D, const double *DU,  double *DLF,  double *DF,  double *DUF,  double *DU2,  int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, DL, D, DU, DLF, DF, DUF, DU2, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  double *A, const int *LDA,  double *AF, const int *LDAF,  char  *EQUED,  double *S,  double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dppsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  double *AP,  double *AFP,  char  *EQUED,  double *S,  double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *KD, const int *NRHS,  double *AB, const int *LDAB,  double *AFB, const int *LDAFB,  char  *EQUED,  double *S,  double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, KD, NRHS, AB, LDAB, AFB, LDAFB, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dptsvx_ (const char  *FACT, const int *N, const int *NRHS, const double *D, const double *E,  double *DF,  double *EF, const double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, N, NRHS, D, E, DF, EF, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsysvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *LDA,  double *AF, const int *LDAF,  int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const double *AP,  double *AFP,  int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgelsx_ (const int *M, const int *N, const int *NRHS,  double *A, const int *LDA,  double *B, const int *LDB,  int *JPVT, const double *RCOND,  int *RANK,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgelsy_ (const int *M, const int *N, const int *NRHS,  double *A, const int *LDA,  double *B, const int *LDB,  int *JPVT, const double *RCOND,  int *RANK,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgelss_ (const int *M, const int *N, const int *NRHS,  double *A, const int *LDA,  double *B, const int *LDB,  double *S, const double *RCOND,  int *RANK,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsyevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  double *A, const int *LDA, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsyevr_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  double *A, const int *LDA, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  int *ISUPPZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsygvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  double *A, const int *LDA,  double *B, const int *LDB, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, A, LDA, B, LDB, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  double *AP, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, AP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspgvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  double *AP,  double *BP, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, AP, BP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KD,  double *AB, const int *LDAB,  double *Q, const int *LDQ, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KD, AB, LDAB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbgvx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KA, const int *KB,  double *AB, const int *LDAB,  double *BB, const int *LDBB,  double *Q, const int *LDQ, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstevx_ (const char  *JOBZ, const char  *RANGE, const int *N,  double *D,  double *E, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  double *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstevr_ (const char  *JOBZ, const char  *RANGE, const int *N,  double *D,  double *E, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  int *ISUPPZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeesx_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const char  *SENSE, const int *N,  double *A, const int *LDA,  int *SDIM,  double *WR,  double *WI,  double *VS, const int *LDVS,  double *RCONDE,  double *RCONDV,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, SENSE, N, A, LDA, SDIM, WR, WI, VS, LDVS, RCONDE, RCONDV, WORK, LWORK, IWORK, LIWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggesx_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const char  *SENSE, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  int *SDIM,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *VSL, const int *LDVSL,  double *VSR, const int *LDVSR,  double *RCONDE,  double *RCONDV,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, SENSE, N, A, LDA, B, LDB, SDIM, ALPHAR, ALPHAI, BETA, VSL, LDVSL, VSR, LDVSR, RCONDE, RCONDV, WORK, LWORK, IWORK, LIWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  double *A, const int *LDA,  double *WR,  double *WI,  double *VL, const int *LDVL,  double *VR, const int *LDVR,  int *ILO,  int *IHI,  double *SCALE,  double *ABNRM,  double *RCONDE,  double *RCONDV,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, WR, WI, VL, LDVL, VR, LDVR, ILO, IHI, SCALE, ABNRM, RCONDE, RCONDV, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *VL, const int *LDVL,  double *VR, const int *LDVR,  int *ILO,  int *IHI,  double *LSCALE,  double *RSCALE,  double *ABNRM,  double *BBNRM,  double *RCONDE,  double *RCONDV,  double *WORK, const int *LWORK,  int *IWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VL, LDVL, VR, LDVR, ILO, IHI, LSCALE, RSCALE, ABNRM, BBNRM, RCONDE, RCONDV, WORK, LWORK, IWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dbdsdc_ (const char  *UPLO, const char  *COMPQ, const int *N,  double *D,  double *E,  double *U, const int *LDU,  double *VT, const int *LDVT,  double *Q,  int *IQ,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, COMPQ, N, D, E, U, LDU, VT, LDVT, Q, IQ, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dbdsqr_ (const char  *UPLO, const int *N, const int *NCVT, const int *NRU, const int *NCC,  double *D,  double *E,  double *VT, const int *LDVT,  double *U, const int *LDU,  double *C, const int *LDC,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NCVT, NRU, NCC, D, E, VT, LDVT, U, LDU, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ddisna_ (const char  *JOB, const int *M, const int *N, const double *D,  double *SEP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, M, N, D, SEP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbbrd_ (const char  *VECT, const int *M, const int *N, const int *NCC, const int *KL, const int *KU,  double *AB, const int *LDAB,  double *D,  double *E,  double *Q, const int *LDQ,  double *PT, const int *LDPT,  double *C, const int *LDC,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, M, N, NCC, KL, KU, AB, LDAB, D, E, Q, LDQ, PT, LDPT, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbcon_ (const char  *NORM, const int *N, const int *KL, const int *KU, const double *AB, const int *LDAB, const int *IPIV, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, KL, KU, AB, LDAB, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbequ_ (const int *M, const int *N, const int *KL, const int *KU, const double *AB, const int *LDAB,  double *R,  double *C,  double *ROWCND,  double *COLCND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, KL, KU, AB, LDAB, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbrfs_ (const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS, const double *AB, const int *LDAB, const double *AFB, const int *LDAFB, const int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbtrf_ (const int *M, const int *N, const int *KL, const int *KU,  double *AB, const int *LDAB,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, KL, KU, AB, LDAB, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgbtrs_ (const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS, const double *AB, const int *LDAB, const int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, KL, KU, NRHS, AB, LDAB, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgebak_ (const char  *JOB, const char  *SIDE, const int *N, const int *ILO, const int *IHI, const double *SCALE, const int *M,  double *V, const int *LDV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, SIDE, N, ILO, IHI, SCALE, M, V, LDV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgebal_ (const char  *JOB, const int *N,  double *A, const int *LDA,  int *ILO,  int *IHI,  double *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, N, A, LDA, ILO, IHI, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgebrd_ (const int *M, const int *N,  double *A, const int *LDA,  double *D,  double *E,  double *TAUQ,  double *TAUP,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, D, E, TAUQ, TAUP, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgecon_ (const char  *NORM, const int *N, const double *A, const int *LDA, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, A, LDA, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeequ_ (const int *M, const int *N, const double *A, const int *LDA,  double *R,  double *C,  double *ROWCND,  double *COLCND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgehrd_ (const int *N, const int *ILO, const int *IHI,  double *A, const int *LDA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgelqf_ (const int *M, const int *N,  double *A, const int *LDA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeqlf_ (const int *M, const int *N,  double *A, const int *LDA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeqp3_ (const int *M, const int *N,  double *A, const int *LDA,  int *JPVT,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, JPVT, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeqpf_ (const int *M, const int *N,  double *A, const int *LDA,  int *JPVT,  double *TAU,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, JPVT, TAU, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgeqrf_ (const int *M, const int *N,  double *A, const int *LDA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgerfs_ (const char  *TRANS, const int *N, const int *NRHS, const double *A, const int *LDA, const double *AF, const int *LDAF, const int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgerqf_ (const int *M, const int *N,  double *A, const int *LDA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgetrf_ (const int *M, const int *N,  double *A, const int *LDA,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgetri_ (const int *N,  double *A, const int *LDA, const int *IPIV,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, LDA, IPIV, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgetrs_ (const char  *TRANS, const int *N, const int *NRHS, const double *A, const int *LDA, const int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggbak_ (const char  *JOB, const char  *SIDE, const int *N, const int *ILO, const int *IHI, const double *LSCALE, const double *RSCALE, const int *M,  double *V, const int *LDV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, SIDE, N, ILO, IHI, LSCALE, RSCALE, M, V, LDV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggbal_ (const char  *JOB, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  int *ILO,  int *IHI,  double *LSCALE,  double *RSCALE,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, N, A, LDA, B, LDB, ILO, IHI, LSCALE, RSCALE, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgghrd_ (const char  *COMPQ, const char  *COMPZ, const int *N, const int *ILO, const int *IHI,  double *A, const int *LDA,  double *B, const int *LDB,  double *Q, const int *LDQ,  double *Z, const int *LDZ,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPQ, COMPZ, N, ILO, IHI, A, LDA, B, LDB, Q, LDQ, Z, LDZ, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggqrf_ (const int *N, const int *M, const int *P,  double *A, const int *LDA,  double *TAUA,  double *B, const int *LDB,  double *TAUB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, LDA, TAUA, B, LDB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggrqf_ (const int *M, const int *P, const int *N,  double *A, const int *LDA,  double *TAUA,  double *B, const int *LDB,  double *TAUB,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, P, N, A, LDA, TAUA, B, LDB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dggsvp_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *P, const int *N,  double *A, const int *LDA,  double *B, const int *LDB, const double *TOLA, const double *TOLB,  int *K,  int *L,  double *U, const int *LDU,  double *V, const int *LDV,  double *Q, const int *LDQ,  int *IWORK,  double *TAU,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, P, N, A, LDA, B, LDB, TOLA, TOLB, K, L, U, LDU, V, LDV, Q, LDQ, IWORK, TAU, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgtcon_ (const char  *NORM, const int *N, const double *DL, const double *D, const double *DU, const double *DU2, const int *IPIV, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, DL, D, DU, DU2, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgtrfs_ (const char  *TRANS, const int *N, const int *NRHS, const double *DL, const double *D, const double *DU, const double *DLF, const double *DF, const double *DUF, const double *DU2, const int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, DLF, DF, DUF, DU2, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgttrf_ (const int *N,  double *DL,  double *D,  double *DU,  double *DU2,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, DL, D, DU, DU2, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dgttrs_ (const char  *TRANS, const int *N, const int *NRHS, const double *DL, const double *D, const double *DU, const double *DU2, const int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, DU2, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dhgeqz_ (const char  *JOB, const char  *COMPQ, const char  *COMPZ, const int *N, const int *ILO, const int *IHI,  double *H, const int *LDH,  double *T, const int *LDT,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *Q, const int *LDQ,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, COMPQ, COMPZ, N, ILO, IHI, H, LDH, T, LDT, ALPHAR, ALPHAI, BETA, Q, LDQ, Z, LDZ, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dhsein_ (const char  *SIDE, const char  *EIGSRC, const char  *INITV,  bool *SELECT, const int *N, const double *H, const int *LDH,  double *WR, const double *WI,  double *VL, const int *LDVL,  double *VR, const int *LDVR, const int *MM,  int *M,  double *WORK,  int *IFAILL,  int *IFAILR,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, EIGSRC, INITV, SELECT, N, H, LDH, WR, WI, VL, LDVL, VR, LDVR, MM, M, WORK, IFAILL, IFAILR, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dhseqr_ (const char  *JOB, const char  *COMPZ, const int *N, const int *ILO, const int *IHI,  double *H, const int *LDH,  double *WR,  double *WI,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, COMPZ, N, ILO, IHI, H, LDH, WR, WI, Z, LDZ, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dopgtr_ (const char  *UPLO, const int *N, const double *AP, const double *TAU,  double *Q, const int *LDQ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, TAU, Q, LDQ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dopmtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const double *AP, const double *TAU,  double *C, const int *LDC,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, AP, TAU, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorcsd_ (const char *JOBU1, const char *JOBU2, const char *JOBV1T, const char *JOBV2T, const char *TRANS, const char *SIGNS, const int *M, const int *P, const int *Q,  double *X, const int *LDX,  double *THETA,  double *U1, const int *LDU1,  double *U2, const int *LDU2,  double *V1T, const int *LDV1T,  double *V2T, const int *LDV2T,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU1, JOBU2, JOBV1T, JOBV2T, TRANS, SIGNS, M, P, Q, X, LDX, THETA, U1, LDU1, U2, LDU2, V1T, LDV1T, V2T, LDV2T, WORK, LWORK, IWORK, INFO);
#include "function_wrapper_body2.c" 
  return;
}


void dorgbr_ (const char  *VECT, const int *M, const int *N, const int *K,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorghr_ (const int *N, const int *ILO, const int *IHI,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorglq_ (const int *M, const int *N, const int *K,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorgql_ (const int *M, const int *N, const int *K,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorgqr_ (const int *M, const int *N, const int *K,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorgrq_ (const int *M, const int *N, const int *K,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dorgtr_ (const char  *UPLO, const int *N,  double *A, const int *LDA, const double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormbr_ (const char  *VECT, const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormhr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *ILO, const int *IHI, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, ILO, IHI, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormlq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormql_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormqr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormr3_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, LDA, TAU, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormrq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormrz_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dormtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const double *A, const int *LDA, const double *TAU,  double *C, const int *LDC,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbcon_ (const char  *UPLO, const int *N, const int *KD, const double *AB, const int *LDAB, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbequ_ (const char  *UPLO, const int *N, const int *KD, const double *AB, const int *LDAB,  double *S,  double *SCOND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, S, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbrfs_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS, const double *AB, const int *LDAB, const double *AFB, const int *LDAFB, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, AFB, LDAFB, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbstf_ (const char  *UPLO, const int *N, const int *KD,  double *AB, const int *LDAB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbtrf_ (const char  *UPLO, const int *N, const int *KD,  double *AB, const int *LDAB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpbtrs_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS, const double *AB, const int *LDAB,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpocon_ (const char  *UPLO, const int *N, const double *A, const int *LDA, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpoequ_ (const int *N, const double *A, const int *LDA,  double *S,  double *SCOND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, LDA, S, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dporfs_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *LDA, const double *AF, const int *LDAF, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, AF, LDAF, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpotrf_ (const char  *UPLO, const int *N,  double *A, const int *LDA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpotri_ (const char  *UPLO, const int *N,  double *A, const int *LDA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpotrs_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *LDA,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dppcon_ (const char  *UPLO, const int *N, const double *AP, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dppequ_ (const char  *UPLO, const int *N, const double *AP,  double *S,  double *SCOND,  double *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, S, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpprfs_ (const char  *UPLO, const int *N, const int *NRHS, const double *AP, const double *AFP, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, AFP, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpptrf_ (const char  *UPLO, const int *N,  double *AP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpptri_ (const char  *UPLO, const int *N,  double *AP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpptrs_ (const char  *UPLO, const int *N, const int *NRHS, const double *AP,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dptcon_ (const int *N, const double *D, const double *E, const double *ANORM,  double *RCOND,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, ANORM, RCOND, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpteqr_ (const char  *COMPZ, const int *N,  double *D,  double *E,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dptrfs_ (const int *N, const int *NRHS, const double *D, const double *E, const double *DF, const double *EF, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, DF, EF, B, LDB, X, LDX, FERR, BERR, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpttrf_ (const int *N,  double *D,  double *E,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dpttrs_ (const int *N, const int *NRHS, const double *D, const double *E,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbgst_ (const char  *VECT, const char  *UPLO, const int *N, const int *KA, const int *KB,  double *AB, const int *LDAB, const double *BB, const int *LDBB,  double *X, const int *LDX,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, X, LDX, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsbtrd_ (const char  *VECT, const char  *UPLO, const int *N, const int *KD,  double *AB, const int *LDAB,  double *D,  double *E,  double *Q, const int *LDQ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, UPLO, N, KD, AB, LDAB, D, E, Q, LDQ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspcon_ (const char  *UPLO, const int *N, const double *AP, const int *IPIV, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dspgst_ (const int *ITYPE, const char  *UPLO, const int *N,  double *AP, const double *BP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, UPLO, N, AP, BP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsprfs_ (const char  *UPLO, const int *N, const int *NRHS, const double *AP, const double *AFP, const int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsptrd_ (const char  *UPLO, const int *N,  double *AP,  double *D,  double *E,  double *TAU,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, D, E, TAU, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsptrf_ (const char  *UPLO, const int *N,  double *AP,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsptri_ (const char  *UPLO, const int *N,  double *AP, const int *IPIV,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, IPIV, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsptrs_ (const char  *UPLO, const int *N, const int *NRHS, const double *AP, const int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstebz_ (const char  *RANGE, const char  *ORDER, const int *N, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL, const double *D, const double *E,  int *M,  int *NSPLIT,  double *W,  int *IBLOCK,  int *ISPLIT,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( RANGE, ORDER, N, VL, VU, IL, IU, ABSTOL, D, E, M, NSPLIT, W, IBLOCK, ISPLIT, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstedc_ (const char  *COMPZ, const int *N,  double *D,  double *E,  double *Z, const int *LDZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstegr_ (const char  *JOBZ, const char  *RANGE, const int *N,  double *D,  double *E, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  double *Z, const int *LDZ,  int *ISUPPZ,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dstein_ (const int *N, const double *D, const double *E, const int *M, const double *W, const int *IBLOCK, const int *ISPLIT,  double *Z, const int *LDZ,  double *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, M, W, IBLOCK, ISPLIT, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsteqr_ (const char  *COMPZ, const int *N,  double *D,  double *E,  double *Z, const int *LDZ,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsterf_ (const int *N,  double *D,  double *E,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsycon_ (const char  *UPLO, const int *N, const double *A, const int *LDA, const int *IPIV, const double *ANORM,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsygst_ (const int *ITYPE, const char  *UPLO, const int *N,  double *A, const int *LDA, const double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, UPLO, N, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsyrfs_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *LDA, const double *AF, const int *LDAF, const int *IPIV, const double *B, const int *LDB,  double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsytrd_ (const char  *UPLO, const int *N,  double *A, const int *LDA,  double *D,  double *E,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, D, E, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsytrf_ (const char  *UPLO, const int *N,  double *A, const int *LDA,  int *IPIV,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsytri_ (const char  *UPLO, const int *N,  double *A, const int *LDA, const int *IPIV,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsytri2_ (const char  *UPLO, const int *N,  double *A, const int *LDA, const int *IPIV,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsytrs_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *LDA, const int *IPIV,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dsytrs2_ (const char  *UPLO, const int *N, const int *NRHS, const double *A, const int *LDA, const int *IPIV,  double *B, const int *LDB,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtbcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const int *KD, const double *AB, const int *LDAB,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, KD, AB, LDAB, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtbrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *KD, const int *NRHS, const double *AB, const int *LDAB, const double *B, const int *LDB, const double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, KD, NRHS, AB, LDAB, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtbtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *KD, const int *NRHS, const double *AB, const int *LDAB,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtgevc_ (const char  *SIDE, const char  *HOWMNY, const bool *SELECT, const int *N, const double *S, const int *LDS, const double *P, const int *LDP,  double *VL, const int *LDVL,  double *VR, const int *LDVR, const int *MM,  int *M,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, HOWMNY, SELECT, N, S, LDS, P, LDP, VL, LDVL, VR, LDVR, MM, M, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtgexc_ (const bool *WANTQ, const bool *WANTZ, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *Q, const int *LDQ,  double *Z, const int *LDZ,  int *IFST,  int *ILST,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( WANTQ, WANTZ, N, A, LDA, B, LDB, Q, LDQ, Z, LDZ, IFST, ILST, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtgsen_ (const int *IJOB, const bool *WANTQ, const bool *WANTZ, const bool *SELECT, const int *N,  double *A, const int *LDA,  double *B, const int *LDB,  double *ALPHAR,  double *ALPHAI,  double *BETA,  double *Q, const int *LDQ,  double *Z, const int *LDZ,  int *M,  double *PL,  double *PR,  double *DIF,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IJOB, WANTQ, WANTZ, SELECT, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, Q, LDQ, Z, LDZ, M, PL, PR, DIF, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtgsja_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *P, const int *N, const int *K, const int *L,  double *A, const int *LDA,  double *B, const int *LDB, const double *TOLA, const double *TOLB,  double *ALPHA,  double *BETA,  double *U, const int *LDU,  double *V, const int *LDV,  double *Q, const int *LDQ,  double *WORK,  int *NCYCLE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, P, N, K, L, A, LDA, B, LDB, TOLA, TOLB, ALPHA, BETA, U, LDU, V, LDV, Q, LDQ, WORK, NCYCLE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtgsna_ (const char  *JOB, const char  *HOWMNY, const bool *SELECT, const int *N, const double *A, const int *LDA, const double *B, const int *LDB, const double *VL, const int *LDVL, const double *VR, const int *LDVR,  double *S,  double *DIF, const int *MM,  int *M,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, HOWMNY, SELECT, N, A, LDA, B, LDB, VL, LDVL, VR, LDVR, S, DIF, MM, M, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtgsyl_ (const char  *TRANS, const int *IJOB, const int *M, const int *N, const double *A, const int *LDA, const double *B, const int *LDB,  double *C, const int *LDC, const double *D, const int *LDD, const double *E, const int *LDE,  double *F, const int *LDF,  double *DIF,  double *SCALE,  double *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, IJOB, M, N, A, LDA, B, LDB, C, LDC, D, LDD, E, LDE, F, LDF, SCALE, DIF, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtpcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const double *AP,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, AP, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtprfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const double *AP, const double *B, const int *LDB, const double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, AP, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtptri_ (const char  *UPLO, const char  *DIAG, const int *N,  double *AP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, AP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtptrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const double *AP,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const double *A, const int *LDA,  double *RCOND,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, A, LDA, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrevc_ (const char  *SIDE, const char  *HOWMNY,  bool *SELECT, const int *N, const double *T, const int *LDT,  double *VL, const int *LDVL,  double *VR, const int *LDVR, const int *MM,  int *M,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, HOWMNY, SELECT, N, T, LDT, VL, LDVL, VR, LDVR, MM, M, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrexc_ (const char  *COMPQ, const int *N,  double *T, const int *LDT,  double *Q, const int *LDQ,  int *IFST,  int *ILST,  double *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPQ, N, T, LDT, Q, LDQ, IFST, ILST, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const double *A, const int *LDA, const double *B, const int *LDB, const double *X, const int *LDX,  double *FERR,  double *BERR,  double *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, LDA, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrsen_ (const char  *JOB, const char  *COMPQ, const bool *SELECT, const int *N,  double *T, const int *LDT,  double *Q, const int *LDQ,  double *WR,  double *WI,  int *M,  double *S,  double *SEP,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, COMPQ, SELECT, N, T, LDT, Q, LDQ, WR, WI, M, S, SEP, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrsna_ (const char  *JOB, const char  *HOWMNY, const bool *SELECT, const int *N, const double *T, const int *LDT, const double *VL, const int *LDVL, const double *VR, const int *LDVR,  double *S,  double *SEP, const int *MM,  int *M,  double *WORK, const int *LDWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, HOWMNY, SELECT, N, T, LDT, VL, LDVL, VR, LDVR, S, SEP, MM, M, WORK, LDWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrsyl_ (const char  *TRANA, const char  *TRANB, const int *ISGN, const int *M, const int *N, const double *A, const int *LDA, const double *B, const int *LDB,  double *C, const int *LDC,  double *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANA, TRANB, ISGN, M, N, A, LDA, B, LDB, C, LDC, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrtri_ (const char  *UPLO, const char  *DIAG, const int *N,  double *A, const int *LDA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, A, LDA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtrtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const double *A, const int *LDA,  double *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtzrqf_ (const int *M, const int *N,  double *A, const int *LDA,  double *TAU,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void dtzrzf_ (const int *M, const int *N,  double *A, const int *LDA,  double *TAU,  double *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


