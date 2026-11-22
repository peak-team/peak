#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <mpi.h>
#include <stdio.h>
#include <string.h>
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

static void
check_mpi(int result)
{
    if (result != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
}

static double
wait_for_sum(int rank, int use_pmpi)
{
    MPI_Request request;
    int input = rank + 1;
    int sum = 0;
    check_mpi(MPI_Iallreduce(&input, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD,
                             &request));
    double start = MPI_Wtime();
    check_mpi(use_pmpi ? PMPI_Wait(&request, MPI_STATUS_IGNORE)
                       : MPI_Wait(&request, MPI_STATUS_IGNORE));
    double waited = MPI_Wtime() - start;
    check_mpi(sum == 3 ? MPI_SUCCESS : MPI_ERR_OTHER);
    return waited;
}

int
main(int argc, char **argv)
{
    int dense = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dense") == 0)
            dense = 1;
    }
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
    double dense_time = 0.0;
    if (dense) {
        int send = rank + 1;
        int recv = 0;
        double start = MPI_Wtime();
        for (int i = 0; i < 32768; ++i) {
            check_mpi(MPI_Allreduce(&send, &recv, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD));
        }
        dense_time = MPI_Wtime() - start;
        check_mpi(recv == 3 ? MPI_SUCCESS : MPI_ERR_OTHER);
    }

    if (rank == 1) {
        pause_ms(350);
    }
    double waited = wait_for_sum(rank, dense);
    double gap = 0.0;
    double second_wait = 0.0;
    if (dense) {
        /* Give rank 0 a short application interval between long waits. */
        double start = MPI_Wtime();
        pause_ms(rank == 0 ? 60 : 350);
        gap = MPI_Wtime() - start;
        second_wait = wait_for_sum(rank, 0);
    }
    peak_mpi_active_clock_test_target(&work);

    /* Check that the heartbeat recovers active credit without another MPI call. */
    double start = MPI_Wtime();
    pause_ms(dense ? 250 : 120);
    double recovery = MPI_Wtime() - start;
    if (work != 2 || MPI_Finalize() != MPI_SUCCESS) {
        return 1;
    }
    printf("mpi_active_clock_ok rank=%d pid=%ld wait=%.9g second_wait=%.9g "
           "dense=%.9g gap=%.9g recovery=%.9g\n",
           rank, (long)getpid(), waited, second_wait, dense_time, gap, recovery);
    return 0;
}
