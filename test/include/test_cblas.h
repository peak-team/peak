#ifndef __TEST_CBLAS_H
#define __TEST_CBLAS_H

#include "FC.h"

#ifdef __cplusplus
extern "C" {
#endif

enum CBLAS_ORDER {
   CblasRowMajor=101,
   CblasColMajor=102
};
typedef enum CBLAS_ORDER CBLAS_ORDER;
enum CBLAS_TRANSPOSE {
   CblasNoTrans=111,
   CblasTrans=112,
   CblasConjTrans=113,
   AtlasConj=114
};
typedef enum CBLAS_TRANSPOSE CBLAS_TRANSPOSE;

float cblas_sdot(const int n, const float* x, const int incx,
                 const float* y, const int incy);

void cblas_dgemm(const enum CBLAS_ORDER Order, const enum CBLAS_TRANSPOSE TransA,
                 const enum CBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const double alpha, const double *A,
                 const int lda, const double *B, const int ldb,
                 const double beta, double *C, const int ldc);

#ifdef __cplusplus
}
#endif

#endif