#ifndef PEAK_MPI_UTILS_H
#define PEAK_MPI_UTILS_H

/**
 * @file mpi_utils.h
 * @brief Provides utility functions related to MPI.
 */

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Checks for a nonempty MPI rank environment variable.
 *
 * The recognized variables are @c PMI_RANK, @c PMIX_RANK,
 * @c MV2_COMM_WORLD_RANK, @c OMPI_COMM_WORLD_RANK, and @c I_MPI_RANK. This is
 * an environment heuristic; it does not query MPI initialization. Empty rank
 * variables, size-only variables, and Slurm task variables are ignored.
 *
 * @retval 1 At least one recognized rank variable is nonempty.
 * @retval 0 No recognized rank variable is nonempty.
 */
int check_MPI();

/**
 * @brief Gets a local rank from launcher environment variables.
 *
 * Variables are checked in this order: @c MPI_LOCALRANKID,
 * @c MV2_COMM_WORLD_LOCAL_RANK, then @c OMPI_COMM_WORLD_LOCAL_RANK. The first
 * variable that is set is converted with atoi(); an empty or nonnumeric value
 * consequently produces 0. Out-of-range input has the undefined or
 * implementation-specific behavior of atoi().
 *
 * @return The converted value of the first set variable, or -1 when none is set.
 */
int get_MPI_local_rank();

#ifdef __cplusplus
}
#endif

#endif /* PEAK_MPI_UTILS_H */
