#include "internal/mpi_teardown_guard.h"

#include "logging.h"
#include "utils/env_parser.h"
#include "utils/timing.h"

#include <mpi.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>

#define PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS \
    "PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS"
#define PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_DEFAULT 250U

typedef struct PeakMpiFinalizeProofRequest PeakMpiFinalizeProofRequest;
struct PeakMpiFinalizeProofRequest {
    MPI_Request request;
    int local_requested;
    int all_requested;
    PeakMpiFinalizeProofRequest* next;
};

static _Atomic int peak_mpi_teardown_failed_closed;
static _Atomic(PeakMpiFinalizeProofRequest*)
    peak_mpi_teardown_quarantine = NULL;
static _Atomic size_t peak_mpi_teardown_quarantine_count;

#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
extern void peak_mpi_teardown_guard_test_observe_request(
    const int* local_requested,
    int* all_requested,
    MPI_Request* request) __attribute__((weak));
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
    PeakMpiFinalizeProofRequest* pending =
        atomic_load_explicit(&peak_mpi_teardown_quarantine,
                             memory_order_acquire);

    while (pending != NULL) {
        if (local_requested == &pending->local_requested &&
            all_requested == &pending->all_requested &&
            request == &pending->request) {
            return true;
        }
        pending = pending->next;
    }
    return false;
}
#endif

static PeakMpiFinalizeProofRequest*
peak_mpi_finalize_proof_request_create(int local_requested)
{
    PeakMpiFinalizeProofRequest* pending = calloc(1, sizeof(*pending));

    if (pending == NULL) {
        peak_log_warn("[peak] Cannot allocate MPI finalize participation proof state; skipping MPI finalizer return path\n");
        peak_mpi_teardown_collectives_mark_failed_closed();
        return NULL;
    }
    pending->request = MPI_REQUEST_NULL;
    pending->local_requested = local_requested;
    return pending;
}

static void
peak_mpi_finalize_proof_request_quarantine(
    PeakMpiFinalizeProofRequest* pending)
{
    PeakMpiFinalizeProofRequest* head;

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
peak_mpi_finalize_proof_request_abandon(
    PeakMpiFinalizeProofRequest* pending)
{
    peak_mpi_teardown_collectives_mark_failed_closed();
    peak_mpi_finalize_proof_request_quarantine(pending);
}

static void
peak_mpi_finalize_proof_observe_request(
    PeakMpiFinalizeProofRequest* pending)
{
#if defined(PEAK_ENABLE_TEST_HOOKS) && \
    (defined(__GNUC__) || defined(__clang__))
    if (peak_mpi_teardown_guard_test_observe_request != NULL) {
        peak_mpi_teardown_guard_test_observe_request(
            &pending->local_requested,
            &pending->all_requested,
            &pending->request);
    }
#else
    (void)pending;
#endif
}

bool
peak_mpi_teardown_all_ranks_requested_finalize(int local_requested)
{
    PeakMpiFinalizeProofRequest* pending;
    MPI_Status status;
    int done = 0;
    int mpi_result;
    unsigned int timeout_ms;
    double deadline;

    if (peak_mpi_teardown_collectives_failed_closed()) {
        return false;
    }

    pending = peak_mpi_finalize_proof_request_create(local_requested);
    if (pending == NULL) {
        return false;
    }
    peak_mpi_finalize_proof_observe_request(pending);
    mpi_result = MPI_Iallreduce(&pending->local_requested,
                                &pending->all_requested,
                                1,
                                MPI_INT,
                                MPI_MIN,
                                MPI_COMM_WORLD,
                                &pending->request);
    if (mpi_result != MPI_SUCCESS) {
        peak_log_warn("[peak] MPI_Iallreduce for finalize participation proof failed; skipping MPI finalizer return path\n");
        peak_mpi_finalize_proof_request_abandon(pending);
        return false;
    }

    timeout_ms =
        parse_env_to_uint_default(PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS,
                                  PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_DEFAULT);
    if (timeout_ms == 0) {
        timeout_ms = PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_DEFAULT;
    }
    deadline = peak_second() + (double)timeout_ms / 1000.0;
    while (1) {
        mpi_result = MPI_Test(&pending->request, &done, &status);
        if (mpi_result != MPI_SUCCESS) {
            peak_log_warn("[peak] MPI_Test for finalize participation proof failed; skipping MPI finalizer return path\n");
            peak_mpi_finalize_proof_request_abandon(pending);
            return false;
        }
        if (done) {
            bool all_requested = pending->all_requested != 0;

            free(pending);
            return all_requested;
        }
        if (peak_second() >= deadline) {
            peak_log_warn("[peak] MPI finalize participation proof timed out after %u ms; assuming not all ranks reached finalizer\n",
                          timeout_ms);
            /*
             * Nonblocking collectives have no portable cancellation path.
             * Keep the request and both buffers alive, and prohibit every
             * subsequent PEAK teardown collective.
             */
            peak_mpi_finalize_proof_request_abandon(pending);
            return false;
        }
        sched_yield();
    }
}
