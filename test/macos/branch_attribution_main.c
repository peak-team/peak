#include <stdio.h>

unsigned long peak_macos_branch_target(unsigned long value);
unsigned long peak_macos_smoke_target(unsigned long value);

int
main(void)
{
    const unsigned long branch_calls = 7;
    const unsigned long direct_calls = 11;
    volatile unsigned long result = 0;
    unsigned long expected = 0;

    for (unsigned long i = 0; i < branch_calls; i++) {
        result += peak_macos_branch_target(i);
        expected += i * 3 + 1;
    }
    for (unsigned long i = 0; i < direct_calls; i++) {
        const unsigned long value = i + branch_calls;

        result += peak_macos_smoke_target(value);
        expected += value * 3 + 1;
    }

    printf("macos_branch_calls=%lu direct_calls=%lu result=%lu\n",
           branch_calls,
           direct_calls,
           result);
    return result == expected ? 0 : 1;
}
