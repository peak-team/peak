#ifndef __TEST_CBLAS_H
#define __TEST_CBLAS_H

#include "FC.h"

#ifdef __cplusplus
extern "C" {
#endif

float cblas_sdot(const int n, const float *x, const int incx,
                  const float *y, const int incy);

#ifdef __cplusplus
}
#endif

#endif