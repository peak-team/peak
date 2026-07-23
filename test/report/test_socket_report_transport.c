#include "internal/general_listener/socket_report_transport.h"

#include <float.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PORT_SLOT_COUNT 800
#define TEST_PORT_SLOT_WIDTH 64
#define TEST_PORT_BASE 10000
#define TEST_RELEASE_ACK 0x51U
#define TEST_RELEASE_FALLBACK 0x52U

typedef enum {
    TEST_ROOT_COMMIT = 0,
    TEST_ROOT_ABORT,
    TEST_ROOT_COMMIT_FAILURE,
    TEST_ROOT_COMMIT_DELAYED_DEFAULT,
    TEST_ROOT_COMMIT_DELAYED_CLAMP,
    TEST_ROOT_COMMIT_DROP_ONCE,
    TEST_ROOT_COMMIT_RESOLVE_AGAIN,
    TEST_ROOT_COMMIT_CONFIRM_RETRY,
    TEST_GATHER_PARTIAL_SUCCESS,
    TEST_GATHER_SLOW_FAILURE,
    TEST_GATHER_DROP_FAILURE,
    TEST_GATHER_PAYLOAD_DROP_FAILURE,
    TEST_GATHER_RECEIPT_FAILURE,
    TEST_GATHER_CONFIRM_DROP_FAILURE,
} TestRootAction;

static int64_t
test_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int
reserve_test_port_slot(int* lock_fd_out)
{
    int first_slot;

    if (lock_fd_out == NULL) {
        return -1;
    }
    *lock_fd_out = -1;
    first_slot = (int)(getpid() % TEST_PORT_SLOT_COUNT);
    for (int offset = 0; offset < TEST_PORT_SLOT_COUNT; offset++) {
        int slot = (first_slot + offset) % TEST_PORT_SLOT_COUNT;
        int port = TEST_PORT_BASE + slot * TEST_PORT_SLOT_WIDTH;
        int lock_fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)port),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };

        /*
         * UDP and TCP have separate port namespaces. Holding the UDP endpoint
         * gives this process a kernel-enforced, cross-UID slot lock while the
         * transport tests use the corresponding TCP range.
         */
        if (lock_fd >= 0 &&
            bind(lock_fd,
                 (const struct sockaddr*)&address,
                 sizeof(address)) == 0) {
            *lock_fd_out = lock_fd;
            return port;
        }
        if (lock_fd >= 0) {
            close(lock_fd);
        }
    }
    return -1;
}

static void
clear_rank_environment(void)
{
    static const char* names[] = {
        "PMI_SIZE",
        "PMIX_SIZE",
        "OMPI_COMM_WORLD_SIZE",
        "MV2_COMM_WORLD_SIZE",
        "I_MPI_SIZE",
        "SLURM_NTASKS",
        "PMI_RANK",
        "PMIX_RANK",
        "OMPI_COMM_WORLD_RANK",
        "MV2_COMM_WORLD_RANK",
        "I_MPI_RANK",
        "SLURM_PROCID",
        NULL,
    };

    for (size_t i = 0; names[i] != NULL; i++) {
        (void)unsetenv(names[i]);
    }
}

static void
set_test_rank(int rank, int size)
{
    char rank_text[16];
    char size_text[16];

    snprintf(rank_text, sizeof(rank_text), "%d", rank);
    snprintf(size_text, sizeof(size_text), "%d", size);
    (void)setenv("PMI_SIZE", size_text, 1);
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
    PeakSocketReportTestTelemetry root_telemetry;
    PeakSocketReportStatus expected_peer =
        (action == TEST_ROOT_COMMIT ||
         action == TEST_ROOT_COMMIT_DELAYED_DEFAULT ||
         action == TEST_ROOT_COMMIT_DELAYED_CLAMP ||
         action == TEST_ROOT_COMMIT_DROP_ONCE ||
         action == TEST_ROOT_COMMIT_RESOLVE_AGAIN ||
         action == TEST_ROOT_COMMIT_CONFIRM_RETRY ||
         action == TEST_GATHER_PARTIAL_SUCCESS) &&
                !mismatched_peer_name
            ? PEAK_SOCKET_REPORT_PEER_RELEASED
            : PEAK_SOCKET_REPORT_FAILED;
    bool gather_must_fail =
        mismatched_peer_name ||
        action == TEST_GATHER_SLOW_FAILURE ||
        action == TEST_GATHER_DROP_FAILURE ||
        action == TEST_GATHER_PAYLOAD_DROP_FAILURE ||
        action == TEST_GATHER_RECEIPT_FAILURE ||
        action == TEST_GATHER_CONFIRM_DROP_FAILURE;
    bool peer_registration_expected =
        !gather_must_fail ||
        action == TEST_GATHER_CONFIRM_DROP_FAILURE;
    char port_text[16];
    char token_text[64];
    pid_t child;
    int64_t case_started_ms;
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
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS",
                 action == TEST_GATHER_SLOW_FAILURE ? "150" : "500",
                 1);
    if (action == TEST_ROOT_COMMIT_DELAYED_DEFAULT) {
        (void)unsetenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS");
    } else if (action == TEST_ROOT_COMMIT_DELAYED_CLAMP) {
        (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                     "100",
                     1);
    } else {
        (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                     "1500",
                     1);
    }
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", token_text, 1);
    if (action == TEST_ROOT_COMMIT_FAILURE) {
        (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL", "1", 1);
    } else {
        (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL");
    }
    if (action == TEST_ROOT_COMMIT_DROP_ONCE) {
        (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE",
                     "1",
                     1);
    } else {
        (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE");
    }
    if (action == TEST_ROOT_COMMIT_RESOLVE_AGAIN) {
        (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE",
                     "1",
                     1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE");
    }
    if (action == TEST_ROOT_COMMIT_CONFIRM_RETRY) {
        (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE",
                     "1",
                     1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE");
    }
    if (action == TEST_GATHER_PARTIAL_SUCCESS) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES",
            "3",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES");
    }
    if (action == TEST_GATHER_SLOW_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS",
            "1000",
            1);
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS");
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK");
    }
    if (action == TEST_GATHER_DROP_FAILURE ||
        action == TEST_GATHER_PAYLOAD_DROP_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES",
            /* wire-v11 header (152 bytes) plus 17 payload bytes. */
            action == TEST_GATHER_DROP_FAILURE ? "1" : "169",
            1);
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_RANK",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES");
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_RANK");
    }
    if (action == TEST_GATHER_RECEIPT_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_RECEIPT_FAIL_ONCE",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_RECEIPT_FAIL_ONCE");
    }
    if (action == TEST_GATHER_CONFIRM_DROP_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_AFTER_BYTES",
            "7",
            1);
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_RANK",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_AFTER_BYTES");
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_RANK");
    }

    case_started_ms = test_monotonic_ms();
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
        PeakSocketReportTestTelemetry telemetry;

        set_test_rank(1, 2);
        if (peer == NULL) {
            _exit(99);
        }
        peer_status = peak_socket_report_transport_begin(
            peer,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &peer_session,
            &peer_aggregate);
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        if (telemetry.wire_version != 11U ||
            telemetry.peer_receipt_received !=
                peer_registration_expected ||
            telemetry.peer_confirmation_sent !=
                !gather_must_fail ||
            telemetry.peer_release_started !=
                peer_registration_expected ||
            (expected_peer == PEAK_SOCKET_REPORT_PEER_RELEASED &&
             (!telemetry.peer_release_decision_received ||
              !telemetry.peer_release_confirmation_sent ||
              telemetry.peer_release_decision != TEST_RELEASE_ACK)) ||
            (action == TEST_ROOT_ABORT &&
             (!telemetry.peer_release_decision_received ||
              !telemetry.peer_release_confirmation_sent ||
              telemetry.peer_release_decision !=
                  TEST_RELEASE_FALLBACK)) ||
            (action == TEST_GATHER_CONFIRM_DROP_FAILURE &&
             (!telemetry.peer_release_decision_received ||
              !telemetry.peer_release_confirmation_sent ||
              telemetry.peer_release_decision !=
                  TEST_RELEASE_FALLBACK))) {
            peak_socket_report_transport_abort(peer_session);
            peak_report_snapshot_destroy(peer_aggregate);
            peak_report_snapshot_destroy(peer);
            _exit(99);
        }
        peak_socket_report_transport_abort(peer_session);
        peak_report_snapshot_destroy(peer_aggregate);
        peak_report_snapshot_destroy(peer);
        _exit((int)peer_status);
    }

    set_test_rank(0, 2);
    root_status = peak_socket_report_transport_begin(
        root,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        &session,
        &aggregate);
    memset(&root_telemetry, 0, sizeof(root_telemetry));
    peak_socket_report_test_telemetry_get(&root_telemetry);
    if (root_telemetry.wire_version != 11U ||
        root_telemetry.root_max_active != 1U ||
        (gather_must_fail &&
         (root_telemetry.root_payload_count !=
              ((action == TEST_GATHER_RECEIPT_FAILURE ||
                action == TEST_GATHER_CONFIRM_DROP_FAILURE)
                   ? 1U
                   : 0U) ||
          root_telemetry.root_receipt_count !=
              (action == TEST_GATHER_CONFIRM_DROP_FAILURE ? 1U : 0U) ||
          root_telemetry.root_confirmation_count != 0U)) ||
        (!gather_must_fail &&
         (root_telemetry.root_payload_count != 1U ||
          root_telemetry.root_receipt_count != 1U ||
          root_telemetry.root_confirmation_count != 1U)) ||
        root_telemetry.root_release_target_count !=
            root_telemetry.root_receipt_count) {
        result = 1;
    }
    if (gather_must_fail) {
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
        if (action == TEST_ROOT_COMMIT_DELAYED_DEFAULT ||
            action == TEST_ROOT_COMMIT_DELAYED_CLAMP) {
            /*
             * Exceed two gather-phase budgets after root preparation. Both
             * the default and a too-small override must retain the complete
             * three-phase gather/publication/release budget.
             */
            usleep(1250000);
        }
        bool committed = peak_socket_report_transport_commit(session);

        session = NULL;
        if (committed != (action != TEST_ROOT_COMMIT_FAILURE)) {
            result = 1;
        }
    }

    memset(&root_telemetry, 0, sizeof(root_telemetry));
    peak_socket_report_test_telemetry_get(&root_telemetry);
    if ((expected_peer == PEAK_SOCKET_REPORT_PEER_RELEASED &&
         (root_telemetry.root_release_decision != TEST_RELEASE_ACK ||
          root_telemetry.root_release_confirmed_count != 1U)) ||
        ((action == TEST_ROOT_ABORT ||
          action == TEST_GATHER_CONFIRM_DROP_FAILURE) &&
         (root_telemetry.root_release_decision !=
              TEST_RELEASE_FALLBACK ||
          root_telemetry.root_release_confirmed_count != 1U)) ||
        (action == TEST_ROOT_COMMIT_FAILURE &&
         (root_telemetry.root_release_decision != TEST_RELEASE_ACK ||
          root_telemetry.root_release_confirmed_count != 0U))) {
        result = 1;
    }
    if (wait_for_expected_child(child, expected_peer) != 0) {
        result = 1;
    }
    if (action == TEST_GATHER_CONFIRM_DROP_FAILURE &&
        test_monotonic_ms() - case_started_ms > 5000) {
        result = 1;
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root);
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_RANK");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_RECEIPT_FAIL_ONCE");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_AFTER_BYTES");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_RANK");
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS");
    return result;
}

static int
run_many_rank_case(int port, int size, bool exercise_concurrency)
{
    PeakReportSnapshot* root = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    pid_t* children = NULL;
    char port_text[16];
    char token_text[64];
    int started = 0;
    int result = 0;

    if (root == NULL || size <= 2) {
        peak_report_snapshot_destroy(root);
        return 1;
    }
    children = calloc((size_t)(size - 1), sizeof(*children));
    if (children == NULL) {
        peak_report_snapshot_destroy(root);
        return 1;
    }
    snprintf(port_text, sizeof(port_text), "%d", port);
    snprintf(token_text,
             sizeof(token_text),
             "socket-report-many-test-%ld-%d",
             (long)getpid(),
             port);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_HOST", "127.0.0.1", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", port_text, 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "3000", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                 "6000",
                 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", token_text, 1);
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE");
    if (exercise_concurrency) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES",
            "3",
            1);
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS",
            "500",
            1);
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES");
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS");
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK");
    }

    for (int rank = 1; rank < size; rank++) {
        pid_t child = fork();

        if (child < 0) {
            result = 1;
            break;
        }
        if (child == 0) {
            PeakReportSnapshot* peer = fixture_snapshot(rank, false);
            PeakReportSnapshot* peer_aggregate = NULL;
            PeakSocketReportSession* peer_session = NULL;
            PeakSocketReportStatus peer_status;
            PeakSocketReportTestTelemetry telemetry;

            set_test_rank(rank, size);
            if (exercise_concurrency) {
                (void)setenv(
                    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER",
                    "1",
                    1);
                if (rank == 1) {
                    (void)unsetenv(
                        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS");
                } else {
                    (void)setenv(
                        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS",
                        "100",
                        1);
                }
            }
            if (peer == NULL) {
                _exit(99);
            }
            peer_status = peak_socket_report_transport_begin(
                peer,
                PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
                &peer_session,
                &peer_aggregate);
            memset(&telemetry, 0, sizeof(telemetry));
            peak_socket_report_test_telemetry_get(&telemetry);
            if (telemetry.wire_version != 11U ||
                !telemetry.peer_receipt_received ||
                !telemetry.peer_confirmation_sent ||
                !telemetry.peer_release_started ||
                !telemetry.peer_release_decision_received ||
                !telemetry.peer_release_confirmation_sent ||
                telemetry.peer_release_decision != TEST_RELEASE_ACK) {
                peak_socket_report_transport_abort(peer_session);
                peak_report_snapshot_destroy(peer_aggregate);
                peak_report_snapshot_destroy(peer);
                _exit(99);
            }
            peak_socket_report_transport_abort(peer_session);
            peak_report_snapshot_destroy(peer_aggregate);
            peak_report_snapshot_destroy(peer);
            _exit((int)peer_status);
        }
        children[started++] = child;
    }

    if (!result) {
        PeakSocketReportStatus root_status;
        PeakSocketReportTestTelemetry telemetry;

        set_test_rank(0, size);
        root_status = peak_socket_report_transport_begin(
            root,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &session,
            &aggregate);
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        if (root_status != PEAK_SOCKET_REPORT_ROOT_PREPARED ||
            session == NULL || aggregate == NULL ||
            telemetry.wire_version != 11U ||
            telemetry.root_payload_count != (uint32_t)(size - 1) ||
            telemetry.root_receipt_count != (uint32_t)(size - 1) ||
            telemetry.root_confirmation_count !=
                (uint32_t)(size - 1) ||
            telemetry.root_max_active == 0U ||
            telemetry.root_max_active > (uint32_t)(size - 1) ||
            (exercise_concurrency &&
             telemetry.root_max_active <= 1U) ||
            telemetry.root_release_target_count !=
                (uint32_t)(size - 1) ||
            aggregate->rank_count != size ||
            aggregate->num_calls[0] !=
                10UL + 7UL * (unsigned long)(size - 1) ||
            aggregate->num_calls[1] !=
                6UL * (unsigned long)(size - 1)) {
            result = 1;
            peak_socket_report_transport_abort(session);
            session = NULL;
        } else if (!peak_socket_report_transport_commit(session)) {
            result = 1;
            session = NULL;
        } else {
            session = NULL;
            memset(&telemetry, 0, sizeof(telemetry));
            peak_socket_report_test_telemetry_get(&telemetry);
            if (telemetry.root_release_decision !=
                    TEST_RELEASE_ACK ||
                telemetry.root_release_confirmed_count !=
                    (uint32_t)(size - 1)) {
                result = 1;
            }
        }
    } else {
        peak_socket_report_transport_abort(session);
        session = NULL;
    }

    for (int i = 0; i < started; i++) {
        if (wait_for_expected_child(
                children[i], PEAK_SOCKET_REPORT_PEER_RELEASED) != 0) {
            result = 1;
        }
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root);
    free(children);
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER");
    return result;
}

static int
run_duplicate_rank_case(int port)
{
    PeakReportSnapshot* root = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    pid_t children[2] = {-1, -1};
    char port_text[16];
    char token_text[64];
    int result = 0;

    if (root == NULL) {
        return 1;
    }
    snprintf(port_text, sizeof(port_text), "%d", port);
    snprintf(token_text,
             sizeof(token_text),
             "socket-report-duplicate-test-%ld-%d",
             (long)getpid(),
             port);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_HOST", "127.0.0.1", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", port_text, 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "500", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                 "1500",
                 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", token_text, 1);

    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        if (children[i] < 0) {
            result = 1;
            break;
        }
        if (children[i] == 0) {
            PeakReportSnapshot* peer = fixture_snapshot(1, false);
            PeakReportSnapshot* peer_aggregate = NULL;
            PeakSocketReportSession* peer_session = NULL;
            PeakSocketReportStatus status;

            set_test_rank(1, 3);
            if (peer == NULL) {
                _exit(99);
            }
            status = peak_socket_report_transport_begin(
                peer,
                PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
                &peer_session,
                &peer_aggregate);
            peak_socket_report_transport_abort(peer_session);
            peak_report_snapshot_destroy(peer_aggregate);
            peak_report_snapshot_destroy(peer);
            _exit((int)status);
        }
    }

    if (!result) {
        PeakSocketReportStatus status;
        PeakSocketReportTestTelemetry telemetry;

        set_test_rank(0, 3);
        status = peak_socket_report_transport_begin(
            root,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &session,
            &aggregate);
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        if (status != PEAK_SOCKET_REPORT_FAILED ||
            session != NULL || aggregate != NULL ||
            telemetry.wire_version != 11U ||
            telemetry.root_payload_count > 1U ||
            telemetry.root_receipt_count >
                telemetry.root_payload_count ||
            telemetry.root_confirmation_count >
                telemetry.root_receipt_count) {
            result = 1;
        }
    }
    peak_socket_report_transport_abort(session);
    for (int i = 0; i < 2; i++) {
        if (children[i] > 0 &&
            wait_for_expected_child(
                children[i], PEAK_SOCKET_REPORT_FAILED) != 0) {
            result = 1;
        }
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root);
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
check_required_rank_metadata(void)
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
        PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED,
        &session,
        &aggregate);
    if (status != PEAK_SOCKET_REPORT_FAILED || session != NULL ||
        aggregate != NULL) {
        result = 1;
    }

    (void)setenv("PMI_RANK", "0", 1);
    (void)setenv("PMI_SIZE", "1", 1);
    status = peak_socket_report_transport_begin(
        local,
        PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED,
        &session,
        &aggregate);
    if (status != PEAK_SOCKET_REPORT_SINGLE_READY || session != NULL ||
        aggregate == NULL || aggregate->rank_count != 1) {
        result = 1;
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(local);
    clear_rank_environment();
    return result;
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
    /*
     * Hold the UDP endpoint paired with a 64-port TCP slot. The test currently
     * consumes base..base+35, and the kernel lock prevents parallel CTest
     * processes, including different UIDs, from choosing the same range.
     */
    int port_lock_fd = -1;
    int base_port = reserve_test_port_slot(&port_lock_fd);
    int failed;

    (void)setenv("PEAK_VERBOSITY", "silent", 1);
    if (base_port < 0) {
        return 1;
    }
    failed = 0;
#define CHECK_SOCKET_CASE(label, expression)                         \
    do {                                                             \
        if ((expression) != 0) {                                     \
            fprintf(stderr, "socket case failed: %s\n", (label));   \
            failed = 1;                                              \
        }                                                            \
    } while (0)
    CHECK_SOCKET_CASE("slurm-host-parser", check_slurm_host_parser());
    CHECK_SOCKET_CASE("single-process", check_single_process_clone());
    CHECK_SOCKET_CASE("required-rank", check_required_rank_metadata());
    CHECK_SOCKET_CASE("invalid-output-pointers",
                      check_invalid_output_pointers_are_cleared());
    CHECK_SOCKET_CASE(
        "commit",
        run_two_rank_case(base_port, TEST_ROOT_COMMIT, false));
    CHECK_SOCKET_CASE(
        "abort",
        run_two_rank_case(base_port + 2, TEST_ROOT_ABORT, false));
    CHECK_SOCKET_CASE(
        "commit-failure",
        run_two_rank_case(
            base_port + 4, TEST_ROOT_COMMIT_FAILURE, false));
    CHECK_SOCKET_CASE(
        "identity-mismatch",
        run_two_rank_case(base_port + 6, TEST_ROOT_COMMIT, true));
    CHECK_SOCKET_CASE(
        "delayed-default",
        run_two_rank_case(base_port + 8,
                          TEST_ROOT_COMMIT_DELAYED_DEFAULT,
                          false));
    CHECK_SOCKET_CASE(
        "delayed-clamp",
        run_two_rank_case(base_port + 10,
                          TEST_ROOT_COMMIT_DELAYED_CLAMP,
                          false));
    CHECK_SOCKET_CASE(
        "release-drop-once",
        run_two_rank_case(base_port + 12,
                          TEST_ROOT_COMMIT_DROP_ONCE,
                          false));
    CHECK_SOCKET_CASE(
        "resolve-again",
        run_two_rank_case(base_port + 14,
                          TEST_ROOT_COMMIT_RESOLVE_AGAIN,
                          false));
    CHECK_SOCKET_CASE(
        "release-confirm-retry",
        run_two_rank_case(base_port + 16,
                          TEST_ROOT_COMMIT_CONFIRM_RETRY,
                          false));
    CHECK_SOCKET_CASE(
        "gather-partial-success",
        run_two_rank_case(base_port + 18,
                          TEST_GATHER_PARTIAL_SUCCESS,
                          false));
    CHECK_SOCKET_CASE(
        "gather-slow-failure",
        run_two_rank_case(base_port + 20,
                          TEST_GATHER_SLOW_FAILURE,
                          false));
    CHECK_SOCKET_CASE(
        "gather-header-drop",
        run_two_rank_case(base_port + 22,
                          TEST_GATHER_DROP_FAILURE,
                          false));
    CHECK_SOCKET_CASE(
        "gather-payload-drop",
        run_two_rank_case(base_port + 24,
                          TEST_GATHER_PAYLOAD_DROP_FAILURE,
                          false));
    CHECK_SOCKET_CASE(
        "gather-receipt-failure",
        run_two_rank_case(base_port + 26,
                          TEST_GATHER_RECEIPT_FAILURE,
                          false));
    CHECK_SOCKET_CASE(
        "gather-confirm-drop",
        run_two_rank_case(base_port + 28,
                          TEST_GATHER_CONFIRM_DROP_FAILURE,
                          false));
    CHECK_SOCKET_CASE("duplicate-rank",
                      run_duplicate_rank_case(base_port + 30));
    CHECK_SOCKET_CASE("slow-chunked-8-rank",
                      run_many_rank_case(base_port + 32, 8, true));
    CHECK_SOCKET_CASE("32-rank",
                      run_many_rank_case(base_port + 34, 32, false));
#undef CHECK_SOCKET_CASE
    close(port_lock_fd);
    if (failed) {
        return 1;
    }

    puts("socket_report_transport_test_ok");
    return 0;
}
