#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fake_ptrace_errno;
static int fake_ptrace_calls;
static int protocol_failure_mode;
static long fake_time_ms;
static pid_t waitpid_for_test(pid_t pid, int* status, int options);
static int clock_gettime_for_test(clockid_t clock_id, struct timespec* ts);

#define PEAK_DETACH_HELPER_PTRACE_STOP_WAIT_UNIT_TEST 1
#define ptrace ptrace_for_test
#define waitpid waitpid_for_test
#define clock_gettime clock_gettime_for_test
#include "../../src/detach_helper.c"
#undef clock_gettime
#undef waitpid
#undef ptrace

long
ptrace_for_test(enum __ptrace_request request, ...)
{
    if (protocol_failure_mode &&
        (request == PTRACE_SEIZE || request == PTRACE_INTERRUPT)) {
        return 0;
    }
    if (request != PTRACE_DETACH) {
        errno = EINVAL;
        return -1;
    }
    fake_ptrace_calls++;
    if (fake_ptrace_errno != 0) {
        errno = fake_ptrace_errno;
        return -1;
    }
    return 0;
}

static pid_t
waitpid_for_test(pid_t pid, int* status, int options)
{
    (void)pid;
    (void)status;
    (void)options;
    return 0;
}

static int
clock_gettime_for_test(clockid_t clock_id, struct timespec* ts)
{
    (void)clock_id;
    fake_time_ms += 10000;
    ts->tv_sec = fake_time_ms / 1000;
    ts->tv_nsec = (fake_time_ms % 1000) * 1000000;
    return 0;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return 1; \
} } while (0)

static int
test_esrch_running_tracee_is_release_failure(void)
{
    int detail = 0;

    held_thread_count = 1;
    held_threads[0] = (PeakHeldThread){.tid = getpid(), .attached = 1};
    fake_ptrace_errno = ESRCH;
    fake_ptrace_calls = 0;
    CHECK(cleanup_held_threads_or_release_failed(
              PEAK_DETACH_HELPER_STATUS_TIMEOUT, &detail) ==
          PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
    CHECK(detail == ESRCH);
    CHECK(held_thread_count == 1);
    CHECK(fake_ptrace_calls == 1);
    return 0;
}

static int
test_esrch_exited_tracee_is_released(void)
{
    int detail = 0;

    held_thread_count = 1;
    held_threads[0] = (PeakHeldThread){.tid = 99999999, .attached = 1};
    fake_ptrace_errno = ESRCH;
    CHECK(cleanup_held_threads_or_release_failed(
              PEAK_DETACH_HELPER_STATUS_TIMEOUT, &detail) ==
          PEAK_DETACH_HELPER_STATUS_TIMEOUT);
    CHECK(held_thread_count == 0);
    return 0;
}

static int
test_normal_detach_releases_tracee(void)
{
    int detail = 0;

    held_thread_count = 1;
    held_threads[0] = (PeakHeldThread){.tid = getpid(), .attached = 1};
    fake_ptrace_errno = 0;
    CHECK(cleanup_held_threads_or_release_failed(
              PEAK_DETACH_HELPER_STATUS_OK, &detail) ==
          PEAK_DETACH_HELPER_STATUS_OK);
    CHECK(held_thread_count == 0);
    return 0;
}

static int
test_stop_release_failure_exits_helper(void)
{
    int pair[2];
    pid_t child;
    int status;
    PeakDetachHelperRequest request = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .command = PEAK_DETACH_HELPER_CMD_STOP,
        .pid = (int32_t)getpid(),
        .controller_tid = 0,
        .instruction_count = 0
    };
    PeakDetachHelperResponse response;
    char trailing;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int result;
        close(pair[0]);
        protocol_failure_mode = 1;
        fake_ptrace_errno = ESRCH;
        fake_time_ms = 0;
        result = serve_protocol(pair[1]);
        close(pair[1]);
        _exit(result);
    }
    close(pair[1]);
    CHECK(write_exact(pair[0], &request, sizeof(request)) == 0);
    CHECK(read_exact(pair[0], &response, sizeof(response)) == 1);
    CHECK(response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
    CHECK(response.errno_value == ETIMEDOUT);
    CHECK(read(pair[0], &trailing, 1) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    close(pair[0]);
    return 0;
}

int
main(void)
{
    if (test_esrch_running_tracee_is_release_failure() != 0 ||
        test_esrch_exited_tracee_is_released() != 0 ||
        test_normal_detach_releases_tracee() != 0 ||
        test_stop_release_failure_exits_helper() != 0) {
        return 1;
    }
    puts("detach_helper_release_ok");
    return 0;
}
