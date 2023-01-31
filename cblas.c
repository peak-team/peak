#include "libperf.h"
#include "hash.h"


#define CBLAS_INT int32_t
#define CBLAS_INDEX size_t /* this may vary between platforms */
typedef enum CBLAS_LAYOUT {CblasRowMajor=101, CblasColMajor=102} CBLAS_LAYOUT;
typedef enum CBLAS_TRANSPOSE {CblasNoTrans=111, CblasTrans=112, CblasConjTrans=113} CBLAS_TRANSPOSE;
typedef enum CBLAS_UPLO {CblasUpper=121, CblasLower=122} CBLAS_UPLO;
typedef enum CBLAS_DIAG {CblasNonUnit=131, CblasUnit=132} CBLAS_DIAG;
typedef enum CBLAS_SIDE {CblasLeft=141, CblasRight=142} CBLAS_SIDE;
  
#define CBLAS_ORDER CBLAS_LAYOUT /* this for backward compatibility with CBLAS_ORDER */


#include "blas_wrapper/cblas_level1.c"
#include "blas_wrapper/cblas_level2.c"
#include "blas_wrapper/cblas_level3.c"
