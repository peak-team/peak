
// 
// BLAS level 3
// 30 subroutines in total
//

#define GET_AVG_MATRIX_SIZE3 int isize=(int)( cbrt(*m)*cbrt(*n)*cbrt(*k) )
#define GET_AVG_MATRIX_SIZE2 int isize=(int)( sqrt(*m)*sqrt(*n) )

//?gemm
//

void sgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k, 
                const float *alpha, const float *a, const int *lda, const float *b, const int *ldb, 
                const float *beta, float *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    //int isize=(int)( cbrt(*m)*cbrt(*n)*cbrt(*k) );
    GET_AVG_MATRIX_SIZE3;
#include "blas_wrapper/function_wrapper_stats.c"
    return;
}

void dgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k,
                const double *alpha, const double *a, const int *lda, const double *b, const int *ldb,
                const double *beta, double *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    if(peakprof_debug && ifrecord) printf ("dgemm -- matrix size A:%dx%d  B:%dx%d:   C:%dx%d\n", *m, *k,*k,*n,*m,*n);
    orig_f(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "blas_wrapper/function_wrapper_stats.c"
    return;
}

void cgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k,
                const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
                const void *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "blas_wrapper/function_wrapper_stats.c"
    return;
}

void zgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k,
                const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
                const void *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "blas_wrapper/function_wrapper_stats.c"
    return;
}

//?hemm 
//

void chemm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void zhemm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

//?herk
//

void cherk_(const char *uplo, const char *trans, const int *n, const int *k,
    const float *alpha, const void *a, const int *lda, const float *beta, void *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void zherk_(const char *uplo, const char *trans, const int *n, const int *k,
    const float *alpha, const void *a, const int *lda, const float *beta, void *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

//?her2k
//

void cher2k_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const float *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void zher2k_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const float *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

//?symm
//

void ssymm_(const char *side, const char *uplo, const int *m, const int *n,
    const float *alpha, const float *a, const int *lda, const float *b, const int *ldb,
    const float *beta, float *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void dsymm_(const char *side, const char *uplo, const int *m, const int *n,
    const double *alpha, const double *a, const int *lda, const double *b, const int *ldb,
    const double *beta, double *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void csymm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void zsymm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

//?syrk
//
void ssyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const float *alpha, const float *a, const int *lda, const float *beta, float *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void dsyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const double *alpha, const double *a, const int *lda, const double *beta, double *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void csyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void zsyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *beta, void *c, const int *ldc) 
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

//?syr2k
//

void ssyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const float *alpha, const float *a, const int *lda, const float *b, const int *ldb, 
    const float *beta, float *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void dsyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const double *alpha, const double *a, const int *lda, const double *b, const int *ldb, 
    const double *beta, double *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void csyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb, 
    const void *beta, void *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

void zsyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb, 
    const void *beta, void *c, const int *ldc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "function_wrapper_body2.c" 
    return;
}

//?trmm 
//
void strmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const float *alpha, const float *a, const int *lda, float *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void dtrmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const double *alpha, const double *a, const int *lda, double *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void ctrmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void ztrmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

//?trsm
//
void strsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const float *alpha, const float *a, const int *lda, float *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void dtrsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const double *alpha, const double *a, const int *lda, double *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void ctrsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}

void ztrsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "function_wrapper_body2.c" 
    return;
}


