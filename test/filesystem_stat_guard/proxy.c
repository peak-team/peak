#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/vfs.h>
#include <time.h>
/* Guard-test builds rename only the guard's clock_gettime calls to this
 * delegate. Delay after capturing the start time to model writer descheduling. */
static _Thread_local int writer_clock_delay;
void proxy_delay_writer_clock(void)
{
    writer_clock_delay = 1;
}
int peak_guard_test_clock_gettime(clockid_t clock, struct timespec *value)
{
    int rc = clock_gettime(clock, value);
    if (writer_clock_delay) {
        writer_clock_delay = 0;
        struct timespec delay = {.tv_nsec = 20000000};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    }
    return rc;
}
static _Atomic int entered, mode;
static _Atomic unsigned long calls;
static void (*query_hook)(void);
void proxy_set_hook(void (*hook)(void)) { query_hook = hook; }
void
proxy_set_mode(int value)
{
    atomic_store(&entered, 0);
    atomic_store(&mode, value);
}
unsigned long
proxy_calls(void)
{
    return atomic_load(&calls);
}
int
proxy_entered(void)
{
    return atomic_load(&entered);
}
int
statfs(const char *path, struct statfs *buf)
{
    (void)path;
    (void)buf;
    atomic_fetch_add(&calls, 1);
    atomic_store(&entered, 1);
    if (query_hook) query_hook();
    if (atomic_load(&mode) == 1)
    {
        struct timespec delay = {.tv_sec = 10};
        return nanosleep(&delay, 0); /* A real user signal produces real EINTR. */
    }
    if (atomic_load(&mode) == 3)
        raise(SIGUSR2);
    if (atomic_load(&mode) == 2)
    {
        struct timespec delay = {.tv_nsec = 200000};
        (void)nanosleep(&delay, 0);
    }
    return 0; /* Preserve incoming errno on success, as this proxy promises. */
}
