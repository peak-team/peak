void cgbsv_ (const int *N, const int *KL, const int *KU, const int *NRHS,  void *AB, const int *LDAB,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, KL, KU, NRHS, AB, LDAB, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgees_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const int *N,  void *A, const int *LDA,  int *SDIM,  void *W,  void *VS, const int *LDVS,  void *WORK, const int *LWORK,  float *RWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, N, A, LDA, SDIM, W, VS, LDVS, WORK, LWORK, RWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgeev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  void *A, const int *LDA,  void *W,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, W, VL, LDVL, VR, LDVR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgegs_ (const char  *JOBVSL, const char  *JOBVSR, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VSL, const int *LDVSL,  void *VSR, const int *LDVSR,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, N, A, LDA, B, LDB, ALPHA, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgegv_ (const char  *JOBVL, const char  *JOBVR, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHA, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, LDA, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgelsd_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  float *S, const float *RCOND,  int *RANK,  void *WORK, const int *LWORK,  float *RWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, RWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgelss_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  float *S, const float *RCOND,  int *RANK,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgelsy_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  int *JPVT, const float *RCOND,  int *RANK,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgesdd_ (const char  *JOBZ, const int *M, const int *N,  void *A, const int *LDA,  float *S,  void *U, const int *LDU,  void *VT, const int *LDVT,  void *WORK, const int *LWORK,  float *RWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, RWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgesv_ (const int *N, const int *NRHS,  void *A, const int *LDA,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgesvd_ (const char  *JOBU, const char  *JOBVT, const int *M, const int *N,  void *A, const int *LDA,  float *S,  void *U, const int *LDU,  void *VT, const int *LDVT,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBVT, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgges_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  int *SDIM,  void *ALPHA,  void *BETA,  void *VSL, const int *LDVSL,  void *VSR, const int *LDVSR,  void *WORK, const int *LWORK,  float *RWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, N, A, LDA, B, LDB, SDIM, ALPHA, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, RWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cggev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHA, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cggglm_ (const int *N, const int *M, const int *P,  void *A, const int *LDA,  void *B, const int *LDB,  void *D,  void *X,  void *Y,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, LDA, B, LDB, D, X, Y, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgglse_ (const int *M, const int *N, const int *P,  void *A, const int *LDA,  void *B, const int *LDB,  void *C,  void *D,  void *X,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, P, A, LDA, B, LDB, C, D, X, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cggsvd_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *N, const int *P,  int *K,  int *L,  void *A, const int *LDA,  void *B, const int *LDB,  float *ALPHA,  float *BETA,  void *U, const int *LDU,  void *V, const int *LDV,  void *Q, const int *LDQ,  void *WORK,  float *RWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, N, P, K, L, A, LDA, B, LDB, ALPHA, BETA, U, LDU, V, LDV, Q, LDQ, WORK, RWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chbev_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  void *AB, const int *LDAB,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chbevd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  void *AB, const int *LDAB,  float *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chbgv_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  void *AB, const int *LDAB,  void *BB, const int *LDBB,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chbgvd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  void *AB, const int *LDAB,  void *BB, const int *LDBB,  float *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cheev_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  float *W,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cheevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  float *W,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cheevr_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *LDA, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  int *ISUPPZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chegv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  float *W,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chegvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  float *W,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chesv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  int *IPIV,  void *B, const int *LDB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpev_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  float *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpgv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  void *BP,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpgvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  void *BP,  float *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  float *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *AP,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cpbsv_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS,  void *AB, const int *LDAB,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cposv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cppsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *AP,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cspsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *AP,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cstemr_ (const char  *JOBZ, const char  *RANGE, const int *N,  float *D,  float *E, const float *VL, const float *VU, const int *IL, const int *IU,  int *M,  float *W,  void *Z, const int *LDZ, const int *NZC,  int *ISUPPZ,  bool *TRYRAC,  float *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, M, W, Z, LDZ, NZC, ISUPPZ, TRYRAC, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void csysv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  int *IPIV,  void *B, const int *LDB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgbsvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS,  void *AB, const int *LDAB,  void *AFB, const int *LDAFB,  int *IPIV,  char  *EQUED,  float *R,  float *C,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgbsvxx_ (const char  *FACT, const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS,  float *AB, const int *LDAB,  float *AFB, const int *LDAFB,  int *IPIV,  char  *EQUED,  float *R,  float *C,  float *B, const int *LDB,  float *X, const int *LDX,  float *RCOND,  float *RPVGRW,  float *BERR, const int *N_ERR_BNDS,  float *ERR_BNDS_NORM,  float *ERR_BNDS_COMP, const int *NPARAMS, float *PARAMS,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO);
#include "function_wrapper_body2.c" 
  return;
}


void cgeesx_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const char  *SENSE, const int *N,  void *A, const int *LDA,  int *SDIM,  void *W,  void *VS, const int *LDVS,  float *RCONDE,  float *RCONDV,  void *WORK, const int *LWORK,  float *RWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, SENSE, N, A, LDA, SDIM, W, VS, LDVS, RCONDE, RCONDV, WORK, LWORK, RWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgeevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  void *A, const int *LDA,  void *W,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  int *ILO,  int *IHI,  float *SCALE,  float *ABNRM,  float *RCONDE,  float *RCONDV,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, W, VL, LDVL, VR, LDVR, ILO, IHI, SCALE, ABNRM, RCONDE, RCONDV, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgelsx_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  int *JPVT, const float *RCOND,  int *RANK,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  float *R,  float *C,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cgesvxx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  float *R,  float *C,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *RPVGRW,  float *BERR, const int *N_ERR_BNDS,  float *ERR_BNDS_NORM,  float *ERR_BNDS_COMP, const int *NPARAMS, float  *PARAMS,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO);
#include "function_wrapper_body2.c" 
  return;
}


void cggesx_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const char  *SENSE, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  int *SDIM,  void *ALPHA,  void *BETA,  void *VSL, const int *LDVSL,  void *VSR, const int *LDVSR,  float *RCONDE,  float *RCONDV,  void *WORK, const int *LWORK,  float *RWORK,  int *IWORK, const int *LIWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, SENSE, N, A, LDA, B, LDB, SDIM, ALPHA, BETA, VSL, LDVSL, VSR, LDVSR, RCONDE, RCONDV, WORK, LWORK, RWORK, IWORK, LIWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cggevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  int *ILO,  int *IHI,  float *LSCALE,  float *RSCALE,  float *ABNRM,  float *BBNRM,  float *RCONDE,  float *RCONDV,  void *WORK, const int *LWORK,  float *RWORK,  int *IWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, B, LDB, ALPHA, BETA, VL, LDVL, VR, LDVR, ILO, IHI, LSCALE, RSCALE, ABNRM, BBNRM, RCONDE, RCONDV, WORK, LWORK, RWORK, IWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chbevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KD,  void *AB, const int *LDAB,  void *Q, const int *LDQ, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KD, AB, LDAB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chbgvx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KA, const int *KB,  void *AB, const int *LDAB,  void *BB, const int *LDBB,  void *Q, const int *LDQ, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cheevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *LDA, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  float *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chegvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *LDA,  void *B, const int *LDB, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  float *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, A, LDA, B, LDB, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chesvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chesvxx_ (const char  *FACT, const char UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  float *S,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *RPVGRW,  float *BERR, const int *N_ERR_BNDS,  float *ERR_BNDS_NORM,  float *ERR_BNDS_COMP, const int *NPARAMS, float *PARAMS,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, S, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *AP, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, AP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpgvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *AP,  void *BP, const float *VL, const float *VU, const int *IL, const int *IU, const float *ABSTOL,  int *M,  float *W,  void *Z, const int *LDZ,  void *WORK,  float *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, AP, BP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void chpsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *AP,  void *AFP,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cpbsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *KD, const int *NRHS,  void *AB, const int *LDAB,  void *AFB, const int *LDAFB,  char  *EQUED,  float *S,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, KD, NRHS, AB, LDAB, AFB, LDAFB, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  char  *EQUED,  float *S,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cposvxx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  char  *EQUED,  float *S,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *RPVGRW,  float *BERR, const int *N_ERR_BNDS,  float *ERR_BNDS_NORM,  float *ERR_BNDS_COMP, const int *NPARAMS, float *PARAMS,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, EQUED, S, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cppsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *AP,  void *AFP,  char  *EQUED,  float *S,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void cspsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *AP,  void *AFP,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void csysvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *FERR,  float *BERR,  void *WORK, const int *LWORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void csysvxx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  float *S,  void *B, const int *LDB,  void *X, const int *LDX,  float *RCOND,  float *RPVGRW,  float *BERR, const int *N_ERR_BNDS,  float *ERR_BNDS_NORM,  float *ERR_BNDS_COMP, const int *NPARAMS, float *PARAMS,  void *WORK,  float *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, S, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


