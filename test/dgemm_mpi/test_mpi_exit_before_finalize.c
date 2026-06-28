#include <mpi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__)
#define PEAK_NOINLINE __attribute__((noinline))
#define PEAK_EXPORT __attribute__((visibility("default")))
#else
#define PEAK_NOINLINE
#define PEAK_EXPORT
#endif

static volatile int peak_mpi_exit_sink;

static int
peak_mpi_exit_parse_loop_count(const char* env_name, int default_value)
{
    const char* value = getenv(env_name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > 10000000L) {
        return default_value;
    }

    return (int)parsed;
}

static int
peak_mpi_exit_loop_count(void)
{
    return peak_mpi_exit_parse_loop_count("PEAK_MPI_EXIT_LOOPS", 16);
}

static void
peak_mpi_exit_wait_for_file(const char* path, int timeout_ms)
{
    if (path == NULL || path[0] == '\0' || timeout_ms <= 0) {
        return;
    }

    struct timespec delay = {0, 5 * 1000 * 1000};
    struct timespec start;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return;
    }
    while (1) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return;
        }
        double elapsed_ms =
            (double)(now.tv_sec - start.tv_sec) * 1000.0 +
            (double)(now.tv_nsec - start.tv_nsec) / 1.0e6;
        if (elapsed_ms >= (double)timeout_ms) {
            return;
        }
        if (access(path, F_OK) == 0) {
            return;
        }
        nanosleep(&delay, NULL);
    }
}

static int
peak_mpi_exit_post_finalize_loop_count(void)
{
    return peak_mpi_exit_parse_loop_count("PEAK_MPI_EXIT_POST_LOOPS", 32);
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

    if (argc > 1 &&
        strcmp(argv[1], "subset-finalize-then-exit0-handoff") == 0) {
        const char* done_file = getenv("PEAK_MPI_SUBSET_FINALIZE_DONE_FILE");
        if (rank == 0) {
            MPI_Finalize();
            if (done_file != NULL) {
                FILE* marker = fopen(done_file, "w");
                if (marker != NULL) {
                    fputs("done", marker);
                    fclose(marker);
                }
            }
            exit(0);
        }
        peak_mpi_exit_wait_for_file(done_file, 10000);
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "exec-failure-rank0") == 0) {
        if (rank == 0) {
            char* const bad_argv[] = {(char*)"/definitely/not/found", NULL};
            errno = 0;
            int result = execv("/definitely/not/found", bad_argv);
            int saved_errno = errno;
            if (result != -1 || saved_errno != ENOENT) {
                fprintf(stderr,
                        "mpi_exec_failure_rank0_bad_result result=%d errno=%d\n",
                        result,
                        saved_errno);
                MPI_Abort(MPI_COMM_WORLD, 3);
            }
            fprintf(stderr,
                    "mpi_exec_failure_rank0_errno=%d\n",
                    saved_errno);
            fflush(stderr);
        }
        MPI_Finalize();
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "exec-success-rank0") == 0) {
        if (rank == 0) {
            const char* child =
                getenv("PEAK_MPI_EXEC_SUCCESS_CHILD") != NULL ?
                    getenv("PEAK_MPI_EXEC_SUCCESS_CHILD") :
                    "/bin/true";
            char* const child_argv[] = {(char*)child, NULL};
            execv(child, child_argv);
            fprintf(stderr,
                    "mpi_exec_success_rank0_failed errno=%d\n",
                    errno);
            fflush(stderr);
            _exit(4);
        }
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "finalize-then-exit0") == 0) {
        MPI_Finalize();
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "finalize-token-mismatch-then-exit0") == 0) {
        if (rank == 1) {
            setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", "peak-token-mismatch", 1);
        } else {
            setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", "peak-token-match", 1);
        }
        MPI_Finalize();
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "finalize-post-work-then-exit0") == 0) {
        MPI_Finalize();
        int post_loops = peak_mpi_exit_post_finalize_loop_count();
        for (int i = 0; i < post_loops; i++) {
            peak_mpi_exit_target(rank);
        }
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
