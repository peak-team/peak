#include "internal/general_listener/socket_report_transport.h"

#include <float.h>
#include <limits.h>
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
#define TEST_DEFAULT_PORT_BASE 42000
#define TEST_DEFAULT_PORT_SPAN 20000
#define TEST_RELEASE_ACK 0x51U
#define TEST_RELEASE_FALLBACK 0x52U

typedef enum {
    TEST_ROOT_COMMIT = 0,
    TEST_ROOT_ABORT,
    TEST_ROOT_COMMIT_FAILURE,
    TEST_ROOT_COMMIT_AFTER_PEER_READY,
    TEST_ROOT_COMMIT_DROP_ONCE,
    TEST_ROOT_COMMIT_RESOLVE_AGAIN,
    TEST_ROOT_COMMIT_CONFIRM_RETRY,
    TEST_ROOT_SESSION_ALLOC_FAILURE,
    TEST_GATHER_PARTIAL_SUCCESS,
    TEST_GATHER_PARTIAL_DRIP_FAILURE,
    TEST_GATHER_PROGRESS_SUCCESS,
    TEST_GATHER_SLOW_FAILURE,
    TEST_GATHER_DROP_FAILURE,
    TEST_GATHER_PAYLOAD_DROP_FAILURE,
    TEST_GATHER_RECEIPT_FAILURE,
    TEST_GATHER_CONFIRM_DROP_FAILURE,
} TestRootAction;

static bool test_saturate_dropped_counters;

static int64_t
test_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool
test_tcp_slot_is_available(int base_port)
{
    int fds[TEST_PORT_SLOT_WIDTH];
    int opened = 0;
    bool available = true;

    for (int offset = 0; offset < TEST_PORT_SLOT_WIDTH; offset++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)(base_port + offset)),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (fd < 0 ||
            bind(fd,
                 (const struct sockaddr*)&address,
                 sizeof(address)) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            available = false;
            break;
        }
        fds[opened++] = fd;
    }

    while (opened > 0) {
        close(fds[--opened]);
    }
    return available;
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
                 sizeof(address)) == 0 &&
            test_tcp_slot_is_available(port)) {
            *lock_fd_out = lock_fd;
            return port;
        }
        if (lock_fd >= 0) {
            close(lock_fd);
        }
    }
    return -1;
}

static int
bind_test_tcp_port(int port, bool reuse_address)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (fd < 0) {
        return -1;
    }
    if (reuse_address) {
        (void)setsockopt(fd,
                         SOL_SOCKET,
                         SO_REUSEADDR,
                         &one,
                         sizeof(one));
    }
    if (bind(fd,
             (const struct sockaddr*)&address,
             sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
check_listener_bind_scope(int port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd;

    (void)setenv("PEAK_OUTPUT_AGGREGATION_HOST", "127.0.0.1", 1);
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_BIND_ADDRESS");
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_ALLOW_BROAD_BIND");
    fd = peak_socket_report_test_bind_listener(port);
    if (fd < 0 || getsockname(fd, (struct sockaddr*)&address, &length) != 0 ||
        address.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        fprintf(stderr, "default listener did not bind root-local address\n");
        if (fd >= 0) close(fd);
        exit(1);
    }
    close(fd);

    (void)setenv("PEAK_OUTPUT_AGGREGATION_BIND_ADDRESS", "0.0.0.0", 1);
    fd = peak_socket_report_test_bind_listener(port + 1);
    if (fd >= 0) {
        fprintf(stderr, "wildcard bind bypassed explicit broad opt-in\n");
        close(fd);
        exit(1);
    }
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_BIND_ADDRESS");

    (void)setenv("PEAK_OUTPUT_AGGREGATION_ALLOW_BROAD_BIND", "1", 1);
    fd = peak_socket_report_test_bind_listener(port + 2);
    if (fd < 0 || getsockname(fd, (struct sockaddr*)&address, &length) != 0 ||
        address.sin_addr.s_addr != htonl(INADDR_ANY)) {
        fprintf(stderr, "explicit broad listener bind was not honored\n");
        if (fd >= 0) close(fd);
        exit(1);
    }
    close(fd);
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_ALLOW_BROAD_BIND");
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
clear_socket_identity_environment(void)
{
    static const char* names[] = {
        "SLURM_JOB_ID",
        "SLURM_STEP_ID",
        "SLURM_STEPID",
        "SLURM_JOB_UID",
        "SLURM_CLUSTER_NAME",
        "SLURM_NODELIST",
        "SLURM_JOB_NODELIST",
        "PMI_JOBID",
        "PMI_KVS",
        "PMI_NAMESPACE",
        "PMIX_NAMESPACE",
        "OMPI_COMM_WORLD_JOBID",
        "PEAK_OUTPUT_AGGREGATION_TOKEN",
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
    if (test_saturate_dropped_counters) {
        snapshot->dropped_calls = rank == 0 ? UINT64_MAX - 2 : 2U;
        snapshot->dropped_threads = rank == 0 ? UINT64_MAX - 1 : 1U;
    } else {
        snapshot->dropped_calls = rank == 0 ? 7U : 11U;
        snapshot->dropped_threads = rank == 0 ? 2U : 3U;
    }

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
        aggregate->thread_count[1] != 3UL ||
        aggregate->dropped_calls !=
            (test_saturate_dropped_counters ? UINT64_MAX - 1 : 18U) ||
        aggregate->dropped_threads !=
            (test_saturate_dropped_counters ? UINT64_MAX - 1 : 5U)) {
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
wait_for_expected_child(pid_t child,
                        PeakSocketReportStatus expected,
                        int* status_out)
{
    int status = -1;

    if (status_out != NULL) {
        *status_out = status;
    }

    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        if (status_out != NULL) {
            *status_out = status;
        }
        return 1;
    }
    if (status_out != NULL) {
        *status_out = status;
    }
    return WEXITSTATUS(status) == (int)expected ? 0 : 1;
}

static void
report_two_rank_case_diagnostic(
    TestRootAction action,
    PeakSocketReportStatus root_status,
    bool commit_attempted,
    bool committed,
    int peer_wait_status,
    bool port_rebound_checked,
    bool port_rebound_ok,
    const PeakSocketReportTestTelemetry* telemetry)
{
    fprintf(stderr,
            "socket two-rank diagnostic: action=%d root_status=%d "
            "commit_attempted=%d commit_result=%d peer_wait_status=%d "
            "peer_exited=%d peer_exit=%d peer_signal=%d "
            "port_rebound_checked=%d port_rebound_ok=%d "
            "root={wire=%u payload=%u receipt=%u confirmation=%u "
            "max_active=%u release_targets=%u release_confirmed=%u "
            "release_decision=%u}\n",
            action,
            root_status,
            commit_attempted,
            committed,
            peer_wait_status,
            peer_wait_status >= 0 && WIFEXITED(peer_wait_status),
            peer_wait_status >= 0 && WIFEXITED(peer_wait_status)
                ? WEXITSTATUS(peer_wait_status)
                : -1,
            peer_wait_status >= 0 && WIFSIGNALED(peer_wait_status)
                ? WTERMSIG(peer_wait_status)
                : -1,
            port_rebound_checked,
            port_rebound_ok,
            telemetry->wire_version,
            telemetry->root_payload_count,
            telemetry->root_receipt_count,
            telemetry->root_confirmation_count,
            telemetry->root_max_active,
            telemetry->root_release_target_count,
            telemetry->root_release_confirmed_count,
            telemetry->root_release_decision);
}

static void
report_many_rank_case_diagnostic(
    int size,
    bool exercise_concurrency,
    PeakSocketReportStatus root_status,
    bool commit_attempted,
    bool committed,
    const PeakSocketReportTestTelemetry* telemetry)
{
    fprintf(stderr,
            "socket many-rank diagnostic: size=%d concurrency=%d "
            "root_status=%d commit_attempted=%d commit_result=%d "
            "root={wire=%u payload=%u receipt=%u confirmation=%u "
            "max_active=%u release_targets=%u release_confirmed=%u "
            "release_decision=%u}\n",
            size,
            exercise_concurrency,
            root_status,
            commit_attempted,
            committed,
            telemetry->wire_version,
            telemetry->root_payload_count,
            telemetry->root_receipt_count,
            telemetry->root_confirmation_count,
            telemetry->root_max_active,
            telemetry->root_release_target_count,
            telemetry->root_release_confirmed_count,
            telemetry->root_release_decision);
}

static int
run_two_rank_case_with_peer_start_delay(int port,
                                        TestRootAction action,
                                        bool mismatched_peer_name,
                                        bool delay_peer_start,
                                        bool exit_after_listener_ready)
{
    PeakReportSnapshot* root = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    PeakSocketReportStatus root_status = PEAK_SOCKET_REPORT_FAILED;
    PeakSocketReportTestTelemetry root_telemetry = {0};
    bool success_path =
        !mismatched_peer_name &&
        (action == TEST_ROOT_COMMIT ||
         action == TEST_ROOT_COMMIT_AFTER_PEER_READY ||
         action == TEST_ROOT_COMMIT_DROP_ONCE ||
         action == TEST_ROOT_COMMIT_RESOLVE_AGAIN ||
         action == TEST_ROOT_COMMIT_CONFIRM_RETRY ||
         action == TEST_GATHER_PARTIAL_SUCCESS ||
         action == TEST_GATHER_PROGRESS_SUCCESS);
    PeakSocketReportStatus expected_peer = success_path
            ? PEAK_SOCKET_REPORT_PEER_RELEASED
            : PEAK_SOCKET_REPORT_FAILED;
    bool gather_must_fail =
        mismatched_peer_name ||
        action == TEST_GATHER_SLOW_FAILURE ||
        action == TEST_GATHER_PARTIAL_DRIP_FAILURE ||
        action == TEST_GATHER_DROP_FAILURE ||
        action == TEST_GATHER_PAYLOAD_DROP_FAILURE ||
        action == TEST_GATHER_RECEIPT_FAILURE ||
        action == TEST_GATHER_CONFIRM_DROP_FAILURE;
    /*
     * These injected peers can connect, send their configured early bytes,
     * and close from the TCP backlog before a delayed root reaches accept().
     * That valid failure path has no userland active connection to observe.
     */
    bool early_drop_may_skip_accept =
        action == TEST_GATHER_DROP_FAILURE ||
        action == TEST_GATHER_PAYLOAD_DROP_FAILURE;
    bool peer_registration_expected =
        !gather_must_fail ||
        action == TEST_GATHER_CONFIRM_DROP_FAILURE;
    bool session_allocation_must_fail =
        action == TEST_ROOT_SESSION_ALLOC_FAILURE;
    bool receipt_failure_barrier =
        action == TEST_GATHER_RECEIPT_FAILURE;
    bool peer_exits_before_connect = exit_after_listener_ready;
    char port_text[16];
    char token_text[64];
    int peer_ready_fds[2] = {-1, -1};
    int receipt_barrier_fds[2] = {-1, -1};
    pid_t child;
    int64_t case_started_ms;
    int64_t root_started_ms = -1;
    int peer_wait_status = -1;
    bool peer_wait_complete = false;
    bool commit_attempted = false;
    bool committed = false;
    bool port_rebound_checked = false;
    bool port_rebound_ok = false;
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
                 action == TEST_GATHER_SLOW_FAILURE
                     ? "150"
                     : action == TEST_GATHER_PARTIAL_DRIP_FAILURE
                           ? "500"
                           : "1500",
                 1);
    if (action == TEST_GATHER_PROGRESS_SUCCESS) {
        /*
         * The peer crosses the initial no-progress deadline only after its
         * accepted connection refreshes that deadline. Leave enough margin
         * on both sides of the refresh for a loaded hosted runner.
         */
        (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "1000", 1);
    }
    if (action == TEST_GATHER_SLOW_FAILURE ||
        action == TEST_GATHER_PROGRESS_SUCCESS) {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS");
    } else {
        /* Test-only grace isolates launcher scheduling before transport I/O. */
        (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS",
                     delay_peer_start ? "100" : "10000",
                     1);
    }
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS",
                 action == TEST_GATHER_PROGRESS_SUCCESS
                     ? "1000"
                     : action == TEST_GATHER_PARTIAL_DRIP_FAILURE
                           ? "2000"
                           : "10",
                 1);
    if (action == TEST_ROOT_COMMIT_AFTER_PEER_READY) {
        /*
         * This integration case exercises the peer's prepared-to-release
         * state, not timeout configuration arithmetic.  The runtime-config
         * test covers default and clamped budget calculations exactly.
         */
        (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                     "10000",
                     1);
    } else {
        (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                     action == TEST_ROOT_COMMIT_CONFIRM_RETRY
                         ? "5000"
                         : "3000",
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
    if (session_allocation_must_fail) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_SESSION_ALLOC_FAIL",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_SESSION_ALLOC_FAIL");
    }
    if (action == TEST_GATHER_PARTIAL_SUCCESS ||
        action == TEST_GATHER_PARTIAL_DRIP_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES",
            "3",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES");
    }
    if (action == TEST_GATHER_PARTIAL_DRIP_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_DELAY_MS",
            "60",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_DELAY_MS");
    }
    if (action == TEST_GATHER_SLOW_FAILURE ||
        action == TEST_GATHER_PROGRESS_SUCCESS) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS",
            action == TEST_GATHER_SLOW_FAILURE ? "1000" : "600",
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
    if (action == TEST_GATHER_PROGRESS_SUCCESS ||
        action == TEST_ROOT_COMMIT_CONFIRM_RETRY) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS",
            action == TEST_GATHER_PROGRESS_SUCCESS ? "600" : "2000",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS");
    }
    if (action == TEST_GATHER_PROGRESS_SUCCESS) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER",
            "1",
            1);
    } else {
        (void)unsetenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER");
    }
    if (action == TEST_GATHER_DROP_FAILURE ||
        action == TEST_GATHER_PAYLOAD_DROP_FAILURE) {
        (void)setenv(
            "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES",
            /* wire-v13 header (168 bytes) plus 17 payload bytes. */
            action == TEST_GATHER_DROP_FAILURE ? "1" : "185",
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

    peak_socket_report_test_receipt_barrier_set(-1, -1);
    peak_socket_report_test_listener_ready_set(-1);
    /*
     * Keep fork/fixture launch out of root transport timing.  The child
     * announces fixture readiness, then waits for the root to finish binding
     * both listeners and allocating gather state before peer timing starts.
     */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, peer_ready_fds) != 0) {
        peak_report_snapshot_destroy(root);
        return 1;
    }
    peak_socket_report_test_listener_ready_set(peer_ready_fds[1]);
    if (receipt_failure_barrier &&
        socketpair(AF_UNIX, SOCK_STREAM, 0, receipt_barrier_fds) != 0) {
        close(peer_ready_fds[0]);
        peak_socket_report_test_listener_ready_set(-1);
        peak_report_snapshot_destroy(root);
        return 1;
    }

    case_started_ms = test_monotonic_ms();
    child = fork();
    if (child < 0) {
        close(peer_ready_fds[0]);
        peak_socket_report_test_listener_ready_set(-1);
        if (receipt_barrier_fds[0] >= 0) {
            close(receipt_barrier_fds[0]);
        }
        if (receipt_barrier_fds[1] >= 0) {
            close(receipt_barrier_fds[1]);
        }
        peak_report_snapshot_destroy(root);
        return 1;
    }
    if (child == 0) {
        PeakReportSnapshot* peer = fixture_snapshot(1, mismatched_peer_name);
        PeakReportSnapshot* peer_aggregate = NULL;
        PeakSocketReportSession* peer_session = NULL;
        PeakSocketReportStatus peer_status;
        PeakSocketReportTestTelemetry telemetry;
        unsigned char ready = 0x71U;

        close(peer_ready_fds[1]);
        peer_ready_fds[1] = -1;

        if (receipt_failure_barrier) {
            close(receipt_barrier_fds[1]);
            receipt_barrier_fds[1] = -1;
            peak_socket_report_test_receipt_barrier_set(
                receipt_barrier_fds[0], -1);
            receipt_barrier_fds[0] = -1;
        }
        set_test_rank(1, 2);
        if (peer == NULL ||
            send(peer_ready_fds[0],
                 &ready,
                 sizeof(ready),
                 MSG_NOSIGNAL) != (ssize_t)sizeof(ready)) {
            close(peer_ready_fds[0]);
            _exit(99);
        }
        if (recv(peer_ready_fds[0], &ready, sizeof(ready), 0) !=
                (ssize_t)sizeof(ready) ||
            ready != 0x73U) {
            close(peer_ready_fds[0]);
            _exit(99);
        }
        if (exit_after_listener_ready) {
            _exit(0);
        }
        if (delay_peer_start) {
            /* Old one-way startup expired its 100 ms grace here. */
            usleep(250000);
        }
        peer_status = peak_socket_report_transport_begin(
            peer,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &peer_session,
            &peer_aggregate);
        close(peer_ready_fds[0]);
        peer_ready_fds[0] = -1;
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        if (telemetry.wire_version != 13U ||
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
            ((action == TEST_ROOT_ABORT ||
              action == TEST_ROOT_SESSION_ALLOC_FAILURE) &&
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

    if (receipt_failure_barrier) {
        close(receipt_barrier_fds[0]);
        receipt_barrier_fds[0] = -1;
        peak_socket_report_test_receipt_barrier_set(
            -1, receipt_barrier_fds[1]);
        receipt_barrier_fds[1] = -1;
    }
    {
        unsigned char ready = 0;

        close(peer_ready_fds[0]);
        peer_ready_fds[0] = -1;
        if (recv(peer_ready_fds[1], &ready, sizeof(ready), 0) !=
            (ssize_t)sizeof(ready) || ready != 0x71U) {
            result = 1;
        }
    }
    if (result == 0) {
        set_test_rank(0, 2);
        root_started_ms = test_monotonic_ms();
        root_status = peak_socket_report_transport_begin(
            root,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &session,
            &aggregate);
    }
    /* Wakes a waiting child by EOF if root failed before listener readiness. */
    peak_socket_report_test_listener_ready_set(-1);
    peer_ready_fds[1] = -1;
    memset(&root_telemetry, 0, sizeof(root_telemetry));
    peak_socket_report_test_telemetry_get(&root_telemetry);
    if (receipt_failure_barrier) {
        /* Also releases the peer if root failed before receipt preparation. */
        peak_socket_report_test_receipt_barrier_set(-1, -1);
    }
    if (!peer_exits_before_connect &&
        (root_telemetry.wire_version != 13U ||
         (!gather_must_fail &&
          root_telemetry.root_receipt_session_nonce == 0) ||
        (!early_drop_may_skip_accept &&
         root_telemetry.root_max_active != 1U) ||
        (early_drop_may_skip_accept &&
         root_telemetry.root_max_active > 1U) ||
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
            root_telemetry.root_receipt_count)) {
        result = 1;
    }
    if (action == TEST_GATHER_RECEIPT_FAILURE &&
        (!root_telemetry.root_receipt_failure_injected ||
         root_telemetry.root_max_active != 1U ||
         root_telemetry.root_payload_count != 1U ||
         root_telemetry.root_receipt_count != 0U ||
         root_telemetry.root_confirmation_count != 0U ||
         root_status != PEAK_SOCKET_REPORT_FAILED || session != NULL ||
         aggregate != NULL)) {
        result = 1;
    }
    if (gather_must_fail || session_allocation_must_fail) {
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
        int competing_fd = bind_test_tcp_port(port + 1, false);

        if (competing_fd >= 0) {
            close(competing_fd);
            result = 1;
        }
        peak_socket_report_transport_abort(session);
        session = NULL;
    } else {
        int competing_fd = bind_test_tcp_port(port + 1, false);

        if (competing_fd >= 0) {
            close(competing_fd);
            result = 1;
        }
        if (action == TEST_ROOT_COMMIT_AFTER_PEER_READY) {
            int child_poll_status = -1;
            pid_t child_poll;

            /*
             * Root preparation proves that the peer's payload, receipt, and
             * confirmation are complete.  Verify the peer remains alive for
             * the release decision, then delay well inside the explicit
             * 10-second release budget.  This avoids testing scheduler jitter
             * against a computed deadline.
             */
            child_poll = waitpid(child, &child_poll_status, WNOHANG);
            if (child_poll > 0) {
                peer_wait_status = child_poll_status;
                peer_wait_complete = true;
                result = 1;
            } else if (child_poll < 0) {
                result = 1;
            }
            usleep(250000);
        }
        commit_attempted = true;
        committed = peak_socket_report_transport_commit(session);

        session = NULL;
        if (committed != (action != TEST_ROOT_COMMIT_FAILURE)) {
            result = 1;
        }
    }

    memset(&root_telemetry, 0, sizeof(root_telemetry));
    peak_socket_report_test_telemetry_get(&root_telemetry);
    if (!peer_exits_before_connect &&
        ((expected_peer == PEAK_SOCKET_REPORT_PEER_RELEASED &&
         (root_telemetry.root_release_decision != TEST_RELEASE_ACK ||
          root_telemetry.root_release_confirmed_count != 1U)) ||
        ((action == TEST_ROOT_ABORT ||
          action == TEST_ROOT_SESSION_ALLOC_FAILURE ||
          action == TEST_GATHER_CONFIRM_DROP_FAILURE) &&
         (root_telemetry.root_release_decision !=
              TEST_RELEASE_FALLBACK ||
          root_telemetry.root_release_confirmed_count != 1U)) ||
        (action == TEST_ROOT_COMMIT_FAILURE &&
         (root_telemetry.root_release_decision != TEST_RELEASE_ACK ||
          root_telemetry.root_release_confirmed_count != 0U)))) {
        result = 1;
    }
    if (peer_exits_before_connect) {
        int64_t elapsed_ms = test_monotonic_ms() - root_started_ms;

        if ((!peer_wait_complete &&
             (waitpid(child, &peer_wait_status, 0) != child)) ||
            !WIFEXITED(peer_wait_status) ||
            WEXITSTATUS(peer_wait_status) != 0 ||
            root_status != PEAK_SOCKET_REPORT_FAILED || session != NULL ||
            aggregate != NULL || root_telemetry.root_payload_count != 0U ||
            root_telemetry.root_receipt_count != 0U ||
            root_telemetry.root_confirmation_count != 0U ||
            root_telemetry.root_max_active != 0U || root_started_ms < 0 ||
            elapsed_ms > 5000) {
            result = 1;
        }
        peer_wait_complete = true;
    } else if (!peer_wait_complete &&
        wait_for_expected_child(child, expected_peer, &peer_wait_status) !=
            0) {
        result = 1;
    } else if (peer_wait_complete &&
               (!WIFEXITED(peer_wait_status) ||
                WEXITSTATUS(peer_wait_status) != (int)expected_peer)) {
        result = 1;
    }
    if (session == NULL) {
        int rebound_fd = bind_test_tcp_port(port + 1, true);

        port_rebound_checked = true;
        if (rebound_fd < 0) {
            result = 1;
        } else {
            port_rebound_ok = true;
            close(rebound_fd);
        }
    }
    if (action == TEST_GATHER_CONFIRM_DROP_FAILURE &&
        test_monotonic_ms() - case_started_ms > 5000) {
        result = 1;
    }
    if (action == TEST_GATHER_PARTIAL_DRIP_FAILURE) {
        int64_t elapsed_ms = test_monotonic_ms() - case_started_ms;

        /*
         * Partial bytes are not protocol progress: fail near the 500 ms
         * inactivity limit, well before the 2500 ms absolute hard cap.
         */
        if (elapsed_ms < 400 || elapsed_ms > 1500) {
            result = 1;
        }
    }
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root);
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_SESSION_ALLOC_FAIL");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_DELAY_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS");
    (void)unsetenv(
        "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER");
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS",
                 "10",
                 1);
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
    if (result != 0) {
        report_two_rank_case_diagnostic(action,
                                        root_status,
                                        commit_attempted,
                                        committed,
                                        peer_wait_status,
                                        port_rebound_checked,
                                        port_rebound_ok,
                                        &root_telemetry);
    }
    return result;
}

static int
run_two_rank_case(int port,
                  TestRootAction action,
                  bool mismatched_peer_name)
{
    return run_two_rank_case_with_peer_start_delay(port,
                                                    action,
                                                    mismatched_peer_name,
                                                    false,
                                                    false);
}

static int
run_dropped_counter_saturation_case(int port)
{
    int result;

    test_saturate_dropped_counters = true;
    result = run_two_rank_case(port, TEST_ROOT_COMMIT, false);
    test_saturate_dropped_counters = false;
    return result;
}

/*
 * Exercises the production sequence used when both CPU and CUDA reporting are
 * enabled.  The same two processes complete CPU first, then use CUDA's
 * independent port pair.  Keeping the child alive across both phases catches
 * accidental channel state leakage as well as TIME_WAIT port reuse bugs.
 */
static int
run_two_rank_sequential_channels(int port, bool cuda_schema_mismatch)
{
    PeakReportSnapshot* root_cpu = fixture_snapshot(0, false);
    PeakReportSnapshot* root_cuda = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    PeakSocketReportTestTelemetry telemetry;
    PeakSocketReportStatus cpu_status = PEAK_SOCKET_REPORT_FAILED;
    PeakSocketReportStatus cuda_status = PEAK_SOCKET_REPORT_FAILED;
    char port_text[16];
    char token_text[64];
    pid_t child;
    int child_status = -1;
    int result = 0;

    if (root_cpu == NULL || root_cuda == NULL) {
        peak_report_snapshot_destroy(root_cpu);
        peak_report_snapshot_destroy(root_cuda);
        return 1;
    }
    snprintf(port_text, sizeof(port_text), "%d", port);
    snprintf(token_text,
             sizeof(token_text),
             "socket-sequential-channels-%ld-%d",
             (long)getpid(),
             port);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_HOST", "127.0.0.1", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", port_text, 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "1500", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS", "3000", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN", token_text, 1);
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS",
                 "10000",
                 1);
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS", "10", 1);

    child = fork();
    if (child < 0) {
        peak_report_snapshot_destroy(root_cpu);
        peak_report_snapshot_destroy(root_cuda);
        return 1;
    }
    if (child == 0) {
        PeakReportSnapshot* peer_cpu = fixture_snapshot(1, false);
        PeakReportSnapshot* peer_cuda =
            fixture_snapshot(1, cuda_schema_mismatch);
        PeakReportSnapshot* peer_aggregate = NULL;
        PeakSocketReportSession* peer_session = NULL;
        PeakSocketReportStatus peer_cpu_status;
        PeakSocketReportStatus peer_cuda_status;

        set_test_rank(1, 2);
        if (peer_cpu == NULL || peer_cuda == NULL) {
            peak_report_snapshot_destroy(peer_cpu);
            peak_report_snapshot_destroy(peer_cuda);
            _exit(99);
        }
        peer_cpu_status = peak_socket_report_transport_begin_channel(
            peer_cpu,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            PEAK_SOCKET_REPORT_CHANNEL_CPU,
            &peer_session,
            &peer_aggregate);
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        peak_socket_report_transport_abort(peer_session);
        peak_report_snapshot_destroy(peer_aggregate);
        if (peer_cpu_status != PEAK_SOCKET_REPORT_PEER_RELEASED ||
            telemetry.wire_version != 13U ||
            !telemetry.peer_receipt_received ||
            !telemetry.peer_confirmation_sent ||
            !telemetry.peer_release_started ||
            !telemetry.peer_release_decision_received ||
            !telemetry.peer_release_confirmation_sent ||
            telemetry.peer_release_decision != TEST_RELEASE_ACK) {
            peak_report_snapshot_destroy(peer_cpu);
            peak_report_snapshot_destroy(peer_cuda);
            _exit(99);
        }
        peak_report_snapshot_destroy(peer_cpu);

        peer_aggregate = NULL;
        peer_session = NULL;
        peer_cuda_status = peak_socket_report_transport_begin_channel(
            peer_cuda,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            PEAK_SOCKET_REPORT_CHANNEL_CUDA,
            &peer_session,
            &peer_aggregate);
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        peak_socket_report_transport_abort(peer_session);
        peak_report_snapshot_destroy(peer_aggregate);
        peak_report_snapshot_destroy(peer_cuda);
        if ((!cuda_schema_mismatch &&
             (peer_cuda_status != PEAK_SOCKET_REPORT_PEER_RELEASED ||
              telemetry.wire_version != 13U ||
              !telemetry.peer_receipt_received ||
              !telemetry.peer_confirmation_sent ||
              !telemetry.peer_release_started ||
              !telemetry.peer_release_decision_received ||
              !telemetry.peer_release_confirmation_sent ||
              telemetry.peer_release_decision != TEST_RELEASE_ACK)) ||
            (cuda_schema_mismatch &&
             (peer_cuda_status != PEAK_SOCKET_REPORT_FAILED ||
              telemetry.peer_release_decision == TEST_RELEASE_ACK))) {
            _exit(99);
        }
        _exit(0);
    }

    set_test_rank(0, 2);
    cpu_status = peak_socket_report_transport_begin_channel(
        root_cpu,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        PEAK_SOCKET_REPORT_CHANNEL_CPU,
        &session,
        &aggregate);
    memset(&telemetry, 0, sizeof(telemetry));
    peak_socket_report_test_telemetry_get(&telemetry);
    if (cpu_status != PEAK_SOCKET_REPORT_ROOT_PREPARED || session == NULL ||
        !aggregate_matches(aggregate) || telemetry.wire_version != 13U ||
        telemetry.root_payload_count != 1U ||
        telemetry.root_receipt_count != 1U ||
        telemetry.root_confirmation_count != 1U ||
        telemetry.root_release_target_count != 1U) {
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
        if (telemetry.root_release_decision != TEST_RELEASE_ACK ||
            telemetry.root_release_confirmed_count != 1U) {
            result = 1;
        }
    }
    peak_report_snapshot_destroy(aggregate);
    aggregate = NULL;
    if (session == NULL) {
        int gather_fd = bind_test_tcp_port(port, true);
        int release_fd = bind_test_tcp_port(port + 1, true);

        if (gather_fd < 0 || release_fd < 0) {
            result = 1;
        }
        if (gather_fd >= 0) {
            close(gather_fd);
        }
        if (release_fd >= 0) {
            close(release_fd);
        }
    }

    if (cpu_status == PEAK_SOCKET_REPORT_ROOT_PREPARED && result == 0) {
        cuda_status = peak_socket_report_transport_begin_channel(
            root_cuda,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            PEAK_SOCKET_REPORT_CHANNEL_CUDA,
            &session,
            &aggregate);
        memset(&telemetry, 0, sizeof(telemetry));
        peak_socket_report_test_telemetry_get(&telemetry);
        if (cuda_schema_mismatch) {
            if (cuda_status != PEAK_SOCKET_REPORT_FAILED || session != NULL ||
                aggregate != NULL || telemetry.root_release_decision ==
                    TEST_RELEASE_ACK) {
                result = 1;
                peak_socket_report_transport_abort(session);
                session = NULL;
            }
        } else if (cuda_status != PEAK_SOCKET_REPORT_ROOT_PREPARED ||
                   session == NULL || !aggregate_matches(aggregate) ||
                   telemetry.wire_version != 13U ||
                   telemetry.root_payload_count != 1U ||
                   telemetry.root_receipt_count != 1U ||
                   telemetry.root_confirmation_count != 1U ||
                   telemetry.root_release_target_count != 1U) {
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
            if (telemetry.root_release_decision != TEST_RELEASE_ACK ||
                telemetry.root_release_confirmed_count != 1U) {
                result = 1;
            }
        }
        peak_report_snapshot_destroy(aggregate);
        aggregate = NULL;
        if (session == NULL) {
            int gather_fd = bind_test_tcp_port(port + 2, true);
            int release_fd = bind_test_tcp_port(port + 3, true);

            if (gather_fd < 0 || release_fd < 0) {
                result = 1;
            }
            if (gather_fd >= 0) {
                close(gather_fd);
            }
            if (release_fd >= 0) {
                close(release_fd);
            }
        }
    }

    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        result = 1;
    }
    peak_socket_report_transport_abort(session);
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root_cpu);
    peak_report_snapshot_destroy(root_cuda);
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS");
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS");
    return result;
}

static int
run_two_rank_sequential_channels_repeatedly(int port)
{
    for (int iteration = 0; iteration < 20; iteration++) {
        if (run_two_rank_sequential_channels(port, false) != 0) {
            return 1;
        }
    }
    return 0;
}

static int
check_release_port_reservation_fails_before_gather(int port)
{
    PeakReportSnapshot* root = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    PeakSocketReportTestTelemetry telemetry;
    char port_text[16];
    int competing_fd;
    PeakSocketReportStatus status;

    if (root == NULL) {
        return 1;
    }
    competing_fd = bind_test_tcp_port(port + 1, false);
    if (competing_fd < 0) {
        peak_report_snapshot_destroy(root);
        return 1;
    }
    snprintf(port_text, sizeof(port_text), "%d", port);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_HOST", "127.0.0.1", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", port_text, 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "100", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN",
                 "release-reservation-collision",
                 1);
    set_test_rank(0, 2);
    status = peak_socket_report_transport_begin(
        root,
        PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
        &session,
        &aggregate);
    memset(&telemetry, 0, sizeof(telemetry));
    peak_socket_report_test_telemetry_get(&telemetry);
    close(competing_fd);
    peak_socket_report_transport_abort(session);
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(root);
    return status != PEAK_SOCKET_REPORT_FAILED ||
           telemetry.root_payload_count != 0U ||
           telemetry.root_receipt_count != 0U ||
           telemetry.root_confirmation_count != 0U;
}

static int
check_progress_deadline_hard_cap(void)
{
    int64_t hard_deadline_us = 250000;
    int64_t deadline_us =
        peak_socket_report_test_progress_deadline_us(
            0, hard_deadline_us, 100);

    if (deadline_us != 100000) {
        return 1;
    }
    deadline_us = peak_socket_report_test_progress_deadline_us(
        90000, hard_deadline_us, 100);
    if (deadline_us != 190000) {
        return 1;
    }
    deadline_us = peak_socket_report_test_progress_deadline_us(
        180000, hard_deadline_us, 100);
    if (deadline_us != hard_deadline_us) {
        return 1;
    }
    return peak_socket_report_test_progress_deadline_us(
               240000, hard_deadline_us, 100) !=
           hard_deadline_us;
}

static int
check_default_port_derivation(void)
{
    int fallback_port;
    int pmi_port_a;
    int pmi_port_b;
    int pmix_port;
    int rank_zero_port;
    int rank_one_port;
    int override_port_a;
    int override_port_b;
    int failed;

    clear_socket_identity_environment();
    clear_rank_environment();
    fallback_port = peak_socket_report_test_default_port();

    (void)setenv("PMI_JOBID", "hydra-launch-a", 1);
    pmi_port_a = peak_socket_report_test_default_port();
    (void)setenv("PMI_RANK", "0", 1);
    rank_zero_port = peak_socket_report_test_default_port();
    (void)setenv("PMI_RANK", "1", 1);
    rank_one_port = peak_socket_report_test_default_port();

    (void)setenv("PMI_JOBID", "hydra-launch-b", 1);
    pmi_port_b = peak_socket_report_test_default_port();

    (void)unsetenv("PMI_JOBID");
    (void)setenv("PMIX_NAMESPACE", "pmix-launch-a", 1);
    pmix_port = peak_socket_report_test_default_port();

    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN",
                 "explicit-launch-token",
                 1);
    override_port_a = peak_socket_report_test_default_port();
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TOKEN",
                 "different-explicit-token",
                 1);
    override_port_b = peak_socket_report_test_default_port();

    failed =
        fallback_port < TEST_DEFAULT_PORT_BASE ||
        fallback_port >=
            TEST_DEFAULT_PORT_BASE + TEST_DEFAULT_PORT_SPAN ||
        pmi_port_a < TEST_DEFAULT_PORT_BASE ||
        pmi_port_a >=
            TEST_DEFAULT_PORT_BASE + TEST_DEFAULT_PORT_SPAN ||
        pmi_port_b < TEST_DEFAULT_PORT_BASE ||
        pmi_port_b >=
            TEST_DEFAULT_PORT_BASE + TEST_DEFAULT_PORT_SPAN ||
        pmix_port < TEST_DEFAULT_PORT_BASE ||
        pmix_port >=
            TEST_DEFAULT_PORT_BASE + TEST_DEFAULT_PORT_SPAN ||
        fallback_port == pmi_port_a ||
        pmi_port_a == pmi_port_b ||
        pmi_port_b == pmix_port ||
        rank_zero_port != pmi_port_a ||
        rank_one_port != pmi_port_a ||
        override_port_a != pmix_port ||
        override_port_a != override_port_b;

    clear_socket_identity_environment();
    clear_rank_environment();
    return failed;
}

static int
check_sequential_channel_ports(void)
{
    int result;

    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", "45100", 1);
    result = peak_socket_report_test_channel_port(
                 PEAK_SOCKET_REPORT_CHANNEL_CPU) != 45100 ||
             peak_socket_report_test_channel_port(
                 PEAK_SOCKET_REPORT_CHANNEL_CUDA) != 45102;
    (void)setenv("PEAK_OUTPUT_AGGREGATION_PORT", "65534", 1);
    result |= peak_socket_report_test_channel_port(
                  PEAK_SOCKET_REPORT_CHANNEL_CUDA) != -1;
    (void)unsetenv("PEAK_OUTPUT_AGGREGATION_PORT");
    return result;
}

static int
check_gather_admission_waves(void)
{
    return peak_socket_report_test_admission_delay_ms(
               0, 4096U, 128U, 5000U, 60000U, 220000U) != 0U ||
           peak_socket_report_test_admission_delay_ms(
               1, 4096U, 128U, 5000U, 60000U, 220000U) != 0U ||
           peak_socket_report_test_admission_delay_ms(
               128, 4096U, 128U, 5000U, 60000U, 220000U) != 0U ||
           peak_socket_report_test_admission_delay_ms(
               129, 4096U, 128U, 5000U, 60000U, 220000U) != 5000U ||
           peak_socket_report_test_admission_delay_ms(
               256, 4096U, 128U, 5000U, 60000U, 220000U) != 5000U ||
           peak_socket_report_test_admission_delay_ms(
               257, 4096U, 128U, 5000U, 60000U, 220000U) != 10000U ||
           peak_socket_report_test_admission_delay_ms(
               4095, 4096U, 128U, 5000U, 60000U, 220000U) != 155000U ||
           peak_socket_report_test_admission_delay_ms(
               4095, 4096U, 64U, 5000U, 60000U, 220000U) != 157500U ||
           peak_socket_report_test_admission_delay_ms(
               4095, 4096U, 128U, 5000U, 1000U, 161000U) != 10323U ||
           peak_socket_report_test_admission_delay_ms(
               INT_MAX,
               UINT_MAX,
               1U,
               UINT_MAX,
               60000U,
               360000U) > 300000U ||
           peak_socket_report_test_latest_admission_ms(
               4095, 4096U, 16U, 5000U, 60000U, 220000U) != 160000U ||
           peak_socket_report_test_latest_admission_ms(
               4095, 4096U, 1U, 5000U, 60000U, 220000U) != 159705U;
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
    PeakSocketReportStatus root_status = PEAK_SOCKET_REPORT_FAILED;
    PeakSocketReportTestTelemetry root_telemetry = {0};
    bool commit_attempted = false;
    bool committed = false;

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
    /*
     * Launching 31 peers on a hosted runner can take longer than the normal
     * per-phase budget even when every connection makes progress. Keep this
     * many-rank regression bounded but allow a scheduling margin before the
     * normal five-second protocol budget begins.
     */
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "5000", 1);
    (void)setenv("PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS",
                 "15000",
                 1);
    /* Allow fork/scheduler startup only; transport progress stays at 5 s. */
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS",
                 "10000",
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
            if (telemetry.wire_version != 13U ||
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
        set_test_rank(0, size);
        root_status = peak_socket_report_transport_begin(
            root,
            PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
            &session,
            &aggregate);
        peak_socket_report_test_telemetry_get(&root_telemetry);
        if (root_status != PEAK_SOCKET_REPORT_ROOT_PREPARED ||
            session == NULL || aggregate == NULL ||
            root_telemetry.wire_version != 13U ||
            root_telemetry.root_payload_count != (uint32_t)(size - 1) ||
            root_telemetry.root_receipt_count != (uint32_t)(size - 1) ||
            root_telemetry.root_confirmation_count !=
                (uint32_t)(size - 1) ||
            root_telemetry.root_max_active == 0U ||
            root_telemetry.root_max_active > (uint32_t)(size - 1) ||
            (exercise_concurrency &&
             root_telemetry.root_max_active <= 1U) ||
            root_telemetry.root_release_target_count !=
                (uint32_t)(size - 1) ||
            aggregate->rank_count != size ||
            aggregate->num_calls[0] !=
                10UL + 7UL * (unsigned long)(size - 1) ||
            aggregate->num_calls[1] !=
                6UL * (unsigned long)(size - 1)) {
            result = 1;
            peak_socket_report_transport_abort(session);
            session = NULL;
        } else {
            commit_attempted = true;
            committed = peak_socket_report_transport_commit(session);
            if (!committed) {
                result = 1;
                session = NULL;
            } else {
                session = NULL;
                peak_socket_report_test_telemetry_get(&root_telemetry);
                if (root_telemetry.root_release_decision !=
                        TEST_RELEASE_ACK ||
                    root_telemetry.root_release_confirmed_count !=
                        (uint32_t)(size - 1)) {
                    result = 1;
                }
            }
        }
    } else {
        peak_socket_report_transport_abort(session);
        session = NULL;
    }

    for (int i = 0; i < started; i++) {
        int child_status;

        if (wait_for_expected_child(
                children[i],
                PEAK_SOCKET_REPORT_PEER_RELEASED,
                &child_status) != 0) {
            fprintf(stderr,
                    "socket many-rank child diagnostic: rank=%d pid=%ld "
                    "wait_status=%d exited=%d exit=%d signal=%d\n",
                    i + 1,
                    (long)children[i],
                    child_status,
                    child_status >= 0 && WIFEXITED(child_status),
                    child_status >= 0 && WIFEXITED(child_status)
                        ? WEXITSTATUS(child_status)
                        : -1,
                    child_status >= 0 && WIFSIGNALED(child_status)
                        ? WTERMSIG(child_status)
                        : -1);
            result = 1;
        }
    }
    if (result != 0) {
        report_many_rank_case_diagnostic(size,
                                         exercise_concurrency,
                                         root_status,
                                         commit_attempted,
                                         committed,
                                         &root_telemetry);
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
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_STARTUP_GRACE_MS");
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
    (void)setenv("PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS", "1500", 1);
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
            telemetry.wire_version != 13U ||
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
                children[i], PEAK_SOCKET_REPORT_FAILED, NULL) != 0) {
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
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_NONCE_FAIL", "1", 1);
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
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_NONCE_FAIL");
    return result;
}

static int
check_multi_rank_nonce_failure(void)
{
    PeakReportSnapshot* local = fixture_snapshot(0, false);
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportSession* session = NULL;
    PeakSocketReportStatus status;

    if (local == NULL) return 1;
    clear_rank_environment();
    set_test_rank(0, 2);
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_NONCE_FAIL", "1", 1);
    status = peak_socket_report_transport_begin(
        local, PEAK_SOCKET_REPORT_RANK_ENV_ONLY, &session, &aggregate);
    (void)unsetenv("PEAK_TEST_OUTPUT_AGGREGATION_NONCE_FAIL");
    clear_rank_environment();
    peak_socket_report_transport_abort(session);
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(local);
    return status == PEAK_SOCKET_REPORT_FAILED && session == NULL &&
                   aggregate == NULL
               ? 0 : 1;
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
     * consumes base..base+55, and the kernel lock prevents parallel CTest
     * processes, including different UIDs, from choosing the same range.
     */
    int port_lock_fd = -1;
    int base_port = reserve_test_port_slot(&port_lock_fd);
    int failed;

    (void)setenv("PEAK_VERBOSITY", "silent", 1);
    (void)setenv("PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS",
                 "10",
                 1);
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
    CHECK_SOCKET_CASE("progress-hard-cap",
                      check_progress_deadline_hard_cap());
    CHECK_SOCKET_CASE("default-port-derivation",
                      check_default_port_derivation());
    CHECK_SOCKET_CASE("sequential-channel-ports",
                      check_sequential_channel_ports());
    CHECK_SOCKET_CASE("gather-admission-waves",
                      check_gather_admission_waves());
    check_listener_bind_scope(base_port + 56);
    CHECK_SOCKET_CASE("single-process", check_single_process_clone());
    CHECK_SOCKET_CASE("multi-rank-nonce-failure", check_multi_rank_nonce_failure());
    CHECK_SOCKET_CASE("required-rank", check_required_rank_metadata());
    CHECK_SOCKET_CASE("invalid-output-pointers",
                      check_invalid_output_pointers_are_cleared());
    CHECK_SOCKET_CASE(
        "release-port-pre-reservation",
        check_release_port_reservation_fails_before_gather(
            base_port + 38));
    CHECK_SOCKET_CASE(
        "sequential-cpu-cuda-20x",
        run_two_rank_sequential_channels_repeatedly(base_port + 46));
    CHECK_SOCKET_CASE(
        "sequential-cpu-success-cuda-schema-fallback",
        run_two_rank_sequential_channels(base_port + 50, true));
    CHECK_SOCKET_CASE(
        "commit",
        run_two_rank_case(base_port, TEST_ROOT_COMMIT, false));
    CHECK_SOCKET_CASE(
        "dropped-counter-saturation",
        run_dropped_counter_saturation_case(base_port + 44));
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
        "commit-after-peer-ready",
        run_two_rank_case(base_port + 8,
                          TEST_ROOT_COMMIT_AFTER_PEER_READY,
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
        "session-allocation-fallback",
        run_two_rank_case(base_port + 40,
                          TEST_ROOT_SESSION_ALLOC_FAILURE,
                          false));
    CHECK_SOCKET_CASE(
        "session-allocation-fallback-delayed-peer",
        run_two_rank_case_with_peer_start_delay(
            base_port + 52,
            TEST_ROOT_SESSION_ALLOC_FAILURE,
            false,
            true,
            false));
    CHECK_SOCKET_CASE(
        "listener-ready-peer-exit",
        run_two_rank_case_with_peer_start_delay(
            base_port + 54,
            TEST_ROOT_SESSION_ALLOC_FAILURE,
            false,
            false,
            true));
    CHECK_SOCKET_CASE(
        "gather-partial-success",
        run_two_rank_case(base_port + 18,
                          TEST_GATHER_PARTIAL_SUCCESS,
                          false));
    CHECK_SOCKET_CASE(
        "gather-partial-drip-failure",
        run_two_rank_case(base_port + 42,
                          TEST_GATHER_PARTIAL_DRIP_FAILURE,
                          false));
    CHECK_SOCKET_CASE(
        "gather-progress-success",
        run_two_rank_case(base_port + 36,
                          TEST_GATHER_PROGRESS_SUCCESS,
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
