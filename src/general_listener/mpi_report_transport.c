#define _GNU_SOURCE

#include "internal/general_listener/mpi_report_transport.h"

#include "internal/general_listener/report_maxima.h"
#include "internal/general_listener/report_model.h"
#include "internal/general_listener/runtime_config.h"
#include "logging.h"
#include "utils/timing.h"

#ifdef PEAK_ENABLE_TEST_HOOKS
#include "general_listener.h"
#include "internal/general_listener/report_formatter.h"
#endif

#include <float.h>
#include <limits.h>
#include <mpi.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV \
    "PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS"
#define PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT 5000U
#define PEAK_TEST_MPI_REDUCER_FAIL_LABEL_ENV \
    "PEAK_TEST_MPI_REDUCER_FAIL_LABEL"
#define PEAK_TEST_MPI_REDUCER_ABANDON_LABEL_ENV \
    "PEAK_TEST_MPI_REDUCER_ABANDON_LABEL"

_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64,
               "PEAK MPI reporting requires an exact 64-bit uint64_t");
_Static_assert(SIZE_MAX <= ULONG_MAX,
               "PEAK MPI reporting requires size_t to fit unsigned long");

#if defined(PEAK_HAVE_MPI_UINT64_T)
#define PEAK_MPI_UINT64_DATATYPE MPI_UINT64_T
#elif UINT64_MAX == ULONG_MAX
#define PEAK_MPI_UINT64_DATATYPE MPI_UNSIGNED_LONG
#elif UINT64_MAX == ULLONG_MAX
#define PEAK_MPI_UINT64_DATATYPE MPI_UNSIGNED_LONG_LONG
#else
#error No exact MPI datatype fallback is available for uint64_t
#endif

typedef enum {
    PEAK_MPI_COLLECTIVE_ALLREDUCE = 0,
    PEAK_MPI_COLLECTIVE_REDUCE,
    PEAK_MPI_COLLECTIVE_BCAST,
} PeakMpiCollectiveKind;

typedef struct PeakMpiPendingCollective PeakMpiPendingCollective;
struct PeakMpiPendingCollective {
    MPI_Request request;
    void* send_buffer;
    void* receive_buffer;
    size_t buffer_size;
    PeakMpiPendingCollective* next;
};

typedef enum {
    PEAK_MPI_TRANSPORT_HEALTHY = 0,
    PEAK_MPI_TRANSPORT_SOFT_POISONED,
    PEAK_MPI_TRANSPORT_HARD_POISONED,
} PeakMpiTransportState;

static _Atomic int peak_mpi_report_transport_state =
    PEAK_MPI_TRANSPORT_HEALTHY;
static _Atomic(PeakMpiPendingCollective*)
    peak_mpi_report_transport_quarantine = NULL;
static _Atomic size_t peak_mpi_report_transport_quarantine_count = 0;

static void*
peak_mpi_report_transport_allocate(size_t count, size_t element_size);

#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
extern void peak_mpi_report_transport_test_observe_collective(
    const char* label,
    int kind,
    int count,
    MPI_Datatype datatype,
    MPI_Op op,
    int root,
    const void* original_send,
    void* original_receive,
    const void* staged_send,
    void* staged_receive,
    size_t buffer_size) __attribute__((weak));
#endif

static void
peak_mpi_report_transport_mark_failed_closed(void)
{
    int expected = PEAK_MPI_TRANSPORT_HEALTHY;

    (void)atomic_compare_exchange_strong_explicit(
        &peak_mpi_report_transport_state,
        &expected,
        PEAK_MPI_TRANSPORT_SOFT_POISONED,
        memory_order_release,
        memory_order_relaxed);
}

static void
peak_mpi_report_transport_mark_hard_failed_closed(void)
{
    atomic_store_explicit(&peak_mpi_report_transport_state,
                          PEAK_MPI_TRANSPORT_HARD_POISONED,
                          memory_order_release);
}

bool
peak_mpi_report_transport_failed_closed(void)
{
    return atomic_load_explicit(&peak_mpi_report_transport_state,
                                memory_order_acquire) !=
           PEAK_MPI_TRANSPORT_HEALTHY;
}

void
peak_mpi_report_transport_reset_failed_closed(void)
{
    int expected = PEAK_MPI_TRANSPORT_SOFT_POISONED;

    (void)atomic_compare_exchange_strong_explicit(
        &peak_mpi_report_transport_state,
        &expected,
        PEAK_MPI_TRANSPORT_HEALTHY,
        memory_order_acq_rel,
        memory_order_acquire);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
size_t
peak_mpi_report_transport_quarantined_request_count(void)
{
    return atomic_load_explicit(&peak_mpi_report_transport_quarantine_count,
                                memory_order_acquire);
}
#endif

static unsigned int
peak_mpi_output_collective_timeout_ms(void)
{
    unsigned int timeout_ms = peak_general_listener_parse_uint_env_default(
        PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
        PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT);

    return timeout_ms == 0 ? PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT
                           : timeout_ms;
}

static bool
peak_mpi_reducer_forced_failure(const char* label)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* value = getenv(PEAK_TEST_MPI_REDUCER_FAIL_LABEL_ENV);

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    return strcmp(value, "*") == 0 || strcmp(value, label) == 0;
#else
    (void)label;
    return false;
#endif
}

static bool
peak_mpi_reducer_forced_abandon(const char* label)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* value = getenv(PEAK_TEST_MPI_REDUCER_ABANDON_LABEL_ENV);

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    return strcmp(value, "*") == 0 || strcmp(value, label) == 0;
#else
    (void)label;
    return false;
#endif
}

static bool
peak_mpi_collective_buffer_size(int count,
                                MPI_Datatype datatype,
                                const char* label,
                                size_t* size_out)
{
    MPI_Aint lower_bound = 0;
    MPI_Aint extent = 0;
    size_t extent_size;

    if (count < 0 || size_out == NULL ||
        MPI_Type_get_extent(datatype, &lower_bound, &extent) != MPI_SUCCESS ||
        lower_bound != 0 || extent < 0) {
        peak_log_warn("[peak] MPI datatype extent for %s is unsupported; abandoning MPI reducer without touching MPI again\n",
                      label);
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    extent_size = (size_t)extent;
    if ((MPI_Aint)extent_size != extent ||
        (count > 0 && extent_size == 0) ||
        (count > 0 && extent_size > SIZE_MAX / (size_t)count)) {
        peak_log_warn("[peak] MPI buffer size for %s is invalid; abandoning MPI reducer without touching MPI again\n",
                      label);
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    *size_out = extent_size * (size_t)count;
    return true;
}

static PeakMpiPendingCollective*
peak_mpi_pending_collective_create(size_t buffer_size,
                                   const void* send_source,
                                   const void* receive_source)
{
    PeakMpiPendingCollective* pending =
        peak_mpi_report_transport_allocate(1, sizeof(*pending));

    pending->request = MPI_REQUEST_NULL;
    pending->buffer_size = buffer_size;
    if (send_source != NULL) {
        pending->send_buffer =
            peak_mpi_report_transport_allocate(buffer_size, 1);
        if (buffer_size != 0) {
            memcpy(pending->send_buffer, send_source, buffer_size);
        }
    }
    pending->receive_buffer =
        peak_mpi_report_transport_allocate(buffer_size, 1);
    if (receive_source != NULL && buffer_size != 0) {
        memcpy(pending->receive_buffer, receive_source, buffer_size);
    }
    return pending;
}

static void
peak_mpi_pending_collective_destroy(PeakMpiPendingCollective* pending)
{
    if (pending == NULL) {
        return;
    }
    free(pending->send_buffer);
    free(pending->receive_buffer);
    free(pending);
}

static void
peak_mpi_pending_collective_quarantine(PeakMpiPendingCollective* pending)
{
    PeakMpiPendingCollective* head;

    if (pending == NULL) {
        return;
    }
    head = atomic_load_explicit(&peak_mpi_report_transport_quarantine,
                                memory_order_acquire);
    do {
        pending->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &peak_mpi_report_transport_quarantine,
        &head,
        pending,
        memory_order_release,
        memory_order_acquire));
    atomic_fetch_add_explicit(&peak_mpi_report_transport_quarantine_count,
                              1,
                              memory_order_release);
}

static void
peak_mpi_pending_collective_abandon(PeakMpiPendingCollective* pending)
{
    peak_mpi_report_transport_mark_hard_failed_closed();
    peak_mpi_pending_collective_quarantine(pending);
}

static void
peak_mpi_observe_collective(const char* label,
                            PeakMpiCollectiveKind kind,
                            int count,
                            MPI_Datatype datatype,
                            MPI_Op op,
                            int root,
                            const void* original_send,
                            void* original_receive,
                            const PeakMpiPendingCollective* pending)
{
#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
    if (peak_mpi_report_transport_test_observe_collective != NULL) {
        peak_mpi_report_transport_test_observe_collective(
            label,
            (int)kind,
            count,
            datatype,
            op,
            root,
            original_send,
            original_receive,
            pending != NULL ? pending->send_buffer : NULL,
            pending != NULL ? pending->receive_buffer : NULL,
            pending != NULL ? pending->buffer_size : 0);
    }
#else
    (void)label;
    (void)kind;
    (void)count;
    (void)datatype;
    (void)op;
    (void)root;
    (void)original_send;
    (void)original_receive;
    (void)pending;
#endif
}

static bool
peak_mpi_wait_collective_request(PeakMpiPendingCollective* pending,
                                 const char* label)
{
    MPI_Status status;
    int done = 0;
    unsigned int timeout_ms = peak_mpi_output_collective_timeout_ms();
    double deadline = peak_second() + (double)timeout_ms / 1000.0;

    while (1) {
        int mpi_result = MPI_Test(&pending->request, &done, &status);

        if (mpi_result != MPI_SUCCESS) {
            peak_log_warn("[peak] MPI_Test for %s failed; abandoning MPI reducer without touching MPI again\n",
                          label);
            peak_mpi_pending_collective_abandon(pending);
            return false;
        }
        if (done) {
            return true;
        }
        if (peak_second() >= deadline) {
            peak_log_warn("[peak] MPI %s timed out after %u ms; abandoning MPI reducer without touching MPI again\n",
                          label,
                          timeout_ms);
            peak_mpi_pending_collective_abandon(pending);
            return false;
        }
        sched_yield();
    }
}

static bool
peak_mpi_allreduce_checked(const void* sendbuf,
                           void* recvbuf,
                           int count,
                           MPI_Datatype datatype,
                           MPI_Op op,
                           const char* label)
{
    PeakMpiPendingCollective* pending;
    size_t buffer_size;
    int mpi_result;

    if (peak_mpi_report_transport_failed_closed()) {
        return false;
    }
    if (peak_mpi_reducer_forced_failure(label)) {
        peak_log_warn("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                      label);
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }

    if ((count > 0 && (sendbuf == NULL || recvbuf == NULL)) ||
        !peak_mpi_collective_buffer_size(
            count, datatype, label, &buffer_size)) {
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    pending = peak_mpi_pending_collective_create(buffer_size, sendbuf, NULL);
    peak_mpi_observe_collective(label,
                                PEAK_MPI_COLLECTIVE_ALLREDUCE,
                                count,
                                datatype,
                                op,
                                -1,
                                sendbuf,
                                recvbuf,
                                pending);
    mpi_result = MPI_Iallreduce(pending->send_buffer,
                                pending->receive_buffer,
                                count,
                                datatype,
                                op,
                                MPI_COMM_WORLD,
                                &pending->request);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Iallreduce for %s failed; abandoning MPI reducer\n",
                      label);
        peak_mpi_pending_collective_abandon(pending);
        return false;
    }
    if (peak_mpi_reducer_forced_abandon(label)) {
        peak_log_warn("[peak] MPI reducer test hook abandoned active request for %s\n",
                      label);
        peak_mpi_pending_collective_abandon(pending);
        return false;
    }
    if (!peak_mpi_wait_collective_request(pending, label)) {
        return false;
    }
    if (buffer_size != 0) {
        memcpy(recvbuf, pending->receive_buffer, buffer_size);
    }
    peak_mpi_pending_collective_destroy(pending);
    return true;
}

static bool
peak_mpi_reduce_checked(const void* sendbuf,
                        void* recvbuf,
                        int count,
                        MPI_Datatype datatype,
                        MPI_Op op,
                        int root,
                        bool receive_result,
                        const char* label)
{
    PeakMpiPendingCollective* pending;
    size_t buffer_size;
    int mpi_result;

    if (peak_mpi_report_transport_failed_closed()) {
        return false;
    }
    if (peak_mpi_reducer_forced_failure(label)) {
        peak_log_warn("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                      label);
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }

    if ((count > 0 && (sendbuf == NULL || recvbuf == NULL)) ||
        !peak_mpi_collective_buffer_size(
            count, datatype, label, &buffer_size)) {
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    pending = peak_mpi_pending_collective_create(buffer_size, sendbuf, NULL);
    peak_mpi_observe_collective(label,
                                PEAK_MPI_COLLECTIVE_REDUCE,
                                count,
                                datatype,
                                op,
                                root,
                                sendbuf,
                                recvbuf,
                                pending);
    mpi_result = MPI_Ireduce(pending->send_buffer,
                             pending->receive_buffer,
                             count,
                             datatype,
                             op,
                             root,
                             MPI_COMM_WORLD,
                             &pending->request);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Ireduce for %s failed; abandoning MPI reducer\n",
                      label);
        peak_mpi_pending_collective_abandon(pending);
        return false;
    }
    if (peak_mpi_reducer_forced_abandon(label)) {
        peak_log_warn("[peak] MPI reducer test hook abandoned active request for %s\n",
                      label);
        peak_mpi_pending_collective_abandon(pending);
        return false;
    }
    if (!peak_mpi_wait_collective_request(pending, label)) {
        return false;
    }
    if (receive_result && buffer_size != 0) {
        memcpy(recvbuf, pending->receive_buffer, buffer_size);
    }
    peak_mpi_pending_collective_destroy(pending);
    return true;
}

static bool
peak_mpi_bcast_checked(void* buffer,
                       int count,
                       MPI_Datatype datatype,
                       int root,
                       const char* label)
{
    PeakMpiPendingCollective* pending;
    size_t buffer_size;
    int mpi_result;

    if (peak_mpi_report_transport_failed_closed()) {
        return false;
    }
    if (peak_mpi_reducer_forced_failure(label)) {
        peak_log_warn("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                      label);
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }

    if ((count > 0 && buffer == NULL) ||
        !peak_mpi_collective_buffer_size(
            count, datatype, label, &buffer_size)) {
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    pending = peak_mpi_pending_collective_create(buffer_size, NULL, buffer);
    peak_mpi_observe_collective(label,
                                PEAK_MPI_COLLECTIVE_BCAST,
                                count,
                                datatype,
                                MPI_OP_NULL,
                                root,
                                buffer,
                                buffer,
                                pending);
    mpi_result = MPI_Ibcast(pending->receive_buffer,
                            count,
                            datatype,
                            root,
                            MPI_COMM_WORLD,
                            &pending->request);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Ibcast for %s failed; abandoning MPI reducer\n",
                      label);
        peak_mpi_pending_collective_abandon(pending);
        return false;
    }
    if (peak_mpi_reducer_forced_abandon(label)) {
        peak_log_warn("[peak] MPI reducer test hook abandoned active request for %s\n",
                      label);
        peak_mpi_pending_collective_abandon(pending);
        return false;
    }
    if (!peak_mpi_wait_collective_request(pending, label)) {
        return false;
    }
    if (buffer_size != 0) {
        memcpy(buffer, pending->receive_buffer, buffer_size);
    }
    peak_mpi_pending_collective_destroy(pending);
    return true;
}

static bool
peak_mpi_bcast_report_rank_tuple(PeakReportRankTuple* tuple, int owner_rank)
{
    int accounting_valid;
    unsigned int local_ranks;
    uint64_t count_fields[2];
    double second_and_ratio_fields[12];

    if (tuple == NULL) {
        return false;
    }
    accounting_valid = tuple->accounting_valid ? 1 : 0;
    local_ranks = tuple->local_ranks;
    count_fields[0] = tuple->stop_window_count;
    count_fields[1] = tuple->failed_stop_window_count;
    second_and_ratio_fields[0] = tuple->elapsed_seconds;
    second_and_ratio_fields[1] = tuple->profile_seconds;
    second_and_ratio_fields[2] = tuple->control_seconds;
    second_and_ratio_fields[3] = tuple->management_seconds;
    second_and_ratio_fields[4] = tuple->control_risk_seconds;
    second_and_ratio_fields[5] = tuple->profile_control_risk_seconds;
    second_and_ratio_fields[6] = tuple->profile_ratio;
    second_and_ratio_fields[7] = tuple->control_ratio;
    second_and_ratio_fields[8] = tuple->profile_control_risk_ratio;
    second_and_ratio_fields[9] = tuple->control_risk_ratio;
    second_and_ratio_fields[10] = tuple->management_ratio;
    second_and_ratio_fields[11] = tuple->ratio;

    if (!peak_mpi_bcast_checked(&accounting_valid,
                                1,
                                MPI_INT,
                                owner_rank,
                                "profile-control-ratio-tuple-valid") ||
        !peak_mpi_bcast_checked(
            &local_ranks,
            1,
            MPI_UNSIGNED,
            owner_rank,
            "profile-control-ratio-tuple-local-ranks") ||
        !peak_mpi_bcast_checked(count_fields,
                                2,
                                PEAK_MPI_UINT64_DATATYPE,
                                owner_rank,
                                "profile-control-ratio-tuple-counts") ||
        !peak_mpi_bcast_checked(second_and_ratio_fields,
                                12,
                                MPI_DOUBLE,
                                owner_rank,
                                "profile-control-ratio-tuple-doubles")) {
        return false;
    }

    tuple->accounting_valid = accounting_valid != 0;
    tuple->local_ranks = local_ranks;
    tuple->stop_window_count = count_fields[0];
    tuple->failed_stop_window_count = count_fields[1];
    tuple->elapsed_seconds = second_and_ratio_fields[0];
    tuple->profile_seconds = second_and_ratio_fields[1];
    tuple->control_seconds = second_and_ratio_fields[2];
    tuple->management_seconds = second_and_ratio_fields[3];
    tuple->control_risk_seconds = second_and_ratio_fields[4];
    tuple->profile_control_risk_seconds = second_and_ratio_fields[5];
    tuple->profile_ratio = second_and_ratio_fields[6];
    tuple->control_ratio = second_and_ratio_fields[7];
    tuple->profile_control_risk_ratio = second_and_ratio_fields[8];
    tuple->control_risk_ratio = second_and_ratio_fields[9];
    tuple->management_ratio = second_and_ratio_fields[10];
    tuple->ratio = second_and_ratio_fields[11];
    return true;
}

static bool
peak_mpi_reduce_report_rank_tuples(
    const PeakReportRankTuple* local_tuple,
    PeakReportRankTuple maximum_tuples[PEAK_REPORT_METRIC_COUNT],
    int owner_ranks[PEAK_REPORT_METRIC_COUNT],
    int rank)
{
    struct {
        double value;
        int rank;
    } local_locations[PEAK_REPORT_METRIC_COUNT],
        maximum_locations[PEAK_REPORT_METRIC_COUNT] = {{0}};
    double ratios[PEAK_REPORT_METRIC_COUNT];

    if (local_tuple == NULL || maximum_tuples == NULL ||
        owner_ranks == NULL) {
        return false;
    }
    ratios[PEAK_REPORT_METRIC_COMBINED] = local_tuple->ratio;
    ratios[PEAK_REPORT_METRIC_PROFILE] = local_tuple->profile_ratio;
    ratios[PEAK_REPORT_METRIC_CONTROL] = local_tuple->control_ratio;
    ratios[PEAK_REPORT_METRIC_MANAGEMENT] = local_tuple->management_ratio;
    ratios[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK] =
        local_tuple->profile_control_risk_ratio;
    ratios[PEAK_REPORT_METRIC_CONTROL_RISK] =
        local_tuple->control_risk_ratio;
    for (size_t i = 0; i < PEAK_REPORT_METRIC_COUNT; i++) {
        local_locations[i].value = ratios[i];
        local_locations[i].rank = rank;
    }
    if (!peak_mpi_reduce_checked(local_locations,
                                 maximum_locations,
                                 PEAK_REPORT_METRIC_COUNT,
                                 MPI_DOUBLE_INT,
                                 MPI_MAXLOC,
                                 0,
                                 rank == 0,
                                 "profile-control-ratio-maxloc")) {
        return false;
    }
    for (size_t i = 0; i < PEAK_REPORT_METRIC_COUNT; i++) {
        int owner_rank = rank == 0 ? maximum_locations[i].rank : 0;

        if (!peak_mpi_bcast_checked(&owner_rank,
                                    1,
                                    MPI_INT,
                                    0,
                                    "profile-control-ratio-owner")) {
            return false;
        }
        if (rank == owner_rank) {
            maximum_tuples[i] = *local_tuple;
        }
        if (!peak_mpi_bcast_report_rank_tuple(&maximum_tuples[i],
                                              owner_rank)) {
            return false;
        }
        owner_ranks[i] = owner_rank;
    }
    return true;
}

static bool
peak_mpi_report_positive_finite(double value)
{
    return value > 0.0 && value == value && value <= DBL_MAX;
}

static bool
peak_mpi_report_transport_initialize(int* rank, int* size)
{
    int initialized = 0;
    int mpi_result;

    if (rank == NULL || size == NULL ||
        peak_mpi_report_transport_failed_closed()) {
        return false;
    }
    mpi_result = MPI_Initialized(&initialized);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Initialized failed; abandoning MPI reducer without touching MPI again\n");
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    if (!initialized) {
        mpi_result = MPI_Init(NULL, NULL);
        if (mpi_result != MPI_SUCCESS) {
            peak_log_warn("[peak] MPI_Init failed; abandoning MPI reducer without touching MPI again\n");
            peak_mpi_report_transport_mark_failed_closed();
            return false;
        }
    }
    mpi_result = MPI_Comm_rank(MPI_COMM_WORLD, rank);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Comm_rank failed; abandoning MPI reducer without touching MPI again\n");
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    mpi_result = MPI_Comm_size(MPI_COMM_WORLD, size);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Comm_size failed; abandoning MPI reducer without touching MPI again\n");
        peak_mpi_report_transport_mark_failed_closed();
        return false;
    }
    return true;
}

static void*
peak_mpi_report_transport_allocate(size_t count, size_t element_size)
{
    void* allocation = calloc(count == 0 ? 1 : count, element_size);

    if (allocation == NULL) {
        peak_log_warn("[peak] MPI report aggregation ran out of memory\n");
        abort();
    }
    return allocation;
}

static PeakMpiReportTransportResult
peak_mpi_report_transport_collective_failure(void)
{
    return peak_mpi_report_transport_failed_closed()
               ? PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED
               : PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
}

static void
peak_mpi_report_transport_set_overhead(
    PeakReportSnapshot* aggregate,
    bool all_accounting_valid,
    const PeakReportRankTuple maximum[PEAK_REPORT_METRIC_COUNT],
    const int owner_ranks[PEAK_REPORT_METRIC_COUNT],
    uint64_t failed_stop_window_count,
    double min_elapsed_seconds,
    double max_elapsed_seconds,
    double profile_seconds)
{
    const PeakReportRankTuple* combined =
        &maximum[PEAK_REPORT_METRIC_COMBINED];
    PeakReportOverhead* overhead = &aggregate->overhead;

    *overhead = (PeakReportOverhead){0};
    overhead->valid = true;
    overhead->accounting_valid = all_accounting_valid;
    overhead->per_rank_max = peak_report_maxima_load(
        &overhead->per_rank_maxima, maximum, owner_ranks);
    overhead->local_ranks = combined->local_ranks;
    overhead->stop_window_count = combined->stop_window_count;
    overhead->failed_stop_window_count = failed_stop_window_count;
    overhead->elapsed_seconds = combined->elapsed_seconds;
    overhead->elapsed_min_seconds = min_elapsed_seconds;
    overhead->elapsed_max_seconds = max_elapsed_seconds;
    overhead->profile_seconds = profile_seconds;
    overhead->control_seconds = combined->control_seconds;
    overhead->management_seconds = combined->management_seconds;
    overhead->control_risk_seconds = combined->control_risk_seconds;
    overhead->profile_control_risk_seconds =
        combined->profile_control_risk_seconds;
    overhead->ratio = combined->ratio;
    overhead->profile_ratio =
        maximum[PEAK_REPORT_METRIC_PROFILE].profile_ratio;
    overhead->control_ratio =
        maximum[PEAK_REPORT_METRIC_CONTROL].control_ratio;
    overhead->management_ratio =
        maximum[PEAK_REPORT_METRIC_MANAGEMENT].management_ratio;
    overhead->profile_control_risk_ratio =
        maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
            .profile_control_risk_ratio;
    overhead->control_risk_ratio =
        maximum[PEAK_REPORT_METRIC_CONTROL_RISK].control_risk_ratio;
}

PeakMpiReportTransportResult
peak_mpi_report_transport_reduce(const PeakReportSnapshot* local,
                                 PeakReportSnapshot** root_aggregate)
{
    int rank = 0;
    int size = 0;
    PeakReportRankTuple local_report_tuple;
    PeakReportRankTuple maximum_reports[PEAK_REPORT_METRIC_COUNT] = {{0}};
    int maximum_owner_ranks[PEAK_REPORT_METRIC_COUNT] = {0};
    double local_profile_seconds;
    double mpi_profile_seconds = 0.0;
    double local_elapsed_seconds;
    double mpi_min_elapsed_seconds = 0.0;
    double mpi_max_elapsed_seconds = 0.0;
    uint64_t local_failed_stop_window_count;
    uint64_t mpi_failed_stop_window_count = 0;
    uint64_t mpi_max_failed_stop_window_count = 0;
    int local_elapsed_valid;
    int all_elapsed_valid = 0;
    int local_accounting_valid;
    int all_accounting_valid = 0;
    unsigned long local_hook_count;
    unsigned long min_hook_count = 0;
    unsigned long max_hook_count = 0;
    int local_duplicate_names;
    int any_duplicate_names = 0;
    uint64_t* slot_hashes;
    uint64_t* min_slot_hashes;
    uint64_t* max_slot_hashes;
    bool slot_identity_mismatch = false;
    int hook_count;
    PeakReportSnapshot* aggregate;

    if (root_aggregate != NULL) {
        *root_aggregate = NULL;
    }
    if (local == NULL || root_aggregate == NULL) {
        return PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
    }
    if (peak_mpi_report_transport_failed_closed()) {
        return PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED;
    }
    if (!peak_mpi_report_transport_initialize(&rank, &size)) {
        return peak_mpi_report_transport_collective_failure();
    }

    local_report_tuple = peak_report_overhead_rank_tuple(&local->overhead);
    local_profile_seconds = local->overhead.profile_seconds;
    local_elapsed_seconds = local->overhead.elapsed_seconds;
    local_failed_stop_window_count =
        local->overhead.failed_stop_window_count;
    local_elapsed_valid =
        peak_mpi_report_positive_finite(local_elapsed_seconds) ? 1 : 0;
    local_accounting_valid = local->overhead.accounting_valid ? 1 : 0;
    local_hook_count = (unsigned long)local->hook_count;

    if (!peak_mpi_allreduce_checked(&local_elapsed_valid,
                                    &all_elapsed_valid,
                                    1,
                                    MPI_INT,
                                    MPI_MIN,
                                    "elapsed-valid")) {
        return peak_mpi_report_transport_collective_failure();
    }
    if (!all_elapsed_valid) {
        if (rank == 0) {
            peak_log_warn("[peak] MPI output observed an invalid per-rank elapsed time; writing rank-local output\n");
        }
        return PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
    }
    if (!peak_mpi_allreduce_checked(&local_accounting_valid,
                                    &all_accounting_valid,
                                    1,
                                    MPI_INT,
                                    MPI_MIN,
                                    "accounting-valid") ||
        !peak_mpi_allreduce_checked(&local_hook_count,
                                    &min_hook_count,
                                    1,
                                    MPI_UNSIGNED_LONG,
                                    MPI_MIN,
                                    "hook-count-min") ||
        !peak_mpi_allreduce_checked(&local_hook_count,
                                    &max_hook_count,
                                    1,
                                    MPI_UNSIGNED_LONG,
                                    MPI_MAX,
                                    "hook-count-max")) {
        return peak_mpi_report_transport_collective_failure();
    }

    local_duplicate_names =
        peak_report_snapshot_has_duplicate_names(local) ? 1 : 0;
    if (!peak_mpi_allreduce_checked(&local_duplicate_names,
                                    &any_duplicate_names,
                                    1,
                                    MPI_INT,
                                    MPI_MAX,
                                    "duplicate-hook-name-check")) {
        return peak_mpi_report_transport_collective_failure();
    }
    if (any_duplicate_names) {
        if (rank == 0) {
            peak_log_warn("[peak] MPI output contains duplicate hook names, likely from multiple JIT generations; writing rank-local PEAK output\n");
        }
        return PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
    }
    if (min_hook_count != max_hook_count) {
        if (rank == 0) {
            peak_log_warn("[peak] MPI ranks observed different JIT hook counts (min=%lu max=%lu); writing rank-local PEAK output\n",
                          min_hook_count,
                          max_hook_count);
        }
        return PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
    }
    if (max_hook_count > INT_MAX) {
        if (rank == 0) {
            peak_log_warn("[peak] MPI output hook count exceeds collective count limits; writing rank-local PEAK output\n");
        }
        return PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
    }
    hook_count = (int)max_hook_count;

    slot_hashes = peak_mpi_report_transport_allocate(
        local->hook_count, sizeof(*slot_hashes));
    min_slot_hashes = peak_mpi_report_transport_allocate(
        local->hook_count, sizeof(*min_slot_hashes));
    max_slot_hashes = peak_mpi_report_transport_allocate(
        local->hook_count, sizeof(*max_slot_hashes));
    for (size_t i = 0; i < local->hook_count; i++) {
        slot_hashes[i] = peak_report_snapshot_slot_identity_hash(local, i);
    }
    if (!peak_mpi_allreduce_checked(slot_hashes,
                                    min_slot_hashes,
                                    hook_count,
                                    PEAK_MPI_UINT64_DATATYPE,
                                    MPI_MIN,
                                    "hook-slot-min-hash") ||
        !peak_mpi_allreduce_checked(slot_hashes,
                                    max_slot_hashes,
                                    hook_count,
                                    PEAK_MPI_UINT64_DATATYPE,
                                    MPI_MAX,
                                    "hook-slot-max-hash")) {
        free(max_slot_hashes);
        free(min_slot_hashes);
        free(slot_hashes);
        return peak_mpi_report_transport_collective_failure();
    }
    for (size_t i = 0; i < local->hook_count; i++) {
        if (min_slot_hashes[i] != max_slot_hashes[i]) {
            slot_identity_mismatch = true;
            break;
        }
    }
    free(max_slot_hashes);
    free(min_slot_hashes);
    free(slot_hashes);
    if (slot_identity_mismatch) {
        if (rank == 0) {
            peak_log_warn("[peak] MPI ranks observed different JIT hook slot identities; writing rank-local PEAK output\n");
        }
        return PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK;
    }

    if (!peak_mpi_reduce_report_rank_tuples(&local_report_tuple,
                                            maximum_reports,
                                            maximum_owner_ranks,
                                            rank) ||
        !peak_mpi_reduce_checked(&local_profile_seconds,
                                 &mpi_profile_seconds,
                                 1,
                                 MPI_DOUBLE,
                                 MPI_SUM,
                                 0,
                                 rank == 0,
                                 "profile-seconds") ||
        !peak_mpi_allreduce_checked(&local_failed_stop_window_count,
                                    &mpi_max_failed_stop_window_count,
                                    1,
                                    PEAK_MPI_UINT64_DATATYPE,
                                    MPI_MAX,
                                    "failed-stop-window-max")) {
        return peak_mpi_report_transport_collective_failure();
    }
    if (mpi_max_failed_stop_window_count >
        (UINT64_MAX - 1) / (uint64_t)size) {
        mpi_failed_stop_window_count = UINT64_MAX - 1;
    } else if (!peak_mpi_reduce_checked(&local_failed_stop_window_count,
                                        &mpi_failed_stop_window_count,
                                        1,
                                        PEAK_MPI_UINT64_DATATYPE,
                                        MPI_SUM,
                                        0,
                                        rank == 0,
                                        "failed-stop-window-count")) {
        return peak_mpi_report_transport_collective_failure();
    }
    if (!peak_mpi_reduce_checked(&local_elapsed_seconds,
                                 &mpi_min_elapsed_seconds,
                                 1,
                                 MPI_DOUBLE,
                                 MPI_MIN,
                                 0,
                                 rank == 0,
                                 "elapsed-min") ||
        !peak_mpi_reduce_checked(&local_elapsed_seconds,
                                 &mpi_max_elapsed_seconds,
                                 1,
                                 MPI_DOUBLE,
                                 MPI_MAX,
                                 0,
                                 rank == 0,
                                 "elapsed-max")) {
        return peak_mpi_report_transport_collective_failure();
    }

    aggregate = peak_report_snapshot_clone(local);
    if (aggregate == NULL) {
        peak_log_warn("[peak] MPI report aggregation ran out of memory\n");
        abort();
    }
    if (!peak_mpi_reduce_checked(local->num_calls,
                                 aggregate->num_calls,
                                 hook_count,
                                 MPI_UNSIGNED_LONG,
                                 MPI_SUM,
                                 0,
                                 rank == 0,
                                 "sum-num-calls") ||
        !peak_mpi_reduce_checked(local->total_time,
                                 aggregate->total_time,
                                 hook_count,
                                 MPI_DOUBLE,
                                 MPI_SUM,
                                 0,
                                 rank == 0,
                                 "sum-total-time") ||
        !peak_mpi_reduce_checked(local->max_total_time,
                                 aggregate->max_total_time,
                                 hook_count,
                                 MPI_DOUBLE,
                                 MPI_MAX,
                                 0,
                                 rank == 0,
                                 "max-total-time") ||
        !peak_mpi_reduce_checked(local->min_total_time,
                                 aggregate->min_total_time,
                                 hook_count,
                                 MPI_DOUBLE,
                                 MPI_MIN,
                                 0,
                                 rank == 0,
                                 "min-total-time") ||
        !peak_mpi_reduce_checked(local->exclusive_time,
                                 aggregate->exclusive_time,
                                 hook_count,
                                 MPI_DOUBLE,
                                 MPI_SUM,
                                 0,
                                 rank == 0,
                                 "sum-exclusive-time") ||
        !peak_mpi_reduce_checked(local->max_time,
                                 aggregate->max_time,
                                 hook_count,
                                 MPI_FLOAT,
                                 MPI_MAX,
                                 0,
                                 rank == 0,
                                 "sum-max-time") ||
        !peak_mpi_reduce_checked(local->min_time,
                                 aggregate->min_time,
                                 hook_count,
                                 MPI_FLOAT,
                                 MPI_MIN,
                                 0,
                                 rank == 0,
                                 "sum-min-time") ||
        !peak_mpi_reduce_checked(local->thread_count,
                                 aggregate->thread_count,
                                 hook_count,
                                 MPI_UNSIGNED_LONG,
                                 MPI_SUM,
                                 0,
                                 rank == 0,
                                 "thread-count") ||
        !peak_mpi_reduce_checked(local->detached,
                                 aggregate->detached,
                                 hook_count,
                                 MPI_INT,
                                 MPI_MAX,
                                 0,
                                 rank == 0,
                                 "detached-marker") ||
        !peak_mpi_reduce_checked(local->reattached,
                                 aggregate->reattached,
                                 hook_count,
                                 MPI_INT,
                                 MPI_MAX,
                                 0,
                                 rank == 0,
                                 "reattached-marker") ||
        !peak_mpi_reduce_checked(local->revisited,
                                 aggregate->revisited,
                                 hook_count,
                                 MPI_INT,
                                 MPI_MAX,
                                 0,
                                 rank == 0,
                                 "revisited-marker")) {
        peak_report_snapshot_destroy(aggregate);
        return peak_mpi_report_transport_collective_failure();
    }

    if (rank != 0) {
        peak_report_snapshot_destroy(aggregate);
        return PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE;
    }
    aggregate->rank_count = size;
    peak_mpi_report_transport_set_overhead(
        aggregate,
        all_accounting_valid != 0,
        maximum_reports,
        maximum_owner_ranks,
        mpi_failed_stop_window_count,
        mpi_min_elapsed_seconds,
        mpi_max_elapsed_seconds,
        mpi_profile_seconds);
    *root_aggregate = aggregate;
    return PEAK_MPI_REPORT_TRANSPORT_ROOT_READY;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static PeakReportRankTuple
peak_report_rank_tuple_from_test(const PeakMpiReportTestTuple* source)
{
    PeakReportRankTuple tuple = {0};

    if (source == NULL) {
        return tuple;
    }
    tuple.accounting_valid = source->accounting_valid != 0;
    tuple.local_ranks = source->local_ranks;
    tuple.stop_window_count = source->stop_window_count;
    tuple.failed_stop_window_count = source->failed_stop_window_count;
    tuple.elapsed_seconds = source->elapsed_seconds;
    tuple.profile_seconds = source->profile_seconds;
    tuple.control_seconds = source->control_seconds;
    tuple.management_seconds = source->management_seconds;
    tuple.control_risk_seconds = source->control_risk_seconds;
    tuple.profile_control_risk_seconds =
        source->profile_control_risk_seconds;
    tuple.profile_ratio = source->profile_ratio;
    tuple.control_ratio = source->control_ratio;
    tuple.profile_control_risk_ratio =
        source->profile_control_risk_ratio;
    tuple.control_risk_ratio = source->control_risk_ratio;
    tuple.management_ratio = source->management_ratio;
    tuple.ratio = source->ratio;
    return tuple;
}

static void
peak_report_rank_tuple_to_test(const PeakReportRankTuple* source,
                               PeakMpiReportTestTuple* destination)
{
    if (source == NULL || destination == NULL) {
        return;
    }
    destination->accounting_valid = source->accounting_valid;
    destination->local_ranks = source->local_ranks;
    destination->stop_window_count = source->stop_window_count;
    destination->failed_stop_window_count = source->failed_stop_window_count;
    destination->elapsed_seconds = source->elapsed_seconds;
    destination->profile_seconds = source->profile_seconds;
    destination->control_seconds = source->control_seconds;
    destination->management_seconds = source->management_seconds;
    destination->control_risk_seconds = source->control_risk_seconds;
    destination->profile_control_risk_seconds =
        source->profile_control_risk_seconds;
    destination->profile_ratio = source->profile_ratio;
    destination->control_ratio = source->control_ratio;
    destination->profile_control_risk_ratio =
        source->profile_control_risk_ratio;
    destination->control_risk_ratio = source->control_risk_ratio;
    destination->management_ratio = source->management_ratio;
    destination->ratio = source->ratio;
}

gboolean
peak_general_listener_test_reduce_report_tuples(
    const PeakMpiReportTestTuple* local_tuple,
    PeakMpiReportTestTuple maximum_tuples[6],
    int owner_ranks[6])
{
    int rank;
    PeakReportRankTuple internal_local;
    PeakReportRankTuple internal_maximum[PEAK_REPORT_METRIC_COUNT] = {{0}};

    if (local_tuple == NULL || maximum_tuples == NULL ||
        owner_ranks == NULL ||
        peak_mpi_report_transport_failed_closed()) {
        return FALSE;
    }
    if (MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS) {
        peak_mpi_report_transport_mark_failed_closed();
        return FALSE;
    }
    internal_local = peak_report_rank_tuple_from_test(local_tuple);
    if (!peak_mpi_reduce_report_rank_tuples(&internal_local,
                                            internal_maximum,
                                            owner_ranks,
                                            rank)) {
        return FALSE;
    }
    for (size_t i = 0; i < PEAK_REPORT_METRIC_COUNT; i++) {
        peak_report_rank_tuple_to_test(&internal_maximum[i],
                                       &maximum_tuples[i]);
    }
    return TRUE;
}

void
peak_general_listener_test_print_report_tuples(
    const PeakMpiReportTestTuple maximum_tuples[6],
    const int owner_ranks[6])
{
    PeakReportRankTuple internal_maximum[PEAK_REPORT_METRIC_COUNT] = {{0}};

    if (maximum_tuples == NULL || owner_ranks == NULL) {
        return;
    }
    for (size_t i = 0; i < PEAK_REPORT_METRIC_COUNT; i++) {
        internal_maximum[i] =
            peak_report_rank_tuple_from_test(&maximum_tuples[i]);
    }
    peak_report_formatter_write_rank_maxima(internal_maximum, owner_ranks);
}

int
peak_general_listener_test_mpi_uint64_type_size(void)
{
    int type_size = 0;

    if (peak_mpi_report_transport_failed_closed() ||
        MPI_Type_size(PEAK_MPI_UINT64_DATATYPE, &type_size) != MPI_SUCCESS) {
        return -1;
    }
    return type_size;
}
#endif
