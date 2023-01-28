
#include "blasperf.h"
#include "hash.h"


// 
// BLAS level 3
// 30 subroutines in total
//

//?gemm
//

void sgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k, 
                const float *alpha, const float *a, const int *lda, const float *b, const int *ldb, 
                const float *beta, float *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void dgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k,
                const double *alpha, const double *a, const int *lda, const double *b, const int *ldb,
                const double *beta, double *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void cgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k,
                const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
                const void *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k,
                const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
                const void *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?hemm 
//

void chemm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zhemm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?herk
//

void cherk_(const char *uplo, const char *trans, const int *n, const int *k,
    const float *alpha, const void *a, const int *lda, const float *beta, void *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zherk_(const char *uplo, const char *trans, const int *n, const int *k,
    const float *alpha, const void *a, const int *lda, const float *beta, void *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?her2k
//

void cher2k_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const float *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zher2k_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const float *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
  orig_blas(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?symm
//

void ssymm_(const char *side, const char *uplo, const int *m, const int *n,
    const float *alpha, const float *a, const int *lda, const float *b, const int *ldb,
    const float *beta, float *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void dsymm_(const char *side, const char *uplo, const int *m, const int *n,
    const double *alpha, const double *a, const int *lda, const double *b, const int *ldb,
    const double *beta, double *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void csymm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zsymm_(const char *side, const char *uplo, const int *m, const int *n,
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb,
    const void *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, m, n, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?syrk
//
void ssyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const float *alpha, const float *a, const int *lda, const float *beta, float *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void dsyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const double *alpha, const double *a, const int *lda, const double *beta, double *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void csyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zsyrk_(const char *uplo, const char *trans, const int *n, const int *k,
    const void *alpha, const void *a, const int *lda, const void *beta, void *c, const int *ldc) 
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?syr2k
//

void ssyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const float *alpha, const float *a, const int *lda, const float *b, const int *ldb, 
    const float *beta, float *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void dsyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const double *alpha, const double *a, const int *lda, const double *b, const int *ldb, 
    const double *beta, double *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void csyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb, 
    const void *beta, void *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

void zsyr2k_(const char *uplo, const char *trans, const int *n, const int *k, 
    const void *alpha, const void *a, const int *lda, const void *b, const int *ldb, 
    const void *beta, void *c, const int *ldc)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
#include "blas_wrapper_body2.h" 
    return;
}

//?trmm 
//
void strmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const float *alpha, const float *a, const int *lda, float *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

void dtrmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const double *alpha, const double *a, const int *lda, double *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

void ctrmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

void ztrmm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

//?trsm
//
void strsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const float *alpha, const float *a, const int *lda, float *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

void dtrsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const double *alpha, const double *a, const int *lda, double *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

void ctrsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}

void ztrsm_(const char *side, const char *uplo, const char *transa, const char *diag, 
    const int *m, const int *n, const void *alpha, const void *a, const int *lda, void *b, const int *ldb)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb);
#include "blas_wrapper_body2.h" 
    return;
}


