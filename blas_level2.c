
#include "blasperf.h"
#include "hash.h"


// 
//BLAS level 2
//

//?gbmv 
//

void sgbmv_(const char *trans, const int *m, const int *n, const int *kl, 
    const int *ku, const float *alpha, const float *a, const int *lda, 
    const float *x, const int *incx, const float *beta, float *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, kl, ku, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void dgbmv_(const char *trans, const int *m, const int *n, const int *kl, 
    const int *ku, const double *alpha, const double *a, const int *lda, 
    const double *x, const int *incx, const double *beta, double *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, kl, ku, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void cgbmv_(const char *trans, const int *m, const int *n, const int *kl, 
    const int *ku, const void *alpha, const void *a, const int *lda, 
    const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, kl, ku, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void zgbmv_(const char *trans, const int *m, const int *n, const int *kl, 
    const int *ku, const void *alpha, const void *a, const int *lda, 
    const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, kl, ku, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}


//?gemv
//

void sgemv_(const char *trans, const int *m, const int *n, const float *alpha, 
    const float *a, const int *lda, const float *x, const int *incx, 
    const float *beta, float *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void dgemv_(const char *trans, const int *m, const int *n, const double *alpha, 
    const double *a, const int *lda, const double *x, const int *incx, 
    const double *beta, double *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void cgemv_(const char *trans, const int *m, const int *n, const void *alpha, 
    const void *a, const int *lda, const void *x, const int *incx, 
    const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void zgemv_(const char *trans, const int *m, const int *n, const void *alpha, 
    const void *a, const int *lda, const void *x, const int *incx, 
    const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(trans, m, n, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}


//?ger
//

void sger_(const int *m, const int *n, const float *alpha, const float *x, 
    const int *incx, const float *y, const int *incy, float *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(m, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

void dger_(const int *m, const int *n, const double *alpha, const double *x, 
    const int *incx, const double *y, const int *incy, double *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(m, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

//?cgerc
//

void cgerc_(const int *m, const int *n, const void *alpha, const void *x, const int *incx, const void *y, const int *incy, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(m, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

void zgerc_(const int *m, const int *n, const void *alpha, const void *x, const int *incx, const void *y, const int *incy, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(m, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

//?geru_
//

void cgeru_(const int *m, const int *n, const void *alpha, const void *x, const int *incx, const void *y, const int *incy, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(m, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

void zgeru_(const int *m, const int *n, const void *alpha, const void *x, const int *incx, const void *y, const int *incy, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(m, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

//?hbmv_
//

void chbmv_(const char *uplo, const int *n, const int *k, const void *alpha, const void *a, const int *lda, 
    const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, k, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void zhbmv_(const char *uplo, const int *n, const int *k, const void *alpha, const void *a, const int *lda, 
    const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, k, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

//?hemv_
//

void chemv_(const char *uplo, const int *n, const void *alpha, const void *a, const int *lda, const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void zhemv_(const char *uplo, const int *n, const void *alpha, const void *a, const int *lda, const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

//?her_
//

void cher_(const char *uplo, const int *n, const float *alpha, const void *x, const int *incx, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

void zher_(const char *uplo, const int *n, const float *alpha, const void *x, const int *incx, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

//?her2_
//

void cher2_(const char *uplo, const int *n, const void *alpha, const void *x, const int *incx, 
    const void *y, const int *incy, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}

void zher2_(const char *uplo, const int *n, const void *alpha, const void *x, const int *incx, 
    const void *y, const int *incy, void *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, y, incy, a, lda);
#include "blas_wrapper_body2.h" 
    return;
}


//?hpmv_
//

void chpmv_(const char *uplo, const int *n, const void *alpha, const void *ap, const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, ap, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void zhpmv_(const char *uplo, const int *n, const void *alpha, const void *ap, const void *x, const int *incx, const void *beta, void *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, ap, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}


//?hpr_
// 

void chpr_(const char *uplo, const int *n, const float *alpha, const void *x, const int *incx, void *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, ap);
#include "blas_wrapper_body2.h" 
    return;
}

void zhpr_(const char *uplo, const int *n, const float *alpha, const void *x, const int *incx, void *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, ap);
#include "blas_wrapper_body2.h" 
    return;
}

//?hpr2_
//

void chpr2_(const char *uplo, const int *n, const void *alpha, const void *x, const int *incx, 
    const void *y, const int *incy, void *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, y, incy, ap);
#include "blas_wrapper_body2.h" 
    return;
}

void zhpr2_(const char *uplo, const int *n, const void *alpha, const void *x, const int *incx, 
    const void *y, const int *incy, void *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, y, incy, ap);
#include "blas_wrapper_body2.h" 
    return;
}


//?sbmv_
//

void ssbmv_(const char *uplo, const int *n, const int *k, const float *alpha, const float *a, 
    const int *lda, const float *x, const int *incx, const float *beta, float *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, k, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void dsbmv_(const char *uplo, const int *n, const int *k, const double *alpha, const double *a, 
    const int *lda, const double *x, const int *incx, const double *beta, double *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, k, alpha, a, lda, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

//?spmv_
//

void sspmv_(const char *uplo, const int *n, const float *alpha, const float *ap, 
    const float *x, const int *incx, const float *beta, float *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, ap, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void dspmv_(const char *uplo, const int *n, const double *alpha, const double *ap, 
    const double *x, const int *incx, const double *beta, double *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, ap, x, incx, beta, y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

//?spr_
// 

void sspr_(const char *uplo, const int *n, const float *alpha, const float *x, const int *incx, float *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, ap);
#include "blas_wrapper_body2.h" 
    return;
}

void dspr_(const char *uplo, const int *n, const double *alpha, const double *x, const int *incx, double *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, ap);
#include "blas_wrapper_body2.h" 
    return;
}

//?spr2_
//

void sspr2_(const char *uplo, const int *n, const float *alpha, const float *x, const int *incx,
    const float *y, const int *incy, float *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, y, incy, ap);
#include "blas_wrapper_body2.h" 
    return;
}

void dspr2_(const char *uplo, const int *n, const double *alpha, const double *x, const int *incx,
    const double *y, const int *incy, double *ap)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, x, incx, y, incy, ap);
#include "blas_wrapper_body2.h" 
    return;
}

//?symv_
//

void ssymv_(const char *uplo, const int *n, const float *alpha, const float *a, const int *lda,
     const float *x, const int *incx, const float *beta, float *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, a, lda, x, incx, beta,  y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

void dsymv_(const char *uplo, const int *n, const double *alpha, const double *a, const int *lda,
     const double *x, const int *incx, const double *beta, double *y, const int *incy)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, n, alpha, a, lda, x, incx, beta,  y, incy);
#include "blas_wrapper_body2.h" 
    return;
}

//?syr_ 
//

void ssyr_(const char *uplo, const int *n, const float *alpha, const float *x, const int *incx, float *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo,n,alpha,x,incx,a,lda);
#include "blas_wrapper_body2.h" 
    return;
}

void dsyr_(const char *uplo, const int *n, const double *alpha, const double *x, const int *incx, double *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo,n,alpha,x,incx,a,lda);
#include "blas_wrapper_body2.h" 
    return;
}



//?syr2_
//

void ssyr2_(const char *uplo, const int *n, const float *alpha, const float *x, const int *incx, 
    const float *y, const int *incy, float *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo,n,alpha,x,incx,y,incy,a,lda);
#include "blas_wrapper_body2.h" 
    return;
}

void dsyr2_(const char *uplo, const int *n, const double *alpha, const double *x, const int *incx, 
    const double *y, const int *incy, double *a, const int *lda)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo,n,alpha,x,incx,y,incy,a,lda);
#include "blas_wrapper_body2.h" 
    return;
}

//?tbmv_
//

void stbmv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const float *a, const int *lda, float *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void dtbmv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const double *a, const int *lda, double *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ctbmv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ztbmv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

//?tbsv
//

void stbsv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const float *a, const int *lda, float *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void dtbsv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const double *a, const int *lda, double *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ctbsv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ztbsv_(const char *uplo, const char *trans, const char *diag, const int *n, const int *k, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, k, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

//?tpmv_
//

void stpmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const float *ap, float *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void dtpmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const double *ap, double *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ctpmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *ap, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ztpmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *ap, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

//?tpsv_
//

void stpsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const float *ap, float *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void dtpsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const double *ap, double *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ctpsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *ap, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ztpsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *ap, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, ap, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

//?trmv_
//

void strmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const float *a, const int *lda, float *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void dtrmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const double *a, const int *lda, double *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ctrmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ztrmv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

//?trsv_
//

void strsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const float *a, const int *lda, float *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void dtrsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const double *a, const int *lda, double *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ctrsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}

void ztrsv_(const char *uplo, const char *trans, const char *diag, const int *n, 
    const void *a, const int *lda, void *x, const int *incx)
{
    void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
    orig_blas(uplo, trans, diag, n, a, lda, x, incx);
#include "blas_wrapper_body2.h" 
    return;
}
