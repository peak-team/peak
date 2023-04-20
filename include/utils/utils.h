#ifndef __UTILS_H
#define __UTILS_H

#include <stddef.h>
#include <sys/time.h>

/**
 * @brief Get the current time in seconds with high precision.
 *
 * This function uses the `clock_gettime` system call to get the current time
 * with nanosecond precision. It returns the number of seconds as a `double`.
 *
 * @return The current time in seconds as a `double`.
 */
double peak_second();

#endif /* UTILS_H */
