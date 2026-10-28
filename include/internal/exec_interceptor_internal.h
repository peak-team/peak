#ifndef PEAK_EXEC_INTERCEPTOR_INTERNAL_H
#define PEAK_EXEC_INTERCEPTOR_INTERNAL_H

/**
 * @file exec_interceptor_internal.h
 * @brief Internal read-only access to the exec checkpoint startup policy.
 */

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_EXEC_INTERNAL __attribute__((visibility("hidden")))
#else
#define PEAK_EXEC_INTERNAL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @name Startup checkpoint policy
 * @{ */

/**
 * @brief Returns the checkpoint policy captured during exec-interceptor priming.
 *
 * The value is initialized once from `PEAK_EXEC_CHECKPOINT`: an environment
 * value other than case-insensitive `0`, `false`, `no`, or `off` enables it.
 * After constructor priming the cached value is immutable; this accessor does
 * not reread the environment or mutate interceptor state.
 *
 * @retval 1 Exec checkpointing was enabled at startup.
 * @retval 0 Exec checkpointing was disabled at startup.
 */
PEAK_EXEC_INTERNAL int peak_exec_checkpoint_enabled_at_startup(void);

/** Consume the one-shot MPI utility marker before runtime activation.
 * ELF startup also recognizes the exact shell-command prefix injected by the
 * MPI-scoped system/popen wrappers. The result is latched for this process.
 */
PEAK_EXEC_INTERNAL int peak_exec_skip_mpi_runtime_child(int argc, char** argv);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_EXEC_INTERCEPTOR_INTERNAL_H */
