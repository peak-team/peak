#include "internal/mpi_teardown_guard.h"

#include "internal/general_listener/runtime_config.h"
#include "logging.h"
#include "utils/env_parser.h"
#include "utils/timing.h"

#include <mpi.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS \
    "PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS"
#define PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_DEFAULT 10000U
#define PEAK_MPI_TEARDOWN_MAX_VALUES 3

typedef struct PeakMpiTeardownRequest PeakMpiTeardownRequest;
struct PeakMpiTeardownRequest {
    MPI_Request request;
    int local_values[PEAK_MPI_TEARDOWN_MAX_VALUES];
    int all_values[PEAK_MPI_TEARDOWN_MAX_VALUES];
    int value_count;
    PeakMpiTeardownRequest* next;
};

static _Atomic int peak_mpi_teardown_failed_closed;
static _Atomic(PeakMpiTeardownRequest*)
    peak_mpi_teardown_quarantine = NULL;
static _Atomic size_t peak_mpi_teardown_quarantine_count;
static PeakEnvWarningState peak_mpi_finalize_timeout_warning_emitted;

#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
extern void peak_mpi_teardown_guard_test_observe_request(
    const int* local_requested,
    int* all_requested,
    MPI_Request* request,
    int value_count) __attribute__((weak));
extern void peak_mpi_teardown_guard_test_observe_timeout(
    unsigned int timeout_ms) __attribute__((weak));
#endif

void
peak_mpi_teardown_collectives_mark_failed_closed(void)
{
    atomic_store_explicit(&peak_mpi_teardown_failed_closed,
                          1,
                          memory_order_release);
}

bool
peak_mpi_teardown_collectives_failed_closed(void)
{
    return atomic_load_explicit(&peak_mpi_teardown_failed_closed,
                                memory_order_acquire) != 0;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
size_t
peak_mpi_teardown_quarantined_request_count(void)
{
    return atomic_load_explicit(&peak_mpi_teardown_quarantine_count,
                                memory_order_acquire);
}

bool
peak_mpi_teardown_test_quarantine_owns(const void* local_requested,
                                       const void* all_requested,
                                       const void* request)
{
    PeakMpiTeardownRequest* pending =
        atomic_load_explicit(&peak_mpi_teardown_quarantine,
                             memory_order_acquire);

    while (pending != NULL) {
        if (local_requested == pending->local_values &&
            all_requested == pending->all_values &&
            request == &pending->request) {
            return true;
        }
        pending = pending->next;
    }
    return false;
}
#endif

static PeakMpiTeardownRequest*
peak_mpi_teardown_request_create(const int* local_values,
                                 int value_count,
                                 const char* operation)
{
    PeakMpiTeardownRequest* pending = calloc(1, sizeof(*pending));

    if (pending == NULL || local_values == NULL || value_count <= 0 ||
        value_count > PEAK_MPI_TEARDOWN_MAX_VALUES) {
        free(pending);
        peak_log_warn("[peak] Cannot allocate MPI %s state; disabling later MPI teardown calls\n",
                      operation);
        peak_mpi_teardown_collectives_mark_failed_closed();
        return NULL;
    }
    pending->request = MPI_REQUEST_NULL;
    pending->value_count = value_count;
    memcpy(pending->local_values,
           local_values,
           (size_t)value_count * sizeof(local_values[0]));
    return pending;
}

static void
peak_mpi_teardown_request_quarantine(PeakMpiTeardownRequest* pending)
{
    PeakMpiTeardownRequest* head;

    if (pending == NULL) {
        return;
    }
    head = atomic_load_explicit(&peak_mpi_teardown_quarantine,
                                memory_order_acquire);
    do {
        pending->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &peak_mpi_teardown_quarantine,
        &head,
        pending,
        memory_order_release,
        memory_order_acquire));
    atomic_fetch_add_explicit(&peak_mpi_teardown_quarantine_count,
                              1,
                              memory_order_release);
}

static void
peak_mpi_teardown_request_abandon(PeakMpiTeardownRequest* pending)
{
    peak_mpi_teardown_collectives_mark_failed_closed();
    peak_mpi_teardown_request_quarantine(pending);
}

static void
peak_mpi_teardown_observe_request(PeakMpiTeardownRequest* pending)
{
#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
    if (peak_mpi_teardown_guard_test_observe_request != NULL) {
        peak_mpi_teardown_guard_test_observe_request(
            pending->local_values,
            pending->all_values,
            &pending->request,
            pending->value_count);
    }
#else
    (void)pending;
#endif
}

static unsigned int
peak_mpi_teardown_finalize_timeout_ms(void)
{
    PeakEnvUnsignedSchema schema = {
        PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS, "milliseconds",
        PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_DEFAULT, 1, UINT_MAX, false,
        &peak_mpi_finalize_timeout_warning_emitted, false,
    };

    return (unsigned int)peak_parse_env_unsigned(&schema);
}

static bool
peak_mpi_teardown_launcher_rank_is_root(void)
{
    long rank = -1;

    return peak_general_listener_mpi_env_rank_size(&rank, NULL) &&
           rank == 0;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
bool
peak_mpi_teardown_test_launcher_rank_is_root(void)
{
    return peak_mpi_teardown_launcher_rank_is_root();
}
#endif

static unsigned int
peak_mpi_teardown_report_release_timeout_ms(
    unsigned int minimum_timeout_ms)
{
    PeakReportTimeoutBudget budget =
        peak_general_listener_report_timeout_budget();
    unsigned int timeout_ms = budget.mpi_report_release_timeout_ms;

    if (timeout_ms < minimum_timeout_ms) {
        timeout_ms = minimum_timeout_ms;
    }
    if (timeout_ms != budget.mpi_report_release_timeout_ms &&
        peak_mpi_teardown_launcher_rank_is_root()) {
        peak_log_info("[peak] Raised MPI report-release timeout from %u to %u ms to satisfy the caller's publication-path minimum\n",
                      budget.mpi_report_release_timeout_ms,
                      timeout_ms);
    }
    return timeout_ms;
}

static void
peak_mpi_teardown_failure_context(char* context,
                                  size_t context_size,
                                  const char* phase,
                                  unsigned int test_calls,
                                  double started,
                                  int mpi_result)
{
    long rank = -1;
    long size = -1;

    (void)peak_general_listener_mpi_env_rank_size(&rank, &size);
    (void)snprintf(context, context_size,
                   "rank=%ld size=%ld pid=%ld phase=%s mpi_test_calls=%u elapsed_ms=%.3f mpi_result=%d",
                   rank, size, (long)getpid(), phase, test_calls,
                   (peak_second() - started) * 1000.0, mpi_result);
}

static bool
peak_mpi_teardown_all_ranks_min(const int* local_values,
                                int value_count,
                                const char* operation,
                                unsigned int timeout_ms,
                                int* all_values)
{
    PeakMpiTeardownRequest* pending;
    MPI_Status status;
    int done = 0;
    int mpi_result;
    double deadline;
    double started;
    unsigned int test_calls = 0;
    char failure_context[192];
    const struct timespec poll_pause = { .tv_sec = 0, .tv_nsec = 1000000L };

    if (peak_mpi_teardown_collectives_failed_closed()) {
        return false;
    }

    if (all_values != NULL && value_count > 0 &&
        value_count <= PEAK_MPI_TEARDOWN_MAX_VALUES) {
        memset(all_values, 0, (size_t)value_count * sizeof(all_values[0]));
    }
    pending = peak_mpi_teardown_request_create(
        local_values, value_count, operation);
    if (pending == NULL) {
        return false;
    }
    peak_mpi_teardown_observe_request(pending);
    started = peak_second();
    mpi_result = MPI_Iallreduce(pending->local_values,
                                pending->all_values,
                                pending->value_count,
                                MPI_INT,
                                MPI_MIN,
                                MPI_COMM_WORLD,
                                &pending->request);
    if (mpi_result != MPI_SUCCESS) {
        peak_mpi_teardown_failure_context(failure_context, sizeof(failure_context),
                                          "iallreduce-init", test_calls,
                                          started, mpi_result);
        peak_log_warn("[peak] MPI_Iallreduce for %s failed; disabling later MPI teardown calls (%s)\n",
                      operation, failure_context);
        peak_mpi_teardown_request_abandon(pending);
        return false;
    }

#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
    if (peak_mpi_teardown_guard_test_observe_timeout != NULL) {
        peak_mpi_teardown_guard_test_observe_timeout(timeout_ms);
    }
#endif
    deadline = peak_second() + (double)timeout_ms / 1000.0;
    while (1) {
        test_calls++;
        mpi_result = MPI_Test(&pending->request, &done, &status);
        if (mpi_result != MPI_SUCCESS) {
            peak_mpi_teardown_failure_context(failure_context, sizeof(failure_context),
                                              "mpi-test", test_calls,
                                              started, mpi_result);
            peak_log_warn("[peak] MPI_Test for %s failed; disabling later MPI teardown calls (%s)\n",
                          operation, failure_context);
            peak_mpi_teardown_request_abandon(pending);
            return false;
        }
        if (done) {
            if (all_values != NULL) {
                memcpy(all_values,
                       pending->all_values,
                       (size_t)pending->value_count *
                           sizeof(pending->all_values[0]));
            }
            free(pending);
            return true;
        }
        if (peak_second() >= deadline) {
            peak_mpi_teardown_failure_context(failure_context, sizeof(failure_context),
                                              "poll-timeout", test_calls,
                                              started, mpi_result);
            peak_log_warn("[peak] MPI %s timed out after %u ms; disabling later MPI teardown calls (%s)\n",
                          operation, timeout_ms, failure_context);
            /*
             * Nonblocking collectives have no portable cancellation path.
             * Keep the request and both buffers alive, and prohibit every
             * subsequent PEAK teardown collective.
             */
            peak_mpi_teardown_request_abandon(pending);
            return false;
        }
        /* Avoid a process-wide busy spin while still driving MPI progress. */
        (void)nanosleep(&poll_pause, NULL);
    }
}

bool
peak_mpi_teardown_all_ranks_requested_finalize(int local_requested)
{
    const int local_values[] = { local_requested };
    int all_requested = 0;

    return peak_mpi_teardown_all_ranks_min(
               local_values,
               1,
               "finalize participation proof",
               peak_mpi_teardown_finalize_timeout_ms(),
               &all_requested) &&
           all_requested != 0;
}

bool
peak_mpi_teardown_complete_report_release(
    int local_complete,
    int local_real_finalize_allowed,
    bool* all_complete,
    bool* all_real_finalize_allowed)
{
    const int local_values[] = {
        local_complete,
        local_real_finalize_allowed,
    };
    int reduced_values[2] = { 0, 0 };
    bool protocol_completed;

    if (all_complete != NULL) {
        *all_complete = false;
    }
    if (all_real_finalize_allowed != NULL) {
        *all_real_finalize_allowed = false;
    }
    protocol_completed = peak_mpi_teardown_all_ranks_min(
        local_values,
        2,
        "report publication release",
        peak_mpi_teardown_report_release_timeout_ms(0),
        reduced_values);
    if (protocol_completed && all_complete != NULL) {
        *all_complete = reduced_values[0] != 0;
    }
    if (protocol_completed && all_real_finalize_allowed != NULL) {
        *all_real_finalize_allowed = reduced_values[1] != 0;
    }
    return protocol_completed;
}

bool
peak_mpi_teardown_complete_post_publication_release(
    int local_requested_finalize,
    int local_complete,
    int local_real_finalize_allowed,
    unsigned int minimum_timeout_ms,
    bool* all_requested_finalize,
    bool* all_complete,
    bool* all_real_finalize_allowed)
{
    const int local_values[] = {
        local_requested_finalize,
        local_complete,
        local_real_finalize_allowed,
    };
    int reduced_values[3] = { 0, 0, 0 };
    bool protocol_completed;

    if (all_requested_finalize != NULL) {
        *all_requested_finalize = false;
    }
    if (all_complete != NULL) {
        *all_complete = false;
    }
    if (all_real_finalize_allowed != NULL) {
        *all_real_finalize_allowed = false;
    }
    protocol_completed = peak_mpi_teardown_all_ranks_min(
        local_values,
        3,
        "combined finalize/report publication release",
        peak_mpi_teardown_report_release_timeout_ms(
            minimum_timeout_ms),
        reduced_values);
    if (protocol_completed && all_requested_finalize != NULL) {
        *all_requested_finalize = reduced_values[0] != 0;
    }
    if (protocol_completed && all_complete != NULL) {
        *all_complete = reduced_values[1] != 0;
    }
    if (protocol_completed && all_real_finalize_allowed != NULL) {
        *all_real_finalize_allowed = reduced_values[2] != 0;
    }
    return protocol_completed;
}
