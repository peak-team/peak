#include <stdio.h>
#include <stdlib.h>
#include <time.h>

__attribute__((noinline, used, visibility("default")))
float my_sleep_func(void)
{
    static int initialized = 0;
    if (!initialized) {
        srand((unsigned)time(NULL));
        initialized = 1;
    }
    return (float)rand() / (float)RAND_MAX;
}

int main()
{
    float total = 0.0;
    for (int i = 0; i < 10000; ++i) {
        total += my_sleep_func();
    }
    printf("Sleep is done: %f\n", total);

    return 0;
}