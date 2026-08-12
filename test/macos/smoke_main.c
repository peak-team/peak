#include <stdio.h>

unsigned long peak_macos_smoke_target(unsigned long value);

int
main(void)
{
    volatile unsigned long result = 0;

    for (unsigned long i = 0; i < 7; i++) {
        result += peak_macos_smoke_target(i);
    }

    printf("macos_smoke_result=%lu\n", result);
    return result == 70 ? 0 : 1;
}
