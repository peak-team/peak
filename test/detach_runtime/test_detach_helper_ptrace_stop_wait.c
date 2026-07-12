#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

static pid_t waitpid_for_test(pid_t pid, int* status, int options);
static int sigtimedwait_for_test(const sigset_t* set,
                                 siginfo_t* info,
                                 const struct timespec* timeout);
static int clock_gettime_for_test(clockid_t clock_id, struct timespec* ts);

#define PEAK_DETACH_HELPER_PTRACE_STOP_WAIT_UNIT_TEST 1
#define waitpid waitpid_for_test
#define sigtimedwait sigtimedwait_for_test
#define clock_gettime clock_gettime_for_test
#include "../../src/detach_helper.c"
#undef clock_gettime
#undef sigtimedwait
#undef waitpid

typedef struct {
    pid_t result;
    int status;
    int error_number;
} WaitStep;

typedef struct {
    int result;
    int error_number;
} SignalStep;

static WaitStep wait_steps[4];
static size_t wait_step_count;
static size_t wait_step_index;
static SignalStep signal_steps[4];
static size_t signal_step_count;
static size_t signal_step_index;
static long clock_values[4];
static size_t clock_value_count;
static size_t clock_value_index;
static int waitpid_calls;
static int sigtimedwait_calls;
static char call_sequence[8];
static size_t call_sequence_length;

static void
reset_fixture(void)
{
    wait_step_count = 0;
    wait_step_index = 0;
    signal_step_count = 0;
    signal_step_index = 0;
    clock_value_count = 0;
    clock_value_index = 0;
    waitpid_calls = 0;
    sigtimedwait_calls = 0;
    call_sequence_length = 0;
}

static pid_t
waitpid_for_test(pid_t pid, int* status, int options)
{
    WaitStep step;

    (void)pid;
    (void)options;
    call_sequence[call_sequence_length++] = 'W';
    waitpid_calls++;
    if (wait_step_index >= wait_step_count) {
        errno = ECHILD;
        return -1;
    }

    step = wait_steps[wait_step_index++];
    if (step.result < 0) {
        errno = step.error_number;
        return -1;
    }
    if (step.result > 0) {
        *status = step.status;
    }
    return step.result;
}

static int
sigtimedwait_for_test(const sigset_t* set,
                      siginfo_t* info,
                      const struct timespec* timeout)
{
    SignalStep step;

    (void)set;
    (void)info;
    (void)timeout;
    call_sequence[call_sequence_length++] = 'S';
    sigtimedwait_calls++;
    if (signal_step_index >= signal_step_count) {
        errno = EAGAIN;
        return -1;
    }

    step = signal_steps[signal_step_index++];
    if (step.result < 0) {
        errno = step.error_number;
    }
    return step.result;
}

static int
clock_gettime_for_test(clockid_t clock_id, struct timespec* ts)
{
    long milliseconds;

    (void)clock_id;
    if (clock_value_index >= clock_value_count) {
        milliseconds = clock_values[clock_value_count - 1];
    } else {
        milliseconds = clock_values[clock_value_index++];
    }
    ts->tv_sec = milliseconds / 1000L;
    ts->tv_nsec = (milliseconds % 1000L) * 1000000L;
    return 0;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed: %s (%s:%d)\n", \
                    #condition, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

static int
test_deadline_times_out(void)
{
    int detach_signal = -1;

    reset_fixture();
    wait_steps[wait_step_count++] = (WaitStep){0, 0, 0};
    clock_values[clock_value_count++] = 100L;
    clock_values[clock_value_count++] =
        100L + PEAK_DETACH_HELPER_STOP_TIMEOUT_MS;

    errno = 0;
    CHECK(wait_for_ptrace_stop(1234, &detach_signal) == -1);
    CHECK(errno == ETIMEDOUT);
    CHECK(detach_signal == 0);
    CHECK(waitpid_calls == 1);
    CHECK(sigtimedwait_calls == 0);
    return 0;
}

static int
test_no_child_failure_propagates(void)
{
    int detach_signal = -1;

    reset_fixture();
    wait_steps[wait_step_count++] = (WaitStep){-1, 0, ECHILD};
    clock_values[clock_value_count++] = 100L;

    errno = 0;
    CHECK(wait_for_ptrace_stop(1234, &detach_signal) == -1);
    CHECK(errno == ECHILD);
    CHECK(detach_signal == 0);
    CHECK(waitpid_calls == 1);
    CHECK(sigtimedwait_calls == 0);
    return 0;
}

static int
test_coalesced_sigchld_wake_consumes_target_stop(void)
{
    const pid_t target_tid = 1234;
    int detach_signal = -1;

    reset_fixture();
    wait_steps[wait_step_count++] = (WaitStep){0, 0, 0};
    wait_steps[wait_step_count++] =
        (WaitStep){target_tid, (SIGSTOP << 8) | 0x7f, 0};
    signal_steps[signal_step_count++] = (SignalStep){SIGCHLD, 0};
    clock_values[clock_value_count++] = 100L;
    clock_values[clock_value_count++] = 101L;

    CHECK(wait_for_ptrace_stop(target_tid, &detach_signal) == 0);
    CHECK(detach_signal == SIGSTOP);
    CHECK(waitpid_calls == 2);
    CHECK(sigtimedwait_calls == 1);
    CHECK(call_sequence_length == 3);
    CHECK(call_sequence[0] == 'W');
    CHECK(call_sequence[1] == 'S');
    CHECK(call_sequence[2] == 'W');
    return 0;
}

static int
test_unrelated_sigchld_wake_precedes_target_progress(void)
{
    const pid_t target_tid = 1234;
    int detach_signal = -1;

    reset_fixture();
    wait_steps[wait_step_count++] = (WaitStep){0, 0, 0};
    wait_steps[wait_step_count++] = (WaitStep){0, 0, 0};
    wait_steps[wait_step_count++] =
        (WaitStep){target_tid, (SIGSTOP << 8) | 0x7f, 0};
    signal_steps[signal_step_count++] = (SignalStep){SIGCHLD, 0};
    signal_steps[signal_step_count++] = (SignalStep){SIGCHLD, 0};
    clock_values[clock_value_count++] = 100L;
    clock_values[clock_value_count++] = 101L;
    clock_values[clock_value_count++] = 102L;

    CHECK(wait_for_ptrace_stop(target_tid, &detach_signal) == 0);
    CHECK(detach_signal == SIGSTOP);
    CHECK(waitpid_calls == 3);
    CHECK(sigtimedwait_calls == 2);
    CHECK(call_sequence_length == 5);
    CHECK(call_sequence[0] == 'W');
    CHECK(call_sequence[1] == 'S');
    CHECK(call_sequence[2] == 'W');
    CHECK(call_sequence[3] == 'S');
    CHECK(call_sequence[4] == 'W');
    return 0;
}

int
main(void)
{
    if (test_deadline_times_out() != 0 ||
        test_no_child_failure_propagates() != 0 ||
        test_coalesced_sigchld_wake_consumes_target_stop() != 0 ||
        test_unrelated_sigchld_wake_precedes_target_progress() != 0) {
        return 1;
    }

    puts("detach_helper_ptrace_stop_wait_unit_ok");
    return 0;
}
