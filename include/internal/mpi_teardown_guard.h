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
 * the timeout configured by `PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS` (10000 ms
 * by default) to tolerate large-rank finalizer arrival skew. An initiation
 * error, completion error, or timeout permanently
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

/**
 * @brief Coordinate completion of every rank's final-report responsibility.
 *
 * This bounded nonblocking reduction is a post-publication release gate. Each
 * rank calls it only after its local output responsibility has completed: the
 * aggregate writer has atomically published and flushed, an aggregate peer has
 * completed its transport role, or a rank-local writer has atomically
 * published its own CSV. The reduction result separately reports whether all
 * of those local responsibilities succeeded.
 *
 * The timeout is configured by @c PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS and
 * defaults to 180000 ms so it covers the default socket gather/release budget
 * plus rank-local fallback publication. An initiation error, completion error,
 * or timeout permanently disables subsequent teardown MPI calls. As with the
 * finalize proof, a request that may still be active and both of its buffers
 * are kept alive until process exit rather than cancelled or freed.
 *
 * @param local_complete Nonzero when this rank completed its output role.
 * @param local_real_finalize_allowed Nonzero when this rank's cached runtime
 *             and environment policy allows return to real PMPI_Finalize.
 * @param[out] all_complete Set to `true` only when the collective completed
 *             and every rank supplied a nonzero value. Set to `false` on a
 *             reported write failure or protocol failure.
 * @param[out] all_real_finalize_allowed Set to `true` only when every rank
 *             supplied a nonzero finalizer-policy value to this successfully
 *             completed reduction. Ranks that complete the same reduction
 *             observe the same minimum. A local error or timeout may not be
 *             observed by every peer and is only locally fail-closed.
 * @return `true` when the release collective completed, even if one rank
 *         reported an output failure; `false` on collective error or timeout.
 * @pre The finalize-participation proof completed successfully and no teardown
 *      collective has failed closed.
 */
bool peak_mpi_teardown_complete_report_release(
    int local_complete,
    int local_real_finalize_allowed,
    bool* all_complete,
    bool* all_real_finalize_allowed);

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
