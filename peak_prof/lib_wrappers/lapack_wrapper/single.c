void sgesv_ (const int *N, const int *NRHS,  float *A, const int *LDA,  int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbsv_ (const int *N, const int *KL, const int *KU, const int *NRHS,  float *AB, const int *LDAB,  int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, KL, KU, NRHS, AB, LDAB, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgtsv_ (const int *N, const int *NRHS,  float *DL,  float *D,  float *DU,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, DL, D, DU, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sposv_ (const char  *UPLO, const int *N, const int *NRHS,  float *A, const int *LDA,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sppsv_ (const char  *UPLO, const int *N, const int *NRHS,  float *AP,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbsv_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS,  float *AB, const int *LDAB,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sptsv_ (const int *N, const int *NRHS,  float *D,  float *E,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssysv_ (const char  *UPLO, const int *N, const int *NRHS,  float *A, const int *LDA,  int *IPIV,  float *B, const int *LDB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspsv_ (const char  *UPLO, const int *N, const int *NRHS,  float *AP,  int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  float *A, const int *LDA,  float *B, const int *LDB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, LDA, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgelsd_ (const int *M, const int *N, const int *NRHS, const float *A, const int *LDA,  float *B, const int *LDB,  float *S, const float *RCOND,  int *RANK,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgglse_ (const int *M, const int *N, const int *P,  float *A, const int *LDA,  float *B, const int *LDB,  float *C,  float *D,  float *X,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, P, A, LDA, B, LDB, C, D, X, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggglm_ (const int *N, const int *M, const int *P,  float *A, const int *LDA,  float *B, const int *LDB,  float *D,  float *X,  float *Y,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, LDA, B, LDB, D, X, Y, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssyev_ (const char  *JOBZ, const char  *UPLO, const int *N,  float *A, const int *LDA,  float *W,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssyevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  float *A, const int *LDA,  float *W,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspev_ (const char  *JOBZ, const char  *UPLO, const int *N,  float *AP,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  float *AP,  float *W,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbev_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  float *AB, const int *LDAB,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbevd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  float *AB, const int *LDAB,  float *W,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstev_ (const char  *JOBZ, const int *N,  float *D,  float *E,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, N, D, E, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstevd_ (const char  *JOBZ, const int *N,  float *D,  float *E,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, N, D, E, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgees_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const int *N,  float *A, const int *LDA,  int *SDIM,  float *WR,  float *WI,  float *VS, const int *LDVS,  float *WORK, const int *LWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, N, A, LDA, SDIM, WR, WI, VS, LDVS, WORK, LWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  float *A, const int *LDA,  float *WR,  float *WI,  float *VL, const int *LDVL,  float *VR, const int *LDVR,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, WR, WI, VL, LDVL, VR, LDVR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgesvd_ (const char  *JOBU, const char  *JOBVT, const int *M, const int *N,  float *A, const int *LDA,  float *S,  float *U, const int *LDU,  float *VT, const int *LDVT,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBVT, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgesdd_ (const char  *JOBZ, const int *M, const int *N,  float *A, const int *LDA,  float *S,  float *U, const int *LDU,  float *VT, const int *LDVT,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssygv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *W,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssygvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *W,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspgv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  float *AP,  float *BP,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspgvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  float *AP,  float *BP,  float *W,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbgv_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  float *AB, const int *LDAB,  float *BB, const int *LDBB,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbgvd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  float *AB, const int *LDAB,  float *BB, const int *LDBB,  float *W,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgegs_ (const char  *JOBVSL, const char  *JOBVSR, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *VSL, const int *LDVSL,  float *VSR, const int *LDVSR,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgges_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  int *SDIM,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *VSL, const int *LDVSL,  float *VSR, const int *LDVSR,  float *WORK, const int *LWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, N, A, LDA, B, LDB, SDIM, ALPHAR, ALPHAI, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgegv_ (const char  *JOBVL, const char  *JOBVR, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *VL, const int *LDVL,  float *VR, const int *LDVR,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *VL, const int *LDVL,  float *VR, const int *LDVR,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggsvd_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *N, const int *P,  int *K,  int *L,  float *A, const int *LDA,  float *B, const int *LDB,  float *ALPHA,  float *BETA,  float *U, const int *LDU,  float *V, const int *LDV,  float *Q, const int *LDQ,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, N, P, K, L, A, LDA, B, LDB, ALPHA, BETA, U, LDU, V, LDV, Q, LDQ, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  float *A, const int *LDA,  float *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  float *R,  float *C,  float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbsvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS,  float *AB, const int *LDAB,  float *AFB, const int *LDAFB,  int *IPIV,  char  *EQUED,  float *R,  float *C,  float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgtsvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS, const float *DL, const float *D, const float *DU,  float *DLF,  float *DF,  float *DUF,  float *DU2,  int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, DL, D, DU, DLF, DF, DUF, DU2, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  float *A, const int *LDA,  float *AF, const int *LDAF,  char  *EQUED,  float *S,  float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sppsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  float *AP,  float *AFP,  char  *EQUED,  float *S,  float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *KD, const int *NRHS,  float *AB, const int *LDAB,  float *AFB, const int *LDAFB,  char  *EQUED,  float *S,  float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, KD, NRHS, AB, LDAB, AFB, LDAFB, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sptsvx_ (const char  *FACT, const int *N, const int *NRHS, const float *D, const float *E,  float *DF,  float *EF, const float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, N, NRHS, D, E, DF, EF, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssysvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *LDA,  float *AF, const int *LDAF,  int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const float *AP,  float *AFP,  int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgelsx_ (const int *M, const int *N, const int *NRHS,  float *A, const int *LDA,  float *B, const int *LDB,  int *JPVT, const float *RCOND,  int *RANK,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgelsy_ (const int *M, const int *N, const int *NRHS,  float *A, const int *LDA,  float *B, const int *LDB,  int *JPVT, const float *RCOND,  int *RANK,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgelss_ (const int *M, const int *N, const int *NRHS,  float *A, const int *LDA,  float *B, const int *LDB,  float *S, const float *RCOND,  int *RANK,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssyevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  float *A, const int *LDA, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssyevr_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  float *A, const int *LDA, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  int *ISUPPZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssygvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  float *A, const int *LDA,  float *B, const int *LDB, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, A, LDA, B, LDB, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  float *AP, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, AP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspgvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  float *AP,  float *BP, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, AP, BP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KD,  float *AB, const int *LDAB,  float *Q, const int *LDQ, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KD, AB, LDAB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbgvx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KA, const int *KB,  float *AB, const int *LDAB,  float *BB, const int *LDBB,  float *Q, const int *LDQ, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstevx_ (const char  *JOBZ, const char  *RANGE, const int *N,  float *D,  float *E, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  float *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstevr_ (const char  *JOBZ, const char  *RANGE, const int *N,  float *D,  float *E, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  int *ISUPPZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeesx_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const char  *SENSE, const int *N,  float *A, const int *LDA,  int *SDIM,  float *WR,  float *WI,  float *VS, const int *LDVS,  float *RCONDE,  float *RCONDV,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, SENSE, N, A, LDA, SDIM, WR, WI, VS, LDVS, RCONDE, RCONDV, WORK, LWORK, IWORK, LIWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggesx_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const char  *SENSE, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  int *SDIM,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *VSL, const int *LDVSL,  float *VSR, const int *LDVSR,  float *RCONDE,  float *RCONDV,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, SENSE, N, A, LDA, B, LDB, SDIM, ALPHAR, ALPHAI, BETA, VSL, LDVSL, VSR, LDVSR, RCONDE, RCONDV, WORK, LWORK, IWORK, LIWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  float *A, const int *LDA,  float *WR,  float *WI,  float *VL, const int *LDVL,  float *VR, const int *LDVR,  int *ILO,  int *IHI,  float *SCALE,  float *ABNRM,  float *RCONDE,  float *RCONDV,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, WR, WI, VL, LDVL, VR, LDVR, ILO, IHI, SCALE, ABNRM, RCONDE, RCONDV, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *VL, const int *LDVL,  float *VR, const int *LDVR,  int *ILO,  int *IHI,  float *LSCALE,  float *RSCALE,  float *ABNRM,  float *BBNRM,  float *RCONDE,  float *RCONDV,  float *WORK, const int *LWORK,  int *IWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, VL, LDVL, VR, LDVR, ILO, IHI, LSCALE, RSCALE, ABNRM, BBNRM, RCONDE, RCONDV, WORK, LWORK, IWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sbdsdc_ (const char  *UPLO, const char  *COMPQ, const int *N,  float *D,  float *E,  float *U, const int *LDU,  float *VT, const int *LDVT,  float *Q,  int *IQ,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, COMPQ, N, D, E, U, LDU, VT, LDVT, Q, IQ, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sbdsqr_ (const char  *UPLO, const int *N, const int *NCVT, const int *NRU, const int *NCC,  float *D,  float *E,  float *VT, const int *LDVT,  float *U, const int *LDU,  float *C, const int *LDC,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NCVT, NRU, NCC, D, E, VT, LDVT, U, LDU, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sdisna_ (const char  *JOB, const int *M, const int *N, const float *D,  float *SEP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, M, N, D, SEP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbbrd_ (const char  *VECT, const int *M, const int *N, const int *NCC, const int *KL, const int *KU,  float *AB, const int *LDAB,  float *D,  float *E,  float *Q, const int *LDQ,  float *PT, const int *LDPT,  float *C, const int *LDC,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, M, N, NCC, KL, KU, AB, LDAB, D, E, Q, LDQ, PT, LDPT, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbcon_ (const char  *NORM, const int *N, const int *KL, const int *KU, const float *AB, const int *LDAB, const int *IPIV, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, KL, KU, AB, LDAB, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbequ_ (const int *M, const int *N, const int *KL, const int *KU, const float *AB, const int *LDAB,  float *R,  float *C,  float *ROWCND,  float *COLCND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, KL, KU, AB, LDAB, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbrfs_ (const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS, const float *AB, const int *LDAB, const float *AFB, const int *LDAFB, const int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbtrf_ (const int *M, const int *N, const int *KL, const int *KU,  float *AB, const int *LDAB,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, KL, KU, AB, LDAB, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgbtrs_ (const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS, const float *AB, const int *LDAB, const int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, KL, KU, NRHS, AB, LDAB, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgebak_ (const char  *JOB, const char  *SIDE, const int *N, const int *ILO, const int *IHI, const float *SCALE, const int *M,  float *V, const int *LDV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, SIDE, N, ILO, IHI, SCALE, M, V, LDV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgebal_ (const char  *JOB, const int *N,  float *A, const int *LDA,  int *ILO,  int *IHI,  float *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, N, A, LDA, ILO, IHI, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgebrd_ (const int *M, const int *N,  float *A, const int *LDA,  float *D,  float *E,  float *TAUQ,  float *TAUP,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, D, E, TAUQ, TAUP, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgecon_ (const char  *NORM, const int *N, const float *A, const int *LDA, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, A, LDA, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeequ_ (const int *M, const int *N, const float *A, const int *LDA,  float *R,  float *C,  float *ROWCND,  float *COLCND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, R, C, ROWCND, COLCND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgehrd_ (const int *N, const int *ILO, const int *IHI,  float *A, const int *LDA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgelqf_ (const int *M, const int *N,  float *A, const int *LDA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeqlf_ (const int *M, const int *N,  float *A, const int *LDA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeqp3_ (const int *M, const int *N,  float *A, const int *LDA,  int *JPVT,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, JPVT, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeqpf_ (const int *M, const int *N,  float *A, const int *LDA,  int *JPVT,  float *TAU,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, JPVT, TAU, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgeqrf_ (const int *M, const int *N,  float *A, const int *LDA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgerfs_ (const char  *TRANS, const int *N, const int *NRHS, const float *A, const int *LDA, const float *AF, const int *LDAF, const int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgerqf_ (const int *M, const int *N,  float *A, const int *LDA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgetrf_ (const int *M, const int *N,  float *A, const int *LDA,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgetri_ (const int *N,  float *A, const int *LDA, const int *IPIV,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, LDA, IPIV, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgetrs_ (const char  *TRANS, const int *N, const int *NRHS, const float *A, const int *LDA, const int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggbak_ (const char  *JOB, const char  *SIDE, const int *N, const int *ILO, const int *IHI, const float *LSCALE, const float *RSCALE, const int *M,  float *V, const int *LDV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, SIDE, N, ILO, IHI, LSCALE, RSCALE, M, V, LDV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggbal_ (const char  *JOB, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  int *ILO,  int *IHI,  float *LSCALE,  float *RSCALE,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, N, A, LDA, B, LDB, ILO, IHI, LSCALE, RSCALE, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgghrd_ (const char  *COMPQ, const char  *COMPZ, const int *N, const int *ILO, const int *IHI,  float *A, const int *LDA,  float *B, const int *LDB,  float *Q, const int *LDQ,  float *Z, const int *LDZ,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPQ, COMPZ, N, ILO, IHI, A, LDA, B, LDB, Q, LDQ, Z, LDZ, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggqrf_ (const int *N, const int *M, const int *P,  float *A, const int *LDA,  float *TAUA,  float *B, const int *LDB,  float *TAUB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, LDA, TAUA, B, LDB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggrqf_ (const int *M, const int *P, const int *N,  float *A, const int *LDA,  float *TAUA,  float *B, const int *LDB,  float *TAUB,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, P, N, A, LDA, TAUA, B, LDB, TAUB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sggsvp_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *P, const int *N,  float *A, const int *LDA,  float *B, const int *LDB, const float *TOLA, const float *TOLB,  int *K,  int *L,  float *U, const int *LDU,  float *V, const int *LDV,  float *Q, const int *LDQ,  int *IWORK,  float *TAU,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, P, N, A, LDA, B, LDB, TOLA, TOLB, K, L, U, LDU, V, LDV, Q, LDQ, IWORK, TAU, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgtcon_ (const char  *NORM, const int *N, const float *DL, const float *D, const float *DU, const float *DU2, const int *IPIV, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, N, DL, D, DU, DU2, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgtrfs_ (const char  *TRANS, const int *N, const int *NRHS, const float *DL, const float *D, const float *DU, const float *DLF, const float *DF, const float *DUF, const float *DU2, const int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, DLF, DF, DUF, DU2, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgttrf_ (const int *N,  float *DL,  float *D,  float *DU,  float *DU2,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, DL, D, DU, DU2, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sgttrs_ (const char  *TRANS, const int *N, const int *NRHS, const float *DL, const float *D, const float *DU, const float *DU2, const int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, N, NRHS, DL, D, DU, DU2, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void shgeqz_ (const char  *JOB, const char  *COMPQ, const char  *COMPZ, const int *N, const int *ILO, const int *IHI,  float *H, const int *LDH,  float *T, const int *LDT,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *Q, const int *LDQ,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, COMPQ, COMPZ, N, ILO, IHI, H, LDH, T, LDT, ALPHAR, ALPHAI, BETA, Q, LDQ, Z, LDZ, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void shsein_ (const char  *SIDE, const char  *EIGSRC, const char  *INITV,  bool *SELECT, const int *N, const float *H, const int *LDH,  float *WR, const float *WI,  float *VL, const int *LDVL,  float *VR, const int *LDVR, const int *MM,  int *M,  float *WORK,  int *IFAILL,  int *IFAILR,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, EIGSRC, INITV, SELECT, N, H, LDH, WR, WI, VL, LDVL, VR, LDVR, MM, M, WORK, IFAILL, IFAILR, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void shseqr_ (const char  *JOB, const char  *COMPZ, const int *N, const int *ILO, const int *IHI,  float *H, const int *LDH,  float *WR,  float *WI,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, COMPZ, N, ILO, IHI, H, LDH, WR, WI, Z, LDZ, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sopgtr_ (const char  *UPLO, const int *N, const float *AP, const float *TAU,  float *Q, const int *LDQ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, TAU, Q, LDQ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sopmtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const float *AP, const float *TAU,  float *C, const int *LDC,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, AP, TAU, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorcsd_ (const char *JOBU1, const char *JOBU2, const char *JOBV1T, const char *JOBV2T, const char *TRANS, const char *SIGNS, const int *M, const int *P, const int *Q,  float *X, const int *LDX,  float *THETA,  float *U1, const int *LDU1,  float *U2, const int *LDU2,  float *V1T, const int *LDV1T,  float *V2T, const int *LDV2T,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU1, JOBU2, JOBV1T, JOBV2T, TRANS, SIGNS, M, P, Q, X, LDX, THETA, U1, LDU1, U2, LDU2, V1T, LDV1T, V2T, LDV2T, WORK, LWORK, IWORK, INFO);
#include "function_wrapper_body2.c" 
  return;
}


void sorgbr_ (const char  *VECT, const int *M, const int *N, const int *K,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorghr_ (const int *N, const int *ILO, const int *IHI,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, ILO, IHI, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorglq_ (const int *M, const int *N, const int *K,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorgql_ (const int *M, const int *N, const int *K,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorgqr_ (const int *M, const int *N, const int *K,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorgrq_ (const int *M, const int *N, const int *K,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, K, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sorgtr_ (const char  *UPLO, const int *N,  float *A, const int *LDA, const float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormbr_ (const char  *VECT, const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormhr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *ILO, const int *IHI, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, ILO, IHI, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormlq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormql_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormqr_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormr3_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, LDA, TAU, C, LDC, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormrq_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormrz_ (const char  *SIDE, const char  *TRANS, const int *M, const int *N, const int *K, const int *L, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, TRANS, M, N, K, L, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sormtr_ (const char  *SIDE, const char  *UPLO, const char  *TRANS, const int *M, const int *N, const float *A, const int *LDA, const float *TAU,  float *C, const int *LDC,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, UPLO, TRANS, M, N, A, LDA, TAU, C, LDC, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbcon_ (const char  *UPLO, const int *N, const int *KD, const float *AB, const int *LDAB, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbequ_ (const char  *UPLO, const int *N, const int *KD, const float *AB, const int *LDAB,  float *S,  float *SCOND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, S, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbrfs_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS, const float *AB, const int *LDAB, const float *AFB, const int *LDAFB, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, AFB, LDAFB, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbstf_ (const char  *UPLO, const int *N, const int *KD,  float *AB, const int *LDAB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbtrf_ (const char  *UPLO, const int *N, const int *KD,  float *AB, const int *LDAB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, AB, LDAB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spbtrs_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS, const float *AB, const int *LDAB,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spocon_ (const char  *UPLO, const int *N, const float *A, const int *LDA, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spoequ_ (const int *N, const float *A, const int *LDA,  float *S,  float *SCOND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, A, LDA, S, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sporfs_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *LDA, const float *AF, const int *LDAF, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, AF, LDAF, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spotrf_ (const char  *UPLO, const int *N,  float *A, const int *LDA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spotri_ (const char  *UPLO, const int *N,  float *A, const int *LDA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spotrs_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *LDA,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sppcon_ (const char  *UPLO, const int *N, const float *AP, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sppequ_ (const char  *UPLO, const int *N, const float *AP,  float *S,  float *SCOND,  float *AMAX,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, S, SCOND, AMAX, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spprfs_ (const char  *UPLO, const int *N, const int *NRHS, const float *AP, const float *AFP, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, AFP, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spptrf_ (const char  *UPLO, const int *N,  float *AP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spptri_ (const char  *UPLO, const int *N,  float *AP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spptrs_ (const char  *UPLO, const int *N, const int *NRHS, const float *AP,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sptcon_ (const int *N, const float *D, const float *E, const float *ANORM,  float *RCOND,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, ANORM, RCOND, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spteqr_ (const char  *COMPZ, const int *N,  float *D,  float *E,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sptrfs_ (const int *N, const int *NRHS, const float *D, const float *E, const float *DF, const float *EF, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, DF, EF, B, LDB, X, LDX, FERR, BERR, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spttrf_ (const int *N,  float *D,  float *E,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void spttrs_ (const int *N, const int *NRHS, const float *D, const float *E,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, D, E, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbgst_ (const char  *VECT, const char  *UPLO, const int *N, const int *KA, const int *KB,  float *AB, const int *LDAB, const float *BB, const int *LDBB,  float *X, const int *LDX,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, X, LDX, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssbtrd_ (const char  *VECT, const char  *UPLO, const int *N, const int *KD,  float *AB, const int *LDAB,  float *D,  float *E,  float *Q, const int *LDQ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( VECT, UPLO, N, KD, AB, LDAB, D, E, Q, LDQ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspcon_ (const char  *UPLO, const int *N, const float *AP, const int *IPIV, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sspgst_ (const int *ITYPE, const char  *UPLO, const int *N,  float *AP, const float *BP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, UPLO, N, AP, BP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssprfs_ (const char  *UPLO, const int *N, const int *NRHS, const float *AP, const float *AFP, const int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssptrd_ (const char  *UPLO, const int *N,  float *AP,  float *D,  float *E,  float *TAU,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, D, E, TAU, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssptrf_ (const char  *UPLO, const int *N,  float *AP,  int *IPIV,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, IPIV, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssptri_ (const char  *UPLO, const int *N,  float *AP, const int *IPIV,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, AP, IPIV, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssptrs_ (const char  *UPLO, const int *N, const int *NRHS, const float *AP, const int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstebz_ (const char  *RANGE, const char  *ORDER, const int *N, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL, const float *D, const float *E,  int *M,  int *NSPLIT,  float *W,  int *IBLOCK,  int *ISPLIT,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( RANGE, ORDER, N, VL, VU, IL, IU, ABSTOL, D, E, M, NSPLIT, W, IBLOCK, ISPLIT, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstedc_ (const char  *COMPZ, const int *N,  float *D,  float *E,  float *Z, const int *LDZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Z, LDZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstegr_ (const char  *JOBZ, const char  *RANGE, const int *N,  float *D,  float *E, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  float *Z, const int *LDZ,  int *ISUPPZ,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void sstein_ (const int *N, const float *D, const float *E, const int *M, const float *W, const int *IBLOCK, const int *ISPLIT,  float *Z, const int *LDZ,  float *WORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, M, W, IBLOCK, ISPLIT, Z, LDZ, WORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssteqr_ (const char  *COMPZ, const int *N,  float *D,  float *E,  float *Z, const int *LDZ,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPZ, N, D, E, Z, LDZ, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssterf_ (const int *N,  float *D,  float *E,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, D, E, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssycon_ (const char  *UPLO, const int *N, const float *A, const int *LDA, const int *IPIV, const float *ANORM,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, ANORM, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssygst_ (const int *ITYPE, const char  *UPLO, const int *N,  float *A, const int *LDA, const float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, UPLO, N, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssyrfs_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *LDA, const float *AF, const int *LDAF, const int *IPIV, const float *B, const int *LDB,  float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssytrd_ (const char  *UPLO, const int *N,  float *A, const int *LDA,  float *D,  float *E,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, D, E, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssytrf_ (const char  *UPLO, const int *N,  float *A, const int *LDA,  int *IPIV,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssytri2_ (const char  *UPLO, const int *N,  float *A, const int *LDA, const int *IPIV,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssytri_ (const char  *UPLO, const int *N,  float *A, const int *LDA, const int *IPIV,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, A, LDA, IPIV, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssytrs2_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *LDA, const int *IPIV,  float *B, const int *LDB,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void ssytrs_ (const char  *UPLO, const int *N, const int *NRHS, const float *A, const int *LDA, const int *IPIV,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stbcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const int *KD, const float *AB, const int *LDAB,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, KD, AB, LDAB, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stbrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *KD, const int *NRHS, const float *AB, const int *LDAB, const float *B, const int *LDB, const float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, KD, NRHS, AB, LDAB, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stbtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *KD, const int *NRHS, const float *AB, const int *LDAB,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stgevc_ (const char  *SIDE, const char  *HOWMNY, const bool *SELECT, const int *N, const float *S, const int *LDS, const float *P, const int *LDP,  float *VL, const int *LDVL,  float *VR, const int *LDVR, const int *MM,  int *M,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, HOWMNY, SELECT, N, S, LDS, P, LDP, VL, LDVL, VR, LDVR, MM, M, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stgexc_ (const bool *WANTQ, const bool *WANTZ, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *Q, const int *LDQ,  float *Z, const int *LDZ,  int *IFST,  int *ILST,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( WANTQ, WANTZ, N, A, LDA, B, LDB, Q, LDQ, Z, LDZ, IFST, ILST, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stgsen_ (const int *IJOB, const bool *WANTQ, const bool *WANTZ, const bool *SELECT, const int *N,  float *A, const int *LDA,  float *B, const int *LDB,  float *ALPHAR,  float *ALPHAI,  float *BETA,  float *Q, const int *LDQ,  float *Z, const int *LDZ,  int *M,  float *PL,  float *PR,  float *DIF,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( IJOB, WANTQ, WANTZ, SELECT, N, A, LDA, B, LDB, ALPHAR, ALPHAI, BETA, Q, LDQ, Z, LDZ, M, PL, PR, DIF, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stgsja_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *P, const int *N, const int *K, const int *L,  float *A, const int *LDA,  float *B, const int *LDB, const float *TOLA, const float *TOLB,  float *ALPHA,  float *BETA,  float *U, const int *LDU,  float *V, const int *LDV,  float *Q, const int *LDQ,  float *WORK,  int *NCYCLE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, P, N, K, L, A, LDA, B, LDB, TOLA, TOLB, ALPHA, BETA, U, LDU, V, LDV, Q, LDQ, WORK, NCYCLE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stgsna_ (const char  *JOB, const char  *HOWMNY, const bool *SELECT, const int *N, const float *A, const int *LDA, const float *B, const int *LDB, const float *VL, const int *LDVL, const float *VR, const int *LDVR,  float *S,  float *DIF, const int *MM,  int *M,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, HOWMNY, SELECT, N, A, LDA, B, LDB, VL, LDVL, VR, LDVR, S, DIF, MM, M, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stgsyl_ (const char  *TRANS, const int *IJOB, const int *M, const int *N, const float *A, const int *LDA, const float *B, const int *LDB,  float *C, const int *LDC, const float *D, const int *LDD, const float *E, const int *LDE,  float *F, const int *LDF,  float *DIF,  float *SCALE,  float *WORK, const int *LWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, IJOB, M, N, A, LDA, B, LDB, C, LDC, D, LDD, E, LDE, F, LDF, SCALE, DIF, WORK, LWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stpcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const float *AP,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, AP, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stprfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const float *AP, const float *B, const int *LDB, const float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, AP, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stptri_ (const char  *UPLO, const char  *DIAG, const int *N,  float *AP,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, AP, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stptrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const float *AP,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strcon_ (const char  *NORM, const char  *UPLO, const char  *DIAG, const int *N, const float *A, const int *LDA,  float *RCOND,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( NORM, UPLO, DIAG, N, A, LDA, RCOND, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strevc_ (const char  *SIDE, const char  *HOWMNY,  bool *SELECT, const int *N, const float *T, const int *LDT,  float *VL, const int *LDVL,  float *VR, const int *LDVR, const int *MM,  int *M,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( SIDE, HOWMNY, SELECT, N, T, LDT, VL, LDVL, VR, LDVR, MM, M, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strexc_ (const char  *COMPQ, const int *N,  float *T, const int *LDT,  float *Q, const int *LDQ,  int *IFST,  int *ILST,  float *WORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( COMPQ, N, T, LDT, Q, LDQ, IFST, ILST, WORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strrfs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const float *A, const int *LDA, const float *B, const int *LDB, const float *X, const int *LDX,  float *FERR,  float *BERR,  float *WORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, LDA, B, LDB, X, LDX, FERR, BERR, WORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strsen_ (const char  *JOB, const char  *COMPQ, const bool *SELECT, const int *N,  float *T, const int *LDT,  float *Q, const int *LDQ,  float *WR,  float *WI,  int *M,  float *S,  float *SEP,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, COMPQ, SELECT, N, T, LDT, Q, LDQ, WR, WI, M, S, SEP, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strsna_ (const char  *JOB, const char  *HOWMNY, const bool *SELECT, const int *N, const float *T, const int *LDT, const float *VL, const int *LDVL, const float *VR, const int *LDVR,  float *S,  float *SEP, const int *MM,  int *M,  float *WORK, const int *LDWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOB, HOWMNY, SELECT, N, T, LDT, VL, LDVL, VR, LDVR, S, SEP, MM, M, WORK, LDWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strsyl_ (const char  *TRANA, const char  *TRANB, const int *ISGN, const int *M, const int *N, const float *A, const int *LDA, const float *B, const int *LDB,  float *C, const int *LDC,  float *SCALE,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANA, TRANB, ISGN, M, N, A, LDA, B, LDB, C, LDC, SCALE, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strtri_ (const char  *UPLO, const char  *DIAG, const int *N,  float *A, const int *LDA,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, DIAG, N, A, LDA, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void strtrs_ (const char  *UPLO, const char  *TRANS, const char  *DIAG, const int *N, const int *NRHS, const float *A, const int *LDA,  float *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, TRANS, DIAG, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stzrqf_ (const int *M, const int *N,  float *A, const int *LDA,  float *TAU,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void stzrzf_ (const int *M, const int *N,  float *A, const int *LDA,  float *TAU,  float *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, A, LDA, TAU, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


