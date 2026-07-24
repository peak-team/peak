#ifndef PEAK_MPI_TEARDOWN_GUARD_H
#define PEAK_MPI_TEARDOWN_GUARD_H

/**
 * @file mpi_teardown_guard.h
 * @brief Fail-closed coordination for MPI teardown collectives.
 *
 * The teardown path uses bounded nonblocking collectives to coordinate final
 * report publication and prove that every rank requested finalization. Active
 * requests that cannot be completed are retained for process lifetime so MPI
 * never observes expired request buffers.
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
 * defaults to 180000 ms. An initiation error, completion error, or timeout
 * permanently disables subsequent teardown MPI calls. As with the finalize
 * proof, a request that may still be active and both of its buffers are kept
 * alive until process exit rather than cancelled or freed.
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

/**
 * @brief Combine finalize participation with the post-publication release gate.
 *
 * Rank-local and socket output publish before any MPI teardown proof. Their
 * finalize-participation value therefore joins the report-completion and
 * real-finalizer-policy values in one @c MPI_Iallreduce with @c MPI_MIN. This
 * avoids imposing the short proof-first timeout on ranks that legitimately
 * arrive late while another rank is still completing PEAK-owned output.
 *
 * The operation uses the same @c PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS baseline
 * and fail-closed request ownership as
 * peak_mpi_teardown_complete_report_release(). The caller may supply a larger
 * @p minimum_timeout_ms for a publication path whose pre-collective work has a
 * longer end-to-end budget. Explicit socket output uses its peer release
 * budget plus two socket-phase margins; rank-local output passes zero and
 * retains the ordinary 180000 ms default.
 *
 * @param local_requested_finalize Nonzero when this rank requested MPI
 *             finalization.
 * @param local_complete Nonzero when this rank completed its output role.
 * @param local_real_finalize_allowed Nonzero when this rank's cached runtime
 *             and environment policy allows return to real PMPI_Finalize.
 * @param minimum_timeout_ms Publication-path-specific lower bound for the
 *             collective timeout, or zero to use only the configured/default
 *             MPI report-release timeout.
 * @param[out] all_requested_finalize Set to `true` only when every rank
 *             supplied a nonzero finalization-participation value.
 * @param[out] all_complete Set to `true` only when every rank supplied a
 *             nonzero report-completion value.
 * @param[out] all_real_finalize_allowed Set to `true` only when every rank
 *             supplied a nonzero finalizer-policy value.
 * @return `true` when the combined release collective completed, even when a
 *         reduced value is zero; `false` on collective error or timeout.
 * @pre Local report publication or socket transport responsibility has
 *      completed, no separate finalize-participation proof has run, and no
 *      teardown collective has failed closed.
 */
bool peak_mpi_teardown_complete_post_publication_release(
    int local_requested_finalize,
    int local_complete,
    int local_real_finalize_allowed,
    unsigned int minimum_timeout_ms,
    bool* all_requested_finalize,
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

/** Test-only strict launcher-metadata root selector used for info logging. */
bool peak_mpi_teardown_test_launcher_rank_is_root(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PEAK_MPI_TEARDOWN_GUARD_H */
