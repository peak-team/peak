#ifndef __MPI_INTERCEPTOR_H
#define __MPI_INTERCEPTOR_H

/**
 * @file mpi_interceptor.h
 * @brief Header file for MPI function interception using Gum library 
 */

#include "frida-gum.h"

#include <mpi.h>

/**
 * @brief Attach MPI function interception
 *
 * This function attaches interception to the MPI library functions using the Gum library.
 * Specifically, it intercepts the `PMPI_Finalize` function and replaces it with a custom implementation,
 * `peak_pmpi_finalize`, which can be used to perform additional actions before calling the original function.
 *
 * @return 0 if the interception was successful, a negative number in the GumReplaceReturn otherwise.
 */
int mpi_interceptor_attach();

/**
 * @brief Detach MPI function interception
 *
 * This function detaches the previously attached MPI function interception and releases any resources used by the Gum library.
 *
 * @return void
 */
void mpi_interceptor_dettach();

#endif /* __MPI_INTERCEPTOR_H */
