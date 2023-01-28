
#include "blasperf.h"
#include "hash.h"
#include "complex.h"


// 
//BLAS level 1 functions 
//

//?asum
//

float sasum_(const int* n, const float* x, const int* incx)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

double dasum_(const int* n, const double* x, const int* incx)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

float scasum_(const int* n, const complex* x, const int* incx)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

double dzasum_(const int* n, const double complex* x, const int* incx)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}


//?dot 
//

float sdot_(const int* n, const float *x, const int *incx, const float *y, const int *incy)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

double ddot_(const int* n, const double *x, const int *incx, const double *y, const int *incy)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

float sdsdot_(const int *n, const float *scale, const float *x, const int *incx,
                const float *y, const int *incy)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, scale, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

double dsdot_(const int *n, const float *scale, const double *x, const int *incx,
                const double *y, const int *incy)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, scale, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

complex cdotc_(const int* n, const complex *x, const int *incx, const complex *y, const int *incy)
{
   complex (*orig_blas)()=NULL;
   complex result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

double complex zdotc_(const int* n, const double complex *x, const int *incx, const double complex *y, const int *incy)
{
   double complex (*orig_blas)()=NULL;
   double complex result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

complex cdotu_(const int* n, const complex *x, const int *incx, const complex *y, const int *incy)
{
   complex (*orig_blas)()=NULL;
   complex result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

double complex zdotu_(const int* n, const double complex *x, const int *incx, const double complex *y, const int *incy)
{
   double complex (*orig_blas)()=NULL;
   double complex result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return result;
}

//?nrm2
//

float snrm2_(const int* n, const float *x, const int *incx)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

double dnrm2_(const int* n, const double *x, const int *incx)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

float scnrm2_(const int* n, const complex *x, const int *incx)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

double dznrm2_(const int* n, const double complex *x, const int *incx)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

//I?amax
//

int isamax_(const int* n, const float *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

int idamax_(const int* n, const double *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

int icamax_(const int* n, const complex *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

int izamax_(const int* n, const double complex *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

//I?amin
//

int isamin_(const int* n, const float *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

int idamin_(const int* n, const double *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

int icamin_(const int* n, const complex *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

int izamin_(const int* n, const double complex *x, const int *incx)
{
   int (*orig_blas)()=NULL;
   int result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(n, x, incx);
#include "blas_wrapper_body2.h" 
   return result;
}

//*abs1
//

float scabs1_(const complex* x)
{
   float (*orig_blas)()=NULL;
   float result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(x);
#include "blas_wrapper_body2.h" 
   return result;
}

double dcabs1_(const double complex* x)
{
   double (*orig_blas)()=NULL;
   double result;
#include "blas_wrapper_body1.h" 
   result=orig_blas(x);
#include "blas_wrapper_body2.h" 
   return result;
}




// 
//BLAS level 1 subroutines
//

//?axpy
//

void saxpy_(const int* n, const float *a, const float *x, const int *incx, float *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void daxpy_(const int* n, const double *a, const double *x, const int *incx, double *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void caxpy_(const int* n, const complex *a, const complex *x, const int *incx, complex *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void zaxpy_(const int* n, const double complex *a, const double complex *x, const int *incx, double complex *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

//?copy 
//

void scopy_(const int* n, const float *x, const int *incx, float *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void dcopy_(const int* n, const double *x, const int *incx, double *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void ccopy_(const int* n, const complex *x, const int *incx, complex *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void zcopy_(const int* n, const double complex *x, const int *incx, double complex *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

//*scal
//

void sscal_(const int* n, const float *a, float *x, const int *incx)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx);
#include "blas_wrapper_body2.h" 
   return;
}

void dscal_(const int* n, const double *a, double *x, const int *incx)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx);
#include "blas_wrapper_body2.h" 
   return;
}

void cscal_(const int* n, const float *a, complex *x, const int *incx)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx);
#include "blas_wrapper_body2.h" 
   return;
}

void zscal_(const int* n, const double *a, double complex *x, const int *incx)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, a, x, incx);
#include "blas_wrapper_body2.h" 
   return;
}


//?swap
//
void sswap_(const int* n, float *x, const int *incx, float *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void dswap_(const int* n, double *x, const int *incx, double *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void cswap_(const int* n, complex *x, const int *incx, complex *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}

void zswap_(const int* n, double complex *x, const int *incx, double complex *y, const int *incy)
{
   void (*orig_blas)()=NULL;
#include "blas_wrapper_body1.h" 
   orig_blas(n, x, incx, y, incy);
#include "blas_wrapper_body2.h" 
   return;
}



/* some additionals 
     srot_, drot_, crot_, zrot_, csrot_, zdrot_,
     srotg_, drotg_, crotg_, zrotg_, 
     srotm_, drotm_, 
     srotmg_, drotmg_,
*/
















































