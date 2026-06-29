#ifndef __EXEC_INTERCEPTOR_H
#define __EXEC_INTERCEPTOR_H

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
 * @brief Returns nonzero when PEAK runtime state can be checkpointed.
 */
PEAK_EXEC_API int peak_runtime_is_active_for_checkpoint(void);

/**
 * @brief Write a non-destructive rank-local checkpoint before an exec handoff.
 *
 * This must not call peak_fini(), detach Gum hooks, stop controller threads, or
 * use MPI collectives. The process may continue normally if the subsequent exec
 * call fails.
 *
 * @return 0 when a checkpoint artifact was written, -1 otherwise.
 */
PEAK_EXEC_API int peak_checkpoint_for_exec(const char* path, char* const argv[]);

/**
 * @brief Try-lock variant for fork-like children that may inherit a held mutex.
 *
 * This has the same non-destructive behavior as peak_checkpoint_for_exec(), but
 * fails instead of waiting for listener state. Normal exec wrappers should use
 * peak_checkpoint_for_exec() so transient listener activity does not lose the
 * pre-exec snapshot.
 */
PEAK_EXEC_API int peak_checkpoint_for_exec_trylock(const char* path,
                                                   char* const argv[]);

/**
 * @brief Internal hook for raw fork/clone syscall children.
 *
 * libc fork() runs pthread_atfork handlers, but user code can call raw fork or
 * clone syscalls through PEAK's syscall bridge. The bridge calls this helper in
 * the child so failed exec recovery does not inherit stale parent threads,
 * controller state, or locked checkpoint mutexes.
 */
void peak_runtime_after_fork_child(void);

/**
 * @brief Internal raw-syscall bridge shared by exec and checkpoint paths.
 *
 * This bypasses PEAK's syscall interposer when a platform-specific fallback is
 * required. It is intentionally not part of the documented user API.
 */
long peak_exec_call_raw_syscall6(long number,
                                 long arg1,
                                 long arg2,
                                 long arg3,
                                 long arg4,
                                 long arg5,
                                 long arg6);

#ifdef __cplusplus
}
#endif

#endif /* __EXEC_INTERCEPTOR_H */
