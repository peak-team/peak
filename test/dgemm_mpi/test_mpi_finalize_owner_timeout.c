#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>

typedef void (*PeakVoidFunction)(void);
typedef int (*PeakIntFunction)(void);

static double
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1.0;
    }
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

int
main(void)
{
    PeakVoidFunction set_in_progress =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_set_finalize_in_progress");
    PeakIntFunction wait_for_owner =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_wait_for_finalize_owner");
    PeakIntFunction failed_closed =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_collectives_failed_closed");
    double started_ms;
    double elapsed_ms;
    int result;

    if (set_in_progress == NULL || wait_for_owner == NULL ||
        failed_closed == NULL) {
        fputs("mpi_finalize_owner_timeout_error missing hooks\n", stderr);
        return 2;
    }
    set_in_progress();
    started_ms = monotonic_milliseconds();
    result = wait_for_owner();
    elapsed_ms = monotonic_milliseconds() - started_ms;
    if (result == 0 || !failed_closed() || elapsed_ms < 0.0 ||
        elapsed_ms > 1000.0) {
        fputs("mpi_finalize_owner_timeout_error wait was not bounded\n",
              stderr);
        return 3;
    }
    puts("mpi_finalize_owner_timeout_ok");
    return 0;
}
