#include "mpi_utils.h"

int check_MPI()
{
    char* pmi_rank = getenv("PMI_RANK");
    char* mvapich_rank = getenv("MV2_COMM_WORLD_RANK");
    char* ompi_rank = getenv("OMPI_COMM_WORLD_RANK");
    if (pmi_rank != NULL || mvapich_rank != NULL || ompi_rank != NULL)
        return 1;
    else
        return 0;
}
/* local ranks
    OMPI_COMM_WORLD_LOCAL_RANK
    MPI_LOCALRANKID
    MV2_COMM_WORLD_LOCAL_RANK
    SLURM_LOCALID
 */

int get_MPI_local_rank()
{
    char* pmi_rank = getenv("MPI_LOCALRANKID");
    char* mvapich_rank = getenv("MV2_COMM_WORLD_LOCAL_RANK");
    char* ompi_rank = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
    if (pmi_rank != NULL)
        return atoi(pmi_rank);
    else if (mvapich_rank != NULL)
        return atoi(mvapich_rank);
    else if (ompi_rank != NULL)
        return atoi(ompi_rank);
    else
        return -1;
}