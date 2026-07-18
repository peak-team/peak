#ifndef PEAK_TIMING_H
#define PEAK_TIMING_H

#include <stddef.h>

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
 * @brief Calculates the median value of an array of doubles.
 *
 * @param arr Pointer to the array of doubles.
 * @param n Number of elements in the array.
 *
 * @return The median value of the array.
 */
double median_double(double* arr, size_t n);

#endif /* PEAK_TIMING_H */
