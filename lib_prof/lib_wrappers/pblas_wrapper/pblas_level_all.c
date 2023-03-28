#define GET_AVG_MATRIX_SIZE3 int isize=(int)( cbrt(*m)*cbrt(*n)*cbrt(*k) )
#define GET_AVG_MATRIX_SIZE2 int isize=(int)( sqrt(*m)*sqrt(*n) )

/* PBLAS Level 1 Routines */

void psamax_( const int *n, float *amax, int *indx, const float *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, amax, indx, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdamax_( const int *n, double *amax, int *indx, const double *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, amax, indx, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pcamax_( const int *n, void *amax, int *indx, const void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, amax, indx, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pzamax_( const int *n, void *amax, int *indx, const void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, amax, indx, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void psasum_( const int *n, float *asum, const float *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, asum, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdasum_( const int *n, double *asum, const double *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, asum, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pscasum_( const int *n, float *asum, const void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, asum, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdzasum_( const int *n, double *asum, const void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, asum, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void psaxpy_( const int *n, const float *a, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdaxpy_( const int *n, const double *a, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcaxpy_( const int *n, const void *a, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzaxpy_( const int *n, const void *a, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void picopy_( const int *n, const int *x, const int *ix, const int *jx, const int *descx, const int *incx, int *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pscopy_( const int *n, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdcopy_( const int *n, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pccopy_( const int *n, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzcopy_( const int *n, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void psdot_( const int *n, float *dot, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, dot, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pddot_( const int *n, double *dot, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, dot, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcdotc_( const int *n, void *dotc, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, dotc, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzdotc_( const int *n, void *dotc, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, dotc, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcdotu_( const int *n, void *dotu, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, dotu, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzdotu_( const int *n, void *dotu, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, dotu, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void psnrm2_( const int *n, float *norm2, const float *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, norm2, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdnrm2_( const int *n, double *norm2, const double *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, norm2, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pscnrm2_( const int *n, float *norm2, const void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, norm2, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdznrm2_( const int *n, double *norm2, const void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, norm2, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void psscal_( const int *n, const float *a, float *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdscal_( const int *n, const double *a, double *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pcscal_( const int *n, const void *a, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pzscal_( const int *n, const void *a, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pcsscal_( const int *n, const float *a, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pzdscal_( const int *n, const double *a, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, a, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void psswap_( const int *n, float *x, const int *ix, const int *jx, const int *descx, const int *incx, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdswap_( const int *n, double *x, const int *ix, const int *jx, const int *descx, const int *incx, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcswap_( const int *n, void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzswap_( const int *n, void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( n, x, ix, jx, descx, incx, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


/* PBLAS Level 2 Routines */

void psgemv_( const char *trans, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *beta, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE2;
#include "function_wrapper_stats.c"
  return;
}


void pdgemv_( const char *trans, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *beta, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE2;
#include "function_wrapper_stats.c"
  return;
}


void pcgemv_( const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE2;
#include "function_wrapper_stats.c"
  return;
}


void pzgemv_( const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE2;
#include "function_wrapper_stats.c"
  return;
}


void psagemv_( const char *trans, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *beta, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdagemv_( const char *trans, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *beta, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcagemv_( const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzagemv_( const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void psger_( const int *m, const int *n, const float *alpha, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *y, const int *iy, const int *jy, const int *descy, const int *incy, float *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pdger_( const int *m, const int *n, const double *alpha, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *y, const int *iy, const int *jy, const int *descy, const int *incy, double *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pcgerc_( const int *m, const int *n, const void *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pzgerc_( const int *m, const int *n, const void *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pcgeru_( const int *m, const int *n, const void *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pzgeru_( const int *m, const int *n, const void *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pchemv_( const char *uplo, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzhemv_( const char *uplo, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcahemv_( const char *uplo, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzahemv_( const char *uplo, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcher_( const char *uplo, const int *n, const float *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pzher_( const char *uplo, const int *n, const double *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pcher2_( const char *uplo, const int *n, const void *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pzher2_( const char *uplo, const int *n, const void *alpha, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *y, const int *iy, const int *jy, const int *descy, const int *incy, void *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pssymv_( const char *uplo, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *beta, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdsymv_( const char *uplo, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *beta, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void psasymv_( const char *uplo, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *beta, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdasymv_( const char *uplo, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *beta, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pssyr_( const char *uplo, const int *n, const float *alpha, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, float *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pdsyr_( const char *uplo, const int *n, const double *alpha, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, double *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pssyr2_( const char *uplo, const int *n, const float *alpha, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *y, const int *iy, const int *jy, const int *descy, const int *incy, float *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pdsyr2_( const char *uplo, const int *n, const double *alpha, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *y, const int *iy, const int *jy, const int *descy, const int *incy, double *a, const int *ia, const int *ja, const int *desca )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, n, alpha, x, ix, jx, descx, incx, y, iy, jy, descy, incy, a, ia, ja, desca );
#include "function_wrapper_body2.c" 
  return;
}


void pstrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const float *a, const int *ia, const int *ja, const int *desca, float *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdtrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const double *a, const int *ia, const int *ja, const int *desca, double *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pctrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const void *a, const int *ia, const int *ja, const int *desca, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pztrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const void *a, const int *ia, const int *ja, const int *desca, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void psatrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *x, const int *ix, const int *jx, const int *descx, const int *incx, const float *beta, float *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pdatrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *x, const int *ix, const int *jx, const int *descx, const int *incx, const double *beta, double *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pcatrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pzatrmv_( const char *uplo, const char *trans, const char *diag, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *x, const int *ix, const int *jx, const int *descx, const int *incx, const void *beta, void *y, const int *iy, const int *jy, const int *descy, const int *incy )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, alpha, a, ia, ja, desca, x, ix, jx, descx, incx, beta, y, iy, jy, descy, incy );
#include "function_wrapper_body2.c" 
  return;
}


void pstrsv_( const char *uplo, const char *trans, const char *diag, const int *n, const float *a, const int *ia, const int *ja, const int *desca, float *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pdtrsv_( const char *uplo, const char *trans, const char *diag, const int *n, const double *a, const int *ia, const int *ja, const int *desca, double *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pctrsv_( const char *uplo, const char *trans, const char *diag, const int *n, const void *a, const int *ia, const int *ja, const int *desca, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


void pztrsv_( const char *uplo, const char *trans, const char *diag, const int *n, const void *a, const int *ia, const int *ja, const int *desca, void *x, const int *ix, const int *jx, const int *descx, const int *incx )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, diag, n, a, ia, ja, desca, x, ix, jx, descx, incx );
#include "function_wrapper_body2.c" 
  return;
}


/* PBLAS Level 3 Routines */

void psgemm_( const char *transa, const char *transb, const int *m, const int *n, const int *k, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *b, const int *ib, const int *jb, const int *descb, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( transa, transb, m, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "function_wrapper_stats.c"
  return;
}


void pdgemm_( const char *transa, const char *transb, const int *m, const int *n, const int *k, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *b, const int *ib, const int *jb, const int *descb, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  //  printf ("pdgemm -- matrix size A:%dx%d  B:%dx%d:   C:%dx%d\n", *m, *k,*k,*n,*m,*n);
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( transa, transb, m, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "function_wrapper_stats.c"
  return;
}


void pcgemm_( const char *transa, const char *transb, const int *m, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( transa, transb, m, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "function_wrapper_stats.c"
  return;
}


void pzgemm_( const char *transa, const char *transb, const int *m, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( transa, transb, m, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
    GET_AVG_MATRIX_SIZE3;
#include "function_wrapper_stats.c"
  return;
}


void pchemm_( const char *side, const char *uplo, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzhemm_( const char *side, const char *uplo, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pcherk_( const char *uplo, const char *trans, const int *n, const int *k, const float *alpha, const void *a, const int *ia, const int *ja, const int *desca, const float *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzherk_( const char *uplo, const char *trans, const int *n, const int *k, const double *alpha, const void *a, const int *ia, const int *ja, const int *desca, const double *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pcher2k_( const char *uplo, const char *trans, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const float *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzher2k_( const char *uplo, const char *trans, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const double *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pssymm_( const char *side, const char *uplo, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *b, const int *ib, const int *jb, const int *descb, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pdsymm_( const char *side, const char *uplo, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *b, const int *ib, const int *jb, const int *descb, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pcsymm_( const char *side, const char *uplo, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzsymm_( const char *side, const char *uplo, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pssyrk_( const char *uplo, const char *trans, const int *n, const int *k, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pdsyrk_( const char *uplo, const char *trans, const int *n, const int *k, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pcsyrk_( const char *uplo, const char *trans, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzsyrk_( const char *uplo, const char *trans, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pssyr2k_( const char *uplo, const char *trans, const int *n, const int *k, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *b, const int *ib, const int *jb, const int *descb, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pdsyr2k_( const char *uplo, const char *trans, const int *n, const int *k, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *b, const int *ib, const int *jb, const int *descb, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pcsyr2k_( const char *uplo, const char *trans, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzsyr2k_( const char *uplo, const char *trans, const int *n, const int *k, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *b, const int *ib, const int *jb, const int *descb, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, n, k, alpha, a, ia, ja, desca, b, ib, jb, descb, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pstran_( const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pdtran_( const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pctranu_( const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pztranu_( const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pctranc_( const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pztranc_( const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pstrmm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, float *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pdtrmm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, double *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pctrmm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, void *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pztrmm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, void *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pstrsm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, float *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pdtrsm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, double *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pctrsm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, void *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void pztrsm_( const char *side, const char *uplo, const char *transa, const char *diag, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, void *b, const int *ib, const int *jb, const int *descb )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( side, uplo, transa, diag, m, n, alpha, a, ia, ja, desca, b, ib, jb, descb );
#include "function_wrapper_body2.c" 
  return;
}


void psgeadd_( const char *trans, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pdgeadd_( const char *trans, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pcgeadd_( const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pzgeadd_( const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pstradd_( const char *uplo, const char *trans, const int *m, const int *n, const float *alpha, const float *a, const int *ia, const int *ja, const int *desca, const float *beta, float *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pdtradd_( const char *uplo, const char *trans, const int *m, const int *n, const double *alpha, const double *a, const int *ia, const int *ja, const int *desca, const double *beta, double *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pctradd_( const char *uplo, const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


void pztradd_( const char *uplo, const char *trans, const int *m, const int *n, const void *alpha, const void *a, const int *ia, const int *ja, const int *desca, const void *beta, void *c, const int *ic, const int *jc, const int *descc )
{
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f( uplo, trans, m, n, alpha, a, ia, ja, desca, beta, c, ic, jc, descc );
#include "function_wrapper_body2.c" 
  return;
}


/* PBLAS Auxiliary Routines */
  
int pilaenv_( int *ictxt, char *prec )
{
  int (*orig_f)()=NULL;
  int result;
#include "function_wrapper_body1.c" 
  result=orig_f( ictxt, prec );
#include "function_wrapper_body2.c" 
  return result;
}

