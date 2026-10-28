#define _GNU_SOURCE
#include "general_listener.h"
#include "internal/general_listener/burst_budget.h"
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile uint64_t sink;
__attribute__((noinline)) void peak_mpi_burst_target(void)
{
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    sink++;
}

static uint64_t now_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void* useful_worker(void* unused)
{
    (void)unused;
    for (unsigned int i = 0; i < 192; i++) {
        peak_mpi_burst_target();
        uint64_t until = now_ns(CLOCK_THREAD_CPUTIME_ID) + 1000000;
        while (now_ns(CLOCK_THREAD_CPUTIME_ID) < until) sink++;
    }
    return NULL;
}

static void* burst_worker(void* unused)
{
    (void)unused;
    useful_worker(NULL);
    struct timespec warmup = { .tv_sec = 2 };
    nanosleep(&warmup, NULL);
    uint64_t until = now_ns(CLOCK_MONOTONIC) + 200000000;
    while (now_ns(CLOCK_MONOTONIC) < until) peak_mpi_burst_target();
    return NULL;
}

int main(int argc, char** argv)
{
    GumInvocationListener*** listeners = dlsym(RTLD_DEFAULT, "array_listener");
    if (!listeners || !*listeners || !(*listeners)[0]) return 1;
    PeakGeneralListener* listener = (PeakGeneralListener*)(*listeners)[0];
    if (argc > 1 && strcmp(argv[1], "generic") == 0) {
        /* The test owns this pre-workload switch; no invocation is in flight. */
        __atomic_store_n(&listener->fast_listener.enabled, 0, __ATOMIC_RELEASE);
    } else if (!listener->fast_dispatch_enabled) {
        return 77;
    }
    /* Join/retirement must clear the window before a thread slot is reused. */
    pthread_t worker;
    if (pthread_create(&worker, NULL, useful_worker, NULL) ||
        pthread_join(worker, NULL)) return 1;
    for (size_t i = 0; i < listener->fast_stats_capacity; i++) {
        PeakBurstBudgetSlot* slot = (PeakBurstBudgetSlot*)(
            (unsigned char*)listener->fast_active + i * 64 + 40);
        if (slot->seeded || slot->calls) {
            fputs("retired burst window was not cleared\n", stderr);
            return 1;
        }
    }
    /* A warmup/wait creates no credit for the following dense call burst. */
    if (pthread_create(&worker, NULL, burst_worker, NULL) ||
        pthread_join(worker, NULL)) return 1;
    puts("mpi_burst_workload_ok");
    return 0;
}
