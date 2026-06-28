#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int
main(void)
{
    struct sigevent event;
    timer_t timerid;
    int signum = SIGRTMIN;

    if (signum > SIGRTMAX) {
        signum = SIGALRM;
    }

    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_SIGNAL;
    event.sigev_signo = signum;
    memset(&timerid, 0x5a, sizeof(timerid));

    errno = 0;
    if (timer_create(CLOCK_MONOTONIC, &event, &timerid) != 0) {
        perror("timer_create");
        return 1;
    }

    errno = 0;
    if (timer_delete(timerid) != 0) {
        perror("timer_delete");
        return 2;
    }

    printf("peak_timer_create_abi_ok pid=%ld timer_t_size=%zu timer=0x%llx\n",
           (long)getpid(),
           sizeof(timerid),
           (unsigned long long)(uintptr_t)timerid);
    return 0;
}
