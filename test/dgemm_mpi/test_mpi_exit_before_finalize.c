#include <glob.h>
#include <mpi.h>
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
static int peak_mpi_exit_target_delay_us;

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

static int
peak_mpi_exit_aggregate_csv_is_published(void)
{
    static const char expected_header[] = "function,count,";
    const char* stats_prefix = getenv("PEAK_STATSLOG_PATH");
    char* pattern;
    glob_t matches = {0};
    int length;
    int published = 0;

    if (stats_prefix == NULL || stats_prefix[0] == '\0') {
        return 0;
    }
    length = snprintf(NULL, 0, "%s-p*.csv", stats_prefix);
    if (length < 0) {
        return 0;
    }
    pattern = malloc((size_t)length + 1);
    if (pattern == NULL) {
        return 0;
    }
    (void)snprintf(pattern, (size_t)length + 1,
                   "%s-p*.csv", stats_prefix);

    if (glob(pattern, 0, NULL, &matches) == 0) {
        for (size_t i = 0; i < matches.gl_pathc; i++) {
            char header[sizeof(expected_header)] = {0};
            FILE* csv;

            /* A temporary writer is never evidence of atomic publication. */
            if (strstr(matches.gl_pathv[i], ".tmp.") != NULL) {
                continue;
            }
            csv = fopen(matches.gl_pathv[i], "r");
            if (csv == NULL) {
                continue;
            }
            if (fread(header, 1, sizeof(expected_header) - 1, csv) ==
                    sizeof(expected_header) - 1 &&
                memcmp(header,
                       expected_header,
                       sizeof(expected_header) - 1) == 0) {
                published = 1;
            }
            (void)fclose(csv);
            if (published) {
                break;
            }
        }
    }
    globfree(&matches);
    free(pattern);
    return published;
}

static int
peak_mpi_exit_record_finalize_entry(int rank)
{
    const char* marker_prefix =
        getenv("PEAK_MPI_FINALIZE_ENTER_MARKER_PREFIX");
    char* marker_path;
    FILE* marker;
    int length;
    int marker_written;

    if (marker_prefix == NULL || marker_prefix[0] == '\0') {
        return 0;
    }
    length = snprintf(NULL, 0, "%s-r%d.txt", marker_prefix, rank);
    if (length < 0) {
        return 0;
    }
    marker_path = malloc((size_t)length + 1);
    if (marker_path == NULL) {
        return 0;
    }
    (void)snprintf(marker_path,
                   (size_t)length + 1,
                   "%s-r%d.txt",
                   marker_prefix,
                   rank);
    marker = fopen(marker_path, "w");
    free(marker_path);
    if (marker == NULL) {
        return 0;
    }
    marker_written = fprintf(marker, "rank=%d entered=1\n", rank) > 0;
    if (fclose(marker) != 0) {
        marker_written = 0;
    }
    return marker_written;
}

static int
peak_mpi_exit_record_finalize_return(int rank, int finalize_result)
{
    const char* marker_prefix =
        getenv("PEAK_MPI_FINALIZE_RETURN_MARKER_PREFIX");
    int published = peak_mpi_exit_aggregate_csv_is_published();
    char* marker_path;
    FILE* marker;
    int length;
    int marker_written;

    if (marker_prefix == NULL || marker_prefix[0] == '\0') {
        return 0;
    }
    length = snprintf(NULL, 0, "%s-r%d.txt", marker_prefix, rank);
    if (length < 0) {
        return 0;
    }
    marker_path = malloc((size_t)length + 1);
    if (marker_path == NULL) {
        return 0;
    }
    (void)snprintf(marker_path,
                   (size_t)length + 1,
                   "%s-r%d.txt",
                   marker_prefix,
                   rank);
    marker = fopen(marker_path, "w");
    free(marker_path);
    if (marker == NULL) {
        return 0;
    }
    marker_written = fprintf(marker,
                             "rank=%d finalize_rc=%d final_csv_published=%d\n",
                             rank,
                             finalize_result,
                             published) > 0;
    if (fclose(marker) != 0) {
        marker_written = 0;
    }
    return marker_written && finalize_result == MPI_SUCCESS &&
           (rank != 0 || published);
}

void PEAK_EXPORT PEAK_NOINLINE
peak_mpi_exit_target(int rank)
{
    if (peak_mpi_exit_target_delay_us > 0) {
        usleep((useconds_t)peak_mpi_exit_target_delay_us);
    }
    peak_mpi_exit_sink += rank + 1;
}

int
main(int argc, char** argv)
{
    int rank = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int uneven_rank_calls =
        argc > 1 && strstr(argv[1], "uneven") != NULL;
    peak_mpi_exit_target_delay_us = peak_mpi_exit_parse_loop_count(
        "PEAK_MPI_EXIT_TARGET_DELAY_US", 0);
    int loops = peak_mpi_exit_loop_count();
    if (!uneven_rank_calls || rank == 0) {
        for (int i = 0; i < loops; i++) {
            peak_mpi_exit_target(rank);
        }
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

    if (argc > 1 &&
        (strcmp(argv[1], "finalize-then-exit0") == 0 ||
         strcmp(argv[1], "finalize-uneven-then-exit0") == 0)) {
        MPI_Finalize();
        exit(0);
    }

    if (argc > 1 &&
        strcmp(argv[1], "finalize-publish-before-root-return") == 0) {
        int finalize_entry_was_recorded =
            peak_mpi_exit_record_finalize_entry(rank);
        int finalize_result = MPI_Finalize();
        int finalize_return_was_valid = peak_mpi_exit_record_finalize_return(
            rank, finalize_result);

        exit(finalize_entry_was_recorded && finalize_return_was_valid ? 0 : 2);
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

    if (argc > 1 &&
        (strcmp(argv[1], "finalize-post-work-then-exit0") == 0 ||
         strcmp(argv[1], "finalize-post-work-uneven-then-exit0") == 0)) {
        MPI_Finalize();
        int post_loops = peak_mpi_exit_post_finalize_loop_count();
        if (!uneven_rank_calls || rank == 0) {
            for (int i = 0; i < post_loops; i++) {
                peak_mpi_exit_target(rank);
            }
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
