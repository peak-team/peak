#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

__attribute__((noinline, visibility("default")))
void peak_attach_mpi_target(void)
{
    __asm__ volatile(".rept 20; nop; .endr" ::: "memory");
}

/* Some transports reject fork once MPI is active. Keep that restriction
 * testable with any MPICH provider, including local development installs. */
static void reject_fork(void)
{
    static const char message[] = "unexpected fork in the active MPI rank\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(97);
}

int main(int argc, char **argv)
{
    int provided, rank, finalized;
    if (argc != 2 || MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE,
                                     &provided) != MPI_SUCCESS ||
        provided < MPI_THREAD_MULTIPLE ||
        MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS)
        return 1;
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0)
        return 1;
    if (pthread_atfork(reject_fork, NULL, NULL) != 0)
        return 1;

    char path[4096];
    snprintf(path, sizeof(path), "%s/pid-%d", argv[1], rank);
    FILE *ready = fopen(path, "w");
    if (ready == NULL)
        return 1;
    fprintf(ready, "%ld\n", (long)getpid());
    fclose(ready);
    snprintf(path, sizeof(path), "%s/release", argv[1]);
    for (int i = 0; access(path, F_OK) != 0; ++i) {
        if (i == 5000)
            return 1;
        peak_attach_mpi_target();
        usleep(10000);
    }
    for (int i = 0; i < 100; ++i)
        peak_attach_mpi_target();
    if (MPI_Finalize() != MPI_SUCCESS ||
        MPI_Finalized(&finalized) != MPI_SUCCESS || !finalized)
        return 1;
    printf("finalized rank=%d\n", rank);
    return 0;
}
