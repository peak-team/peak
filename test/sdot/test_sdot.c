#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
    sleep(1);
    return x[N - 1] + y[N - 1];
}

int main()
{
    float x[N], y[N];
    for (int i = 0; i < N; i++) {
        x[i] = i;
        y[i] = i + 1;
    }
    float dot_product;
#pragma omp parallel for
    for (int i = 0; i < 1000000; i++) {
        dot_product = cblas_sdot(N, x, 1, y, 1);
    }
#pragma omp parallel for
    for (int i = 0; i < 56; i++) {
        my_sleep_func();
    }
    printf("Dot product: %f\n", dot_product);
    return 0;
}
