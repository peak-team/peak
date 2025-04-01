#ifndef __CUDA_INTERCEPTOR_H
#define __CUDA_INTERCEPTOR_H

/**
 * @file cuda_interceptor.h
 * @brief Header file for CUDA function interception using Gum library
 */

#include "frida-gum.h"
#include "utils/cuda_utils.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <pthread.h>
#include <string.h>

#ifdef HAVE_MPI
#include <mpi.h>
#endif

#define max(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b;       \
})

#define min(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b;       \
})

/**
 * @brief Attach CUDA function interception
 *
 * This function attaches interception to the CUDA library functions using the Gum library.
 * Specifically, it intercepts the `cudaLaunchKernel` function and replaces it with a custom implementation,
 * `peak_cuda_launch_kernel`, which can be used to perform additional actions before calling the original function.
 *
 * @return 0 if the interception was successful, a negative number in the GumReplaceReturn otherwise.
 */
int cuda_interceptor_attach();

/**
 * @brief Detach CUDA function interception
 *
 * This function detaches the previously attached CUDA function interception and releases any resources used by the Gum library.
 *
 * @return void
 */
void cuda_interceptor_dettach();

/**
 * @brief Prints stored CUDA kernel launch configurations
 *
 * This function prints the results of the CUDA Interceptor for each kernel launch hook.
 *
 */
void cuda_interceptor_print();

#endif /* __CUDA_INTERCEPTOR_H */

