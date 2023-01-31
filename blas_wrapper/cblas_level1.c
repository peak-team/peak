
/*
 * ===========================================================================
 * Prototypes for level 1 BLAS functions (complex are recast as routines)
 * ===========================================================================
 */
float  cblas_sdsdot(const int N, const float alpha, const float *X,
                    const int incX, const float *Y, const int incY)
{
    float result;
    float (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, alpha, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return result;
}

double cblas_dsdot(const int N, const float *X, const int incX, const float *Y,
                   const int incY)
{
    double result;
    double (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return result;
}

float  cblas_sdot(const int N, const float  *X, const int incX,
                  const float  *Y, const int incY)
{
    float result;
    float (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return result;
}

double cblas_ddot(const int N, const double *X, const int incX,
                  const double *Y, const int incY)
{
    double result;
    double (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return result;
}


/*
 * Functions having prefixes Z and C only
 */
void   cblas_cdotu_sub(const int N, const void *X, const int incX,
                       const void *Y, const int incY, void *dotu)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, dotu);
#include "function_wrapper_body2.c" 
    return;
}

void   cblas_cdotc_sub(const int N, const void *X, const int incX,
                       const void *Y, const int incY, void *dotc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, dotc);
#include "function_wrapper_body2.c" 
    return;
}


void   cblas_zdotu_sub(const int N, const void *X, const int incX,
                       const void *Y, const int incY, void *dotu)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, dotu);
#include "function_wrapper_body2.c" 
    return;
}

void   cblas_zdotc_sub(const int N, const void *X, const int incX,
                       const void *Y, const int incY, void *dotc)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, dotc);
#include "function_wrapper_body2.c" 
    return;
}



/*
 * Functions having prefixes S D SC DZ
 */
float  cblas_snrm2(const int N, const float *X, const int incX)
{
    float result;
    float (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

float  cblas_sasum(const int N, const float *X, const int incX)
{
    float result;
    float (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}


double cblas_dnrm2(const int N, const double *X, const int incX)
{
    double result;
    double (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

double cblas_dasum(const int N, const double *X, const int incX)
{
    double result;
    double (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}


float  cblas_scnrm2(const int N, const void *X, const int incX)
{
    float result;
    float (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

float  cblas_scasum(const int N, const void *X, const int incX)
{
    float result;
    float (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}


double cblas_dznrm2(const int N, const void *X, const int incX)
{
    double result;
    double (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

double cblas_dzasum(const int N, const void *X, const int incX)
{
    double result;
    double (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}



/*
 * Functions having standard 4 prefixes (S D C Z)
 */
CBLAS_INDEX cblas_isamax(const int N, const float  *X, const int incX)
{
    CBLAS_INDEX result;
    CBLAS_INDEX (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

CBLAS_INDEX cblas_idamax(const int N, const double *X, const int incX)
{
    CBLAS_INDEX result;
    CBLAS_INDEX (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

CBLAS_INDEX cblas_icamax(const int N, const void   *X, const int incX)
{
    CBLAS_INDEX result;
    CBLAS_INDEX (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}

CBLAS_INDEX cblas_izamax(const int N, const void   *X, const int incX)
{
    CBLAS_INDEX result;
    CBLAS_INDEX (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    result=orig_f(N, X, incX);
#include "function_wrapper_body2.c" 
    return result;
}


/*
 * ===========================================================================
 * Prototypes for level 1 BLAS routines
 * ===========================================================================
 */

/* 
 * Routines with standard 4 prefixes (s, d, c, z)
 */
void cblas_sswap(const int N, float *X, const int incX, 
                 float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_scopy(const int N, const float *X, const int incX, 
                 float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_saxpy(const int N, const float alpha, const float *X,
                 const int incX, float *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_dswap(const int N, double *X, const int incX, 
                 double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dcopy(const int N, const double *X, const int incX, 
                 double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_daxpy(const int N, const double alpha, const double *X,
                 const int incX, double *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cswap(const int N, void *X, const int incX, 
                 void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_ccopy(const int N, const void *X, const int incX, 
                 void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_caxpy(const int N, const void *alpha, const void *X,
                 const int incX, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_zswap(const int N, void *X, const int incX, 
                 void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zcopy(const int N, const void *X, const int incX, 
                 void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zaxpy(const int N, const void *alpha, const void *X,
                 const int incX, void *Y, const int incY)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX, Y, incY);
#include "function_wrapper_body2.c" 
    return;
}


/* 
 * Routines with S and D prefix only
 */
void cblas_srotg(float *a, float *b, float *c, float *s)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(a, b, c, s);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_srotmg(float *d1, float *d2, float *b1, const float b2, float *P)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(d1, d2, b1, b2, P);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_srot(const int N, float *X, const int incX,
                float *Y, const int incY, const float c, const float s)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, c, s);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_srotm(const int N, float *X, const int incX,
                float *Y, const int incY, const float *P)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, P);
#include "function_wrapper_body2.c" 
    return;
}


void cblas_drotg(double *a, double *b, double *c, double *s)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(a, b, c, s);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_drotmg(double *d1, double *d2, double *b1, const double b2, double *P)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(d1, d2, b1, b2, P);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_drot(const int N, double *X, const int incX,
                double *Y, const int incY, const double c, const double  s)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, c, s);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_drotm(const int N, double *X, const int incX,
                double *Y, const int incY, const double *P)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, X, incX, Y, incY, P);
#include "function_wrapper_body2.c" 
    return;
}

/* 
 * Routines with S D C Z CS and ZD prefixes
 */
void cblas_sscal(const int N, const float alpha, float *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_dscal(const int N, const double alpha, double *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_cscal(const int N, const void *alpha, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zscal(const int N, const void *alpha, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_csscal(const int N, const float alpha, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

void cblas_zdscal(const int N, const double alpha, void *X, const int incX)
{
    void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
    orig_f(N, alpha, X, incX);
#include "function_wrapper_body2.c" 
    return;
}

