
/* A gettimeofday routine to give access to the wall
   clock timer on most UNIX-like systems.

   This version defines two entry points -- with 
   and without appended underscores, so it *should*
   automagically link with FORTRAN */

#include <sys/time.h>

double mysecond()
{
/* struct timeval { long        tv_sec;
            long        tv_usec;        };

struct timezone { int   tz_minuteswest;
             int        tz_dsttime;      };     */

        struct timeval tp;
        struct timezone tzp;
        int i;

        i = gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

double mysecond_() {return mysecond();}




#include <stdio.h>
#include <stdlib.h>

int check_MPI() {
    char* pmi_rank = getenv("PMI_RANK");
    //char* pmix_rank = getenv("PMIX_RANK");
    char* mvapich_rank = getenv("MV2_COMM_WORLD_RANK");
   // char* ompi_rank = getenv("OMPI_COMM_WORLD_RANK");
    //char* slurm_rank = getenv("SLURM_PROCID");
    if (pmi_rank != NULL  || mvapich_rank != NULL ) // || slurm_rank != NULL)
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


#include <string.h>
void get_argv0(char **argv0) {
    char* buffer = (char *)malloc(sizeof(char) * (1024));
    strcpy(buffer, "null\0");
    FILE *fp = fopen("/proc/self/cmdline", "r");
    if (!fp) {
        perror("fopen");
        *argv0 = buffer;
        return;
    }

    int n = fread(buffer, 1, 1024, fp);
    if (n == 0) {
        perror("fread");
        *argv0 = buffer;
        return;
    }
    buffer[n-1] = '\0';
    *argv0 = buffer;
}

