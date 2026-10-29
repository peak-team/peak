#define _GNU_SOURCE
#include <mpi.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "internal/global_budget.h"

#define A UINT64_C(6364136223846793005)
#define B UINT64_C(1442695040888963407)
#if defined(__GNUC__) && !defined(__clang__)
#define NOIPA __attribute__((noclone, noipa))
#else
#define NOIPA
#endif
#define TARGET(n) \
    __attribute__((noinline, used, visibility("default"), patchable_function_entry(16, 0))) \
    NOIPA uint64_t budget_target_##n(uint64_t x) { __asm__ __volatile__("" : "+r"(x)); return A * x + B + n; }
TARGET(0)
TARGET(1)
TARGET(2)
TARGET(3)
TARGET(4)
TARGET(5)
TARGET(6)
TARGET(7)
TARGET(8)
TARGET(9)
TARGET(10)
TARGET(11)
TARGET(12)
TARGET(13)
TARGET(14)
TARGET(15)
TARGET(16)
TARGET(17)
TARGET(18)
TARGET(19)
TARGET(20)
TARGET(21)
TARGET(22)
TARGET(23)
TARGET(24)
TARGET(25)
TARGET(26)
TARGET(27)
TARGET(28)
TARGET(29)
TARGET(30)
TARGET(31)
TARGET(32)
TARGET(33)
TARGET(34)
TARGET(35)
TARGET(36)
TARGET(37)
TARGET(38)
TARGET(39)
TARGET(40)
TARGET(41)
TARGET(42)
TARGET(43)
TARGET(44)
TARGET(45)
TARGET(46)
TARGET(47)
TARGET(48)
TARGET(49)
TARGET(50)
TARGET(51)
TARGET(52)
TARGET(53)
TARGET(54)
TARGET(55)
TARGET(56)
TARGET(57)
TARGET(58)
TARGET(59)
TARGET(60)
TARGET(61)
TARGET(62)
TARGET(63)
TARGET(64)
TARGET(65)
TARGET(66)
TARGET(67)
TARGET(68)
TARGET(69)
TARGET(70)
TARGET(71)
TARGET(72)
TARGET(73)
TARGET(74)
TARGET(75)
TARGET(76)
TARGET(77)
TARGET(78)
TARGET(79)
#undef TARGET

static int (*coordinator_active)(void);
static uint32_t (*physical_targets)(void);
static int (*physical_hook)(unsigned);
static uint64_t (*target_calls)(void);
static PeakGlobalBudgetView (*budget_view)(void);

static double now(void)
{
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return stamp.tv_sec + stamp.tv_nsec / 1e9;
}

__attribute__((noinline)) static uint64_t native_work(uint64_t x)
{
    for (unsigned i = 0; i < 16384; i++)
        x = A * x + B;
    return x;
}

static int work(double duration, int hot)
{
    uint64_t x = 1;
    double deadline = now() + duration;
    do {
        if (hot) {
            uint64_t expected = x;
            for (unsigned i = 0; i < 2000; i++)
                expected = A * expected + B;
            for (unsigned i = 0; i < 2000; i++)
                x = budget_target_0(x);
            if (x != expected)
                return 1;
        } else {
            x = native_work(x);
        }
        if (!coordinator_active())
            return 1;
    } while (now() < deadline);
    /* Retain the native computation as observable work. */
    printf("work hot=%d checksum=%llu\n", hot, (unsigned long long)x);
    return 0;
}

static int cold_targets_attached(void)
{
    for (unsigned hook = 1; hook < 80; ++hook)
        if (!physical_hook(hook)) return 0;
    return 1;
}

static int wait_physical(unsigned count, double timeout)
{
    uint64_t x = 1;
    double deadline = now() + timeout;
    do {
        x = native_work(x);
        if (!coordinator_active() || !cold_targets_attached())
            return 1;
        if ((count ? physical_targets() == count : physical_targets() > 0)) {
            printf("physical=%u checksum=%llu\n", count, (unsigned long long)x);
            return 0;
        }
    } while (now() < deadline);
    return 1;
}

static int exhaust_credit(void)
{
    double deadline = now() + 6;
    do {
        if (work(.01, 1))
            return 1;
        PeakGlobalBudgetView view = budget_view();
        if (!cold_targets_attached()) return 1;
        if (view.wave && view.root_completed_wave >= view.wave &&
            physical_targets() == 79 && !physical_hook(0)) {
            puts("selective_exhaustion hot_attached=0 cold_attached=79");
            return 0;
        }
    } while (now() < deadline);
    return 1;
}

int main(int argc, char **argv)
{
    physical_targets = dlsym(RTLD_DEFAULT, "peak_general_listener_test_physical_targets");
    physical_hook = dlsym(RTLD_DEFAULT, "peak_general_listener_test_hook_physical");
    target_calls = dlsym(RTLD_DEFAULT, "peak_general_listener_test_all_target_calls");
    coordinator_active = dlsym(RTLD_DEFAULT, "peak_general_listener_test_global_active");
    budget_view = dlsym(RTLD_DEFAULT, "peak_general_listener_test_global_view");
    if (!physical_targets || !physical_hook || !target_calls || !coordinator_active || !budget_view || argc != 2)
        return 1;
    /* Standard installation precedes MPI worker creation. The finite initial
     * window records real enter/leave data and obeys the same affine oracle. */
    if (physical_targets() != 80 || target_calls() != 0 || !cold_targets_attached()) return 2;
    uint64_t initial = 1, expected_initial = 1;
    for (unsigned i = 0; i < 64; ++i) {
        initial = budget_target_0(initial);
        expected_initial = A * expected_initial + B;
    }
    if (initial != expected_initial || target_calls() != 64) return 2;
    printf("initial_observation calls=64 checksum=%llu\n", (unsigned long long)initial);
    int provided, rank;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (provided < MPI_THREAD_FUNNELED || !coordinator_active())
        MPI_Abort(MPI_COMM_WORLD, 3);
    int bad = 0;
    if (!strcmp(argv[1], "early-finalize")) {
        MPI_Barrier(MPI_COMM_WORLD);
        if (!rank) {
            puts("root_enter_finalize=1");
            MPI_Finalize();
            return 0;
        }
        bad = wait_physical(0, 8) || work(.1, 1) || work(.1, 0);
    } else if (!strcmp(argv[1], "recovery")) {
        bad = wait_physical(80, 8);
        if (target_calls() != 64 || !cold_targets_attached()) bad = 1;
        int exhausted = exhaust_credit();
        int restored = wait_physical(80, 8);
        int sampled = work(.05, 1);
        printf("stages initial=%d exhausted=%d restored=%d sampled=%d\n", bad, exhausted, restored, sampled);
        bad |= exhausted || restored || sampled;
        PeakGlobalBudgetView view = budget_view();
        printf("final wave=%llu completed=%llu calls=%llu\n",
               (unsigned long long)view.wave, (unsigned long long)view.root_completed_wave,
               (unsigned long long)target_calls());
        if (!view.root_completed_wave || !target_calls())
            bad = 1;
    } else {
        bad = 1;
    }
    printf("mpi_budget rank=%d result=%s\n", rank, bad ? "FAIL" : "PASS");
    MPI_Finalize();
    return bad ? 1 : 0;
}
