#include "internal/general_listener/socket_report_transport.h"

#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    TEST_ROOT_COMMIT = 0,
    TEST_ROOT_ABORT,
    TEST_ROOT_COMMIT_FAILURE,
} TestRootAction;

static void
clear_rank_environment(void)
{
    static const char* names[] = {
        "PMI_SIZE",
        "PMIX_SIZE",
        "OMPI_COMM_WORLD_SIZE",
        "SLURM_NTASKS",
        "PMI_RANK",
        "PMIX_RANK",
        "OMPI_COMM_WORLD_RANK",
        "MV2_COMM_WORLD_RANK",
        "SLURM_PROCID",
        NULL,
    };

    for (size_t i = 0; names[i] != NULL; i++) {
        (void)unsetenv(names[i]);
    }
}

static void
set_test_rank(int rank)
{
    char rank_text[16];

    snprintf(rank_text, sizeof(rank_text), "%d", rank);
    (void)setenv("PMI_SIZE", "2", 1);
    (void)setenv("PMI_RANK", rank_text, 1);
}

static PeakReportSnapshot*
fixture_snapshot(int rank, bool mismatch_name)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(3);
    double scale = rank == 0 ? 1.0 : 2.0;

    if (snapshot == NULL ||
        !peak_report_snapshot_set_program(
            snapshot,
            rank == 0 ? "root-program" : "peer-program") ||
        !peak_report_snapshot_set_name(snapshot, 0, "alpha") ||
        !peak_report_snapshot_set_name(
            snapshot,
            1,
            mismatch_name ? "mismatched-beta" : "beta") ||
        !peak_report_snapshot_set_name(snapshot, 2, "gamma")) {
        peak_report_snapshot_destroy(snapshot);
        return NULL;
    }

    snapshot->instrumented[0] = 1;
    snapshot->instrumented[1] = rank != 0;
    snapshot->instrumented[2] = rank != 0;
    snapshot->detached[0] = rank != 0;
    snapshot->reattached[1] = rank == 0;
    snapshot->revisited[1] = rank != 0;

    snapshot->num_calls[0] = rank == 0 ? 10UL : 7UL;
    snapshot->num_calls[1] = rank == 0 ? 0UL : 6UL;
    snapshot->total_time[0] = rank == 0 ? 8.0 : 3.0;
    snapshot->total_time[1] = rank == 0 ? 0.0 : 9.0;
    snapshot->max_total_time[0] = rank == 0 ? 6.0 : 4.0;
    snapshot->max_total_time[1] = rank == 0 ? 0.0 : 8.0;
    snapshot->min_total_time[0] = rank == 0 ? 2.0 : 1.5;
    snapshot->min_total_time[1] = rank == 0 ? DBL_MAX : 1.0;
    snapshot->exclusive_time[0] = rank == 0 ? 7.0 : 2.0;
    snapshot->exclusive_time[1] = rank == 0 ? 0.0 : 8.0;
    snapshot->max_time[0] = rank == 0 ? 0.6f : 0.4f;
    snapshot->max_time[1] = rank == 0 ? 0.0f : 0.8f;
    snapshot->min_time[0] = rank == 0 ? 0.2f : 0.15f;
    snapshot->min_time[1] = rank == 0 ? FLT_MAX : 0.1f;
    snapshot->thread_count[0] = rank == 0 ? 2UL : 1UL;
    snapshot->thread_count[1] = rank == 0 ? 0UL : 3UL;

    snapshot->overhead_per_call = rank == 0 ? 1e-7 : 9e-7;
    snapshot->overhead.valid = true;
    snapshot->overhead.accounting_valid = rank == 0;
    snapshot->overhead.local_ranks = rank == 0 ? 4U : 8U;
    snapshot->overhead.stop_window_count = rank == 0 ? 11U : 22U;
    snapshot->overhead.failed_stop_window_count =
        rank == 0 ? UINT64_MAX - 3 : 10U;
    snapshot->overhead.elapsed_seconds = 10.0 * scale;
    snapshot->overhead.elapsed_min_seconds = 10.0 * scale;
    snapshot->overhead.elapsed_max_seconds = 10.0 * scale;
    snapshot->overhead.profile_seconds = 1.0 * scale;
    snapshot->overhead.control_seconds = 1.0 * scale;
    snapshot->overhead.management_seconds = 0.5 * scale;
    snapshot->overhead.control_risk_seconds = 4.0 * scale;
    snapshot->overhead.profile_control_risk_seconds = 5.0 * scale;
    snapshot->overhead.profile_ratio = 0.1;
    snapshot->overhead.control_ratio = 0.1;
    snapshot->overhead.profile_control_risk_ratio = 0.5;
    snapshot->overhead.control_risk_ratio = 0.4;
    snapshot->overhead.management_ratio = 0.05;
    snapshot->overhead.ratio = 0.2;
    return snapshot;
}

static bool
aggregate_matches(const PeakReportSnapshot* aggregate)
{
    if (aggregate == NULL || aggregate->hook_count != 3 ||
        aggregate->rank_count != 2 ||
        strcmp(aggregate->program, "root-program") != 0 ||
        strcmp(aggregate->names[0], "alpha") != 0 ||
        strcmp(aggregate->names[1], "beta") != 0 ||
        strcmp(aggregate->names[2], "gamma") != 0 ||
        aggregate->overhead_per_call != 1e-7) {
        return false;
    }

    if (aggregate->instrumented[0] != 1 ||
        aggregate->instrumented[1] != 1 ||
        aggregate->instrumented[2] != 0 ||
        aggregate->detached[0] != 1 ||
        aggregate->reattached[1] != 1 ||
        aggregate->revisited[1] != 1 ||
        aggregate->num_calls[0] != 17UL ||
        aggregate->num_calls[1] != 6UL ||
        aggregate->num_calls[2] != 0UL ||
        aggregate->total_time[0] != 11.0 ||
        aggregate->total_time[1] != 9.0 ||
        aggregate->max_total_time[0] != 6.0 ||
        aggregate->max_total_time[1] != 8.0 ||
        aggregate->min_total_time[0] != 1.5 ||
        aggregate->min_total_time[1] != 1.0 ||
        aggregate->exclusive_time[0] != 9.0 ||
        aggregate->exclusive_time[1] != 8.0 ||
        aggregate->max_time[0] != 0.6f ||
        aggregate->max_time[1] != 0.8f ||
        aggregate->min_time[0] != 0.15f ||
        aggregate->min_time[1] != 0.1f ||
        aggregate->thread_count[0] != 3UL ||
        aggregate->thread_count[1] != 3UL) {
        return false;
    }

    const PeakReportOverhead* overhead = &aggregate->overhead;
    if (!overhead->valid || overhead->accounting_valid ||
        !overhead->per_rank_max ||
        overhead->failed_stop_window_count != UINT64_MAX - 1 ||
        overhead->elapsed_seconds != 10.0 ||
        overhead->elapsed_min_seconds != 10.0 ||
        overhead->elapsed_max_seconds != 20.0 ||
        overhead->profile_seconds != 3.0 ||
        overhead->control_seconds != 1.0 ||
        overhead->management_seconds != 0.5 ||
        overhead->profile_ratio != 0.1 ||
        overhead->control_ratio != 0.1 || overhead->ratio != 0.2 ||
        overhead->profile_control_risk_ratio != 0.5 ||
        overhead->control_risk_ratio != 0.4 ||
        overhead->management_ratio != 0.05) {
        return false;
    }

    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        const PeakReportRankTuple* tuple =
            &overhead->per_rank_maxima.tuples[metric];

        if (!overhead->per_rank_maxima.present[metric] ||
            overhead->per_rank_maxima.owner_ranks[metric] != 0 ||
            tuple->local_ranks != 4U || tuple->stop_window_count != 11U ||
            tuple->failed_stop_window_count != UINT64_MAX - 3 ||
            tuple->elapsed_seconds != 10.0 ||
            tuple->profile_seconds != 1.0 ||
            tuple->control_seconds != 1.0 ||
            tuple->management_seconds != 0.5 ||
            tuple->control_risk_seconds != 4.0 ||
            tuple->profile_control_risk_seconds != 5.0) {
            return false;
        }
    }
    return true;
}

static int
wait_for_expected_child(pid_t child, PeakSocketReportStatus expected)
{
    int status;

    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status) == (int)expected ? 0 : 1;
}

static int
run_two_rank_case(int port,
                  TestRootAction action,
                  bool mismatched_peer_name)
{
    PeakReportSnapshot* root = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    PeakSocketReportStatus root_status;
    PeakSocketReportStatus expected_peer =
        action == TEST_ROOT_COMMIT && !mismatched_peer_name
            ? PEAK_SOCKET_REPORT_PEER_RELEASED
            : PEAK_SOCKET_REPORT_FAILED;
    char port_text[16];
    char token_text[64];
    pid_t child;
    int result = 0;

    if (root == NULL) {
        return 1;
    }
    snprintf(port_text, sizeof(port_text), "%d", port);
    snprintf(token_text,
             sizeof(token_text),
             "socket-report-test-%ld-%d",
             (long)getpid(),
             port);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_HOST", "127.0.0.1", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", port_text, 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "1500", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", token_text, 1);
    if (action == TEST_ROOT_COMMIT_FAILURE) {
        (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL", "1", 1);
    } else {
        (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL");
    }

    child = fork();
    if (child < 0) {
        peak_report_snapshot_destroy(root);
        return 1;
    }
    if (child == 0) {
        PeakReportSnapshot* peer = fixture_snapshot(1, mismatched_peer_name);
        PeakReportSnapshot* peer_aggregate = NULL;
        PeakSocketReportSession* peer_session = NULL;
        PeakSocketReportStatus peer_status;

        set_test_rank(1);
        if (peer == NULL) {
            _exit(99);
        }
        peer_status = peak_socket_report_transport_begin(
            peer,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &peer_session,
            &peer_aggregate);
        peak_socket_report_transport_abort(peer_session);
        peak_report_snapshot_destroy(peer_aggregate);
        peak_report_snapshot_destroy(peer);
        _exit((int)peer_status);
    }

    set_test_rank(0);
    root_status = peak_socket_report_transport_begin(
        root,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        &session,
        &aggregate);
    if (mismatched_peer_name) {
        if (root_status != PEAK_SOCKET_REPORT_FAILED || session != NULL ||
            aggregate != NULL) {
            result = 1;
            peak_socket_report_transport_abort(session);
        }
    } else if (root_status != PEAK_SOCKET_REPORT_ROOT_PREPARED ||
               session == NULL || !aggregate_matches(aggregate)) {
        result = 1;
        peak_socket_report_transport_abort(session);
        session = NULL;
    } else if (action == TEST_ROOT_ABORT) {
        peak_socket_report_transport_abort(session);
        session = NULL;
    } else {
        bool committed = peak_socket_report_transport_commit(session);

        session = NULL;
        if (committed != (action == TEST_ROOT_COMMIT)) {
            result = 1;
        }
    }

    if (wait_for_expected_child(child, expected_peer) != 0) {
        result = 1;
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root);
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL");
    return result;
}

static int
check_single_process_clone(void)
{
    PeakReportSnapshot* local = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    PeakSocketReportStatus status;
    int result = 0;

    clear_rank_environment();
    if (local == NULL) {
        return 1;
    }
    status = peak_socket_report_transport_begin(
        local,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        &session,
        &aggregate);
    if (status != PEAK_SOCKET_REPORT_SINGLE_READY || session != NULL ||
        aggregate == NULL || aggregate == local || aggregate->rank_count != 1 ||
        strcmp(aggregate->names[0], local->names[0]) != 0) {
        result = 1;
    } else {
        aggregate->names[0][0] = 'A';
        if (strcmp(local->names[0], "alpha") != 0) {
            result = 1;
        }
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(local);
    return result;
}

static int
check_invalid_output_pointers_are_cleared(void)
{
    PeakSocketReportSession* session =
        (PeakSocketReportSession*)(uintptr_t)1;
    PeakReportSnapshot* aggregate = (PeakReportSnapshot*)(uintptr_t)1;
    PeakSocketReportStatus status;

    status = peak_socket_report_transport_begin(
        NULL,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        &session,
        NULL);
    if (status != PEAK_SOCKET_REPORT_FAILED || session != NULL) {
        return 1;
    }

    status = peak_socket_report_transport_begin(
        NULL,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        NULL,
        &aggregate);
    return status != PEAK_SOCKET_REPORT_FAILED || aggregate != NULL;
}

static int
check_slurm_host_parser(void)
{
    char host[64];

    return !peak_general_listener_test_first_slurm_host(
               "c[001-004]", host, sizeof(host)) ||
                   strcmp(host, "c001") != 0 ||
           !peak_general_listener_test_first_slurm_host(
               "c101-063,c102-[161-162],c103-[001-004]",
               host,
               sizeof(host)) ||
                   strcmp(host, "c101-063") != 0 ||
           !peak_general_listener_test_first_slurm_host(
               "c001,c[002-004]", host, sizeof(host)) ||
                   strcmp(host, "c001") != 0 ||
           !peak_general_listener_test_first_slurm_host(
               "node[007,009]", host, sizeof(host)) ||
                   strcmp(host, "node007") != 0 ||
           !peak_general_listener_test_first_slurm_host(
               "plain01,plain02", host, sizeof(host)) ||
                   strcmp(host, "plain01") != 0 ||
           peak_general_listener_test_first_slurm_host(
               "broken[001-004", host, sizeof(host));
}

int
main(void)
{
    int base_port = 22000 + (int)(getpid() % 8000) * 4;

    (void)setenv("PEAK_VERBOSITY", "silent", 1);
    if (check_slurm_host_parser() != 0 ||
        check_single_process_clone() != 0 ||
        check_invalid_output_pointers_are_cleared() != 0 ||
        run_two_rank_case(base_port, TEST_ROOT_COMMIT, false) != 0 ||
        run_two_rank_case(base_port + 2, TEST_ROOT_ABORT, false) != 0 ||
        run_two_rank_case(base_port + 4,
                          TEST_ROOT_COMMIT_FAILURE,
                          false) != 0 ||
        run_two_rank_case(base_port + 6, TEST_ROOT_COMMIT, true) != 0) {
        return 1;
    }

    puts("socket_report_transport_test_ok");
    return 0;
}
