#define _GNU_SOURCE
#include "internal/filesystem_stat_guard.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
extern void
proxy_set_mode(int);
extern int
proxy_entered(void);
extern unsigned long
proxy_calls(void);
static _Atomic int result, error_value, started;
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
    return NULL;
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
    puts("filesystem-stat stop exclusion and external-signal semantics passed");
    return 0;
}
