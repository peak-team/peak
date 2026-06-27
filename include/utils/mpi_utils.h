#ifndef __MPI_UTILS_H
#define __MPI_UTILS_H

/**
 * @file mpi_utils.h
 * @brief Provides utility functions related to MPI.
 */

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Checks if MPI is being used.
 *
 * This function checks for explicit MPI rank environment variables from PMI,
 * PMIx, MVAPICH, Open MPI, and Intel MPI. Size-only variables and Slurm task
 * variables are intentionally ignored because they can be present in launcher
 * or wrapper processes that are not MPI ranks.
 *
 * @return 1 if MPI is being used, otherwise 0.
 */
int check_MPI();

/**
 * @brief Gets the local rank when using MPI.
 *
 * This function attempts to retrieve the local rank of the MPI process from the MPI_LOCALRANKID,
 * MV2_COMM_WORLD_LOCAL_RANK, or OMPI_COMM_WORLD_LOCAL_RANK environment variable, in that order.
 *
 * @return The local rank of the MPI process, or -1 if MPI is not being used or the local rank cannot be determined.
 */
int get_MPI_local_rank();

#endif /* __MPI_UTILS_H */
