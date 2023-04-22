#ifndef __MPI_INTERCEPTOR_H
#define __MPI_INTERCEPTOR_H

/**
 * @file mpi_interceptor.h
 * @brief MPI Interceptor header file
 *
 * This header file defines the Peak General Listener and State structs and their associated functions.
 * It also contains the main entrance of the library for interception.
 */

#include "frida-gum.h"

#include <mpi.h>

/**
 * @brief Attaches the Peak General Listener.
 *
 * This function attaches the Peak General Listener to the function hooks specified
 * in `hook_strings`. It will record the number of times each function is called as
 * well as its total execution time in seconds. The time spent in multiple threads 
 * will be summed up.
 *
 * @return void
 */
int mpi_interceptor_attach();

/**
 * @brief Detaches the Peak General Listener.
 *
 * This function detaches the Peak General Listener and frees the memory allocated for it.
 *
 * @return void
 */
void mpi_interceptor_dettach();

#endif /* __MPI_INTERCEPTOR_H */
