#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

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

int
main(void)
{
    PeakVoidFunction pause_enable =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_pause_enable");
    PeakIntFunction activation_held =
        (PeakIntFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_is_held");
    PeakVoidFunction activation_release =
        (PeakVoidFunction)dlsym(
            RTLD_DEFAULT, "peak_test_activation_release");
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
        activation_release == NULL || fini == NULL ||
        runtime_active == NULL || completion.complete == NULL) {
        fputs("mpi_activation_exit_race_error missing hooks\n", stderr);
        return 2;
    }

    pause_enable();
    if (pthread_create(
            &completion_thread, NULL, complete_mpi_init, &completion) != 0) {
        fputs("mpi_activation_exit_race_error pthread_create\n", stderr);
        return 3;
    }
    while (!activation_held()) {
        sched_yield();
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
