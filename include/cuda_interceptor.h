#ifndef PEAK_CUDA_INTERCEPTOR_H
#define PEAK_CUDA_INTERCEPTOR_H

/**
 * @file cuda_interceptor.h
 * @brief CUDA Runtime and Driver launch interception and reporting.
 *
 * PEAK replaces the CUDA Runtime and Driver kernel- and graph-launch entry
 * points that are present in the process.  The wrappers record launch
 * metadata and CUDA events while preserving the original CUDA return value.
 * All interceptor state, CUDA events, and result maps are owned by this
 * module; callers do not acquire ownership through this interface.
 */
#include "frida-gum.h"
#include "utils/utils.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <nv_decode.h>
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

#ifdef HAVE_MPI
#include <mpi.h>
#endif

#define max(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b;       \
})

#define min(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b;       \
})

#ifdef __cplusplus
extern "C" {
#endif

/** @name Interceptor lifecycle
 * @{ */

/**
 * @brief Installs the available CUDA launch replacements.
 *
 * The implementation initializes the module-owned result maps and per-target
 * CUDA events, then attempts each supported Runtime and Driver replacement in
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
 * not observe freed state.  After a successful flush it drains pending CUDA
 * events and releases the module-owned maps and event arrays.  The Gum
 * interceptor reference itself remains pinned to cover wrappers that may
 * already have entered before in-flight accounting began.
 *
 * The function is a no-op when no interceptor has been obtained.
 */
void cuda_interceptor_dettach();

/** @} */

/** @name Reporting
 * @{ */

/**
 * @brief Synchronizes pending CUDA work and reports collected launch data.
 *
 * Calling this function permanently stops admission of new events for the
 * current attachment, synchronizes the CUDA device, and drains kernel and
 * graph event records.  When PEAK is built with MPI and @p is_MPI is nonzero,
 * results are reduced through the MPI reporting path; otherwise they are
 * printed locally.  The function is a no-op if the result maps were not
 * initialized.
 *
 * @param[in] is_MPI Nonzero to request MPI reduction in an MPI-enabled build;
 *                   zero to emit the local report.
 */
void cuda_interceptor_print(int is_MPI);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_CUDA_INTERCEPTOR_H */
