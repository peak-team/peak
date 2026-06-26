#include <stdint.h>
#include <stdio.h>

static volatile uint64_t preload_target_calls;

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_PRELOAD_TEST_EXPORT __attribute__((visibility("default")))
#else
#define PEAK_PRELOAD_TEST_EXPORT
#endif

PEAK_PRELOAD_TEST_EXPORT __attribute__((noinline, noclone))
void
peak_shutdown_preload_target(void)
{
    preload_target_calls++;
#if defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__)
    asm volatile(
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t"
        "nop\n\t" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

int
main(void)
{
    for (int i = 0; i < 8; i++) {
        peak_shutdown_preload_target();
    }
    if (preload_target_calls != 8) {
        fprintf(stderr,
                "FAIL: expected 8 target calls, got %llu\n",
                (unsigned long long)preload_target_calls);
        return 1;
    }
    fprintf(stderr, "peak-shutdown-preload-main-ok\n");
    return 0;
}
