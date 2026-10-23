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
#include <unistd.h>

/* The policy itself is real; these two process selectors enable its normal
 * application path. Exec dispatch is irrelevant to the tested signal syscalls. */
int peak_process_profile_enabled(void) { return 1; }
int peak_process_requests_work(void) { return 1; }
int peak_exec_raw_syscall_handle(long n, long a, long b, long c, long d,
                                long e, long f, long *r)
{ (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)r; return 0; }

static long (*native_syscall)(long, ...);
static int event_fd;
static volatile sig_atomic_t armed, returned, raw_mode;
static volatile sig_atomic_t deliveries, verify_delivery;
static void ordinary_handler(int sig) { (void)sig; }
struct kernel_action { uintptr_t handler, flags, restorer; uint64_t mask; };

static void interrupted_handler(int sig)
{
    (void)sig;
    deliveries++;
    if (verify_delivery) return;
    (void)write(event_fd, "E", 1); /* Genuine handler entry before tested API. */
    uint64_t before = 0, after = 0;
    int mask_rc = native_syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &before, 8);
    int rc, call_errno;
    if (raw_mode) {
        struct kernel_action action = { .handler = (uintptr_t)SIG_IGN,
            .flags = SA_RESTART, .mask = UINT64_C(1) << (SIGTERM - 1) };
        errno = EDOM;
        rc = syscall(SYS_rt_sigaction, SIGUSR2, &action, NULL, 8);
        call_errno = errno;
    } else {
        struct sigaction action = { .sa_handler = ordinary_handler,
                                    .sa_flags = SA_RESTART };
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGTERM);
        errno = EDOM;
        rc = sigaction(SIGUSR2, &action, NULL);
        call_errno = errno;
    }
    int after_rc = native_syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &after, 8);
    struct kernel_action installed = {0};
    int query_rc = native_syscall(SYS_rt_sigaction, SIGUSR2, NULL, &installed, 8);
    uintptr_t expected = raw_mode ? (uintptr_t)SIG_IGN : (uintptr_t)ordinary_handler;
    /* libc supplies its ABI restorer flag; all requested semantic fields match. */
    returned = rc == 0 && call_errno == EDOM && mask_rc == 0 && after_rc == 0 &&
        before == after && (before & (UINT64_C(1) << (SIGUSR1 - 1))) != 0 &&
        query_rc == 0 && installed.handler == expected &&
        (installed.flags & ~UINT64_C(0x04000000)) == SA_RESTART &&
        installed.mask == (UINT64_C(1) << (SIGTERM - 1));
    (void)write(event_fd, returned ? "R" : "F", 1);
}

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int rc = __real_pthread_mutex_lock(mutex);
    if (rc == 0 && armed) {
        armed = 0;
        /* First lock after arming is prepare's migration_mutex, before any
         * internal-depth scope. Native tgkill cannot recurse through policy. */
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
        if (sigaction(SIGUSR1, &action, NULL) != 0) _exit(2);
        struct kernel_action usr1_before = {0}, usr1_after = {0};
        if (native_syscall(SYS_rt_sigaction, SIGUSR1, NULL, &usr1_before, 8)) _exit(5);
        int reserved = peak_signal_policy_choose_reserved_signal();
        if (reserved < SIGRTMIN) _exit(3);
        action.sa_handler = SIG_IGN;
        int migrations = peak_signal_policy_migration_count();
        armed = 1;
        int rc = sigaction(reserved, &action, NULL);
        int query_rc = native_syscall(SYS_rt_sigaction, SIGUSR1, NULL, &usr1_after, 8);
        verify_delivery = 1;
        int send_rc = native_syscall(SYS_tgkill, getpid(), native_syscall(SYS_gettid), SIGUSR1);
        _exit(rc == 0 && returned &&
              query_rc == 0 && send_rc == 0 && deliveries == 2 &&
              usr1_before.handler == (uintptr_t)interrupted_handler &&
              usr1_before.handler == usr1_after.handler &&
              usr1_before.flags == usr1_after.flags &&
              usr1_before.restorer == usr1_after.restorer &&
              usr1_before.mask == usr1_after.mask &&
              peak_signal_policy_migration_count() == migrations + 1 &&
              peak_signal_policy_reserved_signal() != reserved ? 0 : 4);
    }
    close(pipefd[1]);
    char events[16] = {0}; int used = 0, status = 0, finished = 0;
    /* Bounded child poll; baseline deadlock is killed and reaped by its parent. */
    for (int i = 0; i < 100; i++) {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN) && used < 15) {
            int n = read(pipefd[0], events + used, 15 - used);
            if (n > 0) used += n;
        }
        if (waitpid(child, &status, WNOHANG) == child) { finished = 1; break; }
        usleep(1000); /* Also bound loops when poll reports immediate HUP. */
    }
    if (!finished) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    /* The writer is gone after waitpid: drain bytes written after last poll. */
    while (used < 15) {
        int n = read(pipefd[0], events + used, 15 - used);
        if (n <= 0) break;
        used += n;
    }
    close(pipefd[0]);
    printf("api=%s events=%s completed=%d status=%d reaped=1 mask_errno_action_usr1=%s\n",
           raw ? "raw-rt_sigaction" : "libc-sigaction", events, finished, status,
           finished && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "PASS" : "NOT-PASSED");
    return !(finished && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             strcmp(events, "LERS") == 0);
}

static int controls(void)
{
    int failed = 0;
    int (*native_sigaction)(int, const struct sigaction *, struct sigaction *) =
        dlsym(RTLD_NEXT, "sigaction");
    struct sigaction action = { .sa_handler = ordinary_handler };
    sigemptyset(&action.sa_mask);
    int invalid[] = {-1, 0, SIGRTMAX + 1};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        errno = 0; int native_rc = native_sigaction(invalid[i], &action, NULL);
        int native_errno = errno;
        errno = 0; int rc = sigaction(invalid[i], &action, NULL); int err = errno;
        printf("libc invalid=%d rc=%d errno=%d native=%d/%d\n", invalid[i], rc, err, native_rc, native_errno);
        failed |= rc != native_rc || err != native_errno;
        errno = 0;
        long nraw = native_syscall(SYS_rt_sigaction, invalid[i], (void *)1, NULL, 8);
        int nerr = errno;
        errno = 0;
        long raw = syscall(SYS_rt_sigaction, invalid[i], (void *)1, NULL, 8);
        int rerr = errno;
        printf("raw invalid=%d bad-input rc=%ld errno=%d native=%ld/%d\n", invalid[i], raw, rerr, nraw, nerr);
        failed |= raw != nraw || rerr != nerr;
    }
    struct kernel_action ka = { .handler = (uintptr_t)SIG_IGN }, old, set_native = {0};
    native_syscall(SYS_rt_sigaction, SIGUSR2, &ka, NULL, 8);
    const void *inputs[] = {&ka, (void *)1, &ka, (void *)1};
    void *outputs[] = {&old, NULL, (void *)1, (void *)1};
    for (int i = 0; i < 4; i++) {
        errno = 0; long native_rc = native_syscall(SYS_rt_sigaction, SIGUSR2, inputs[i], outputs[i], 8);
        int native_errno = errno;
        if (i == 0) set_native = old;
        errno = 0; long rc = syscall(SYS_rt_sigaction, SIGUSR2, inputs[i], outputs[i], 8); int err = errno;
        printf("raw control=%d rc=%ld errno=%d native=%ld/%d\n", i, rc, err, native_rc, native_errno);
        failed |= rc != native_rc || err != native_errno;
        if (i == 0) {
            printf("raw SET oldact native=%lu/%lu/%lu/%llu observed=%lu/%lu/%lu/%llu\n",
                   (unsigned long)set_native.handler, (unsigned long)set_native.flags,
                   (unsigned long)set_native.restorer, (unsigned long long)set_native.mask,
                   (unsigned long)old.handler, (unsigned long)old.flags,
                   (unsigned long)old.restorer, (unsigned long long)old.mask);
            failed |= set_native.handler != old.handler || set_native.flags != old.flags ||
                      set_native.restorer != old.restorer || set_native.mask != old.mask;
        }
    }
    struct kernel_action native_old = {0}, observed = {0};
    long nr = native_syscall(SYS_rt_sigaction, SIGUSR2, NULL, &native_old, 8);
    long qr = syscall(SYS_rt_sigaction, SIGUSR2, NULL, &observed, 8);
    printf("raw oldact native=%lu/%lu/%lu/%llu observed=%lu/%lu/%lu/%llu\n",
           (unsigned long)native_old.handler, (unsigned long)native_old.flags,
           (unsigned long)native_old.restorer, (unsigned long long)native_old.mask,
           (unsigned long)observed.handler, (unsigned long)observed.flags,
           (unsigned long)observed.restorer, (unsigned long long)observed.mask);
    failed |= nr != 0 || qr != 0 || native_old.handler != observed.handler ||
              native_old.flags != observed.flags || native_old.restorer != observed.restorer ||
              native_old.mask != observed.mask;
    return failed;
}

int main(void)
{
    native_syscall = dlsym(RTLD_NEXT, "syscall");
    if (!native_syscall) return 2;
    unsetenv("PEAK_DETACH_SIGNAL");
    int failures = controls();
    failures += reentry(0);
    failures += reentry(1);
    /* This hidden setter has no product callers, but must preserve the RT-only
     * reservation invariant required by the ordinary-signal bypass. */
    peak_signal_policy_clear_reserved_signal();
    struct kernel_action default_action = { .handler = (uintptr_t)SIG_DFL };
    failures += native_syscall(SYS_rt_sigaction, SIGUSR2, &default_action, NULL, 8) != 0;
    peak_signal_policy_set_reserved_signal(SIGUSR2);
    int domain_ok = peak_signal_policy_reserved_signal() == 0;
    peak_signal_policy_set_reserved_signal(SIGRTMAX + 1);
    domain_ok &= peak_signal_policy_reserved_signal() == 0;
    peak_signal_policy_set_reserved_signal(SIGRTMAX);
    int valid = peak_signal_policy_reserved_signal();
    domain_ok &= valid == SIGRTMAX;
    failures += !domain_ok;
    printf("commit-domain nonRT_rejected_and_RT_accepted=%s reserved=%d\n",
           domain_ok ? "PASS" : "FAIL", valid);
    peak_signal_policy_clear_reserved_signal();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
