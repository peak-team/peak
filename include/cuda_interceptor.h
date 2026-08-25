#ifndef PEAK_CUDA_INTERCEPTOR_H
#define PEAK_CUDA_INTERCEPTOR_H

/**
 * @file cuda_interceptor.h
 * @brief CUDA Runtime and Driver launch interception and reporting.
 *
 * PEAK replaces the CUDA Runtime and Driver kernel- and graph-launch entry
 * points that are present in the process.  The wrappers record launch
 * metadata and CUDA events while preserving the original CUDA return value.
 * A dedicated helper harvests completed event pairs within a fixed work
 * budget; launch wrappers never synchronize a CUDA device or stream. All
 * interceptor state, CUDA events, and result maps are owned by this module;
 * callers do not acquire ownership through this interface.
 */
#include "frida-gum.h"
#include "utils/utils.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <pthread.h>
#include <string.h>

#ifdef __cplusplus
#include "utils/cxx_utils.h"
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @name Interceptor lifecycle
 * @{ */

/**
 * @brief Installs the available CUDA launch replacements.
 *
 * The implementation initializes the module-owned result maps and bounded
 * CUDA event pool, then attempts each supported Runtime and Driver replacement in
 * one Gum transaction.  Missing entry points are skipped.  If one replacement
 * fails, already successful replacements remain installed; the function does
 * not roll them back.
 *
 * @return `GUM_REPLACE_OK` when every replacement that was attempted
 *         succeeded; otherwise, the first non-OK `GumReplaceReturn`.  A return
 *         of `GUM_REPLACE_OK` does not imply that every optional CUDA entry
 *         point was present.
 */
int cuda_interceptor_attach();

/**
 * @brief Reverts installed CUDA replacements and releases collected state.
 *
 * New event admission is stopped before the Gum replacements are reverted.
 * If Gum cannot flush, the function logs the failure and deliberately retains
 * the interceptor, CUDA events, and result maps so live trampoline users do
 * not observe freed state.  A successful flush still retains the bounded CUDA
 * state when an in-flight wrapper is observed; a later detach retry drains
 * pending events until the configured finite finalization deadline and
 * releases the module-owned maps and event pool only once no admitted
 * lifecycle readers remain.
 * Incomplete context-owned CUDA state is retained after a deadline or context
 * failure instead of being destroyed from the wrong context. The Gum
 * interceptor reference itself remains pinned so a wrapper rejected by the
 * lifecycle gate can still delegate through Gum's original trampoline.
 *
 * The function is a no-op when no interceptor has been obtained.
 */
void cuda_interceptor_dettach();

/** @} */

/** @name Reporting
 * @{ */

/**
 * @brief Compatibility CUDA report entry point.
 *
 * Retains the historical boolean interface: nonzero selects MPI aggregation;
 * zero selects rank-local output. It never selects socket aggregation.
 *
 * @param[in] is_MPI Nonzero for MPI aggregation, zero for rank-local output.
 */
void cuda_interceptor_print(int is_MPI);

/**
 * @brief Boundedly harvests pending CUDA work and reports collected data.
 *
 * Calling this function permanently stops admission of new events for the
 * current attachment and gives PEAK-owned event records up to
 * `PEAK_CUDA_FINALIZATION_TIMEOUT_MS` (default 1000 ms) to complete. It never
 * synchronizes a CUDA device or stream. Records still incomplete at the
 * deadline are counted and retained safely. Kernel rows use a separate,
 * bounded report snapshot through the established aggregation transport;
 * rank-local graph rows are printed here. The function is a no-op if the
 * result maps were not initialized.
 *
 * @param[in] aggregation_mode A `PeakOutputAggregationMode` numeric value.
 * @param[in] active_mpi_job Nonzero when this process belongs to an active MPI
 *            job.  CUDA socket aggregation then obtains rank metadata only
 *            from launcher environment, so it never touches MPI while the
 *            CPU reducer is failed-closed or MPI teardown is in progress.
 */
void cuda_interceptor_print_with_mpi_job_policy(int aggregation_mode,
                                                int active_mpi_job);

#ifdef PEAK_ENABLE_TEST_HOOKS
/** Forces accepted CUDA events to remain pending in regression tests. */
void peak_cuda_test_force_incomplete_events(int enabled);
/** Forces the next harvested CUDA event query down the error path. */
void peak_cuda_test_force_query_error_once(int enabled);
/** Returns completed real event queries made by the CUDA helper. */
unsigned long long peak_cuda_test_harvester_query_count(void);
/** Returns nonzero after the deferred CUDA helper handshake succeeds. */
int peak_cuda_test_harvester_ready(void);
/** Returns the number of currently leased CUDA timing slots. */
unsigned long long peak_cuda_test_active_slot_count(void);
/** Pauses a capture-begin hook after CUDA sections are drained. */
void peak_cuda_test_pause_capture_begin(int enabled);
/** Returns nonzero while a capture-begin hook is paused. */
int peak_cuda_test_capture_begin_waiting(void);
/** Pauses CUDA sections immediately after capture-gate admission. */
void peak_cuda_test_pause_cuda_section(int enabled);
/** Returns the number of CUDA sections paused by the test seam. */
unsigned int peak_cuda_test_cuda_sections_waiting(void);
/** Returns nonzero while capture coordination blocks CUDA sections. */
int peak_cuda_test_capture_blocked(void);
/** Pauses lifecycle admission after the first open-gate observation. */
void peak_cuda_test_pause_lifecycle_before_increment(int enabled);
/** Returns the number of lifecycle readers paused before shard admission. */
unsigned int peak_cuda_test_lifecycle_before_increment_waiting(void);
/** Pauses lifecycle readers after successful shard admission. */
void peak_cuda_test_pause_lifecycle_after_admission(int enabled);
/** Returns the number of admitted lifecycle readers paused by the test seam. */
unsigned int peak_cuda_test_lifecycle_after_admission_waiting(void);
/** Returns nonzero after lifecycle admission is permanently closed. */
int peak_cuda_test_lifecycle_closing(void);
/** Bounded-finalization seam for CUDA regression tests. */
void peak_cuda_test_finalize(void);
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_CUDA_INTERCEPTOR_H */
