#ifndef PEAK_EXEC_MEMORY_H
#define PEAK_EXEC_MEMORY_H

/**
 * @file exec_memory.h
 * @brief Fault-contained readability preflight for exec arguments.
 *
 * These probes copy from caller-provided addresses through kernel-mediated
 * reads and mapping checks rather than directly dereferencing the input.  All
 * pointers are borrowed for the call and are never retained or modified.
 *
 * @section exec_memory_results Result and errno contract
 * `PEAK_EXEC_PREFLIGHT_INVALID` denotes a definite bounds or readability
 * failure and sets `errno` to an applicable value such as `EFAULT`,
 * `ENAMETOOLONG`, or `E2BIG`. `PEAK_EXEC_PREFLIGHT_UNKNOWN` means the available
 * probes could not establish safety and may leave an underlying probe error in
 * `errno`; callers must not treat it as proof that the memory is invalid.
 *
 * @section exec_memory_test_hooks Test-only environment controls
 * In `PEAK_ENABLE_TEST_HOOKS` builds, each switch is enabled by a nonempty
 * value other than the exact string `0`. `PEAK_TEST_EXEC_PREFLIGHT_TRAP` makes
 * peak_exec_args_readable() terminate the process with `_exit(97)`; it is not
 * checked by peak_exec_argv_envp_readable().
 * `PEAK_TEST_EXEC_PREFLIGHT_UNAVAILABLE` forces the `process_vm_readv` probe to
 * fail with `EPERM`; `PEAK_TEST_EXEC_PREFLIGHT_MAPS_UNAVAILABLE` forces the
 * maps probe to return unknown; and `PEAK_TEST_EXEC_PREFLIGHT_NO_PROC_MEM`
 * disables the `/proc/self/mem` fallback.  These switches do not transfer
 * ownership of argument storage.
 *
 * A valid result does not promise to preserve `errno`: an earlier failed probe
 * may leave its error value even when a fallback establishes readability.
 */

#include <stddef.h>

/** Maximum pointer entries inspected in each argv or envp array. */
#ifndef PEAK_EXEC_MAX_ENV_ENTRIES
#define PEAK_EXEC_MAX_ENV_ENTRIES 32768U
#endif

/** Maximum bytes inspected for each argv or envp string, including its terminator. */
#ifndef PEAK_EXEC_USER_STRING_MAX
#define PEAK_EXEC_USER_STRING_MAX (1024U * 1024U)
#endif

/** Outcome of a fault-contained exec-argument readability probe. */
typedef enum {
    PEAK_EXEC_PREFLIGHT_VALID = 0, /**< Every required byte was read safely. */
    PEAK_EXEC_PREFLIGHT_INVALID,   /**< The input is definitely invalid or exceeds a limit. */
    PEAK_EXEC_PREFLIGHT_UNKNOWN    /**< Safety could not be established by available probes. */
} PeakExecPreflightResult;

#ifdef __cplusplus
extern "C" {
#endif

/** @name Exec-argument preflight
 * @{ */

/**
 * @brief Checks a path plus argv and envp without direct input dereferences.
 *
 * @p path must contain a NUL terminator within the platform path limit.  Each
 * non-null vector must contain a null pointer within
 * `PEAK_EXEC_MAX_ENV_ENTRIES`, and each referenced string must terminate within
 * `PEAK_EXEC_USER_STRING_MAX` bytes.  Null @p argv and @p envp are valid empty
 * vectors; a null @p path is invalid.
 *
 * @param[in] path Borrowed path string to probe.
 * @param[in] argv Borrowed, optional argument vector and strings.
 * @param[in] envp Borrowed, optional environment vector and strings.
 * @return A `PeakExecPreflightResult` under the file-level errno contract.
 */
PeakExecPreflightResult peak_exec_args_readable(const char* path,
                                                char* const argv[],
                                                char* const envp[]);

/**
 * @brief Checks argv and envp without directly dereferencing either vector.
 *
 * Each non-null vector must contain a null pointer within
 * `PEAK_EXEC_MAX_ENV_ENTRIES`, and each referenced string must terminate within
 * `PEAK_EXEC_USER_STRING_MAX` bytes.  Either vector may itself be null.
 *
 * @param[in] argv Borrowed, optional argument vector and strings.
 * @param[in] envp Borrowed, optional environment vector and strings.
 * @return A `PeakExecPreflightResult` under the file-level errno contract.
 */
PeakExecPreflightResult peak_exec_argv_envp_readable(char* const argv[],
                                                     char* const envp[]);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_EXEC_MEMORY_H */
