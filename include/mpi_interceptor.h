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
 * then uses a bounded post-publication release gate before deciding whether to
 * return to the real `PMPI_Finalize()`. The real finalizer normally supplies
 * the MPI-runtime/launcher-aware clean-exit protocol; a PEAK-owned collective
 * cannot replace that protocol. Intel MPI 2019 is a compatibility exception:
 * PEAK skips its crash-prone hwloc finalizer by default after the release gate.
 * On the default report path, rank-local and socket CPU output is published
 * before MPI teardown coordination, and finalize participation joins their
 * long post-publication release gate. MPI aggregate output remains
 * proof-first. Every healthy path completes a post-publication release gate
 * before deciding whether to enter the real finalizer.
 * `PEAK_MPI_FINALIZE_POLICY=defer` instead attempts the real finalizer
 * immediately and leaves PEAK profiling/output until process exit. Unless
 * `PEAK_MPI_REAL_FINALIZE=0`, it therefore bypasses the Intel MPI 2019
 * compatibility skip on the normal report path.
 * `PEAK_MPI_REAL_FINALIZE=1` opts Intel MPI 2019 back into the real finalizer;
 * `0` disables it on both `report` and `defer` paths. Explicit `1` may not
 * override a failed all-rank safety gate on the `report` path, and skipping the
 * finalizer cannot guarantee clean launcher termination. PEAK does not replay
 * the real `PMPI_Finalize()` later from process teardown; doing so can re-enter
 * MPI from an application state that has already logically finalized.
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
 * The real MPI finalizer is enabled by default for healthy runtimes other than
 * Intel MPI 2019, but only after PEAK has proven that every rank reached the
 * application finalizer and completed its report responsibility. MPI
 * aggregation uses separate bounded participation and post-publication gates;
 * rank-local and socket output use one combined participation/publication
 * gate after publishing. PEAK fails closed if the required gate cannot be
 * confirmed, so a subset-rank finalizer does not block on collectives or
 * re-enter MPI unexpectedly. A timed-out nonblocking request is intentionally
 * abandoned rather than cancelled or freed because active nonblocking
 * collective cancellation is not portable; after that point PEAK must not use
 * MPI again during teardown.
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
