void zgbsv_ (const int *N, const int *KL, const int *KU, const int *NRHS,  void *AB, const int *LDAB,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, KL, KU, NRHS, AB, LDAB, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgees_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const int *N,  void *A, const int *LDA,  int *SDIM,  void *W,  void *VS, const int *LDVS,  void *WORK, const int *LWORK,  double *RWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, N, A, LDA, SDIM, W, VS, LDVS, WORK, LWORK, RWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgeev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  void *A, const int *LDA,  void *W,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, W, VL, LDVL, VR, LDVR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgegs_ (const char  *JOBVSL, const char  *JOBVSR, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VSL, const int *LDVSL,  void *VSR, const int *LDVSR,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, N, A, LDA, B, LDB, ALPHA, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgegv_ (const char  *JOBVL, const char  *JOBVR, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHA, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgels_ (const char  *TRANS, const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( TRANS, M, N, NRHS, A, LDA, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgelsd_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  double *S, const double *RCOND,  int *RANK,  void *WORK, const int *LWORK,  double *RWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, RWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgelss_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  double *S, const double *RCOND,  int *RANK,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, S, RCOND, RANK, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgelsy_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  int *JPVT, const double *RCOND,  int *RANK,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgesdd_ (const char  *JOBZ, const int *M, const int *N,  void *A, const int *LDA,  double *S,  void *U, const int *LDU,  void *VT, const int *LDVT,  void *WORK, const int *LWORK,  double *RWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, RWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgesv_ (const int *N, const int *NRHS,  void *A, const int *LDA,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, NRHS, A, LDA, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgesvd_ (const char  *JOBU, const char  *JOBVT, const int *M, const int *N,  void *A, const int *LDA,  double *S,  void *U, const int *LDU,  void *VT, const int *LDVT,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBVT, M, N, A, LDA, S, U, LDU, VT, LDVT, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgges_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  int *SDIM,  void *ALPHA,  void *BETA,  void *VSL, const int *LDVSL,  void *VSR, const int *LDVSR,  void *WORK, const int *LWORK,  double *RWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, N, A, LDA, B, LDB, SDIM, ALPHA, BETA, VSL, LDVSL, VSR, LDVSR, WORK, LWORK, RWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zggev_ (const char  *JOBVL, const char  *JOBVR, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVL, JOBVR, N, A, LDA, B, LDB, ALPHA, BETA, VL, LDVL, VR, LDVR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zggglm_ (const int *N, const int *M, const int *P,  void *A, const int *LDA,  void *B, const int *LDB,  void *D,  void *X,  void *Y,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( N, M, P, A, LDA, B, LDB, D, X, Y, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgglse_ (const int *M, const int *N, const int *P,  void *A, const int *LDA,  void *B, const int *LDB,  void *C,  void *D,  void *X,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, P, A, LDA, B, LDB, C, D, X, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zggsvd_ (const char  *JOBU, const char  *JOBV, const char  *JOBQ, const int *M, const int *N, const int *P,  int *K,  int *L,  void *A, const int *LDA,  void *B, const int *LDB,  double *ALPHA,  double *BETA,  void *U, const int *LDU,  void *V, const int *LDV,  void *Q, const int *LDQ,  void *WORK,  double *RWORK,  int *IWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBU, JOBV, JOBQ, M, N, P, K, L, A, LDA, B, LDB, ALPHA, BETA, U, LDU, V, LDV, Q, LDQ, WORK, RWORK, IWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhbev_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  void *AB, const int *LDAB,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhbevd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KD,  void *AB, const int *LDAB,  double *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KD, AB, LDAB, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhbgv_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  void *AB, const int *LDAB,  void *BB, const int *LDBB,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhbgvd_ (const char  *JOBZ, const char  *UPLO, const int *N, const int *KA, const int *KB,  void *AB, const int *LDAB,  void *BB, const int *LDBB,  double *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zheev_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  double *W,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zheevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  double *W,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, A, LDA, W, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zheevr_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *LDA, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  int *ISUPPZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, ISUPPZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhegv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  double *W,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhegvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  double *W,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, A, LDA, B, LDB, W, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhesv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  int *IPIV,  void *B, const int *LDB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpev_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpevd_ (const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  double *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, UPLO, N, AP, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpgv_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  void *BP,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpgvd_ (const int *ITYPE, const char  *JOBZ, const char  *UPLO, const int *N,  void *AP,  void *BP,  double *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  double *RWORK, const int *LRWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, UPLO, N, AP, BP, W, Z, LDZ, WORK, LWORK, RWORK, LRWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *AP,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zpbsv_ (const char  *UPLO, const int *N, const int *KD, const int *NRHS,  void *AB, const int *LDAB,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, KD, NRHS, AB, LDAB, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zposv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zppsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *AP,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zspsv_ (const char  *UPLO, const int *N, const int *NRHS,  void *AP,  int *IPIV,  void *B, const int *LDB,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, AP, IPIV, B, LDB, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zstemr_ (const char  *JOBZ, const char  *RANGE, const int *N,  double *D,  double *E, const double *VL, const double *VU, const int *IL, const int *IU,  int *M,  double *W,  void *Z, const int *LDZ, const int *NZC,  int *ISUPPZ,  bool *TRYRAC,  double *WORK, const int *LWORK,  int *IWORK, const int *LIWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, N, D, E, VL, VU, IL, IU, M, W, Z, LDZ, NZC, ISUPPZ, TRYRAC, WORK, LWORK, IWORK, LIWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zsysv_ (const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  int *IPIV,  void *B, const int *LDB,  void *WORK, const int *LWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( UPLO, N, NRHS, A, LDA, IPIV, B, LDB, WORK, LWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgbsvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS,  void *AB, const int *LDAB,  void *AFB, const int *LDAFB,  int *IPIV,  char  *EQUED,  double *R,  double *C,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgbsvxx_ (const char  *FACT, const char  *TRANS, const int *N, const int *KL, const int *KU, const int *NRHS,  double *AB, const int *LDAB,  double *AFB, const int *LDAFB,  int *IPIV,  char  *EQUED,  double *R,  double *C,  double *B, const int *LDB,  double *X, const int *LDX,  double *RCOND,  double *RPVGRW,  double *BERR, const int *N_ERR_BNDS,  double *ERR_BNDS_NORM,  double *ERR_BNDS_COMP, const int *NPARAMS, double *PARAMS,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, KL, KU, NRHS, AB, LDAB, AFB, LDAFB, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO);
#include "function_wrapper_body2.c" 
  return;
}


void zgeesx_ (const char  *JOBVS, const char  *SORT,  bool *SELECT, const char  *SENSE, const int *N,  void *A, const int *LDA,  int *SDIM,  void *W,  void *VS, const int *LDVS,  double *RCONDE,  double *RCONDV,  void *WORK, const int *LWORK,  double *RWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVS, SORT, SELECT, SENSE, N, A, LDA, SDIM, W, VS, LDVS, RCONDE, RCONDV, WORK, LWORK, RWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgeevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  void *A, const int *LDA,  void *W,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  int *ILO,  int *IHI,  double *SCALE,  double *ABNRM,  double *RCONDE,  double *RCONDV,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, W, VL, LDVL, VR, LDVR, ILO, IHI, SCALE, ABNRM, RCONDE, RCONDV, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgelsx_ (const int *M, const int *N, const int *NRHS,  void *A, const int *LDA,  void *B, const int *LDB,  int *JPVT, const double *RCOND,  int *RANK,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( M, N, NRHS, A, LDA, B, LDB, JPVT, RCOND, RANK, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgesvx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  double *R,  double *C,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zgesvxx_ (const char  *FACT, const char  *TRANS, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  double *R,  double *C,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *RPVGRW,  double *BERR, const int *N_ERR_BNDS,  double *ERR_BNDS_NORM,  double *ERR_BNDS_COMP, const int *NPARAMS, double  *PARAMS,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, TRANS, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, R, C, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO);
#include "function_wrapper_body2.c" 
  return;
}


void zggesx_ (const char  *JOBVSL, const char  *JOBVSR, const char  *SORT,  bool *SELCTG, const char  *SENSE, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  int *SDIM,  void *ALPHA,  void *BETA,  void *VSL, const int *LDVSL,  void *VSR, const int *LDVSR,  double *RCONDE,  double *RCONDV,  void *WORK, const int *LWORK,  double *RWORK,  int *IWORK, const int *LIWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBVSL, JOBVSR, SORT, SELCTG, SENSE, N, A, LDA, B, LDB, SDIM, ALPHA, BETA, VSL, LDVSL, VSR, LDVSR, RCONDE, RCONDV, WORK, LWORK, RWORK, IWORK, LIWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zggevx_ (const char  *BALANC, const char  *JOBVL, const char  *JOBVR, const char  *SENSE, const int *N,  void *A, const int *LDA,  void *B, const int *LDB,  void *ALPHA,  void *BETA,  void *VL, const int *LDVL,  void *VR, const int *LDVR,  int *ILO,  int *IHI,  double *LSCALE,  double *RSCALE,  double *ABNRM,  double *BBNRM,  double *RCONDE,  double *RCONDV,  void *WORK, const int *LWORK,  double *RWORK,  int *IWORK,  bool *BWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( BALANC, JOBVL, JOBVR, SENSE, N, A, LDA, B, LDB, ALPHA, BETA, VL, LDVL, VR, LDVR, ILO, IHI, LSCALE, RSCALE, ABNRM, BBNRM, RCONDE, RCONDV, WORK, LWORK, RWORK, IWORK, BWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhbevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KD,  void *AB, const int *LDAB,  void *Q, const int *LDQ, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KD, AB, LDAB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhbgvx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N, const int *KA, const int *KB,  void *AB, const int *LDAB,  void *BB, const int *LDBB,  void *Q, const int *LDQ, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, KA, KB, AB, LDAB, BB, LDBB, Q, LDQ, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zheevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *LDA, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  double *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, A, LDA, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhegvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *A, const int *LDA,  void *B, const int *LDB, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  void *WORK, const int *LWORK,  double *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, A, LDA, B, LDB, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, LWORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhesvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhesvxx_ (const char  *FACT, const char UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  double *S,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *RPVGRW,  double *BERR, const int *N_ERR_BNDS,  double *ERR_BNDS_NORM,  double *ERR_BNDS_COMP, const int *NPARAMS, double *PARAMS,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, S, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpevx_ (const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *AP, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( JOBZ, RANGE, UPLO, N, AP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpgvx_ (const int *ITYPE, const char  *JOBZ, const char  *RANGE, const char  *UPLO, const int *N,  void *AP,  void *BP, const double *VL, const double *VU, const int *IL, const int *IU, const double *ABSTOL,  int *M,  double *W,  void *Z, const int *LDZ,  void *WORK,  double *RWORK,  int *IWORK,  int *IFAIL,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( ITYPE, JOBZ, RANGE, UPLO, N, AP, BP, VL, VU, IL, IU, ABSTOL, M, W, Z, LDZ, WORK, RWORK, IWORK, IFAIL, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zhpsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *AP,  void *AFP,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zpbsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *KD, const int *NRHS,  void *AB, const int *LDAB,  void *AFB, const int *LDAFB,  char  *EQUED,  double *S,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, KD, NRHS, AB, LDAB, AFB, LDAFB, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zposvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  char  *EQUED,  double *S,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zposvxx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  char  *EQUED,  double *S,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *RPVGRW,  double *BERR, const int *N_ERR_BNDS,  double *ERR_BNDS_NORM,  double *ERR_BNDS_COMP, const int *NPARAMS, double *PARAMS,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, EQUED, S, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zppsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *AP,  void *AFP,  char  *EQUED,  double *S,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, EQUED, S, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zspsvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *AP,  void *AFP,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, AP, AFP, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zsysvx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS, const void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV, const void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *FERR,  double *BERR,  void *WORK, const int *LWORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, B, LDB, X, LDX, RCOND, FERR, BERR, WORK, LWORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


void zsysvxx_ (const char  *FACT, const char  *UPLO, const int *N, const int *NRHS,  void *A, const int *LDA,  void *AF, const int *LDAF,  int *IPIV,  char  *EQUED,  double *S,  void *B, const int *LDB,  void *X, const int *LDX,  double *RCOND,  double *RPVGRW,  double *BERR, const int *N_ERR_BNDS,  double *ERR_BNDS_NORM,  double *ERR_BNDS_COMP, const int *NPARAMS, double *PARAMS,  void *WORK,  double *RWORK,  int *INFO)
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( FACT, UPLO, N, NRHS, A, LDA, AF, LDAF, IPIV, EQUED, S, B, LDB, X, LDX, RCOND, RPVGRW, BERR, N_ERR_BNDS, ERR_BNDS_NORM, ERR_BNDS_COMP, NPARAMS, PARAMS, WORK, RWORK, INFO );
#include "function_wrapper_body2.c" 
  return;
}


