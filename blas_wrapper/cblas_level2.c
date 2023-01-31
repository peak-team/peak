


/*
 * ===========================================================================
 * Prototypes for level 2 BLAS
 * ===========================================================================
 */

/* 
 * Routines with standard 4 prefixes (S, D, C, Z)
 */
void cblas_sgemv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const float alpha, const float *A, const int lda,
                 const float *X, const int incX, const float beta,
                 float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_sgbmv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const int KL, const int KU, const float alpha,
                 const float *A, const int lda, const float *X,
                 const int incX, const float beta, float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, KL, KU, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_strmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const float *A, const int lda, 
                 float *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_stbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const float *A, const int lda, 
                 float *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_stpmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const float *Ap, float *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_strsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const float *A, const int lda, float *X,
                 const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_stbsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const float *A, const int lda,
                 float *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_stpsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const float *Ap, float *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}



void cblas_dgemv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const double alpha, const double *A, const int lda,
                 const double *X, const int incX, const double beta,
                 double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dgbmv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const int KL, const int KU, const double alpha,
                 const double *A, const int lda, const double *X,
                 const int incX, const double beta, double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, KL, KU, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtrmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const double *A, const int lda, 
                 double *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const double *A, const int lda, 
                 double *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtpmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const double *Ap, double *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtrsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const double *A, const int lda, double *X,
                 const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtbsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const double *A, const int lda,
                 double *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dtpsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const double *Ap, double *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}



void cblas_cgemv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 const void *X, const int incX, const void *beta,
                 void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_cgbmv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const int KL, const int KU, const void *alpha,
                 const void *A, const int lda, const void *X,
                 const int incX, const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, KL, KU, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctrmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *A, const int lda, 
                 void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const void *A, const int lda, 
                 void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctpmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *Ap, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctrsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *A, const int lda, void *X,
                 const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctbsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const void *A, const int lda,
                 void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ctpsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *Ap, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zgemv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const void *alpha, const void *A, const int lda,
                 const void *X, const int incX, const void *beta,
                 void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zgbmv(const enum CBLAS_ORDER order,
                 const enum CBLAS_TRANSPOSE TransA, const int M, const int N,
                 const int KL, const int KU, const void *alpha,
                 const void *A, const int lda, const void *X,
                 const int incX, const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, TransA, M, N, KL, KU, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztrmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *A, const int lda, 
                 void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const void *A, const int lda, 
                 void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztpmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *Ap, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztrsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *A, const int lda, void *X,
                 const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztbsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const int K, const void *A, const int lda,
                 void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, K, A, lda, X, incX);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ztpsv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const enum CBLAS_TRANSPOSE TransA, const enum CBLAS_DIAG Diag,
                 const int N, const void *Ap, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, TransA, Diag, N, Ap, X, incX);
#include "function_wrapper_body2.c" 
    return;
}




/* 
 * Routines with S and D prefixes only
 */
void cblas_ssymv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const float alpha, const float *A,
                 const int lda, const float *X, const int incX,
                 const float beta, float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ssbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const int K, const float alpha, const float *A,
                 const int lda, const float *X, const int incX,
                 const float beta, float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, K, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_sspmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const float alpha, const float *Ap,
                 const float *X, const int incX,
                 const float beta, float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, Ap, X, incX, beta, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_sger(const enum CBLAS_ORDER order, const int M, const int N,
                const float alpha, const float *X, const int incX,
                const float *Y, const int incY, float *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, M, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ssyr(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const float alpha, const float *X,
                const int incX, float *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, A, lda);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_sspr(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const float alpha, const float *X,
                const int incX, float *Ap)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Ap);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_ssyr2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const float alpha, const float *X,
                const int incX, const float *Y, const int incY, float *A,
                const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_sspr2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const float alpha, const float *X,
                const int incX, const float *Y, const int incY, float *A)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, A);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dsymv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const double alpha, const double *A,
                 const int lda, const double *X, const int incX,
                 const double beta, double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dsbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const int K, const double alpha, const double *A,
                 const int lda, const double *X, const int incX,
                 const double beta, double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, K, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dspmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const double alpha, const double *Ap,
                 const double *X, const int incX,
                 const double beta, double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, Ap, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dger(const enum CBLAS_ORDER order, const int M, const int N,
                const double alpha, const double *X, const int incX,
                const double *Y, const int incY, double *A, const int lda){
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, M, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dsyr(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const double alpha, const double *X,
                const int incX, double *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dspr(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const double alpha, const double *X,
                const int incX, double *Ap)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Ap);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dsyr2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const double alpha, const double *X,
                const int incX, const double *Y, const int incY, double *A,
                const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dspr2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const double alpha, const double *X,
                const int incX, const double *Y, const int incY, double *A)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, A);
#include "function_wrapper_body2.c" 
    return;
}




/* 
 * Routines with C and Z prefixes only
 */
void cblas_chemv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const void *alpha, const void *A,
                 const int lda, const void *X, const int incX,
                 const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_chbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const int K, const void *alpha, const void *A,
                 const int lda, const void *X, const int incX,
                 const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, K, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_chpmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const void *alpha, const void *Ap,
                 const void *X, const int incX,
                 const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, Ap, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cgeru(const enum CBLAS_ORDER order, const int M, const int N,
                 const void *alpha, const void *X, const int incX,
                 const void *Y, const int incY, void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, M, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cgerc(const enum CBLAS_ORDER order, const int M, const int N,
                 const void *alpha, const void *X, const int incX,
                 const void *Y, const int incY, void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, M, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cher(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const float alpha, const void *X, const int incX,
                void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_chpr(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const float alpha, const void *X,
                const int incX, void *A)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, A);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cher2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo, const int N,
                const void *alpha, const void *X, const int incX,
                const void *Y, const int incY, void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_chpr2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo, const int N,
                const void *alpha, const void *X, const int incX,
                const void *Y, const int incY, void *Ap)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, Ap);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zhemv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const void *alpha, const void *A,
                 const int lda, const void *X, const int incX,
                 const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zhbmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const int K, const void *alpha, const void *A,
                 const int lda, const void *X, const int incX,
                 const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, K, alpha, A, lda, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zhpmv(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                 const int N, const void *alpha, const void *Ap,
                 const void *X, const int incX,
                 const void *beta, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, Ap, X, incX, beta, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zgeru(const enum CBLAS_ORDER order, const int M, const int N,
                 const void *alpha, const void *X, const int incX,
                 const void *Y, const int incY, void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, M, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zgerc(const enum CBLAS_ORDER order, const int M, const int N,
                 const void *alpha, const void *X, const int incX,
                 const void *Y, const int incY, void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, M, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zher(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const double alpha, const void *X, const int incX,
                void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zhpr(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo,
                const int N, const double alpha, const void *X,
                const int incX, void *A)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, A);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zher2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo, const int N,
                const void *alpha, const void *X, const int incX,
                const void *Y, const int incY, void *A, const int lda)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, A, lda);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zhpr2(const enum CBLAS_ORDER order, const enum CBLAS_UPLO Uplo, const int N,
                const void *alpha, const void *X, const int incX,
                const void *Y, const int incY, void *Ap)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(order, Uplo, N, alpha, X, incX, Y, incY, Ap);
#include "function_wrapper_body2.c" 
    return;
}



