#ifndef PEAK_EXEC_INTERCEPTOR_H
#define PEAK_EXEC_INTERCEPTOR_H

/**
 * @file exec_interceptor.h
 * @brief Exec-chain checkpoint and environment propagation support.
 */

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_EXEC_API __attribute__((visibility("default")))
#else
#define PEAK_EXEC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns nonzero when PEAK runtime state may be checkpointed.
 */
PEAK_EXEC_API int peak_runtime_is_active_for_checkpoint(void);

/**
 * @brief Write a best-effort, rank-local checkpoint before an exec handoff.
 *
 * This API is intentionally non-destructive: it must not call peak_fini(),
 * detach or reattach hooks, stop PEAK background workers, or use MPI.
 * It requires PEAK_EXEC_CHECKPOINT to be enabled at startup.
 *
 * @return 0 when a checkpoint artifact was written, -1 otherwise.
 */
PEAK_EXEC_API int peak_checkpoint_for_exec(const char* path,
                                           char* const argv[]);

#ifdef PEAK_ENABLE_TEST_HOOKS
/* Test-only synchronization for the checkpoint/fini lifetime-gate regression. */
PEAK_EXEC_API void peak_test_checkpoint_reader_pause_enable(void);
PEAK_EXEC_API int peak_test_checkpoint_reader_is_held(void);
PEAK_EXEC_API void peak_test_checkpoint_reader_release(void);
PEAK_EXEC_API int peak_test_fini_waiting_for_checkpoint_reader(void);
PEAK_EXEC_API void peak_test_fini(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PEAK_EXEC_INTERCEPTOR_H */
