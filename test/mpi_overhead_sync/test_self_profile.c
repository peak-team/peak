#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <mpi.h>
#include <stdio.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) return 1;
    int rank, size, sum = -1;
    if (MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS ||
        MPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS) return 1;
    /* Read-only module observation: waiting here is protocol liveness evidence,
     * never a profiling-coverage or performance measurement. */
    int (*ever_ready)(void) = (int (*)(void))dlsym(
        RTLD_DEFAULT, "peak_mpi_overhead_sync_test_ever_ready");
    if (!ever_ready) return 1;
    struct timespec start, now, interval = {0, 5000000};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return 1;
    while (!ever_ready()) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;
        double elapsed = (double)(now.tv_sec-start.tv_sec) +
            (double)(now.tv_nsec-start.tv_nsec)/1e9;
        if (elapsed >= 5) {
            fprintf(stderr, "self_profile transport never became ready rank=%d\n", rank);
            return 1;
        }
        nanosleep(&interval, NULL);
    }
    int value = rank + 1;
    int status = MPI_Allreduce(&value, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    int valid = status == MPI_SUCCESS && size == 2 && sum == 3;
    printf("self_profile_science rank=%d valid=%d sum=%d pid=%ld\n",
           rank, valid, sum, (long)getpid());
    /* Let an ordinary HB consume the validated ACK before teardown. */
    struct timespec observe = {1, 0};
    nanosleep(&observe, NULL);
    if (MPI_Finalize() != MPI_SUCCESS) return 1;
    return valid ? 0 : 1;
}
