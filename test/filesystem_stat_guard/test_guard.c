#define _GNU_SOURCE
#include "internal/filesystem_stat_guard.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
extern void
proxy_set_mode(int);
extern int
proxy_entered(void);
extern unsigned long
proxy_calls(void);
extern uint64_t peak_filesystem_stat_guard_test_state(void);
extern void peak_filesystem_stat_guard_test_child_hook(void (*)(void));
static int child_hook_called;
static void reset_interruption_hook(void)
{
    if (child_hook_called) return;
    child_hook_called = 1;
    proxy_set_mode(0);
    struct statfs value;
    assert(statfs("/nested-during-child-reset", &value) == 0);
}
static _Atomic int result, error_value, started;
static _Atomic int keep_reading;
static _Atomic unsigned writer_successes;
static _Atomic int nested_completed;
static _Atomic int fork_child_query;
static _Atomic int forked_pid;
static sigjmp_buf abandoned_query;
static void jump_handler(int signo)
{
    (void)signo;
    siglongjmp(abandoned_query, 1);
}
static void *hot_reader(void *unused)
{
    (void)unused;
    struct statfs value;
    while (atomic_load(&keep_reading))
        assert(statfs("/", &value) == 0);
    return NULL;
}
static void *one_writer(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1000 && !atomic_load(&nested_completed); ++i)
    {
        if (peak_filesystem_stat_guard_try_stop())
        {
            atomic_fetch_add(&writer_successes, 1);
            peak_filesystem_stat_guard_resume();
        }
    }
    return NULL;
}
static void nested_handler(int signo)
{
    (void)signo;
    struct statfs value;
    /* This thread retains its outer reader even while writer admission closes. */
    proxy_set_mode(0);
    assert(statfs("/nested", &value) == 0);
    atomic_store(&nested_completed, 1);
}
static void
handler(int signo)
{
    (void)signo;
}
static void *
query(void *unused)
{
    if (unused)
        assert(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) == 0);
    struct statfs value;
    errno = EDOM;
    atomic_store(&started, 1);
    int rc = statfs("/", &value);
    atomic_store(&result, rc);
    atomic_store(&error_value, errno);
    if (atomic_load(&fork_child_query))
    {
        assert(child_hook_called);
        assert(peak_filesystem_stat_guard_try_stop());
        peak_filesystem_stat_guard_resume();
        _exit(0);
    }
    return NULL;
}
static void fork_handler(int signo)
{
    (void)signo;
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        /* Outer live query survives in this thread: no physical stop yet. */
        assert(!peak_filesystem_stat_guard_try_stop());
        proxy_set_mode(0);
        struct statfs value;
        assert(statfs("/nested-child", &value) == 0);
        assert(!peak_filesystem_stat_guard_try_stop());
        atomic_store(&fork_child_query, 1);
    }
    else
        atomic_store(&forked_pid, child);
}
static void *
query_loop(void *unused)
{
    (void)unused;
    struct statfs value;
    for (unsigned i = 0; i < 10000; i++)
        assert(statfs("/", &value) == 0);
    return NULL;
}
static void
until_entered(void)
{
    for (unsigned i = 0; !proxy_entered() && i < 10000; i++)
        usleep(100);
    assert(proxy_entered());
}
int
main(void)
{
    struct sigaction action = {.sa_handler = handler};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    proxy_set_mode(1);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    until_entered();
    assert(!peak_filesystem_stat_guard_try_stop()); /* No stop admitted during real
                                                       query. */
    assert(peak_filesystem_stat_guard_deferred_stop_count() == 1);
    assert(pthread_kill(worker, SIGUSR1) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&result) == -1 &&
           atomic_load(&error_value) == EINTR); /* Legitimate user EINTR unchanged. */
    proxy_set_mode(0);
    assert(peak_filesystem_stat_guard_try_stop());
    atomic_store(&started, 0);
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    while (!atomic_load(&started))
        usleep(100);
    usleep(10000);
    assert(!proxy_entered()); /* Query cannot reach backend while a stop is admitted. */
    peak_filesystem_stat_guard_resume();
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&result) == 0 && atomic_load(&error_value) == EDOM);
    assert(peak_filesystem_stat_guard_try_stop());
    peak_filesystem_stat_guard_resume();
    proxy_set_mode(1);
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    until_entered();
    assert(pthread_cancel(worker) == 0);
    void *joined = NULL;
    assert(pthread_join(worker, &joined) == 0);
    assert(joined == PTHREAD_CANCELED);
    assert(peak_filesystem_stat_guard_try_stop());
    peak_filesystem_stat_guard_resume(); /* cancellation releases admission */
    proxy_set_mode(1);
    assert(pthread_create(&worker, NULL, query, (void *)1) == 0);
    until_entered();
    assert(pthread_cancel(worker) == 0);
    assert(pthread_join(worker, &joined) == 0);
    assert(joined == PTHREAD_CANCELED);
    assert(peak_filesystem_stat_guard_try_stop());
    peak_filesystem_stat_guard_resume();

    struct statfs native;
    struct statfs64 large;
    errno = 0;
    assert(fstatfs(-1, &native) == -1 && errno == EBADF);
    errno = 0;
    assert(fstatfs64(-1, &large) == -1 && errno == EBADF);
    errno = EDOM;
    assert(fstatfs(STDOUT_FILENO, &native) == 0 && errno == EDOM);
    errno = EDOM;
    assert(fstatfs64(STDOUT_FILENO, &large) == 0 && errno == EDOM);
    errno = 0;
    assert(statfs64("/peak-statfs-regression-no-such-path", &large) == -1 &&
           errno == ENOENT);
    assert(pthread_atfork(NULL, NULL, peak_filesystem_stat_guard_after_fork_child) ==
           0);
    assert(peak_filesystem_stat_guard_try_stop());
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        assert(peak_filesystem_stat_guard_try_stop());
        peak_filesystem_stat_guard_resume();
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0);
    peak_filesystem_stat_guard_resume();
    pthread_t readers[4];
    proxy_set_mode(0);
    for (unsigned i = 0; i < 4; i++)
        assert(pthread_create(&readers[i], NULL, query_loop, NULL) == 0);
    unsigned stops = 0;
    for (unsigned i = 0; i < 10000; i++)
    {
        if (peak_filesystem_stat_guard_try_stop())
        {
            unsigned long before = proxy_calls();
            usleep(1);
            assert(proxy_calls() == before);
            peak_filesystem_stat_guard_resume();
            stops++;
        }
    }
    for (unsigned i = 0; i < 4; i++)
        assert(pthread_join(readers[i], NULL) == 0);
    assert(stops > 0);
    /* Forced long query: timeout preserves its count and reopens admission. */
    proxy_set_mode(1);
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    until_entered();
    errno = EDOM;
    assert(!peak_filesystem_stat_guard_try_stop() && errno == EDOM);
    assert((peak_filesystem_stat_guard_test_state() & UINT64_C(0xffffffff)) == 1);
    proxy_set_mode(0);
    assert(statfs("/new-reader-after-timeout", &native) == 0);
    assert(pthread_kill(worker, SIGUSR1) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert((peak_filesystem_stat_guard_test_state() & UINT64_C(0xffffffff)) == 0);

    /* Nested user handler must enter while its own outer query is draining. */
    action.sa_handler = nested_handler;
    assert(sigaction(SIGUSR2, &action, NULL) == 0);
    proxy_set_mode(1);
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    until_entered();
    pthread_t writer;
    assert(pthread_create(&writer, NULL, one_writer, NULL) == 0);
    while (!(peak_filesystem_stat_guard_test_state() & (UINT64_C(1) << 31)))
        ;
    assert(pthread_kill(worker, SIGUSR2) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0);
    assert(atomic_load(&nested_completed));
    assert(peak_filesystem_stat_guard_try_stop());
    peak_filesystem_stat_guard_resume();

    /* Cancellation while entry is closed must drain its admitted reader. */
    atomic_store(&nested_completed, 0);
    proxy_set_mode(1);
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    until_entered();
    assert(pthread_create(&writer, NULL, one_writer, NULL) == 0);
    while (!(peak_filesystem_stat_guard_test_state() & (UINT64_C(1) << 31)))
        ;
    assert(pthread_cancel(worker) == 0);
    assert(pthread_join(worker, &joined) == 0 && joined == PTHREAD_CANCELED);
    atomic_store(&nested_completed, 1);
    assert(pthread_join(writer, NULL) == 0);
    assert(peak_filesystem_stat_guard_try_stop());
    peak_filesystem_stat_guard_resume();

    /* Fork inside an application's signal handler must preserve the outer
     * query in the child, then release its updated-generation token exactly. */
    action.sa_handler = fork_handler;
    assert(sigaction(SIGUSR2, &action, NULL) == 0);
    proxy_set_mode(1);
    peak_filesystem_stat_guard_test_child_hook(reset_interruption_hook);
    atomic_store(&nested_completed, 0);
    assert(pthread_create(&worker, NULL, query, NULL) == 0);
    until_entered();
    assert(pthread_create(&writer, NULL, one_writer, NULL) == 0);
    while (!(peak_filesystem_stat_guard_test_state() & (UINT64_C(1) << 31)))
        ;
    assert(pthread_kill(worker, SIGUSR2) == 0);
    assert(pthread_join(worker, NULL) == 0);
    atomic_store(&nested_completed, 1);
    assert(pthread_join(writer, NULL) == 0);
    peak_filesystem_stat_guard_test_child_hook(NULL);
    child = atomic_load(&forked_pid);
    assert(child > 0);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(peak_filesystem_stat_guard_try_stop());
    peak_filesystem_stat_guard_resume();

    /* Overlapping 200 us readers intentionally eliminate lucky zero-reader gaps.
     * Closed entry drains them, admitting real stop windows under sustained load. */
    proxy_set_mode(2);
    atomic_store(&keep_reading, 1);
    for (unsigned i = 0; i < 4; ++i)
        assert(pthread_create(&readers[i], NULL, hot_reader, NULL) == 0);
    until_entered();
    stops = 0;
    for (unsigned i = 0; i < 100; ++i)
        if (peak_filesystem_stat_guard_try_stop())
        {
            assert((peak_filesystem_stat_guard_test_state() & UINT64_C(0xffffffff)) == (UINT64_C(1) << 31));
            ++stops;
            peak_filesystem_stat_guard_resume();
        }
    atomic_store(&keep_reading, 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(pthread_join(readers[i], NULL) == 0);
    assert(stops >= 50);
    /* Nonlocal exits abandon more than the stable-token capacity. Subsequent
     * queries retain original success semantics, while stops fail closed; fork
     * traverses only persistent TLS storage, never an abandoned stack frame. */
    child = fork();
    assert(child >= 0);
    if (!child)
    {
        action.sa_handler = jump_handler;
        assert(sigaction(SIGUSR2, &action, NULL) == 0);
        proxy_set_mode(3);
        for (volatile unsigned i = 0; i < 80; ++i)
            if (!sigsetjmp(abandoned_query, 1))
            {
                (void)statfs("/abandoned", &native);
                assert(0);
            }
        proxy_set_mode(0);
        errno = EDOM;
        assert(statfs("/after-abandonment", &native) == 0 && errno == EDOM);
        assert(!peak_filesystem_stat_guard_try_stop());
        pid_t grandchild = fork();
        assert(grandchild >= 0);
        if (!grandchild)
        {
            assert(statfs("/fork-after-abandonment", &native) == 0);
            assert(!peak_filesystem_stat_guard_try_stop());
            _exit(0);
        }
        assert(waitpid(grandchild, &status, 0) == grandchild && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("filesystem-stat stop exclusion and external-signal semantics passed");
    return 0;
}
