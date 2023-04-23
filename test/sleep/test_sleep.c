#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "test_cblas.h"

#define N 80

float my_sleep_func()
{
    float x[N], y[N];
    for (int i = 0; i < N; i++) {
        x[i] = i;
        y[i] = i + 1;
    }
    // printf("y[N]: %f\n", y[N-1]);
    if(x[N-1] + y[N-1]) {
        struct timespec ts = { 0, 10000000 }; // Sleep for 0.01 seconds (1,000,000 nanoseconds)
        nanosleep(&ts, NULL);
    }
    return 0.01;
}

int main()
{
    float total = 0.0;
#pragma omp parallel for reduction(+:total)
    for (int i = 0; i < 1000; i++) {
        total+=my_sleep_func();
    }
    printf("Sleep is done: %f\n", total);
    return 0;
}
