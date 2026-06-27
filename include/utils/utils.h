#ifndef __UTILS_H
#define __UTILS_H

/**
 * @file utils.h
 * @brief Provides functions of utilities to be used in the project.
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Get the current time in seconds with high precision.
 *
 * This function uses the `clock_gettime` system call to get the current time
 * with nanosecond precision. It returns the number of seconds as a `double`.
 *
 * @return The current time in seconds as a `double`.
 */
double peak_second();

/**
 * @brief Check if the parent process ID matches the ID in the lock file.
 * 
 * This function checks if the parent process ID matches the ID in the lock file specified
 * by lock_file. It obtains an exclusive lock on the file to ensure synchronization
 * between multiple processes. If the lock file exists, it reads the file to extract the
 * parent process ID and compares it with the actual parent process ID. If it matches,
 * the function returns 1; otherwise, it returns 0. If the lock file does not exist, it
 * creates the file and writes the current process ID to the file. It sets need_to_clean
 * to 1 if the lock file was just created, which means the caller needs to clean up the
 * lock file when the program exits.
 * 
 * @param lock_file the path of the lock file to use
 * @param need_to_clean a pointer to an integer that indicates whether the lock file needs to be cleaned up
 * @return 1 if the parent process ID matches the ID in the lock file; 0 otherwise; -1 on error
 */
int check_parent_process(char* lock_file, int* need_to_clean);

/**
 * @brief Remove the lock file.
 * 
 * This function removes the lock file specified by lock_file.
 * 
 * @param lock_file the path of the lock file to remove
 * @return void
 */
void remove_ppid_file(char* lock_file);

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
 * @brief Calculates the median value of an array of doubles.
 *
 * @param arr Pointer to the array of doubles.
 * @param n Number of elements in the array.
 *
 * @return The median value of the array.
 */
double median_double(double* arr, size_t n);

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
int check_command(const char *str);

/**
 * @brief Checks whether a command is a language interpreter that should not
 *        initialize PEAK unless interpreter profiling is explicitly requested.
 *
 * @param str The command to check. This can be a full path or bare command.
 * @return 1 if the command is a recognized Python/Lua/Perl/Tcl interpreter,
 *         0 otherwise.
 */
int check_interpreter_command(const char *str);

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
int check_module_helper_command(int argc, char *const argv[]);

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
int peak_should_profile_command(int argc, char *const argv[]);

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

#endif /* __UTILS_H */
