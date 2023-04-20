#include <stdio.h>
#include <stdlib.h>
#include <test_cblas.h>

#define N 80

int main() {
    float x[N], y[N];
    for (int i = 0; i < N; i++) {
        x[i] = i;
        y[i] = i + 1;
    }
    float dot_product;
    for (int i = 0; i < 10000; i++) {
        dot_product = cblas_sdot(N, x, 1, y, 1);
    }
    printf("Dot product: %f\n", dot_product);
    return 0;
}
