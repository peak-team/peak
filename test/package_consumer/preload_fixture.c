#include <stdio.h>

static volatile int peak_package_consumer_sink;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, visibility("default")))
#endif
int
peak_package_consumer_target(int value)
{
    peak_package_consumer_sink += value;
    return peak_package_consumer_sink;
}

int
main(void)
{
    for (int value = 1; value <= 7; ++value) {
        (void)peak_package_consumer_target(value);
    }
    puts("peak_package_consumer_preload_ok");
    return peak_package_consumer_sink == 28 ? 0 : 1;
}
