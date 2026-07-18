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
 * then returns to the real `PMPI_Finalize()` after all-rank proof. Intel MPI
 * is a fail-closed exception: PEAK skips the real finalizer by default after
 * output because Intel MPI 2019 has crashed in hwloc teardown after large
 * Frontera PEAK reports; `PEAK_MPI_REAL_FINALIZE=1` opts back in.
 * `PEAK_MPI_FINALIZE_POLICY=defer`, or explicit socket output without an
 * explicit finalize policy, instead calls the real finalizer immediately and
 * leaves PEAK profiling/output until process exit.
 * `PEAK_MPI_REAL_FINALIZE=0` may skip the real finalizer for diagnostics. PEAK
 * does not replay the real `PMPI_Finalize()` later from process teardown; doing
 * so can re-enter MPI from an application state that has already logically
 * finalized.
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
 * The real MPI finalizer is enabled by default for most MPI runtimes, but only
 * after PEAK has proven that every rank reached the application finalizer.
 * Intel MPI defaults to skipping it unless `PEAK_MPI_REAL_FINALIZE=1` is set.
 * PEAK uses a short proof timeout and fails closed if proof cannot be
 * confirmed, so a subset-rank finalizer does not block on collectives or
 * re-enter MPI unexpectedly. A timed-out nonblocking collective proof is
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
