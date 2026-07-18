#ifndef PEAK_PROCESS_FILTER_H
#define PEAK_PROCESS_FILTER_H

/**
 * @file process_filter.h
 * @brief Process-name filtering and cached PEAK activation decisions.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocates a copy of the current process's @c argv[0].
 *
 * The value is read from @c /proc/self/cmdline, so it is the process-supplied
 * argument rather than necessarily a canonical executable path. At most 1024
 * bytes are read and the final byte read is replaced with a null terminator;
 * long values can therefore be truncated to 1023 bytes. If opening fails or
 * fread() returns zero bytes, an allocated copy of @c "null" is returned;
 * a nonzero partial read is treated as available data.
 *
 * @param[out] argv0 Receives a newly allocated null-terminated string. The
 *                   caller owns the string and must release it with free().
 * @pre @p argv0 is not NULL.
 * @warning The current implementation assumes its initial allocation succeeds.
 */
void get_argv0(char** argv0);

/**
 * @brief Checks whether a command is in PEAK's hardcoded process filter.
 *
 * The basename is compared with a predefined list of PEAK helpers, launchers,
 * scheduler commands, and common tools. Full paths (for example @c /bin/awk)
 * and bare command names are both accepted.
 *
 * @param[in] str Command to check, as a full path or bare name. NULL and the
 *                empty string do not match.
 *
 * @return 1 if the basename matches a hardcoded filtered infrastructure or
 *         tool command, 0 otherwise.
 *
 * @note The filter list is hardcoded; unlisted commands return 0.
 */
int check_command(const char* str);

/**
 * @brief Checks whether a command is a language interpreter that should not
 *        initialize PEAK unless interpreter profiling is explicitly requested.
 *
 * A versioned interpreter prefix is accepted when it is followed by a decimal
 * digit; the remaining suffix is not validated as a version number.
 *
 * @param[in] str Command to check, as a full path or bare name. NULL does not
 *                match.
 * @return 1 if the command is a recognized Python/Lua/Perl/Tcl interpreter,
 *         0 otherwise.
 */
int check_interpreter_command(const char* str);

/**
 * @brief Checks whether argv describes an interpreter running module-system
 *        helper code, such as Lmod's Lua entrypoint.
 *
 * Interpreter applications are valid profiling targets, so this intentionally
 * uses argv context instead of filtering all python/lua/perl/tclsh processes by
 * basename.
 *
 * The first argument must name a recognized interpreter. Arguments are then
 * scanned, stopping at @p argc or the first NULL pointer, for the substrings
 * used by Lmod and environment-modules helpers.
 *
 * @param[in] argc Argument count from main.
 * @param[in] argv Argument vector from main. The strings are not modified.
 * @return 1 if the process should be treated as module helper infrastructure,
 *         0 otherwise.
 */
int check_module_helper_command(int argc, char* const argv[]);

/**
 * @brief Determine whether PEAK should profile a process with the supplied
 *        argv, applying the same command-filter rules used by LD_PRELOAD
 *        startup.
 *
 * A nonpositive @p argc, NULL @p argv, or NULL @c argv[0] is rejected. An empty
 * @c argv[0] is treated as an unfiltered command and is enabled. Recognized
 * interpreters are enabled only by a truthy @c PEAK_PROFILE_INTERPRETERS value.
 * Commands outside the hardcoded filter are enabled. Filtered commands remain
 * disabled except for the Node JIT runtime when @c PEAK_JIT_ENABLE is truthy.
 * Truthy values are @c 1, @c true, @c yes, and @c on, compared
 * case-insensitively.
 *
 * @param[in] argc Argument count from main or @c /proc/self/cmdline.
 * @param[in] argv Argument vector. The strings are not modified.
 * @return 1 if PEAK should initialize for this command, 0 if it should stay
 *         inert.
 */
int peak_should_profile_command(int argc, char* const argv[]);

/**
 * @brief Override the cached current-process profiling decision.
 *
 * This lets the __libc_start_main wrapper publish the exact argv-based
 * decision to constructors and interposed functions that run later.
 *
 * The value is stored atomically and normalized to 0 or 1. It replaces any
 * previously cached or automatically detected decision.
 *
 * @param[in] enabled Nonzero to enable PEAK behavior for this process, zero to
 *                    keep preload-side hooks inert.
 */
void peak_set_process_profile_enabled(int enabled);

/**
 * @brief Override whether the current process requested any PEAK work.
 *
 * This is separate from the command-profile decision: a process may be a valid
 * profiling target, but an LD_PRELOAD with no target, GPU monitor, or memory
 * profiling request should remain inert.
 *
 * The value is stored atomically and normalized to 0 or 1. It replaces any
 * previously cached or environment-derived decision.
 *
 * @param[in] enabled Nonzero when the process has at least one requested PEAK
 *                    activity, zero to keep preload-side support hooks inert.
 */
void peak_set_process_requests_work(int enabled);

/**
 * @brief Return whether the current environment requests PEAK activity.
 *
 * On first use, this checks nonempty target, target-file, target-group, and GPU
 * target controls, plus truthy GPU-monitor and memory-profile controls. The
 * result is cached atomically; later environment changes are not observed
 * unless peak_set_process_requests_work() publishes a new value. Generic
 * tuning variables are intentionally ignored.
 *
 * @return 1 if PEAK should perform runtime work, 0 if preload should stay
 *         inert.
 */
int peak_process_requests_work(void);

/**
 * @brief Return whether PEAK behavior should be active in this process.
 *
 * If @c __libc_start_main has not published a decision, this first consults
 * @c /proc/self/cmdline and then @c /proc/self/exe. If neither can be read, PEAK
 * defaults to active. The decision is cached atomically, so later environment
 * or process-command changes are not re-evaluated unless explicitly overridden.
 *
 * @return 1 if PEAK should be active for this process, 0 if it should be inert.
 */
int peak_process_profile_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_PROCESS_FILTER_H */
