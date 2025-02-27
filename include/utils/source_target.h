#ifndef SOURCE_TARGET_H
#define SOURCE_TARGET_H

#include <stddef.h>

extern char* source_target_array_BLAS[];
extern size_t source_count_BLAS;

extern char* source_target_array_LAPACK[];
extern size_t source_count_LAPACK;

extern char* source_target_array_PBLAS[];
extern size_t source_count_PBLAS;

extern char* source_target_array_ScaLAPACK[];
extern size_t source_count_ScaLAPACK;

extern char* source_target_array_FFTW[];
extern size_t source_count_FFTW;

#endif // SOURCE_TARGET_H