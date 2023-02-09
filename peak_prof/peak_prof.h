

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

#define MAX_LAYER 20
void lib_init();
extern bool peakprof_init_flag;
extern int peakprof_mkl_tune;
extern int peakprof_debug;
extern double apptime;
extern double libtime;
extern double layer_time[MAX_LAYER];
extern int layer_count;
//extern FILE *bpfile;

double mysecond();


