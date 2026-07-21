#ifndef PEAK_MPI_INTERCEPTOR_H
#define PEAK_MPI_INTERCEPTOR_H

/**
 * @file mpi_interceptor.h
 * @brief Intercept PMPI_Finalize while PEAK emits its final report.
 */

#include "frida-gum.h"

#include <mpi.h>

/**
 * @brief Attach MPI function interception
 *
 * This function attaches interception to the MPI library functions using the Gum library.
 * Specifically, it intercepts the `PMPI_Finalize` function and replaces it with
 * a custom implementation, `peak_pmpi_finalize`, which records the
 * application's finalization request. By default it lets PEAK write final
 * output while MPI is still alive on the application's own finalize path, and
 * then returns to the real `PMPI_Finalize()` after all-rank proof. The real
 * finalizer supplies the MPI-runtime/launcher-aware clean-exit protocol; a
 * PEAK-owned collective cannot replace that protocol.
 * Output aggregation selects only the report transport; socket output uses
 * the same pre-finalize report ordering by default.
 * `PEAK_MPI_FINALIZE_POLICY=defer` instead calls the real finalizer immediately
 * and leaves PEAK profiling/output until process exit.
 * `PEAK_MPI_REAL_FINALIZE=0` may skip the real finalizer for diagnostics, but
 * clean launcher termination is then not guaranteed. PEAK does not replay the
 * real `PMPI_Finalize()` later from process teardown; doing so can re-enter MPI
 * from an application state that has already logically finalized.
 *
 * @return GUM_REPLACE_OK on success; otherwise a Gum replacement failure code.
 */
int mpi_interceptor_attach();

/**
 * @brief Returns non-zero after the application has attempted PMPI_Finalize.
 */
int mpi_interceptor_finalize_was_requested();

/**
 * @brief Returns non-zero only while PEAK is running from PMPI_Finalize.
 */
int mpi_interceptor_finalize_path_active();

/**
 * @brief Controls whether the intercepted finalizer may call real PMPI_Finalize.
 *
 * The real MPI finalizer is enabled by default, but only after PEAK has proven
 * that every rank reached the application finalizer. PEAK uses a short proof
 * timeout and fails closed if proof cannot be confirmed, so a subset-rank
 * finalizer does not block on collectives or re-enter MPI unexpectedly. A
 * timed-out nonblocking collective proof is
 * intentionally abandoned rather than cancelled or freed because active
 * nonblocking collective cancellation is not portable; after that point PEAK
 * must not use MPI again during teardown.
 */
void mpi_interceptor_set_real_finalize_allowed(int allowed);

/**
 * @brief Attempts to revert PMPI_Finalize interception before finalization.
 *
 * Once finalization has been requested, the replacement remains pinned until
 * process exit. Before that point the function reverts and flushes the hook;
 * a failed flush also leaves it pinned. The interceptor object is retained for
 * process lifetime. The argument is retained for ABI compatibility and is
 * ignored.
 *
 * @param allow_delayed_finalize Ignored ABI-compatibility argument.
 */
void mpi_interceptor_dettach(int allow_delayed_finalize);

#endif /* PEAK_MPI_INTERCEPTOR_H */
