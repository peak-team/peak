#ifndef PEAK_EXEC_INTERCEPTOR_H
#define PEAK_EXEC_INTERCEPTOR_H

/**
 * @file exec_interceptor.h
 * @brief Public checkpoint operations used by exec-chain interception.
 *
 * The exec wrappers use these operations to take a non-destructive,
 * rank-local snapshot before handing control to a new executable.  Callers
 * retain ownership of every pointer passed to this interface.
 */

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_EXEC_API __attribute__((visibility("default")))
#else
#define PEAK_EXEC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @name Runtime checkpoint interface
 * @{ */

/**
 * @brief Tests whether the current runtime lifetime is safe for a checkpoint.
 *
 * The predicate is true only while the PEAK runtime is active, finalization
 * has not started, and the caller is the runtime-owning process rather than a
 * post-fork child.  It does not check whether checkpoint policy was enabled at
 * startup and does not mutate runtime state.
 *
 * @retval 1 Runtime lifetime conditions currently permit a checkpoint attempt;
 *           policy may still disable the operation.
 * @retval 0 The runtime is inactive, finalizing/finalized, or running in a
 *           fork child.
 */
PEAK_EXEC_API int peak_runtime_is_active_for_checkpoint(void);

/**
 * @brief Writes a best-effort, rank-local checkpoint before an exec handoff.
 *
 * This operation is intentionally non-destructive: it does not call
 * `peak_fini()`, detach or reattach hooks, stop PEAK background workers, or
 * use MPI.  `PEAK_EXEC_CHECKPOINT` must have been enabled when the exec
 * interceptor was primed.  A reader gate prevents the snapshot from racing
 * finalization. The operation does not detach hooks or mutate profiling data;
 * a report-writer failure can still consume a checkpoint sequence number.
 *
 * The current implementation reserves @p path and @p argv as call-site
 * context but does not inspect or retain them.  Their ownership always
 * remains with the caller.  The function preserves the caller's incoming
 * `errno` value on every return path.
 *
 * @param[in] path Path supplied to the exec operation; borrowed and currently
 *                 unused, and therefore permitted to be `NULL`.
 * @param[in] argv Argument vector supplied to the exec operation; borrowed
 *                 and currently unused, and therefore permitted to be `NULL`.
 * @retval 0 A checkpoint artifact was written.
 * @retval -1 Checkpointing was disabled or unsafe, the reader gate was closed,
 *            or the report writer did not produce an artifact.
 */
PEAK_EXEC_API int peak_checkpoint_for_exec(const char* path,
                                           char* const argv[]);

/** @} */

#ifdef PEAK_ENABLE_TEST_HOOKS
/** @name Checkpoint/finalization test hooks
 *
 * These hooks are available only in test-enabled builds.  They coordinate a
 * deterministic regression test of the checkpoint-reader/finalizer lifetime
 * gate and are not part of the production runtime contract.
 * @{ */

/** Arms a one-shot pause after the next checkpoint reader is acquired. */
PEAK_EXEC_API void peak_test_checkpoint_reader_pause_enable(void);

/** @return Nonzero once the test checkpoint reader is paused while holding the gate. */
PEAK_EXEC_API int peak_test_checkpoint_reader_is_held(void);

/** Releases a checkpoint reader paused by `peak_test_checkpoint_reader_pause_enable()`. */
PEAK_EXEC_API void peak_test_checkpoint_reader_release(void);

/** @return Nonzero once finalization is waiting for the paused reader. */
PEAK_EXEC_API int peak_test_fini_waiting_for_checkpoint_reader(void);

/** Invokes the normal `peak_fini()` path for lifetime-gate testing. */
PEAK_EXEC_API void peak_test_fini(void);

/** @} */
#endif

#ifdef __cplusplus
}
#endif

#endif /* PEAK_EXEC_INTERCEPTOR_H */
