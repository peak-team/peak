#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/mpi_report_transport.h"
#include "internal/general_listener/report_formatter.h"

#include <float.h>
#include <math.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int collective_observer_count;
static int collective_observer_failures;
static int collective_observer_target_seen;

static char*
read_stream(FILE* stream)
{
    long length;
    char* contents;

    if (fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0 || fseek(stream, 0, SEEK_SET) != 0) {
        return NULL;
    }
    contents = malloc((size_t)length + 1);
    if (contents == NULL ||
        fread(contents, 1, (size_t)length, stream) != (size_t)length) {
        free(contents);
        return NULL;
    }
    contents[length] = '\0';
    return contents;
}

static int
validate_root_formatting(PeakReportSnapshot* aggregate)
{
    PeakReportFormatOptions options = {.print_text = true};
    char stats_base[160];
    char stats_path[192];
    FILE* csv = NULL;
    FILE* capture = NULL;
    char* csv_text = NULL;
    char* report_text = NULL;
    int saved_stderr = -1;
    int stats_base_length;
    int stats_path_length;
    int failures = 0;

    stats_base_length = snprintf(stats_base,
                                 sizeof(stats_base),
                                 "/tmp/peak-mpi-report-transport-%ld",
                                 (long)getpid());
    stats_path_length = snprintf(stats_path,
                                 sizeof(stats_path),
                                 "%s-p%ld.csv",
                                 stats_base,
                                 (long)getpid());
    if (stats_base_length < 0 ||
        (size_t)stats_base_length >= sizeof(stats_base) ||
        stats_path_length < 0 ||
        (size_t)stats_path_length >= sizeof(stats_path) ||
        setenv("PEAK_STATSLOG_PATH", stats_base, 1) != 0) {
        return 1;
    }

    peak_report_snapshot_prepare_for_render(aggregate);
    if (!peak_report_formatter_write_csv(aggregate)) {
        failures++;
    } else {
        csv = fopen(stats_path, "r");
        if (csv == NULL || (csv_text = read_stream(csv)) == NULL ||
            strstr(csv_text, "\"beta\",") == NULL) {
            failures++;
        }
    }
    free(csv_text);
    if (csv != NULL && fclose(csv) != 0) {
        failures++;
    }
    if (unlink(stats_path) != 0) {
        failures++;
    }

    capture = tmpfile();
    if (capture == NULL || fflush(stderr) != 0 ||
        (saved_stderr = dup(STDERR_FILENO)) < 0 ||
        dup2(fileno(capture), STDERR_FILENO) < 0) {
        failures++;
    } else {
        if (!peak_report_formatter_write_text(aggregate, &options) ||
            fflush(stderr) != 0) {
            failures++;
        }
    }
    if (saved_stderr >= 0 && dup2(saved_stderr, STDERR_FILENO) < 0) {
        failures++;
    }
    if (capture != NULL &&
        ((report_text = read_stream(capture)) == NULL ||
         strstr(report_text, "beta") == NULL)) {
        failures++;
    }
    if (saved_stderr >= 0 && close(saved_stderr) != 0) {
        failures++;
    }
    free(report_text);
    if (capture != NULL && fclose(capture) != 0) {
        failures++;
    }
    if (unsetenv("PEAK_STATSLOG_PATH") != 0) {
        failures++;
    }
    return failures;
}

void
peak_mpi_report_transport_test_observe_collective(
    const char* label,
    int kind,
    int count,
    MPI_Datatype datatype,
    MPI_Op operation,
    int root,
    const void* original_send,
    void* original_receive,
    const void* staged_send,
    void* staged_receive,
    size_t buffer_size)
{
    const char* abandon_label =
        getenv("PEAK_TEST_MPI_REDUCER_ABANDON_LABEL");

    (void)count;
    (void)datatype;
    (void)operation;
    (void)root;
    collective_observer_count++;
    if (label == NULL || staged_receive == NULL ||
        staged_receive == original_receive) {
        collective_observer_failures++;
        return;
    }
    if (kind == 2) {
        if (staged_send != NULL ||
            (buffer_size != 0 &&
             memcmp(staged_receive, original_receive, buffer_size) != 0)) {
            collective_observer_failures++;
        }
    } else if (staged_send == NULL || staged_send == original_send ||
               (buffer_size != 0 &&
                memcmp(staged_send, original_send, buffer_size) != 0)) {
        collective_observer_failures++;
    }
    if (abandon_label != NULL && strcmp(label, abandon_label) == 0) {
        collective_observer_target_seen = 1;
    }
}

static int
close_enough(double actual, double expected)
{
    return fabs(actual - expected) <= 1e-6;
}

static PeakReportSnapshot*
fixture_snapshot(int rank)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(2);

    if (snapshot == NULL ||
        !peak_report_snapshot_set_program(snapshot, "mpi-fixture") ||
        !peak_report_snapshot_set_name(snapshot, 0, "alpha") ||
        !peak_report_snapshot_set_name(snapshot, 1, "beta")) {
        peak_report_snapshot_destroy(snapshot);
        return NULL;
    }

    snapshot->instrumented[0] = 1;
    snapshot->instrumented[1] = rank != 0;
    snapshot->detached[0] = rank == 1;
    snapshot->reattached[0] = rank == 2;
    snapshot->revisited[1] = rank > 0;
    snapshot->num_calls[0] = (unsigned long)(rank + 1);
    snapshot->num_calls[1] = rank == 0 ? 0UL : (unsigned long)(10 + rank);
    snapshot->total_time[0] = 1.0 + (double)rank;
    snapshot->total_time[1] = rank == 0 ? 0.0 : 2.0 + (double)rank;
    snapshot->max_total_time[0] = 3.0 + (double)rank;
    snapshot->max_total_time[1] = rank == 0 ? 0.0 : 4.0 + (double)rank;
    snapshot->min_total_time[0] = 0.5 + (double)rank;
    snapshot->min_total_time[1] =
        rank == 0 ? DBL_MAX : 0.75 + (double)rank;
    snapshot->exclusive_time[0] = 0.25 + (double)rank;
    snapshot->exclusive_time[1] = rank == 0 ? 0.0 : 0.5 + (double)rank;
    snapshot->max_time[0] = 0.1F + (float)rank;
    snapshot->max_time[1] = rank == 0 ? 0.0F : 0.2F + (float)rank;
    snapshot->min_time[0] = 0.01F + (float)rank;
    snapshot->min_time[1] = rank == 0 ? FLT_MAX : 0.02F + (float)rank;
    snapshot->thread_count[0] = (unsigned long)(rank + 1);
    snapshot->thread_count[1] =
        rank == 0 ? 0UL : (unsigned long)(rank + 2);
    snapshot->overhead_per_call = 0.001 + (double)rank;

    snapshot->overhead.valid = true;
    snapshot->overhead.accounting_valid = true;
    snapshot->overhead.local_ranks = (unsigned int)(rank + 1);
    snapshot->overhead.stop_window_count = (uint64_t)(100 + rank);
    snapshot->overhead.failed_stop_window_count = (uint64_t)(rank + 1);
    snapshot->overhead.elapsed_seconds = 10.0 + (double)rank;
    snapshot->overhead.elapsed_min_seconds =
        snapshot->overhead.elapsed_seconds;
    snapshot->overhead.elapsed_max_seconds =
        snapshot->overhead.elapsed_seconds;
    snapshot->overhead.profile_seconds = 1.0 + (double)rank;
    snapshot->overhead.control_seconds = 2.0 + (double)rank;
    snapshot->overhead.management_seconds = 3.0 + (double)rank;
    snapshot->overhead.control_risk_seconds = 4.0 + (double)rank;
    snapshot->overhead.profile_control_risk_seconds =
        5.0 + (double)rank;
    snapshot->overhead.ratio = 0.10 + 0.01 * (double)rank;
    snapshot->overhead.profile_ratio = 0.50 - 0.01 * (double)rank;
    snapshot->overhead.control_ratio = 0.20 + 0.01 * (double)rank;
    snapshot->overhead.management_ratio = 0.30 - 0.01 * (double)rank;
    snapshot->overhead.profile_control_risk_ratio =
        0.40 + 0.01 * (double)rank;
    snapshot->overhead.control_risk_ratio =
        0.60 - 0.01 * (double)rank;
    return snapshot;
}

static int
validate_root_aggregate(const PeakReportSnapshot* aggregate, int size)
{
    unsigned long rank_sum =
        (unsigned long)size * (unsigned long)(size - 1) / 2UL;
    double double_rank_sum = (double)size * (double)(size - 1) / 2.0;
    int last_rank = size - 1;
    int failures = 0;

    if (aggregate == NULL || aggregate->rank_count != size ||
        aggregate->hook_count != 2 ||
        strcmp(aggregate->program, "mpi-fixture") != 0 ||
        strcmp(aggregate->names[0], "alpha") != 0 ||
        strcmp(aggregate->names[1], "beta") != 0 ||
        !close_enough(aggregate->overhead_per_call, 0.001)) {
        fprintf(stderr, "root aggregate metadata validation failed\n");
        failures++;
    }
    if (aggregate == NULL) {
        return failures;
    }
    if (aggregate->num_calls[0] != rank_sum + (unsigned long)size ||
        aggregate->num_calls[1] !=
            10UL * (unsigned long)(size - 1) + rank_sum ||
        !close_enough(aggregate->total_time[0],
                      (double)size + double_rank_sum) ||
        !close_enough(aggregate->total_time[1],
                      2.0 * (double)(size - 1) + double_rank_sum) ||
        !close_enough(aggregate->exclusive_time[1],
                      0.5 * (double)(size - 1) + double_rank_sum) ||
        !close_enough(aggregate->max_total_time[0],
                      3.0 + (double)last_rank) ||
        !close_enough(aggregate->max_total_time[1],
                      size > 1 ? 4.0 + (double)last_rank : 0.0) ||
        !close_enough(aggregate->min_total_time[1],
                      size > 1 ? 1.75 : DBL_MAX) ||
        !close_enough(aggregate->max_time[1],
                      size > 1 ? 0.2 + (double)last_rank : 0.0) ||
        !close_enough(aggregate->min_time[0], 0.01) ||
        !close_enough(aggregate->min_time[1],
                      size > 1 ? 1.02 : FLT_MAX) ||
        aggregate->thread_count[1] !=
            2UL * (unsigned long)(size - 1) + rank_sum) {
        fprintf(stderr, "root aggregate metric validation failed\n");
        failures++;
    }
    if (!aggregate->instrumented[0] ||
        aggregate->instrumented[1] != (size > 1) ||
        (size > 1 && !aggregate->detached[0]) ||
        (size > 2 && !aggregate->reattached[0]) ||
        (size > 1 && !aggregate->revisited[1])) {
        fprintf(stderr, "root aggregate marker validation failed\n");
        failures++;
    }
    if (!aggregate->overhead.valid ||
        !aggregate->overhead.accounting_valid ||
        !aggregate->overhead.per_rank_max ||
        aggregate->overhead.failed_stop_window_count !=
            rank_sum + (uint64_t)size ||
        !close_enough(aggregate->overhead.elapsed_min_seconds, 10.0) ||
        !close_enough(aggregate->overhead.elapsed_max_seconds,
                      10.0 + (double)last_rank) ||
        !close_enough(aggregate->overhead.profile_seconds,
                      (double)size + double_rank_sum) ||
        aggregate->overhead.per_rank_maxima.owner_ranks[
            PEAK_REPORT_METRIC_COMBINED] != last_rank ||
        aggregate->overhead.per_rank_maxima.owner_ranks[
            PEAK_REPORT_METRIC_PROFILE] != 0 ||
        aggregate->overhead.per_rank_maxima.owner_ranks[
            PEAK_REPORT_METRIC_CONTROL] != last_rank ||
        aggregate->overhead.per_rank_maxima.owner_ranks[
            PEAK_REPORT_METRIC_MANAGEMENT] != 0 ||
        aggregate->overhead.per_rank_maxima.owner_ranks[
            PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK] != last_rank ||
        aggregate->overhead.per_rank_maxima.owner_ranks[
            PEAK_REPORT_METRIC_CONTROL_RISK] != 0) {
        fprintf(stderr, "root aggregate overhead validation failed\n");
        failures++;
    }
    return failures;
}

static int
run_fail_closed_case(int rank, const char* label, bool abandon_active_request)
{
    PeakReportSnapshot* local = fixture_snapshot(rank);
    PeakReportSnapshot* aggregate = NULL;
    PeakMpiReportTransportResult result;
    int failures = local == NULL ? 1 : 0;

    peak_mpi_report_transport_reset_failed_closed();
    if (unsetenv("PEAK_TEST_MPI_REDUCER_FAIL_LABEL") != 0 ||
        unsetenv("PEAK_TEST_MPI_REDUCER_ABANDON_LABEL") != 0 ||
        setenv(abandon_active_request
                   ? "PEAK_TEST_MPI_REDUCER_ABANDON_LABEL"
                   : "PEAK_TEST_MPI_REDUCER_FAIL_LABEL",
               label,
               1) != 0) {
        failures++;
    }
    if (local != NULL) {
        result = peak_mpi_report_transport_reduce(local, &aggregate);
        if (result != PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED ||
            aggregate != NULL ||
            !peak_mpi_report_transport_failed_closed()) {
            failures++;
        }
        if (abandon_active_request &&
            (peak_mpi_report_transport_quarantined_request_count() != 1 ||
             !collective_observer_target_seen ||
             collective_observer_failures != 0)) {
            failures++;
        }
        if (abandon_active_request) {
            peak_mpi_report_transport_reset_failed_closed();
            if (!peak_mpi_report_transport_failed_closed()) {
                failures++;
            }
        }
        result = peak_mpi_report_transport_reduce(local, &aggregate);
        if (result != PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED ||
            aggregate != NULL) {
            failures++;
        }
    }
    peak_report_snapshot_destroy(local);

    /* The poisoned-path contract deliberately forbids MPI_Finalize here. */
    if (rank == 0 && failures == 0) {
        puts(abandon_active_request
                 ? "mpi_report_transport_active_request_fail_closed_ok"
                 : "mpi_report_transport_fail_closed_ok");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main(int argc, char** argv)
{
    PeakReportSnapshot* local;
    PeakReportSnapshot* aggregate = NULL;
    PeakReportSnapshot* root_aggregate = NULL;
    PeakMpiReportTransportResult result;
    int rank;
    int size;
    int failures = 0;
    int global_failures = 0;

    if (setenv("PEAK_VERBOSITY", "warn", 1) != 0) {
        return EXIT_FAILURE;
    }
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size < 3) {
        if (rank == 0) {
            fprintf(stderr,
                    "test_mpi_report_transport requires at least three ranks\n");
        }
        MPI_Finalize();
        return EXIT_FAILURE;
    }
    if (argc > 1 && strcmp(argv[1], "fail-closed") == 0) {
        const char* label = argc > 2 ? argv[2] : "accounting-valid";

        return run_fail_closed_case(rank, label, false);
    }
    if (argc > 1 && strcmp(argv[1], "active-request-fail-closed") == 0) {
        const char* label = argc > 2 ? argv[2] : "accounting-valid";

        return run_fail_closed_case(rank, label, true);
    }

    peak_mpi_report_transport_reset_failed_closed();
    local = fixture_snapshot(rank);
    if (local == NULL) {
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    result = peak_mpi_report_transport_reduce(local, &aggregate);
    if (collective_observer_count != 54 ||
        collective_observer_failures != 0) {
        failures++;
    }
    if (rank == 0) {
        if (result != PEAK_MPI_REPORT_TRANSPORT_ROOT_READY) {
            failures++;
        }
        failures += validate_root_aggregate(aggregate, size);
        if (result == PEAK_MPI_REPORT_TRANSPORT_ROOT_READY &&
            aggregate != NULL) {
            root_aggregate = aggregate;
            aggregate = NULL;
        }
    } else if (result != PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE ||
               aggregate != NULL) {
        failures++;
    }
    peak_report_snapshot_destroy(aggregate);
    aggregate = NULL;

    if (rank == size - 1 &&
        !peak_report_snapshot_set_name(local, 1, "rank-mismatch")) {
        failures++;
    }
    result = peak_mpi_report_transport_reduce(local, &aggregate);
    if (result != PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK ||
        aggregate != NULL || peak_mpi_report_transport_failed_closed()) {
        failures++;
    }

    if (!peak_report_snapshot_set_name(local, 0, "duplicate") ||
        !peak_report_snapshot_set_name(local, 1, "duplicate")) {
        failures++;
    }
    result = peak_mpi_report_transport_reduce(local, &aggregate);
    if (result != PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK ||
        aggregate != NULL || peak_mpi_report_transport_failed_closed()) {
        failures++;
    }
    peak_report_snapshot_destroy(local);
    if (rank == 0 && root_aggregate != NULL) {
        failures += validate_root_formatting(root_aggregate);
    }
    peak_report_snapshot_destroy(root_aggregate);

    MPI_Reduce(&failures,
               &global_failures,
               1,
               MPI_INT,
               MPI_SUM,
               0,
               MPI_COMM_WORLD);
    if (rank == 0 && global_failures == 0) {
        puts("mpi_report_transport_ok");
    } else if (rank == 0) {
        fprintf(stderr,
                "mpi_report_transport observed %d validation failures\n",
                global_failures);
    }
    MPI_Finalize();
    return global_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
