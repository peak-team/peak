
/*
 * ===========================================================================
 * Prototypes for level 3 BLAS
 * ===========================================================================
 */

/* 
 * Routines with standard 4 prefixes (S, D, C, Z)
 */
void cblas_sgemm(const enum CBLAS_ORDER Order, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const float alpha, const float *A,
                 const int lda, const float *B, const int ldb,
                 const float beta, float *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_ssymm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const int M, const int N,
                 const float alpha, const float *A, const int lda,
                 const float *B, const int ldb, const float beta,
                 float *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_ssyrk(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                 const float alpha, const float *A, const int lda,
                 const float beta, float *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_ssyr2k(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                  const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                  const float alpha, const float *A, const int lda,
                  const float *B, const int ldb, const float beta,
                  float *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_strmm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const float alpha, const float *A, const int lda,
                 float *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_strsm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const float alpha, const float *A, const int lda,
                 float *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dgemm(const enum CBLAS_ORDER Order, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const double alpha, const double *A,
                 const int lda, const double *B, const int ldb,
                 const double beta, double *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dsymm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const int M, const int N,
                 const double alpha, const double *A, const int lda,
                 const double *B, const int ldb, const double beta,
                 double *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dsyrk(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                 const double alpha, const double *A, const int lda,
                 const double beta, double *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dsyr2k(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                  const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                  const double alpha, const double *A, const int lda,
                  const double *B, const int ldb, const double beta,
                  double *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtrmm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const double alpha, const double *A, const int lda,
                 double *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtrsm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const double alpha, const double *A, const int lda,
                 double *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_cgemm(const enum CBLAS_ORDER Order, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const void *alpha, const void *A,
                 const int lda, const void *B, const int ldb,
                 const void *beta, void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_csymm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 const void *B, const int ldb, const void *beta,
                 void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_csyrk(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                 const void *alpha, const void *A, const int lda,
                 const void *beta, void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_csyr2k(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                  const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                  const void *alpha, const void *A, const int lda,
                  const void *B, const int ldb, const void *beta,
                  void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctrmm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 void *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctrsm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 void *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zgemm(const enum CBLAS_ORDER Order, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const void *alpha, const void *A,
                 const int lda, const void *B, const int ldb,
                 const void *beta, void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zsymm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 const void *B, const int ldb, const void *beta,
                 void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order,Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zsyrk(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                 const void *alpha, const void *A, const int lda,
                 const void *beta, void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zsyr2k(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                  const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                  const void *alpha, const void *A, const int lda,
                  const void *B, const int ldb, const void *beta,
                  void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztrmm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 void *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztrsm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_DIAG Diag, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 void *B, const int ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb);
#include "function_wrapper_body2.c" 
    return;
}



/* 
 * Routines with prefixes C and Z only
 */
void cblas_chemm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 const void *B, const int ldb, const void *beta,
                 void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cherk(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                 const float alpha, const void *A, const int lda,
                 const float beta, void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cher2k(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                  const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                  const void *alpha, const void *A, const int lda,
                  const void *B, const int ldb, const float beta,
                  void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zhemm(const enum CBLAS_ORDER Order, const enum CBLAS_SIDE Side,
                 const enum CBLAS_UPLO Uplo, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 const void *B, const int ldb, const void *beta,
                 void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zherk(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                 const double alpha, const void *A, const int lda,
                 const double beta, void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zher2k(const enum CBLAS_ORDER Order, const enum CBLAS_UPLO Uplo,
                  const enum CBLAS_TRANSPOSE Trans, const int N, const int K,
                  const void *alpha, const void *A, const int lda,
                  const void *B, const int ldb, const double beta,
                  void *C, const int ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
#include "function_wrapper_body2.c" 
    return;
}

