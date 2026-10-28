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
__attribute__((noinline, visibility("default"))) void peak_mpi_burst_target(void)
{
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    sink++;
}

/* Never called: rank-wide protection intentionally sacrifices these samples. */
#define COLD_TARGET(N) \
    __attribute__((noinline, used, visibility("default"))) void peak_mpi_burst_cold_##N(void) { \
        __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory"); \
        sink += (N) + 1; \
    }
COLD_TARGET(0)
COLD_TARGET(1)
COLD_TARGET(2)
COLD_TARGET(3)
COLD_TARGET(4)
COLD_TARGET(5)
COLD_TARGET(6)
COLD_TARGET(7)
COLD_TARGET(8)
COLD_TARGET(9)
COLD_TARGET(10)
COLD_TARGET(11)
COLD_TARGET(12)
COLD_TARGET(13)
COLD_TARGET(14)
COLD_TARGET(15)
COLD_TARGET(16)
COLD_TARGET(17)
COLD_TARGET(18)
COLD_TARGET(19)
COLD_TARGET(20)
COLD_TARGET(21)
COLD_TARGET(22)
COLD_TARGET(23)
COLD_TARGET(24)
COLD_TARGET(25)
COLD_TARGET(26)
COLD_TARGET(27)
COLD_TARGET(28)
COLD_TARGET(29)
COLD_TARGET(30)
COLD_TARGET(31)
COLD_TARGET(32)
COLD_TARGET(33)
COLD_TARGET(34)
COLD_TARGET(35)
COLD_TARGET(36)
COLD_TARGET(37)
COLD_TARGET(38)
COLD_TARGET(39)
COLD_TARGET(40)
COLD_TARGET(41)
COLD_TARGET(42)
COLD_TARGET(43)
COLD_TARGET(44)
COLD_TARGET(45)
COLD_TARGET(46)
COLD_TARGET(47)
COLD_TARGET(48)
COLD_TARGET(49)
COLD_TARGET(50)
COLD_TARGET(51)
COLD_TARGET(52)
COLD_TARGET(53)
COLD_TARGET(54)
COLD_TARGET(55)
COLD_TARGET(56)
COLD_TARGET(57)
COLD_TARGET(58)
COLD_TARGET(59)
COLD_TARGET(60)
COLD_TARGET(61)
COLD_TARGET(62)
COLD_TARGET(63)

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
    if (argc > 3 && strcmp(argv[3], "1") == 0) {
        size_t targets = strtoul(argv[2], NULL, 10);
        PeakHookState (*state)(size_t) =
            dlsym(RTLD_DEFAULT, "peak_general_listener_hook_state");
        if (!state) return 1;
        uint64_t deadline = now_ns(CLOCK_MONOTONIC) + 1000000000;
        size_t detached = 0;
        do {
            detached = 0;
            for (size_t i = 0; i < targets; i++) {
                detached += state(i) == PEAK_HOOK_DETACHED;
            }
            if (detached == targets) break;
            struct timespec retry = { .tv_nsec = 1000000 };
            nanosleep(&retry, NULL);
        } while (now_ns(CLOCK_MONOTONIC) < deadline);
        if (detached != targets) {
            fprintf(stderr, "runtime wave incomplete: %zu/%zu detached\n", detached, targets);
            return 1;
        }
        printf("runtime_detached=%zu\n", detached);
    }
    puts("mpi_burst_workload_ok");
    return 0;
}
