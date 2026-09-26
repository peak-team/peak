#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/vfs.h>
#include <time.h>
static _Atomic int entered, mode;
static _Atomic unsigned long calls;
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
