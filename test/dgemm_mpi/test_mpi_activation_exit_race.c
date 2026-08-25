#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef void (*PeakVoidFunction)(void);
typedef int (*PeakIntFunction)(void);
typedef void (*PeakMpiInitCompletedFunction)(int);

typedef struct {
    PeakMpiInitCompletedFunction complete;
} CompletionArgs;

__attribute__((visibility("default"), noinline))
int
peak_mpi_activation_exit_race_target(int value)
{
    return value + 1;
}

static void*
complete_mpi_init(void* opaque)
{
    CompletionArgs* args = (CompletionArgs*)opaque;

    args->complete(0);
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
    int timeout_mode = argc == 2 && strcmp(argv[1], "timeout") == 0;
    PeakVoidFunction pause_enable =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_pause_enable");
    PeakIntFunction activation_held =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_is_held");
    PeakVoidFunction activation_release =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_release");
    PeakVoidFunction after_claim_enable =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_pause_after_claim_enable");
    PeakIntFunction after_claim_held =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_after_claim_is_held");
    PeakVoidFunction after_claim_release =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_after_claim_release");
    PeakVoidFunction fini =
        (PeakVoidFunction)dlsym(RTLD_DEFAULT, "peak_test_fini");
    PeakIntFunction runtime_active =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT, "peak_runtime_is_active_for_checkpoint");
    CompletionArgs completion = {
        .complete = (PeakMpiInitCompletedFunction)dlsym(
            RTLD_DEFAULT, "peak_mpi_init_completed"),
    };
    pthread_t completion_thread;

    if (pause_enable == NULL || activation_held == NULL ||
        activation_release == NULL || after_claim_enable == NULL ||
        after_claim_held == NULL || after_claim_release == NULL || fini == NULL ||
        runtime_active == NULL || completion.complete == NULL) {
        fputs("mpi_activation_exit_race_error missing hooks\n", stderr);
        return 2;
    }

    if (timeout_mode) {
        after_claim_enable();
    } else {
        pause_enable();
    }
    if (pthread_create(
            &completion_thread, NULL, complete_mpi_init, &completion) != 0) {
        fputs("mpi_activation_exit_race_error pthread_create\n", stderr);
        return 3;
    }
    while (!(timeout_mode ? after_claim_held() : activation_held())) {
        sched_yield();
    }

    if (timeout_mode) {
        double started_ms = monotonic_milliseconds();

        fini();
        double elapsed_ms = monotonic_milliseconds() - started_ms;
        if (elapsed_ms < 0.0 || elapsed_ms > 1000.0 ||
            runtime_active() != 0) {
            fputs("mpi_activation_exit_race_error unbounded activation wait\n",
                  stderr);
            return 5;
        }
        after_claim_release();
        pthread_join(completion_thread, NULL);
        if (runtime_active() == 0) {
            fputs("mpi_activation_exit_race_error activation did not resume\n",
                  stderr);
            return 6;
        }
        fini();
        puts("mpi_activation_wait_timeout_ok");
        return 0;
    }

    /*
     * Teardown must atomically claim READY -> CANCELED.  Releasing the
     * completion afterward verifies that its READY -> IN_PROGRESS CAS loses.
     */
    fini();
    activation_release();
    pthread_join(completion_thread, NULL);

    if (runtime_active() != 0) {
        fputs("mpi_activation_exit_race_error activation won after fini\n",
              stderr);
        return 4;
    }

    puts("mpi_activation_exit_race_ok");
    return peak_mpi_activation_exit_race_target(0) != 1;
}
