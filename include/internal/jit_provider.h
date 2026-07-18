#ifndef PEAK_JIT_PROVIDER_H
#define PEAK_JIT_PROVIDER_H

/**
 * @file jit_provider.h
 * @brief Controller-side ingestion of JIT perf-map symbol records.
 *
 * The provider owns its duplicated map path, committed file offset, file
 * identity, pending records, and copied symbol names.  Dynamic hooks installed
 * from those records belong to the general-listener lifecycle, not to callers
 * of this interface.
 *
 * When `PEAK_JIT_TRACE_PATH` is nonempty, provider activity is appended there
 * with timestamps in `CLOCK_MONOTONIC` seconds as returned by `peak_second()`;
 * these timestamps are not wall-clock or Unix-epoch time.  The non-executable
 * retry timeout is configured by `PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS` in
 * milliseconds (default 1000), using the same monotonic clock.
 *
 * In `PEAK_ENABLE_TEST_HOOKS` builds, the comma-separated
 * `PEAK_JIT_TEST_ATTACH_SEQUENCE` values `not-matched`/`no-match`, `retry`,
 * `failed`, and `real` force successive attach outcomes.  Enabling resets the
 * sequence index.
 */

#include "frida-gum.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Provider lifecycle
 * @{ */

/**
 * @brief Resets provider state and enables the configured perf-map source.
 *
 * Enablement requires `PEAK_JIT_ENABLE` to equal, case-insensitively, `1`,
 * `true`, `yes`, or `on`, and the comma-separated `PEAK_JIT_PROVIDER` list to
 * contain `perfmap` or `perf-map`.  The map path is copied from nonempty
 * `PEAK_JIT_MAP_PATH`, or defaults to `/tmp/perf-<pid>.map`.  Successful
 * enablement resets the committed offset and pending queue and wakes the
 * general-listener controller.  Disabled or unsupported configuration leaves
 * the provider disabled after clearing prior provider state.
 */
void peak_jit_provider_enable(void);

/**
 * @brief Stops ingestion and releases module-owned provider state.
 *
 * This clears pending records and the copied map path.  It does not detach
 * dynamic hooks already installed through the general listener; those remain
 * subject to that listener's teardown lifecycle.
 */
void peak_jit_provider_disable(void);

/** @} */

/** @name Bounded controller drains
 * @{ */

/**
 * @brief Retries pending symbols and consumes new perf-map records.
 *
 * Work begins at the committed map-file offset.  Pending records and new lines
 * share the limit from `PEAK_JIT_DRAIN_RECORD_BUDGET` (default 1024 records;
 * zero also selects the default).  Incomplete lines remain uncommitted for a
 * later drain.  Non-executable matching symbols remain pending until their
 * millisecond timeout, while retryable attach results remain pending without
 * that timeout.
 *
 * @retval TRUE More work remains because of a retry, an incomplete record, or
 *              budget exhaustion.
 * @retval FALSE The provider is disabled or no known work remains.
 */
gboolean peak_jit_provider_drain_pending(void);

/**
 * @brief Drains while immediately expiring non-executable pending symbols.
 *
 * This mode drops matching symbols whose ranges are still non-executable
 * instead of waiting for `PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS`.  It may still
 * return true for retryable attach results, incomplete input, or budget
 * exhaustion.
 *
 * @retval TRUE Work remains for a reason not eliminated by forced timeout.
 * @retval FALSE The provider is disabled or no known work remains.
 */
gboolean peak_jit_provider_drain_pending_force_not_exec_timeout(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_JIT_PROVIDER_H */
