#define _GNU_SOURCE
#include <time.h>
static unsigned mode;
static int controlled_clock_gettime(clockid_t clock, struct timespec *t);
#define clock_gettime controlled_clock_gettime
#include "../../src/global_budget.c"
#undef clock_gettime
#include <assert.h>
static int controlled_clock_gettime(clockid_t c, struct timespec *t)
{
    if (c == CLOCK_PROCESS_CPUTIME_ID) {
        if (mode == 1)
            return -1;
        t->tv_sec = mode == 2 ? 0 : 1;
        t->tv_nsec = 500000000;
        return 0;
    }
    return clock_gettime(c, t);
}
int main(void)
{
    uint64_t last = 0;
    double seconds = 0;
    assert(broker_cpu_snapshot(&last, &seconds));
    assert(seconds == 1.5);
    mode = 1;
    assert(!broker_cpu_snapshot(&last, &seconds));
    assert(seconds == 1.5);
    mode = 2;
    assert(!broker_cpu_snapshot(&last, &seconds));
    assert(last == 1500000000 && seconds == 1.5);
    puts("global_budget_broker_clock_ok");
}
