#ifndef __MPI_INTERCEPTOR_H
#define __MPI_INTERCEPTOR_H

/**
 * @file mpi_interceptor.h
 * @brief Header file for MPI function interception using Gum library 
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
 * then returns to the real `PMPI_Finalize()` after all-rank proof.
 * `PEAK_MPI_FINALIZE_POLICY=defer` instead calls the real finalizer immediately
 * and leaves PEAK profiling/output until process exit.
 * `PEAK_MPI_REAL_FINALIZE=0` may skip the real finalizer for diagnostics. PEAK
 * does not replay the real `PMPI_Finalize()` later from process teardown; doing
 * so can re-enter MPI from an application state that has already logically
 * finalized.
 *
 * @return 0 if the interception was successful, a negative number in the GumReplaceReturn otherwise.
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
 * that every rank reached the application finalizer. Otherwise a subset-rank
 * finalizer can hang or be killed by the MPI runtime.
 */
void mpi_interceptor_set_real_finalize_allowed(int allowed);

/**
 * @brief Detach MPI function interception
 *
 * This function detaches the previously attached MPI function interception and
 * releases any resources used by the Gum library. The argument is retained for
 * ABI compatibility and is ignored.
 *
 * @return void
 */
void mpi_interceptor_dettach(int allow_delayed_finalize);

#endif /* __MPI_INTERCEPTOR_H */
