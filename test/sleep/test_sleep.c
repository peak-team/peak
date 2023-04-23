#include <stdio.h>
#include <stdlib.h>
#include <time.h>

__attribute__((noinline)) float my_sleep_func()
{
    struct timespec ts = { 0, 10000000 }; // Sleep for 0.01 seconds (10,000,000 nanoseconds)
    nanosleep(&ts, NULL);
    return 0.01;
}

int main()
{
    float total = 0.0;
#pragma omp parallel for reduction(+ : total)
    for (int i = 0; i < 1000; i++) {
        total += my_sleep_func();
    }
    printf("Sleep is done: %f\n", total);
    return 0;
}
