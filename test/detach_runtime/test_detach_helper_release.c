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
static ssize_t send_for_test(int fd, const void* buffer, size_t size, int flags);

#define PEAK_DETACH_HELPER_PTRACE_STOP_WAIT_UNIT_TEST 1
#define ptrace ptrace_for_test
#define waitpid waitpid_for_test
#define clock_gettime clock_gettime_for_test
#define send send_for_test
#include "../../src/detach_helper.c"
#undef send
#undef clock_gettime
#undef waitpid
#undef ptrace

static ssize_t
send_for_test(int fd, const void* buffer, size_t size, int flags)
{
    if (flags != (MSG_DONTWAIT | MSG_NOSIGNAL)) {
        errno = EINVAL;
        return -1;
    }
    return write(fd, buffer, size);
}

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
test_transient_retry_deadline_captures_precleanup_state(void)
{
    int detail = ESRCH;
    int retry = -1;

    held_thread_count = 0;
    stop_diagnostic = (PeakStopDiagnostic){.enabled = 1, .active = 1,
        .stage = "getregs",
        .tid = getpid(), .proc_state = '?', .proc_tracer_pid = -1};
    fake_time_ms = 0;
    CHECK(cleanup_or_retry_stop_snapshot(PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR,
                                         &detail, 0, &retry) ==
          PEAK_DETACH_HELPER_STATUS_TIMEOUT);
    CHECK(detail == ETIMEDOUT);
    CHECK(retry == 0);
    CHECK(strcmp(stop_diagnostic.stage, "transient-retry") == 0);
    CHECK(stop_diagnostic.tid == getpid());
    CHECK(stop_diagnostic.proc_state != '?');
    CHECK(stop_diagnostic.proc_tracer_pid == 0);
    return 0;
}

static int
test_recovered_transient_retry_skips_proc_capture(void)
{
    int detail = ESRCH;
    int retry = -1;

    held_thread_count = 0;
    stop_diagnostic = (PeakStopDiagnostic){.enabled = 1, .active = 1,
        .stage = "getregs",
        .tid = getpid(), .proc_state = '?', .proc_tracer_pid = -1};
    fake_time_ms = 0;
    CHECK(cleanup_or_retry_stop_snapshot(PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR,
                                         &detail, 1000000, &retry) ==
          PEAK_DETACH_HELPER_STATUS_OK);
    CHECK(detail == 0);
    CHECK(retry == 1);
    CHECK(stop_diagnostic.proc_state == '?');
    CHECK(stop_diagnostic.failed_ms == 0);
    return 0;
}

static int
test_enrollment_reports_missing_tid(void)
{
    int detail = 0;

    held_thread_count = 0;
    stop_diagnostic = (PeakStopDiagnostic){.enabled = 1, .active = 1,
        .proc_state = '?', .proc_tracer_pid = -1};
    CHECK(verify_no_unstopped_threads(getpid(), 0, &detail) ==
          PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);
    CHECK(detail == EAGAIN);
    CHECK(strcmp(stop_diagnostic.stage, "enrollment") == 0);
    CHECK(stop_diagnostic.tid > 0);
    detail = ETIMEDOUT;
    CHECK(cleanup_held_threads_or_release_failed(PEAK_DETACH_HELPER_STATUS_TIMEOUT,
                                                  &detail) ==
          PEAK_DETACH_HELPER_STATUS_TIMEOUT);
    CHECK(detail == ETIMEDOUT);
    CHECK(stop_diagnostic.proc_state != '?');
    CHECK(stop_diagnostic.proc_tracer_pid == 0);
    return 0;
}

static int
test_stop_diagnostic_scope_and_tid_reset(void)
{
    stop_diagnostic = (PeakStopDiagnostic){.enabled = 1, .active = 1,
        .stage = "wait-stop", .tid = getpid(), .proc_state = '?',
        .proc_tracer_pid = -1, .wait_result = getpid()};
    stop_diagnostic_ptrace(PTRACE_INTERRUPT, -1, ESRCH);
    stop_diagnostic_stage("seize", getpid() + 1);
    CHECK(stop_diagnostic.ptrace_request == 0);
    CHECK(stop_diagnostic.wait_result == -1);
    stop_diagnostic.active = 0;
    stop_diagnostic_stage("enrollment", getpid());
    CHECK(strcmp(stop_diagnostic.stage, "seize") == 0);
    CHECK(stop_diagnostic.failed_ms == 0);
    return 0;
}

static int
test_stop_release_failure_exits_helper(void)
{
    int pair[2];
    int diagnostic_pair[2];
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
    char diagnostic[512] = {0};
    char expected[64];

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, diagnostic_pair) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int result;
        close(pair[0]);
        close(diagnostic_pair[0]);
        stop_diagnostic_fd = diagnostic_pair[1];
        protocol_failure_mode = 1;
        fake_ptrace_errno = ESRCH;
        fake_time_ms = 0;
        stop_diagnostic.enabled = 1;
        result = serve_protocol(pair[1]);
        close(pair[1]);
        _exit(result);
    }
    close(pair[1]);
    close(diagnostic_pair[1]);
    CHECK(write_exact(pair[0], &request, sizeof(request)) == 0);
    CHECK(read_exact(pair[0], &response, sizeof(response)) == 1);
    CHECK(response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
    CHECK(response.errno_value == ETIMEDOUT);
    CHECK(read(diagnostic_pair[0], diagnostic, sizeof(diagnostic) - 1u) > 0);
    CHECK(strstr(diagnostic, "stage=wait-stop") != NULL);
    snprintf(expected, sizeof(expected), "status=%u", (unsigned int)PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
    CHECK(strstr(diagnostic, expected) != NULL);
    snprintf(expected, sizeof(expected), "errno=%d", ETIMEDOUT);
    CHECK(strstr(diagnostic, expected) != NULL);
    snprintf(expected, sizeof(expected), "ptrace_request=%ld", (long)PTRACE_INTERRUPT);
    CHECK(strstr(diagnostic, expected) != NULL);
    CHECK(strstr(diagnostic, "waitpid_ret=0") != NULL);
    CHECK(strstr(diagnostic, "pre_cleanup_snapshot_ms=") != NULL);
    CHECK(strstr(diagnostic, "pre_cleanup_snapshot_ms=-1") == NULL);
    CHECK(strstr(diagnostic, "pre_cleanup_state=") != NULL);
    CHECK(strstr(diagnostic, "pre_cleanup_state=?") == NULL);
    CHECK(strstr(diagnostic, "pre_cleanup_tracer_pid=0") != NULL);
    CHECK(strstr(diagnostic, "proc_errno=0") != NULL);
    CHECK(read(pair[0], &trailing, 1) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    close(pair[0]);
    close(diagnostic_pair[0]);
    return 0;
}

int
main(void)
{
    if (test_esrch_running_tracee_is_release_failure() != 0 ||
        test_esrch_exited_tracee_is_released() != 0 ||
        test_normal_detach_releases_tracee() != 0 ||
        test_transient_retry_deadline_captures_precleanup_state() != 0 ||
        test_recovered_transient_retry_skips_proc_capture() != 0 ||
        test_enrollment_reports_missing_tid() != 0 ||
        test_stop_diagnostic_scope_and_tid_reset() != 0 ||
        test_stop_release_failure_exits_helper() != 0) {
        return 1;
    }
    puts("detach_helper_release_ok");
    return 0;
}
