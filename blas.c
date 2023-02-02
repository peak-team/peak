
#include "simpleperf.h"
#include "hash.h"
#include "complex.h"

#define func_group "BLAS"

// 
//BLAS level 1 functions 
//
#include "blas_wrapper/blas_level1.c"


// 
//BLAS level 2 functions 
//
#include "blas_wrapper/blas_level2.c"

// 
//BLAS level 3 functions 
//
#include "blas_wrapper/blas_level3.c"
