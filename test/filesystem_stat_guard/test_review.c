#define _GNU_SOURCE
#include "internal/filesystem_stat_guard.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
extern void proxy_set_hook(void (*)(void));
extern uint64_t peak_filesystem_stat_guard_test_state(void);
static unsigned depth, limit;
static int expect_bug, fork_nested, cancel_nested, abandon_nested;
static _Atomic int deepest;
static sigjmp_buf abandon;
static pid_t nested_child;
static int in_child;
static void nested(void)
{
    struct statfs value;
    if (++depth < limit) assert(statfs("/nested", &value) == 0);
    else
    {
        assert(!peak_filesystem_stat_guard_try_stop());
        if (fork_nested)
        {
            nested_child = fork();
            assert(nested_child >= 0);
            if (!nested_child)
            {
                in_child = 1;
                assert(!peak_filesystem_stat_guard_try_stop());
            }
        }
        if (abandon_nested) siglongjmp(abandon, 1);
        if (cancel_nested)
        {
            atomic_store(&deepest, 1);
            struct timespec delay = {.tv_sec = 10};
            (void)nanosleep(&delay, NULL);
            assert(0);
        }
    }
    --depth;
}
static void *nested_worker(void *unused)
{
    (void)unused;
    struct statfs value;
    assert(statfs("/nested-worker", &value) == 0);
    return NULL;
}
static void *cold_stop_probe(void *data)
{
    int *stopped = data;
    *stopped = peak_filesystem_stat_guard_try_stop();
    if (*stopped) peak_filesystem_stat_guard_resume();
    return NULL;
}
static void cold_query(void)
{
    uint64_t readers = peak_filesystem_stat_guard_test_state() & 0x7fffffff;
    int stopped = -1;
    pthread_t controller;
    /* A different thread has no query TLS head, so this checks the global
     * reader count rather than the caller's self-stop rejection. */
    assert(pthread_create(&controller, NULL, cold_stop_probe, &stopped) == 0);
    assert(pthread_join(controller, NULL) == 0);
    printf("cold child readers=%llu other_thread_stop=%d\n", (unsigned long long)readers, stopped);
    fflush(stdout);
    assert(expect_bug ? readers == 0 && stopped : readers == 1 && !stopped);
}
#ifndef REVIEW_ACTUAL_CONTROLLER
static void child_scope_reset(void)
{
#ifdef REVIEW_FIXED
    peak_filesystem_stat_guard_controller_after_fork_child();
#endif
}
#else
extern void peak_detach_controller_test_register_atfork(void);
#endif
int main(int argc, char **argv)
{
    assert(argc >= 2);
    expect_bug = argc == 3 && !strcmp(argv[2], "--expect-bug");
    struct statfs value;
    if (!strncmp(argv[1], "cold", 4))
    {
        /* Exact controller prepare/parent/child scope lifecycle, before the
         * guard has resolved or registered its own child callback. */
        if (!strcmp(argv[1], "cold-warm-before")) assert(statfs("/", &value) == 0);
#ifdef REVIEW_ACTUAL_CONTROLLER
        peak_detach_controller_test_register_atfork();
#else
        assert(pthread_atfork(peak_filesystem_stat_guard_controller_enter,
                             peak_filesystem_stat_guard_controller_leave,
                             child_scope_reset) == 0);
#endif
        if (!strcmp(argv[1], "cold-warm-after")) assert(statfs("/", &value) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { proxy_set_hook(cold_query); assert(statfs("/", &value) == 0); _exit(0); }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    else
    {
        limit = !strcmp(argv[1], "four") ? 4 : !strcmp(argv[1], "five") ? 5 : 32;
        proxy_set_hook(nested);
        fork_nested = !strcmp(argv[1], "nested-fork");
        cancel_nested = !strcmp(argv[1], "nested-cancel");
        abandon_nested = !strcmp(argv[1], "nested-abandon");
        if (cancel_nested)
        {
            pthread_t worker;
            assert(pthread_create(&worker, NULL, nested_worker, NULL) == 0);
            while (!atomic_load(&deepest)) usleep(100);
            assert(pthread_cancel(worker) == 0);
            void *result;
            assert(pthread_join(worker, &result) == 0 && result == PTHREAD_CANCELED);
        }
        else if (!abandon_nested || !sigsetjmp(abandon, 1))
            assert(statfs("/", &value) == 0);
        if (abandon_nested)
        {
            proxy_set_hook(NULL);
            assert(statfs("/after-nonlocal-exit", &value) == 0);
            assert(!peak_filesystem_stat_guard_try_stop());
            pid_t child = fork();
            assert(child >= 0);
            if (!child)
            {
                assert(statfs("/after-fork-with-stale-head", &value) == 0);
                assert(!peak_filesystem_stat_guard_try_stop());
                _exit(0);
            }
            int status;
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
            puts("nonlocal-exit borrowed query remains fail closed across fork");
            return 0;
        }
        uint64_t readers = peak_filesystem_stat_guard_test_state() & 0x7fffffff;
        int stopped = peak_filesystem_stat_guard_try_stop();
        printf("nested depth=%u readers=%llu stop=%d\n", limit, (unsigned long long)readers, stopped);
        assert(expect_bug ? readers == 1 && !stopped : readers == 0 && stopped);
        if (stopped) peak_filesystem_stat_guard_resume();
        if (fork_nested)
        {
            if (in_child) _exit(0);
            int status;
            assert(waitpid(nested_child, &status, 0) == nested_child && WIFEXITED(status) && !WEXITSTATUS(status));
        }
    }
    return 0;
}
