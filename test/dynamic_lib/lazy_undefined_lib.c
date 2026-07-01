#include <stdint.h>

extern void peak_lazy_missing_dependency(void);

volatile int lazy_profile_sink;

__attribute__((visibility("default"), noinline))
int lazy_profile_target(int value)
{
    int total = value;

    for (int i = 0; i < 8; i++) {
        total += (i * 3) + value;
    }
    lazy_profile_sink += total;
    return lazy_profile_sink;
}

__attribute__((visibility("default"), noinline))
void lazy_never_called_dependency(void)
{
    peak_lazy_missing_dependency();
}
