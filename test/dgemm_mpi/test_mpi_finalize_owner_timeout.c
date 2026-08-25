#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef void (*PeakVoidFunction)(void);
typedef int (*PeakIntFunction)(void);
typedef unsigned int (*PeakUnsignedFunction)(void);
typedef void (*PeakPublishFunction)(int);

typedef struct {
    PeakIntFunction wait_for_owner;
} WaiterArgs;

static void*
wait_for_finalize_owner(void* opaque)
{
    WaiterArgs* args = (WaiterArgs*)opaque;

    (void)args->wait_for_owner();
    return NULL;
}

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
main(int argc, char** argv)
{
    int cancel_mode = argc == 2 && strcmp(argv[1], "cancel") == 0;
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
    PeakUnsignedFunction waiter_count =
        (PeakUnsignedFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_finalize_waiter_count");
    PeakPublishFunction publish_result =
        (PeakPublishFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_publish_finalize_result");
    PeakVoidFunction prepare_finalize_stub =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_prepare_original_finalize_stub");
    PeakIntFunction call_original_finalize_once =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_call_original_finalize_once");
    PeakUnsignedFunction original_finalize_call_count =
        (PeakUnsignedFunction)dlsym(
            RTLD_DEFAULT,
            "mpi_interceptor_test_original_finalize_call_count");
    double started_ms;
    double elapsed_ms;
    int result;

    if (set_in_progress == NULL || wait_for_owner == NULL ||
        failed_closed == NULL || waiter_count == NULL ||
        publish_result == NULL || prepare_finalize_stub == NULL ||
        call_original_finalize_once == NULL ||
        original_finalize_call_count == NULL) {
        fputs("mpi_finalize_owner_timeout_error missing hooks\n", stderr);
        return 2;
    }
    prepare_finalize_stub();
    set_in_progress();
    if (cancel_mode) {
        pthread_t waiter;
        void* canceled_result = NULL;
        WaiterArgs waiter_args = { .wait_for_owner = wait_for_owner };

        if (pthread_create(&waiter, NULL, wait_for_finalize_owner,
                           &waiter_args) != 0) {
            fputs("mpi_finalize_owner_timeout_error waiter create\n", stderr);
            return 3;
        }
        while (waiter_count() == 0) {
            sched_yield();
        }
        if (pthread_cancel(waiter) != 0 ||
            pthread_join(waiter, &canceled_result) != 0 ||
            canceled_result != PTHREAD_CANCELED) {
            fputs("mpi_finalize_owner_timeout_error waiter cancel\n", stderr);
            return 4;
        }
        publish_result(0);
        if (wait_for_owner() != 0 || failed_closed()) {
            fputs("mpi_finalize_owner_timeout_error publication blocked\n",
                  stderr);
            return 5;
        }
        puts("mpi_finalize_owner_cancellation_ok");
        return 0;
    }
    started_ms = monotonic_milliseconds();
    result = wait_for_owner();
    elapsed_ms = monotonic_milliseconds() - started_ms;
    if (result == 0 || !failed_closed() || elapsed_ms < 0.0 ||
        elapsed_ms > 1000.0) {
        fputs("mpi_finalize_owner_timeout_error wait was not bounded\n",
              stderr);
        return 3;
    }
    (void)call_original_finalize_once();
    if (original_finalize_call_count() != 0) {
        fputs("mpi_finalize_owner_timeout_error real MPI call after timeout\n",
              stderr);
        return 4;
    }
    puts("mpi_finalize_owner_timeout_ok");
    return 0;
}
