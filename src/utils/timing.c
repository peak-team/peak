#include "timing.h"

#include <stdlib.h>
#include <time.h>

double peak_second()
{
    struct timespec measure;

    // Get the current time as the start time
    clock_gettime(CLOCK_MONOTONIC, &measure);

    // Return the elapsed time in seconds
    return (double)measure.tv_sec + (double)measure.tv_nsec * 1e-9;
}

/**
 * @brief Compare function used by qsort to compare two doubles.
 *
 * @param a Pointer to the first double to compare.
 * @param b Pointer to the second double to compare.
 *
 * @return -1 if a < b, 0 if a == b, or 1 if a > b.
 */
static int cmpfunc_double(const void* a, const void* b)
{
    if (*(double*)a < *(double*)b) {
        return -1;
    } else if (*(double*)a > *(double*)b) {
        return 1;
    } else {
        return 0;
    }
}

double median_double(double* arr, size_t n)
{
    qsort(arr, n, sizeof(double), cmpfunc_double);
    if (n % 2 == 0) {
        return (double)(arr[n / 2 - 1] + arr[n / 2]) / 2.0;
    } else {
        return (double)arr[n / 2];
    }
}
