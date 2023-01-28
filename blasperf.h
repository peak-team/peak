

#define _GNU_SOURCE
//#define USE_MKL 1

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
extern omp_lock_t lock;
#endif

void blas_init();
extern bool blasperf_init_flag;
extern int blasperf_mkl_tune;
extern int blasperf_debug;
extern double apptime;
//extern FILE *bpfile;

double mysecond();


