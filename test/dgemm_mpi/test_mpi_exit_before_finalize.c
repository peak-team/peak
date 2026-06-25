#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define PEAK_NOINLINE __attribute__((noinline))
#define PEAK_EXPORT __attribute__((visibility("default")))
#else
#define PEAK_NOINLINE
#define PEAK_EXPORT
#endif

static volatile int peak_mpi_exit_sink;

static int
peak_mpi_exit_loop_count(void)
{
    const char* value = getenv("PEAK_MPI_EXIT_LOOPS");
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return 16;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > 10000000L) {
        return 16;
    }

    return (int)parsed;
}

void PEAK_EXPORT PEAK_NOINLINE
peak_mpi_exit_target(int rank)
{
    peak_mpi_exit_sink += rank + 1;
}

int
main(int argc, char** argv)
{
    int rank = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int loops = peak_mpi_exit_loop_count();
    for (int i = 0; i < loops; i++) {
        peak_mpi_exit_target(rank);
    }

    if (rank == 0) {
        fprintf(stderr, "mpi_exit_before_finalize_ready\n");
        fflush(stderr);
    }

    if (argc > 1 && strcmp(argv[1], "no-finalize-then-exit1") == 0) {
        exit(1);
    }

    if (argc > 1 && strcmp(argv[1], "subset-finalize-then-exit1") == 0) {
        if (rank == 0) {
            MPI_Finalize();
        }
        exit(1);
    }

    if (argc > 1 && strcmp(argv[1], "subset-finalize-then-exit0") == 0) {
        if (rank == 0) {
            MPI_Finalize();
        }
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "finalize-then-exit0") == 0) {
        MPI_Finalize();
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "finalize-then-return1") == 0) {
        MPI_Finalize();
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "finalize-then-exit1") == 0) {
        MPI_Finalize();
        exit(1);
    }

    exit(0);
}
