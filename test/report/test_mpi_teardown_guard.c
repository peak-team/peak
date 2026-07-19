#include "internal/mpi_teardown_guard.h"

#include <mpi.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    TEST_REQUEST_COMPLETE_ALL = 0,
    TEST_REQUEST_COMPLETE_NOT_ALL,
    TEST_REQUEST_INIT_ERROR,
    TEST_REQUEST_TEST_ERROR,
    TEST_REQUEST_TIMEOUT,
} TestRequestMode;

static TestRequestMode fake_mode;
static int fake_iallreduce_calls;
static int fake_test_calls;
static bool fake_request_completed;
static const int* fake_send_buffer;
static int* fake_receive_buffer;
static MPI_Request* fake_request;

static void
fake_reset(TestRequestMode mode)
{
    fake_mode = mode;
    fake_iallreduce_calls = 0;
    fake_test_calls = 0;
    fake_request_completed = false;
    fake_send_buffer = NULL;
    fake_receive_buffer = NULL;
    fake_request = NULL;
}

void
peak_mpi_teardown_guard_test_observe_request(const int* local_requested,
                                             int* all_requested,
                                             MPI_Request* request)
{
    fake_send_buffer = local_requested;
    fake_receive_buffer = all_requested;
    fake_request = request;
}

int
MPI_Iallreduce(const void* send_buffer,
               void* receive_buffer,
               int count,
               MPI_Datatype datatype,
               MPI_Op operation,
               MPI_Comm communicator,
               MPI_Request* request)
{
    fake_iallreduce_calls++;
    if (send_buffer != fake_send_buffer ||
        receive_buffer != fake_receive_buffer ||
        request != fake_request || count != 1 || datatype != MPI_INT ||
        operation != MPI_MIN || communicator != MPI_COMM_WORLD) {
        return MPI_ERR_OTHER;
    }
    request->ordinal = fake_iallreduce_calls;
    request->active = 1;
    if (fake_mode == TEST_REQUEST_INIT_ERROR) {
        return MPI_ERR_OTHER;
    }
    *(int*)receive_buffer =
        fake_mode == TEST_REQUEST_COMPLETE_NOT_ALL ? 0 : 1;
    return MPI_SUCCESS;
}

int
MPI_Test(MPI_Request* request, int* done, MPI_Status* status)
{
    (void)status;
    fake_test_calls++;
    if (request != fake_request || !request->active) {
        return MPI_ERR_OTHER;
    }
    if (fake_mode == TEST_REQUEST_TEST_ERROR) {
        return MPI_ERR_OTHER;
    }
    if (fake_mode == TEST_REQUEST_TIMEOUT) {
        *done = 0;
        return MPI_SUCCESS;
    }
    request->active = 0;
    fake_request_completed = true;
    *done = 1;
    return MPI_SUCCESS;
}

static int
run_success_case(TestRequestMode mode, bool expected)
{
    bool result;

    fake_reset(mode);
    result = peak_mpi_teardown_all_ranks_requested_finalize(1);
    if (result != expected ||
        peak_mpi_teardown_collectives_failed_closed() ||
        peak_mpi_teardown_quarantined_request_count() != 0 ||
        fake_iallreduce_calls != 1 || fake_test_calls != 1 ||
        fake_send_buffer == NULL || fake_receive_buffer == NULL ||
        fake_request == NULL || !fake_request_completed) {
        return 1;
    }
    return 0;
}

static int
run_failure_case(TestRequestMode mode)
{
    const int delayed_local_requested = 17;
    const int delayed_all_requested = 23;
    const int delayed_request_ordinal = 31;
    const int delayed_request_active = 37;
    int operations_after_failure;
    int tests_after_failure;

    fake_reset(mode);
    if (peak_mpi_teardown_all_ranks_requested_finalize(1) ||
        !peak_mpi_teardown_collectives_failed_closed() ||
        peak_mpi_teardown_quarantined_request_count() != 1 ||
        fake_iallreduce_calls != 1 || fake_send_buffer == NULL ||
        fake_receive_buffer == NULL || fake_request == NULL ||
        !fake_request->active ||
        !peak_mpi_teardown_test_quarantine_owns(fake_send_buffer,
                                                fake_receive_buffer,
                                                fake_request)) {
        return 1;
    }
    if (mode == TEST_REQUEST_INIT_ERROR && fake_test_calls != 0) {
        return 1;
    }
    if (mode == TEST_REQUEST_TEST_ERROR && fake_test_calls != 1) {
        return 1;
    }
    if (mode == TEST_REQUEST_TIMEOUT && fake_test_calls == 0) {
        return 1;
    }

    /* The fake MPI library may still access this state after the return. */
    *(int*)fake_send_buffer = delayed_local_requested;
    *fake_receive_buffer = delayed_all_requested;
    fake_request->ordinal = delayed_request_ordinal;
    fake_request->active = delayed_request_active;
    if (*fake_send_buffer != delayed_local_requested ||
        *fake_receive_buffer != delayed_all_requested ||
        fake_request->ordinal != delayed_request_ordinal ||
        fake_request->active != delayed_request_active) {
        return 1;
    }

    operations_after_failure = fake_iallreduce_calls;
    tests_after_failure = fake_test_calls;
    if (peak_mpi_teardown_all_ranks_requested_finalize(1) ||
        fake_iallreduce_calls != operations_after_failure ||
        fake_test_calls != tests_after_failure ||
        peak_mpi_teardown_quarantined_request_count() != 1 ||
        *fake_send_buffer != delayed_local_requested ||
        *fake_receive_buffer != delayed_all_requested ||
        fake_request->ordinal != delayed_request_ordinal ||
        fake_request->active != delayed_request_active) {
        return 1;
    }
    return 0;
}

static int
run_failure_child(TestRequestMode mode)
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        _exit(run_failure_case(mode) == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != EXIT_SUCCESS) {
        return 1;
    }
    return 0;
}

static int
run_prepoison_child(void)
{
    pid_t child = fork();
    int status;

    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        fake_reset(TEST_REQUEST_COMPLETE_ALL);
        peak_mpi_teardown_collectives_mark_failed_closed();
        _exit(!peak_mpi_teardown_all_ranks_requested_finalize(1) &&
                      peak_mpi_teardown_collectives_failed_closed() &&
                      fake_iallreduce_calls == 0
                  ? EXIT_SUCCESS
                  : EXIT_FAILURE);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != EXIT_SUCCESS) {
        return 1;
    }
    return 0;
}

int
main(void)
{
    int failures = 0;

    (void)setenv("PEAK_VERBOSITY", "silent", 1);
    (void)setenv("PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS", "1", 1);
    failures += run_success_case(TEST_REQUEST_COMPLETE_ALL, true);
    failures += run_success_case(TEST_REQUEST_COMPLETE_NOT_ALL, false);
    failures += run_failure_child(TEST_REQUEST_INIT_ERROR);
    failures += run_failure_child(TEST_REQUEST_TEST_ERROR);
    failures += run_failure_child(TEST_REQUEST_TIMEOUT);
    failures += run_prepoison_child();
    if (failures != 0) {
        fprintf(stderr, "mpi_teardown_guard failures=%d\n", failures);
        return EXIT_FAILURE;
    }
    puts("mpi_teardown_guard_test_ok success=2 failure_paths=4");
    return EXIT_SUCCESS;
}
