

#define _GNU_SOURCE
//#define USE_MKL 1

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>

#define MAX_LAYER 20   //max tracked nested calls
extern double layer_time[MAX_LAYER];

#define OUTFILE stdout

extern bool peakprof_init_flag;
extern int peakprof_debug;
extern int peakprof_mkl_fake;
extern int peakprof_record_rank;
extern double peakprof_record_threshold;
extern double apptime;
extern double libtime;
extern int layer_count;
//extern FILE *bpfile;

#include "util.h"


