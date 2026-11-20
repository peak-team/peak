#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <mpi.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, visibility("default")))
#endif
void
peak_mpi_active_clock_test_target(volatile int *work)
{
    *work += 1;
}

static void
pause_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000,
    };
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
    }
}

int
main(void)
{
    int rank, size, provided;
    if (MPI_Init_thread(NULL, NULL, MPI_THREAD_FUNNELED, &provided) != MPI_SUCCESS) {
        return 1;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != 2 ||
        (provided != MPI_THREAD_SINGLE && provided != MPI_THREAD_FUNNELED)) {
        fprintf(stderr, "expected two ranks with SINGLE or FUNNELED, got %d ranks and thread level %d\n",
                size, provided);
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }

    /* Align the ranks without calling any interposed communication routine
     * before the Wait being tested. */
    MPI_Request request;
    int complete = 0;
    if (MPI_Ibarrier(MPI_COMM_WORLD, &request) != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    do {
        if (MPI_Test(&request, &complete, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
            MPI_Abort(MPI_COMM_WORLD, 1);
            return 1;
        }
    } while (!complete);

    volatile int work = 0;
    peak_mpi_active_clock_test_target(&work);
    if (rank == 1) {
        pause_ms(350);
    }

    int input = rank + 1;
    int sum = 0;
    if (MPI_Iallreduce(&input, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD,
                       &request) != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    const double start = MPI_Wtime();
    if (MPI_Wait(&request, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    const double waited = MPI_Wtime() - start;
    peak_mpi_active_clock_test_target(&work);

    /* Give the global heartbeat several observations after Wait completes. */
    pause_ms(120);
    if (sum != 3 || work != 2 || MPI_Finalize() != MPI_SUCCESS) {
        return 1;
    }
    printf("mpi_active_clock_ok rank=%d pid=%ld wait=%.9g\n",
           rank, (long)getpid(), waited);
    return 0;
}
