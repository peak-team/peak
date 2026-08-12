#include <stdio.h>
#include <unistd.h>

unsigned long peak_macos_smoke_target(unsigned long value);

int
main(void)
{
    volatile unsigned long result = 0;
    unsigned long expected = 0;

    /* Cross the explicit detach threshold, then leave enough observation
     * windows for physical detach and conservative retry-based reattach. */
    for (unsigned long i = 0; i < 10; i++) {
        result += peak_macos_smoke_target(i);
        expected += i * 3 + 1;
    }
    for (unsigned long i = 10; i < 3010; i++) {
        result += peak_macos_smoke_target(i);
        expected += i * 3 + 1;
        usleep(1000);
    }

    printf("macos_detach_result=%lu\n", result);
    return result == expected ? 0 : 1;
}
