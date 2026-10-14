#ifndef PEAK_MPI_INTERCEPTOR_H
#define PEAK_MPI_INTERCEPTOR_H

/**
 * @file mpi_interceptor.h
 * @brief Intercept MPI initialization completion and PMPI_Finalize.
 *
 * MPI_Init/PMPI_Init and their thread-level variants are interposed with
 * ordinary dynamic symbols rather than Gum patches. By default PEAK is already
 * active when they run. With PEAK_MPI_ACTIVATION_POLICY=post-init, PEAK remains
 * configured but inactive while the real initializer loads transport
 * providers, then activates Gum hooks from the outermost return path.
 */

#include "frida-gum.h"

#include <mpi.h>

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_MPI_INTERCEPTOR_API __attribute__((visibility("default")))
#else
#define PEAK_MPI_INTERCEPTOR_API
#endif

/**
 * @brief Activates a configured PEAK MPI runtime after MPI initialization.
 *
 * This internal cross-translation-unit callback is invoked by the MPI
 * initialization interposers only after the outermost real initializer
 * returns.
 * A failed initializer makes the opt-in deferred path fall back to non-MPI
 * activation after the initializer has returned.
 *
 * @param result Return code from the real MPI or PMPI initializer.
 */
PEAK_MPI_INTERCEPTOR_API void peak_mpi_init_completed(int result);

/**
 * @brief Attach MPI function interception
 *
 * On the default report path, rank-local and socket CPU output is published
 * before MPI teardown coordination, and finalize participation joins their
 * long post-publication release gate. MPI aggregate output remains
 * proof-first. Every healthy path completes a post-publication release gate
 * before deciding whether to enter the real finalizer.
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

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_MPI_INTERCEPTOR_API void
mpi_interceptor_test_set_finalize_in_progress(void);
PEAK_MPI_INTERCEPTOR_API int
mpi_interceptor_test_wait_for_finalize_owner(void);
PEAK_MPI_INTERCEPTOR_API int
mpi_interceptor_test_collectives_failed_closed(void);
PEAK_MPI_INTERCEPTOR_API unsigned int
mpi_interceptor_test_finalize_waiter_count(void);
PEAK_MPI_INTERCEPTOR_API void
mpi_interceptor_test_publish_finalize_result(int result);
PEAK_MPI_INTERCEPTOR_API void
mpi_interceptor_test_prepare_original_finalize_stub(void);
PEAK_MPI_INTERCEPTOR_API int
mpi_interceptor_test_call_original_finalize_once(void);
PEAK_MPI_INTERCEPTOR_API unsigned int
mpi_interceptor_test_original_finalize_call_count(void);
#endif

/**
 * @brief Controls whether the intercepted finalizer may call real PMPI_Finalize.
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
