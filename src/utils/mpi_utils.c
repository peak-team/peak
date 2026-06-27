#include "mpi_utils.h"

static int
env_is_set(const char* name)
{
    char* value = getenv(name);
    return value != NULL && value[0] != '\0';
}

int check_MPI()
{
    static const char* rank_envs[] = {
        "PMI_RANK",
        "PMIX_RANK",
        "MV2_COMM_WORLD_RANK",
        "OMPI_COMM_WORLD_RANK",
        "I_MPI_RANK",
        NULL
    };

    for (const char** rank_env = rank_envs; *rank_env != NULL; rank_env++) {
        if (env_is_set(*rank_env)) {
            return 1;
        }
    }

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
