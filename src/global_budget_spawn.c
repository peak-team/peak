#define _GNU_SOURCE
#include "internal/global_budget_spawn.h"
#if defined(__linux__)
#include "internal/exec_raw_syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static bool executable(const char *candidate, char *path)
{
    struct stat st;
    return candidate && realpath(candidate, path) && !stat(path, &st) && S_ISREG(st.st_mode) &&
           !access(path, X_OK);
}
static void free_env(char **env)
{
    if (env) {
        for (size_t i = 0; env[i]; i++)
            free(env[i]);
        free(env);
    }
}
bool peak_global_budget_reap(pid_t pid)
{
    if (pid <= 0)
        return true;
    for (unsigned stage = 0; stage < 3; stage++) {
        if (stage)
            (void)kill(pid, stage == 1 ? SIGTERM : SIGKILL);
        for (unsigned i = 0; i < 100; i++) {
            pid_t result = waitpid(pid, NULL, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD))
                return true;
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    /* SIGKILL has been delivered; leave no unbounded wait in profiler teardown. */
    pid_t last = waitpid(pid, NULL, WNOHANG);
    if (last == pid || (last < 0 && errno == ECHILD))
        return true;
    fprintf(stderr,
            "[peak] native global broker: pid=%ld reap_confirmed=0 after bounded SIGKILL wait\n",
            (long)pid);
    return false;
}
/* Signals do not restart the one-second readiness budget. */
static bool wait_ready(int fd)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) || stamp.tv_sec < 0)
        return false;
    uint64_t start = (uint64_t)stamp.tv_sec * UINT64_C(1000000000) + stamp.tv_nsec;
    uint64_t previous = start;
    for (unsigned attempt = 0; attempt < 4096; ++attempt) {
        if (clock_gettime(CLOCK_MONOTONIC, &stamp) || stamp.tv_sec < 0)
            return false;
        uint64_t now = (uint64_t)stamp.tv_sec * UINT64_C(1000000000) + stamp.tv_nsec;
        if (now < previous || now - start >= UINT64_C(1000000000))
            return false;
        previous = now;
        int timeout = (int)((UINT64_C(1000000000) - (now - start) + 999999) / 1000000);
        struct pollfd ready = {fd, POLLIN, 0};
        int result = poll(&ready, 1, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        unsigned char byte = 0;
        ssize_t count = read(fd, &byte, 1);
        if (count < 0 && errno == EINTR)
            continue;
        return count == 1 && byte == 0xa7;
    }
    return false;
}

pid_t peak_global_budget_spawn(int listener, unsigned size, double target, uint64_t nonce,
                               bool trace)
{
    char path[PATH_MAX];
    const char *override = getenv("PEAK_DETACH_HELPER");
    if (override) {
        if (!executable(override, path))
            return -1;
    } else {
        bool found = false;
#ifdef PEAK_DEFAULT_DETACH_HELPER_PATH
        found = executable(PEAK_DEFAULT_DETACH_HELPER_PATH, path);
#endif
#ifdef PEAK_INSTALL_DETACH_HELPER_PATH
        if (!found)
            found = executable(PEAK_INSTALL_DETACH_HELPER_PATH, path);
#endif
        if (!found)
            return -1;
    }
    size_t count = 0;
    while (environ && environ[count])
        count++;
    char **env = calloc(count + 1, sizeof(*env));
    if (!env)
        return -1;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strncmp(environ[i], "LD_PRELOAD=", 11) || !strncmp(environ[i], "LD_AUDIT=", 9) ||
            !strncmp(environ[i], "PEAK_", 5))
            continue;
        env[kept] = strdup(environ[i]);
        if (!env[kept]) {
            free_env(env);
            return -1;
        }
        kept++;
    }
    int ready[2];
    if (pipe2(ready, O_CLOEXEC)) {
        free_env(env);
        return -1;
    }
    int pass = fcntl(listener, F_DUPFD_CLOEXEC, 10), ack = fcntl(ready[1], F_DUPFD_CLOEXEC, 10);
    if (pass < 0 || ack < 0) {
        if (pass >= 0)
            close(pass);
        if (ack >= 0)
            close(ack);
        close(ready[0]);
        close(ready[1]);
        free_env(env);
        return -1;
    }
    char world[24], ratio[32], token[32], parent[24], tracing[4];
    uint64_t target_bits;
    memcpy(&target_bits, &target, sizeof target_bits);
    snprintf(world, sizeof world, "%u", size);
    snprintf(ratio, sizeof ratio, "%llu", (unsigned long long)target_bits);
    snprintf(token, sizeof token, "%llu", (unsigned long long)nonce);
    snprintf(parent, sizeof parent, "%ld", (long)getpid());
    snprintf(tracing, sizeof tracing, "%d", trace);
    char *argv[] = {path, "--global-budget-broker", "3", "4", world, ratio, token, parent, tracing,
                    NULL};
    pid_t parent_pid = getpid();
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 5 || maxfd > 1048576)
        maxfd = 1048576;
    pid_t pid = fork();
    if (!pid) {
        if (peak_exec_raw_syscall6(SYS_dup3, pass, 3, 0, 0, 0, 0) < 0 ||
            peak_exec_raw_syscall6(SYS_dup3, ack, 4, 0, 0, 0, 0) < 0) {
            peak_exec_raw_syscall6(SYS_exit_group, 126, 0, 0, 0, 0, 0);
            __builtin_unreachable();
        }
#ifdef SYS_close_range
        if (peak_exec_raw_syscall6(SYS_close_range, 5u, ~0u, 0, 0, 0, 0) < 0)
#endif
            for (int fd = 5; fd < maxfd; fd++)
                peak_exec_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
        if (peak_exec_raw_syscall6(SYS_getppid, 0, 0, 0, 0, 0, 0) != parent_pid) {
            peak_exec_raw_syscall6(SYS_exit_group, 126, 0, 0, 0, 0, 0);
            __builtin_unreachable();
        }
        peak_exec_raw_syscall6(SYS_execve, (long)path, (long)argv, (long)env, 0, 0, 0);
        peak_exec_raw_syscall6(SYS_exit_group, 127, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
    close(pass);
    close(ack);
    close(ready[1]);
    free_env(env);
    if (pid < 0) {
        close(ready[0]);
        return -1;
    }
    bool okay = wait_ready(ready[0]);
    close(ready[0]);
    if (!okay) {
        peak_global_budget_reap(pid);
        return -1;
    }
    return pid;
}
#else
pid_t peak_global_budget_spawn(int l, unsigned s, double t, uint64_t n, bool r)
{
    (void)l;
    (void)s;
    (void)t;
    (void)n;
    (void)r;
    return -1;
}
bool peak_global_budget_reap(pid_t p)
{
    (void)p;
    return true;
}
#endif
