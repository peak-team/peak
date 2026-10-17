#define _GNU_SOURCE
#include "internal/signal_policy_internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Real production policy and syscall trampoline; only process selection and
 * unrelated exec dispatch are stubbed, as in test_signal_nonrt_reentry. */
int peak_process_profile_enabled(void) { return 1; }
int peak_process_requests_work(void) { return 1; }
int peak_exec_raw_syscall_handle(long n, long a, long b, long c, long d,
                                long e, long f, long *r)
{ (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)r; return 0; }
static long (*native_syscall)(long, ...);
static int (*native_timer_create)(clockid_t, struct sigevent *, timer_t *);
static volatile sig_atomic_t armed, returned, raw_mode, deliveries;
static int event_fd, raw_ids[2];
static timer_t libc_ids[2];
static int observed[9];
struct kernel_action { uintptr_t handler, flags, restorer; uint64_t mask; };

static void interrupted_handler(int sig)
{
    (void)sig;
    deliveries++;
    (void)write(event_fd, "E", 1);
    struct sigevent event = { .sigev_notify = SIGEV_SIGNAL,
        .sigev_signo = SIGUSR2, .sigev_value.sival_int = 0x12345 };
    struct sigevent original = event;
    uint64_t before = 0, after = 0;
    struct kernel_action action_before = {0}, action_after = {0};
    int mr = native_syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &before, 8);
    int ar = native_syscall(SYS_rt_sigaction, SIGUSR2, NULL, &action_before, 8);
    errno = EDOM;
    int nr = raw_mode ? native_syscall(SYS_timer_create, CLOCK_MONOTONIC, &event, &raw_ids[0])
                      : native_timer_create(CLOCK_MONOTONIC, &event, &libc_ids[0]);
    int ne = errno;
    (void)write(event_fd, nr == 0 && ne == EDOM ? "N" : "F", 1);
    errno = EDOM;
    int rc = raw_mode ? syscall(SYS_timer_create, CLOCK_MONOTONIC, &event, &raw_ids[1])
                      : timer_create(CLOCK_MONOTONIC, &event, &libc_ids[1]);
    int err = errno;
    int ma = native_syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &after, 8);
    int aa = native_syscall(SYS_rt_sigaction, SIGUSR2, NULL, &action_after, 8);
    observed[0] = nr; observed[1] = rc; observed[2] = ne; observed[3] = err;
    observed[4] = mr; observed[5] = ma; observed[6] = before == after;
    observed[7] = memcmp(&action_before, &action_after, sizeof(action_before));
    observed[8] = memcmp(&event, &original, sizeof(event));
    returned = nr == 0 && rc == nr && err == ne && ne == EDOM &&
        mr == 0 && ma == 0 && before == after &&
        (before & (UINT64_C(1) << (SIGUSR1 - 1))) && ar == 0 && aa == 0 &&
        memcmp(&action_before, &action_after, sizeof(action_before)) == 0 &&
        memcmp(&event, &original, sizeof(event)) == 0;
    (void)write(event_fd, returned ? "R" : "F", 1);
}

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int rc = __real_pthread_mutex_lock(mutex);
    if (rc == 0 && armed) {
        armed = 0;
        (void)write(event_fd, "L", 1);
        native_syscall(SYS_tgkill, getpid(), native_syscall(SYS_gettid), SIGUSR1);
        (void)write(event_fd, returned ? "S" : "F", 1);
    }
    return rc;
}

static int reentry(int raw)
{
    int pipefd[2];
    if (pipe(pipefd)) return 1;
    pid_t child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); return 1; }
    if (child == 0) {
        close(pipefd[0]); event_fd = pipefd[1]; raw_mode = raw;
        struct sigaction action = { .sa_handler = interrupted_handler };
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR1, &action, NULL)) _exit(2);
        action.sa_handler = SIG_IGN;
        if (sigaction(SIGUSR2, &action, NULL)) _exit(3);
        struct kernel_action before = {0}, after = {0};
        if (native_syscall(SYS_rt_sigaction, SIGUSR1, NULL, &before, 8)) _exit(4);
        int reserved = peak_signal_policy_choose_reserved_signal();
        if (reserved < SIGRTMIN) _exit(5);
        int migrations = peak_signal_policy_migration_count();
        armed = 1;
        int rc = sigaction(reserved, &action, NULL);
        int qr = native_syscall(SYS_rt_sigaction, SIGUSR1, NULL, &after, 8);
        dprintf(STDOUT_FILENO, "detail raw=%d native_rc=%d wrapped_rc=%d native_errno=%d wrapped_errno=%d mask_rc=%d/%d mask_equal=%d action_cmp=%d event_cmp=%d\n",
            raw, observed[0], observed[1], observed[2], observed[3], observed[4], observed[5], observed[6], observed[7], observed[8]);
        int cleaned = 1;
        /* Neither native nor wrapped timer is armed. Validate and reclaim both
         * outside the handler, after the outer migration has resumed. */
        for (int i = 0; i < 2; i++) {
            struct itimerspec value = {0};
            int gr = raw ? native_syscall(SYS_timer_gettime, raw_ids[i], &value)
                         : timer_gettime(libc_ids[i], &value);
            int dr = raw ? native_syscall(SYS_timer_delete, raw_ids[i]) : timer_delete(libc_ids[i]);
            cleaned &= gr == 0 && dr == 0 && value.it_value.tv_sec == 0 &&
                value.it_value.tv_nsec == 0 && value.it_interval.tv_sec == 0 &&
                value.it_interval.tv_nsec == 0;
        }
        _exit(rc == 0 && returned && deliveries == 1 && cleaned && qr == 0 &&
            memcmp(&before, &after, sizeof(before)) == 0 &&
            peak_signal_policy_migration_count() == migrations + 1 &&
            peak_signal_policy_reserved_signal() != reserved ? 0 : 6);
    }
    close(pipefd[1]);
    char events[16] = {0}; int used = 0, status = 0, finished = 0;
    for (int i = 0; i < 100; i++) {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN) && used < 15) {
            int n = read(pipefd[0], events + used, 15 - used);
            if (n > 0) used += n;
        }
        if (waitpid(child, &status, WNOHANG) == child) { finished = 1; break; }
        usleep(1000);
    }
    if (!finished) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    while (used < 15) {
        int n = read(pipefd[0], events + used, 15 - used);
        if (n <= 0) break;
        used += n;
    }
    close(pipefd[0]);
    int passed = finished && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        strcmp(events, "LENRS") == 0;
    printf("api=%s events=%s completed=%d status=%d reaped=1 native_event_mask_errno_actions_unarmed_delete=%s\n",
        raw ? "raw-timer_create" : "libc-timer_create", events, finished, status,
        passed ? "PASS" : "NOT-PASSED");
    return !passed;
}

int main(void)
{
    native_syscall = dlsym(RTLD_NEXT, "syscall");
    native_timer_create = dlvsym(RTLD_NEXT, "timer_create", "GLIBC_2.34");
    if (!native_timer_create)
        native_timer_create = dlvsym(RTLD_NEXT, "timer_create", "GLIBC_2.3.3");
    if (!native_timer_create) native_timer_create = dlsym(RTLD_NEXT, "timer_create");
    if (!native_syscall || !native_timer_create) return 2;
    unsetenv("PEAK_DETACH_SIGNAL");
    int failures = reentry(0);
    failures += reentry(1);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
