#ifndef PEAK_TIMING_H
#define PEAK_TIMING_H

/**
 * @file timing.h
 * @brief Monotonic timing and in-place median helpers.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Gets the monotonic clock value in seconds.
 *
 * This converts CLOCK_MONOTONIC seconds and nanoseconds to a double. The clock
 * has an unspecified origin and is suitable for elapsed-time measurements, not
 * wall-clock timestamps.
 *
 * @return Monotonic time in seconds.
 * @pre The platform's CLOCK_MONOTONIC query succeeds; the current implementation
 *      does not report clock_gettime() failure.
 */
double peak_second();

/**
 * @brief Calculates the median after sorting an array in place.
 *
 * For an even number of elements, the result is the arithmetic mean of the two
 * middle values. The input order is destroyed by qsort().
 *
 * @param[in,out] arr Writable array that is sorted in ascending order.
 * @param[in] n Number of elements in @p arr.
 * @return The median value.
 * @pre @p arr points to at least @p n writable doubles.
 * @pre @p n is greater than zero.
 * @pre No element of @p arr is NaN.
 */
double median_double(double* arr, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_TIMING_H */
