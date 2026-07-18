#ifndef PEAK_PROCESS_FILTER_H
#define PEAK_PROCESS_FILTER_H

/**
 * @brief Get the path to the current running binary.
 *
 * This function obtains the path to the current running binary by reading the
 * command line from the /proc/self/cmdline file. If an error occurs while
 * reading the file, the function returns the string "null".
 *
 * @param[out] argv0 A pointer to a char pointer where the path to the current running binary will be stored.
 */
void get_argv0(char** argv0);

/**
 * @brief Checks if the given command is a basic command.
 *
 * This function determines whether a given string represents a basic
 * command by comparing the base name of the input string against a predefined
 * list of command names. It supports both full paths (e.g., "/bin/awk") and
 * bare command names (e.g., "awk").
 *
 * @param str The command to check. This can be a full path or a bare command name.
 *            Example: "/bin/awk" or "awk".
 *
 * @return 1 if the command matches a basic command, 0 otherwise.
 *
 * @note The list of basic commands is hardcoded in the implementation.
 *       Custom or user-defined commands will return 0.
 */
int check_command(const char* str);

/**
 * @brief Checks whether a command is a language interpreter that should not
 *        initialize PEAK unless interpreter profiling is explicitly requested.
 *
 * @param str The command to check. This can be a full path or bare command.
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
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return 1 if the process should be treated as module helper infrastructure,
 *         0 otherwise.
 */
int check_module_helper_command(int argc, char* const argv[]);

/**
 * @brief Determine whether PEAK should profile a process with the supplied
 *        argv, applying the same command-filter rules used by LD_PRELOAD
 *        startup.
 *
 * @param argc Argument count from main or /proc/self/cmdline.
 * @param argv Argument vector.
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
 * @param enabled Nonzero to enable PEAK behavior for this process, zero to
 *        keep preload-side hooks inert.
 */
void peak_set_process_profile_enabled(int enabled);

/**
 * @brief Override whether the current process requested any PEAK work.
 *
 * This is separate from the command-profile decision: a process may be a valid
 * profiling target, but an LD_PRELOAD with no target, GPU monitor, or memory
 * profiling request should remain inert.
 *
 * @param enabled Nonzero when the process has at least one requested PEAK
 *        activity, zero to keep preload-side support hooks inert.
 */
void peak_set_process_requests_work(int enabled);

/**
 * @brief Return whether the current environment requests PEAK activity.
 *
 * This checks target, GPU monitor, and memory profiling controls, intentionally
 * ignoring generic tuning variables that have no effect without a requested
 * target.
 *
 * @return 1 if PEAK should perform runtime work, 0 if preload should stay
 *         inert.
 */
int peak_process_requests_work(void);

/**
 * @brief Return whether PEAK behavior should be active in this process.
 *
 * If __libc_start_main has not published a decision yet, this consults
 * /proc/self/cmdline so early constructors can still honor filtered wrapper
 * commands such as timeout, env, ibrun, and MPI launch helpers.
 *
 * @return 1 if PEAK should be active for this process, 0 if it should be inert.
 */
int peak_process_profile_enabled(void);

#endif /* PEAK_PROCESS_FILTER_H */
