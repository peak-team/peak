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

#endif /* __UTILS_H */
