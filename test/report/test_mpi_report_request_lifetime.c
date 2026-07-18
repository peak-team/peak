#include "internal/general_listener/mpi_report_transport.h"

#include <mpi.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    TEST_COLLECTIVE_ALLREDUCE = 0,
    TEST_COLLECTIVE_REDUCE = 1,
    TEST_COLLECTIVE_BCAST = 2,
    TEST_COLLECTIVE_COUNT = 54,
    TEST_UNIQUE_LABEL_COUNT = 29,
};

typedef enum {
    TEST_REQUEST_COMPLETE = 0,
    TEST_REQUEST_ERROR,
    TEST_REQUEST_TIMEOUT,
} TestRequestMode;

typedef struct {
    const char* label;
    int kind;
    int count;
    MPI_Datatype datatype;
    MPI_Op operation;
    int root;
} TestCollectiveTrace;

typedef struct {
    double value;
    int rank;
} TestDoubleInt;

static TestCollectiveTrace trace_records[TEST_COLLECTIVE_COUNT];
static int trace_count;
static int trace_failures;
static int fake_rank;
static int fake_size = 1;
static int fake_operation_count;
static int fake_target_ordinal;
static TestRequestMode fake_request_mode;
static void* fake_target_send;
static void* fake_target_receive;
static MPI_Request* fake_target_request;
static size_t fake_target_bytes;

static size_t
fake_datatype_extent(MPI_Datatype datatype)
{
    switch (datatype) {
    case MPI_INT:
        return sizeof(int);
    case MPI_UNSIGNED:
        return sizeof(unsigned int);
    case MPI_UINT64_T:
        return sizeof(uint64_t);
    case MPI_UNSIGNED_LONG:
        return sizeof(unsigned long);
    case MPI_UNSIGNED_LONG_LONG:
        return sizeof(unsigned long long);
    case MPI_DOUBLE:
        return sizeof(double);
    case MPI_FLOAT:
        return sizeof(float);
    case MPI_DOUBLE_INT:
        return sizeof(TestDoubleInt);
    default:
        return 0;
    }
}

static void
fake_reset(int target_ordinal, TestRequestMode mode, int rank, int size)
{
    memset(trace_records, 0, sizeof(trace_records));
    trace_count = 0;
    trace_failures = 0;
    fake_rank = rank;
    fake_size = size;
    fake_operation_count = 0;
    fake_target_ordinal = target_ordinal;
    fake_request_mode = mode;
    fake_target_send = NULL;
    fake_target_receive = NULL;
    fake_target_request = NULL;
    fake_target_bytes = 0;
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
    if (trace_count >= TEST_COLLECTIVE_COUNT || label == NULL) {
        trace_failures++;
        return;
    }
    trace_records[trace_count++] = (TestCollectiveTrace){
        .label = label,
        .kind = kind,
        .count = count,
        .datatype = datatype,
        .operation = operation,
        .root = root,
    };
    if (staged_receive == NULL || staged_receive == original_receive) {
        trace_failures++;
    }
    if (kind == TEST_COLLECTIVE_BCAST) {
        if (staged_send != NULL ||
            (buffer_size != 0 &&
             memcmp(staged_receive, original_receive, buffer_size) != 0)) {
            trace_failures++;
        }
    } else {
        if (staged_send == NULL || staged_send == original_send ||
            (buffer_size != 0 &&
             memcmp(staged_send, original_send, buffer_size) != 0)) {
            trace_failures++;
        }
    }
}

int
MPI_Init(int* argc, char*** argv)
{
    (void)argc;
    (void)argv;
    return MPI_SUCCESS;
}

int
MPI_Initialized(int* initialized)
{
    *initialized = 1;
    return MPI_SUCCESS;
}

int
MPI_Comm_rank(MPI_Comm communicator, int* rank)
{
    (void)communicator;
    *rank = fake_rank;
    return MPI_SUCCESS;
}

int
MPI_Comm_size(MPI_Comm communicator, int* size)
{
    (void)communicator;
    *size = fake_size;
    return MPI_SUCCESS;
}

int
MPI_Type_get_extent(MPI_Datatype datatype,
                    MPI_Aint* lower_bound,
                    MPI_Aint* extent)
{
    size_t size = fake_datatype_extent(datatype);

    if (size == 0) {
        return MPI_ERR_OTHER;
    }
    *lower_bound = 0;
    *extent = (MPI_Aint)size;
    return MPI_SUCCESS;
}

int
MPI_Type_size(MPI_Datatype datatype, int* size)
{
    size_t extent = fake_datatype_extent(datatype);

    if (extent == 0 || extent > (size_t)INT32_MAX) {
        return MPI_ERR_OTHER;
    }
    *size = (int)extent;
    return MPI_SUCCESS;
}

static int
fake_begin_collective(const void* send_buffer,
                      void* receive_buffer,
                      int count,
                      MPI_Datatype datatype,
                      MPI_Request* request,
                      bool copy_send)
{
    size_t bytes = fake_datatype_extent(datatype) * (size_t)count;
    int ordinal = ++fake_operation_count;

    if (copy_send && bytes != 0) {
        memcpy(receive_buffer, send_buffer, bytes);
    }
    request->ordinal = ordinal;
    request->active = 1;
    if (ordinal == fake_target_ordinal) {
        fake_target_send = (void*)send_buffer;
        fake_target_receive = receive_buffer;
        fake_target_request = request;
        fake_target_bytes = bytes;
    }
    return MPI_SUCCESS;
}

int
MPI_Iallreduce(const void* send_buffer,
               void* receive_buffer,
               int count,
               MPI_Datatype datatype,
               MPI_Op operation,
               MPI_Comm communicator,
               MPI_Request* request)
{
    (void)operation;
    (void)communicator;
    return fake_begin_collective(send_buffer,
                                 receive_buffer,
                                 count,
                                 datatype,
                                 request,
                                 true);
}

int
MPI_Ireduce(const void* send_buffer,
            void* receive_buffer,
            int count,
            MPI_Datatype datatype,
            MPI_Op operation,
            int root,
            MPI_Comm communicator,
            MPI_Request* request)
{
    (void)operation;
    (void)root;
    (void)communicator;
    return fake_begin_collective(send_buffer,
                                 receive_buffer,
                                 count,
                                 datatype,
                                 request,
                                 true);
}

int
MPI_Ibcast(void* buffer,
           int count,
           MPI_Datatype datatype,
           int root,
           MPI_Comm communicator,
           MPI_Request* request)
{
    (void)root;
    (void)communicator;
    return fake_begin_collective(NULL,
                                 buffer,
                                 count,
                                 datatype,
                                 request,
                                 false);
}

int
MPI_Test(MPI_Request* request, int* done, MPI_Status* status)
{
    (void)status;
    if (request->ordinal == fake_target_ordinal) {
        if (fake_request_mode == TEST_REQUEST_ERROR) {
            return MPI_ERR_OTHER;
        }
        if (fake_request_mode == TEST_REQUEST_TIMEOUT) {
            *done = 0;
            return MPI_SUCCESS;
        }
    }
    request->active = 0;
    *done = 1;
    return MPI_SUCCESS;
}

static PeakReportSnapshot*
fixture_snapshot(void)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(2);

    if (snapshot == NULL ||
        !peak_report_snapshot_set_program(snapshot, "fake-mpi") ||
        !peak_report_snapshot_set_name(snapshot, 0, "alpha") ||
        !peak_report_snapshot_set_name(snapshot, 1, "beta")) {
        peak_report_snapshot_destroy(snapshot);
        return NULL;
    }
    snapshot->instrumented[0] = 1;
    snapshot->instrumented[1] = 1;
    snapshot->detached[0] = 1;
    snapshot->reattached[1] = 1;
    snapshot->revisited[1] = 1;
    snapshot->num_calls[0] = 3;
    snapshot->num_calls[1] = 5;
    snapshot->total_time[0] = 2.0;
    snapshot->total_time[1] = 4.0;
    snapshot->max_total_time[0] = 2.0;
    snapshot->max_total_time[1] = 4.0;
    snapshot->min_total_time[0] = 2.0;
    snapshot->min_total_time[1] = 4.0;
    snapshot->exclusive_time[0] = 1.0;
    snapshot->exclusive_time[1] = 3.0;
    snapshot->max_time[0] = 0.5f;
    snapshot->max_time[1] = 0.75f;
    snapshot->min_time[0] = 0.1f;
    snapshot->min_time[1] = 0.2f;
    snapshot->thread_count[0] = 1;
    snapshot->thread_count[1] = 2;
    snapshot->overhead_per_call = 1e-7;
    snapshot->overhead.valid = true;
    snapshot->overhead.accounting_valid = true;
    snapshot->overhead.local_ranks = 1;
    snapshot->overhead.stop_window_count = 2;
    snapshot->overhead.failed_stop_window_count = 1;
    snapshot->overhead.elapsed_seconds = 10.0;
    snapshot->overhead.profile_seconds = 1.0;
    snapshot->overhead.control_seconds = 0.5;
    snapshot->overhead.management_seconds = 0.25;
    snapshot->overhead.control_risk_seconds = 0.75;
    snapshot->overhead.profile_control_risk_seconds = 1.75;
    snapshot->overhead.profile_ratio = 0.1;
    snapshot->overhead.control_ratio = 0.05;
    snapshot->overhead.management_ratio = 0.025;
    snapshot->overhead.control_risk_ratio = 0.075;
    snapshot->overhead.profile_control_risk_ratio = 0.175;
    snapshot->overhead.ratio = 0.15;
    return snapshot;
}

static bool
trace_matches(int ordinal,
              const char* label,
              int kind,
              int count,
              MPI_Datatype datatype,
              MPI_Op operation,
              int root)
{
    const TestCollectiveTrace* actual = &trace_records[ordinal - 1];

    return strcmp(actual->label, label) == 0 && actual->kind == kind &&
           actual->count == count && actual->datatype == datatype &&
           actual->operation == operation && actual->root == root;
}

static int
validate_golden_trace(void)
{
    static const char* bcast_labels[] = {
        "profile-control-ratio-owner",
        "profile-control-ratio-tuple-valid",
        "profile-control-ratio-tuple-local-ranks",
        "profile-control-ratio-tuple-counts",
        "profile-control-ratio-tuple-doubles",
    };
    static const int bcast_counts[] = {1, 1, 1, 2, 12};
    static const MPI_Datatype bcast_types[] = {
        MPI_INT,
        MPI_INT,
        MPI_UNSIGNED,
        MPI_UINT64_T,
        MPI_DOUBLE,
    };
    int failures = 0;
    int unique_labels = 0;

#define EXPECT(ordinal, label, kind, count, datatype, operation, root)       \
    do {                                                                      \
        if (!trace_matches((ordinal),                                         \
                           (label),                                           \
                           (kind),                                            \
                           (count),                                           \
                           (datatype),                                        \
                           (operation),                                       \
                           (root))) {                                         \
            failures++;                                                       \
        }                                                                     \
    } while (0)

    if (trace_count != TEST_COLLECTIVE_COUNT || trace_failures != 0) {
        failures++;
    }
    EXPECT(1, "elapsed-valid", TEST_COLLECTIVE_ALLREDUCE, 1, MPI_INT, MPI_MIN, -1);
    EXPECT(2, "accounting-valid", TEST_COLLECTIVE_ALLREDUCE, 1, MPI_INT, MPI_MIN, -1);
    EXPECT(3, "hook-count-min", TEST_COLLECTIVE_ALLREDUCE, 1, MPI_UNSIGNED_LONG, MPI_MIN, -1);
    EXPECT(4, "hook-count-max", TEST_COLLECTIVE_ALLREDUCE, 1, MPI_UNSIGNED_LONG, MPI_MAX, -1);
    EXPECT(5, "duplicate-hook-name-check", TEST_COLLECTIVE_ALLREDUCE, 1, MPI_INT, MPI_MAX, -1);
    EXPECT(6, "hook-slot-min-hash", TEST_COLLECTIVE_ALLREDUCE, 2, MPI_UINT64_T, MPI_MIN, -1);
    EXPECT(7, "hook-slot-max-hash", TEST_COLLECTIVE_ALLREDUCE, 2, MPI_UINT64_T, MPI_MAX, -1);
    EXPECT(8, "profile-control-ratio-maxloc", TEST_COLLECTIVE_REDUCE, 6, MPI_DOUBLE_INT, MPI_MAXLOC, 0);
    for (int ordinal = 9; ordinal <= 38; ordinal++) {
        int field = (ordinal - 9) % 5;
        EXPECT(ordinal,
               bcast_labels[field],
               TEST_COLLECTIVE_BCAST,
               bcast_counts[field],
               bcast_types[field],
               MPI_OP_NULL,
               0);
    }
    EXPECT(39, "profile-seconds", TEST_COLLECTIVE_REDUCE, 1, MPI_DOUBLE, MPI_SUM, 0);
    EXPECT(40, "failed-stop-window-max", TEST_COLLECTIVE_ALLREDUCE, 1, MPI_UINT64_T, MPI_MAX, -1);
    EXPECT(41, "failed-stop-window-count", TEST_COLLECTIVE_REDUCE, 1, MPI_UINT64_T, MPI_SUM, 0);
    EXPECT(42, "elapsed-min", TEST_COLLECTIVE_REDUCE, 1, MPI_DOUBLE, MPI_MIN, 0);
    EXPECT(43, "elapsed-max", TEST_COLLECTIVE_REDUCE, 1, MPI_DOUBLE, MPI_MAX, 0);
    EXPECT(44, "sum-num-calls", TEST_COLLECTIVE_REDUCE, 2, MPI_UNSIGNED_LONG, MPI_SUM, 0);
    EXPECT(45, "sum-total-time", TEST_COLLECTIVE_REDUCE, 2, MPI_DOUBLE, MPI_SUM, 0);
    EXPECT(46, "max-total-time", TEST_COLLECTIVE_REDUCE, 2, MPI_DOUBLE, MPI_MAX, 0);
    EXPECT(47, "min-total-time", TEST_COLLECTIVE_REDUCE, 2, MPI_DOUBLE, MPI_MIN, 0);
    EXPECT(48, "sum-exclusive-time", TEST_COLLECTIVE_REDUCE, 2, MPI_DOUBLE, MPI_SUM, 0);
    EXPECT(49, "sum-max-time", TEST_COLLECTIVE_REDUCE, 2, MPI_FLOAT, MPI_MAX, 0);
    EXPECT(50, "sum-min-time", TEST_COLLECTIVE_REDUCE, 2, MPI_FLOAT, MPI_MIN, 0);
    EXPECT(51, "thread-count", TEST_COLLECTIVE_REDUCE, 2, MPI_UNSIGNED_LONG, MPI_SUM, 0);
    EXPECT(52, "detached-marker", TEST_COLLECTIVE_REDUCE, 2, MPI_INT, MPI_MAX, 0);
    EXPECT(53, "reattached-marker", TEST_COLLECTIVE_REDUCE, 2, MPI_INT, MPI_MAX, 0);
    EXPECT(54, "revisited-marker", TEST_COLLECTIVE_REDUCE, 2, MPI_INT, MPI_MAX, 0);

#undef EXPECT

    for (int i = 0; i < trace_count; i++) {
        bool seen = false;

        for (int j = 0; j < i; j++) {
            if (strcmp(trace_records[i].label, trace_records[j].label) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique_labels++;
        }
    }
    if (unique_labels != TEST_UNIQUE_LABEL_COUNT) {
        failures++;
    }
    return failures;
}

static int
run_success_case(int rank, int size)
{
    PeakReportSnapshot* local;
    PeakReportSnapshot* aggregate = NULL;
    PeakMpiReportTransportResult result;
    int failures = 0;

    fake_reset(0, TEST_REQUEST_COMPLETE, rank, size);
    peak_mpi_report_transport_reset_failed_closed();
    local = fixture_snapshot();
    if (local == NULL) {
        return 1;
    }
    result = peak_mpi_report_transport_reduce(local, &aggregate);
    if (rank == 0) {
        if (result != PEAK_MPI_REPORT_TRANSPORT_ROOT_READY ||
            aggregate == NULL || aggregate->rank_count != size ||
            aggregate->num_calls[0] != local->num_calls[0] ||
            aggregate->num_calls[1] != local->num_calls[1] ||
            aggregate->total_time[0] != local->total_time[0] ||
            aggregate->max_time[1] != local->max_time[1] ||
            aggregate->detached[0] != local->detached[0] ||
            aggregate->reattached[1] != local->reattached[1] ||
            !aggregate->overhead.per_rank_max ||
            aggregate->overhead.per_rank_maxima.owner_ranks[0] != 0) {
            failures++;
        }
    } else if (result != PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE ||
               aggregate != NULL) {
        failures++;
    }
    if (fake_operation_count != TEST_COLLECTIVE_COUNT ||
        peak_mpi_report_transport_failed_closed() ||
        peak_mpi_report_transport_quarantined_request_count() != 0) {
        failures++;
    }
    failures += validate_golden_trace();
    peak_report_snapshot_destroy(aggregate);
    peak_report_snapshot_destroy(local);
    return failures;
}

static int
run_failure_case(int ordinal, TestRequestMode mode)
{
    PeakReportSnapshot* local;
    PeakReportSnapshot* second;
    PeakReportSnapshot* aggregate = NULL;
    PeakMpiReportTransportResult result;
    int operations_after_failure;
    int failures = 0;

    fake_reset(ordinal, mode, 0, 1);
    (void)setenv("PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS", "1", 1);
    local = fixture_snapshot();
    if (local == NULL) {
        return 1;
    }
    result = peak_mpi_report_transport_reduce(local, &aggregate);
    if (result != PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED ||
        aggregate != NULL || !peak_mpi_report_transport_failed_closed() ||
        peak_mpi_report_transport_quarantined_request_count() != 1 ||
        fake_operation_count != ordinal || trace_count != ordinal ||
        trace_failures != 0 || fake_target_receive == NULL ||
        fake_target_request == NULL || !fake_target_request->active) {
        failures++;
    }

    peak_report_snapshot_destroy(local);
    if (fake_target_bytes != 0) {
        volatile unsigned char value =
            fake_target_send != NULL
                ? *(volatile unsigned char*)fake_target_send
                : *(volatile unsigned char*)fake_target_receive;
        memset(fake_target_receive, (int)(value ^ 0x5aU), fake_target_bytes);
    }

    peak_mpi_report_transport_reset_failed_closed();
    if (!peak_mpi_report_transport_failed_closed()) {
        failures++;
    }
    operations_after_failure = fake_operation_count;
    second = fixture_snapshot();
    if (second == NULL) {
        failures++;
    } else {
        result = peak_mpi_report_transport_reduce(second, &aggregate);
        if (result != PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED ||
            aggregate != NULL ||
            fake_operation_count != operations_after_failure) {
            failures++;
        }
    }
    peak_report_snapshot_destroy(second);
    return failures;
}

static int
run_failure_child(int ordinal, TestRequestMode mode)
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        _exit(run_failure_case(ordinal, mode) == 0 ? EXIT_SUCCESS
                                                   : EXIT_FAILURE);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != EXIT_SUCCESS) {
        return 1;
    }
    return 0;
}

int
main(void)
{
    int failures = 0;

    (void)setenv("PEAK_VERBOSITY", "silent", 1);
    failures += run_success_case(0, 1);
    failures += run_success_case(1, 2);
    for (int ordinal = 1; ordinal <= TEST_COLLECTIVE_COUNT; ordinal++) {
        failures += run_failure_child(ordinal, TEST_REQUEST_ERROR);
        failures += run_failure_child(ordinal, TEST_REQUEST_TIMEOUT);
    }
    if (failures != 0) {
        fprintf(stderr,
                "mpi_report_request_lifetime failures=%d\n",
                failures);
        return EXIT_FAILURE;
    }
    puts("mpi_report_request_lifetime_ok operations=54 labels=29 failures=108");
    return EXIT_SUCCESS;
}
