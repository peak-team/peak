#define _GNU_SOURCE
#include <dlfcn.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PEAK_TEST_MPI_INIT_DEFERRED_MODULE
#error PEAK_TEST_MPI_INIT_DEFERRED_MODULE is required
#endif

typedef int (*PeakRuntimeActiveFunction)(void);
typedef int (*PeakHookStateFunction)(size_t);
typedef unsigned long (*PeakCallCountFunction)(size_t);
typedef int (*PeakDeferredTargetFunction)(int);
typedef void (*PeakMpiInitCompletedFunction)(int);

static void
fail(const char* message)
{
    fprintf(stderr, "mpi_init_deferred_activation_error: %s\n", message);
    exit(2);
}

static int
call_requested_init(const char* mode, int* argc, char*** argv)
{
    int provided = MPI_THREAD_SINGLE;

    if (strcmp(mode, "mpi-init") == 0) {
        return MPI_Init(argc, argv);
    }
    if (strcmp(mode, "pmpi-init") == 0) {
        return PMPI_Init(argc, argv);
    }
    if (strcmp(mode, "mpi-init-thread") == 0) {
        return MPI_Init_thread(argc, argv, MPI_THREAD_FUNNELED, &provided);
    }
    if (strcmp(mode, "pmpi-init-thread") == 0) {
        return PMPI_Init_thread(argc, argv, MPI_THREAD_FUNNELED, &provided);
    }
    fail("unknown init mode");
    return MPI_ERR_OTHER;
}

static void
wait_for_attached(PeakHookStateFunction hook_state)
{
    for (int attempt = 0; attempt < 5000; attempt++) {
        if (hook_state(0) == 1) {
            return;
        }
        usleep(1000);
    }
    fail("target hook did not reach attached state");
}

int
main(int argc, char** argv)
{
    PeakRuntimeActiveFunction runtime_active;
    PeakHookStateFunction hook_state;
    PeakCallCountFunction call_count;
    PeakDeferredTargetFunction target;
    PeakMpiInitCompletedFunction init_completed;
    void* module;
    char mode[32];
    char policy[16];
    int expect_post_init;
    int synthetic_failure;
    int rank = -1;
    int sum = 0;

    if (argc != 3) {
        fail("init mode and activation expectation are required");
    }
    if (snprintf(mode, sizeof(mode), "%s", argv[1]) >= (int)sizeof(mode) ||
        snprintf(policy, sizeof(policy), "%s", argv[2]) >=
            (int)sizeof(policy)) {
        fail("activation test argument is too long");
    }
    expect_post_init = strcmp(policy, "post-init") == 0;
    synthetic_failure = strcmp(mode, "synthetic-failure") == 0;
    if (!expect_post_init && strcmp(policy, "immediate") != 0) {
        fail("unknown activation expectation");
    }

    runtime_active = (PeakRuntimeActiveFunction)dlsym(
        RTLD_DEFAULT, "peak_runtime_is_active_for_checkpoint");
    hook_state = (PeakHookStateFunction)dlsym(
        RTLD_DEFAULT, "peak_general_listener_hook_state");
    call_count = (PeakCallCountFunction)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_call_count");
    if (runtime_active == NULL || hook_state == NULL || call_count == NULL) {
        fail("PEAK lifecycle test API is unavailable");
    }
    if ((runtime_active() == 0) != expect_post_init) {
        fail("unexpected PEAK activation state before MPI_Init");
    }

    module = dlopen(PEAK_TEST_MPI_INIT_DEFERRED_MODULE,
                    RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) {
        fail(dlerror());
    }
    target = (PeakDeferredTargetFunction)dlsym(
        module, "peak_mpi_init_deferred_target");
    if (target == NULL) {
        fail("deferred target is unavailable");
    }
    if (expect_post_init) {
        if (runtime_active() != 0 || hook_state(0) != 0) {
            fail("loader activity activated PEAK before MPI_Init");
        }
    } else {
        wait_for_attached(hook_state);
    }

    for (int value = 0; value < 8; value++) {
        sum += target(value);
    }
    if (sum != 36) {
        fail("pre-init target returned an unexpected result");
    }

    if (synthetic_failure) {
        init_completed = (PeakMpiInitCompletedFunction)dlsym(
            RTLD_DEFAULT, "peak_mpi_init_completed");
        if (init_completed == NULL) {
            fail("MPI init completion callback is unavailable");
        }
        init_completed(MPI_ERR_OTHER);
        rank = 0;
    } else {
        if (call_requested_init(mode, &argc, &argv) != MPI_SUCCESS) {
            fail("MPI initializer failed");
        }
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    }
    if (runtime_active() == 0) {
        fail("PEAK did not activate after MPI_Init");
    }
    wait_for_attached(hook_state);

    sum = 0;
    for (int value = 0; value < 8; value++) {
        sum += target(value);
    }
    if (sum != 36) {
        fail("deferred target returned an unexpected result");
    }
    if (call_count(0) != (expect_post_init ? 8UL : 16UL)) {
        fail("profiled target call count does not match activation policy");
    }

    printf("mpi_init_activation_policy_ok mode=%s policy=%s rank=%d\n",
           mode,
           policy,
           rank);
    fflush(stdout);
    if (!synthetic_failure) {
        MPI_Finalize();
    }
    return 0;
}
