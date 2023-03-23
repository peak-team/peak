

// 
//BLAS level 1 functions 
//

//?asum
//

float sasum_(const int* n, const float* x, const int* incx)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

double dasum_(const int* n, const double* x, const int* incx)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

float scasum_(const int* n, const complex* x, const int* incx)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

double dzasum_(const int* n, const double complex* x, const int* incx)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}


//?dot 
//

float sdot_(const int* n, const float *x, const int *incx, const float *y, const int *incy)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

double ddot_(const int* n, const double *x, const int *incx, const double *y, const int *incy)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

float sdsdot_(const int *n, const float *scale, const float *x, const int *incx,
                const float *y, const int *incy)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, scale, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

double dsdot_(const int *n, const float *scale, const double *x, const int *incx,
                const double *y, const int *incy)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, scale, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

/* //complex Fortran functions will be crash by the following override.
complex cdotc_(const int* n, const complex *x, const int *incx, const complex *y, const int *incy)
{
   complex (*orig_f)()=NULL;
   complex result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

double complex zdotc_(const int* n, const double complex *x, const int *incx, const double complex *y, const int *incy)
{
   double complex (*orig_f)()=NULL;
   double complex result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

complex cdotu_(const int* n, const complex *x, const int *incx, const complex *y, const int *incy)
{
   complex (*orig_f)()=NULL;
   complex result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}

double complex zdotu_(const int* n, const double complex *x, const int *incx, const double complex *y, const int *incy)
{
   double complex (*orig_f)()=NULL;
   double complex result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return result;
}
*/

//?nrm2
//

float snrm2_(const int* n, const float *x, const int *incx)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

double dnrm2_(const int* n, const double *x, const int *incx)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

float scnrm2_(const int* n, const complex *x, const int *incx)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

double dznrm2_(const int* n, const double complex *x, const int *incx)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

//I?amax
//

int isamax_(const int* n, const float *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

int idamax_(const int* n, const double *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

int icamax_(const int* n, const complex *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

int izamax_(const int* n, const double complex *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

//I?amin
//

int isamin_(const int* n, const float *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

int idamin_(const int* n, const double *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

int icamin_(const int* n, const complex *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

int izamin_(const int* n, const double complex *x, const int *incx)
{
   int (*orig_f)()=NULL;
   int result;
#include "function_wrapper_body1.c" 
   result=orig_f(n, x, incx);
#include "function_wrapper_body2.c" 
   return result;
}

//*abs1
//

float scabs1_(const complex* x)
{
   float (*orig_f)()=NULL;
   float result;
#include "function_wrapper_body1.c" 
   result=orig_f(x);
#include "function_wrapper_body2.c" 
   return result;
}

double dcabs1_(const double complex* x)
{
   double (*orig_f)()=NULL;
   double result;
#include "function_wrapper_body1.c" 
   result=orig_f(x);
#include "function_wrapper_body2.c" 
   return result;
}




// 
//BLAS level 1 subroutines
//

//?axpy
//

void saxpy_(const int* n, const float *a, const float *x, const int *incx, float *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void daxpy_(const int* n, const double *a, const double *x, const int *incx, double *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void caxpy_(const int* n, const complex *a, const complex *x, const int *incx, complex *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void zaxpy_(const int* n, const double complex *a, const double complex *x, const int *incx, double complex *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

//?copy 
//

void scopy_(const int* n, const float *x, const int *incx, float *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void dcopy_(const int* n, const double *x, const int *incx, double *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void ccopy_(const int* n, const complex *x, const int *incx, complex *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void zcopy_(const int* n, const double complex *x, const int *incx, double complex *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

//*scal
//

void sscal_(const int* n, const float *a, float *x, const int *incx)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx);
#include "function_wrapper_body2.c" 
   return;
}

void dscal_(const int* n, const double *a, double *x, const int *incx)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx);
#include "function_wrapper_body2.c" 
   return;
}

void cscal_(const int* n, const float *a, complex *x, const int *incx)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx);
#include "function_wrapper_body2.c" 
   return;
}

void zscal_(const int* n, const double *a, double complex *x, const int *incx)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, a, x, incx);
#include "function_wrapper_body2.c" 
   return;
}


//?swap
//
void sswap_(const int* n, float *x, const int *incx, float *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void dswap_(const int* n, double *x, const int *incx, double *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void cswap_(const int* n, complex *x, const int *incx, complex *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}

void zswap_(const int* n, double complex *x, const int *incx, double complex *y, const int *incy)
{
   void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
   orig_f(n, x, incx, y, incy);
#include "function_wrapper_body2.c" 
   return;
}



/* some additionals 
     srot_, drot_, crot_, zrot_, csrot_, zdrot_,
     srotg_, drotg_, crotg_, zrotg_, 
     srotm_, drotm_, 
     srotmg_, drotmg_,
*/
















































