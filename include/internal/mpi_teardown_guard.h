#ifndef PEAK_MPI_TEARDOWN_GUARD_H
#define PEAK_MPI_TEARDOWN_GUARD_H

/**
 * @file mpi_teardown_guard.h
 * @brief Fail-closed coordination for MPI teardown collectives.
 *
 * The teardown path uses a bounded nonblocking collective to prove that every
 * rank requested finalization. Active requests that cannot be completed are
 * retained for process lifetime so MPI never observes expired request buffers.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mark MPI teardown collectives unusable for the rest of the process.
 *
 * This transition is permanent. It is also used when a later MPI report
 * collective fails after the finalize-participation proof has completed.
 */
void peak_mpi_teardown_collectives_mark_failed_closed(void);

/**
 * @brief Check whether MPI teardown collectives have failed closed.
 *
 * @return `true` after any teardown collective error or timeout.
 */
bool peak_mpi_teardown_collectives_failed_closed(void);

/**
 * @brief Prove that every rank requested MPI finalization.
 *
 * The proof uses `MPI_Iallreduce` with `MPI_MIN` and waits for at most
 * the timeout configured by `PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS` (250 ms by
 * default). An initiation error, completion error, or timeout permanently
 * disables subsequent teardown collectives. If a request may still be active,
 * its request object and both reduction buffers are retained until process
 * exit; the operation is never cancelled or freed.
 *
 * @param local_requested Nonzero when the local rank requested finalization.
 * @return `true` only when the collective completed and every rank supplied a
 *         nonzero value; otherwise `false`.
 * @pre The caller serializes teardown; PEAK invokes this only from its
 *      single-entry finalization path.
 */
bool peak_mpi_teardown_all_ranks_requested_finalize(int local_requested);

#ifdef PEAK_ENABLE_TEST_HOOKS
/**
 * @brief Return the number of process-lifetime quarantined proof requests.
 */
size_t peak_mpi_teardown_quarantined_request_count(void);

/**
 * @brief Check that three observed addresses belong to one quarantined owner.
 *
 * This test-only query validates that the request and both MPI buffers remain
 * members of the same process-lifetime object after an abandoned collective.
 */
bool peak_mpi_teardown_test_quarantine_owns(const void* local_requested,
                                            const void* all_requested,
                                            const void* request);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PEAK_MPI_TEARDOWN_GUARD_H */
