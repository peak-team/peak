#include <stdio.h>
#include <stdlib.h>
#include "test_cblas.h"

#define N 80

int main() {
    int i;

    double* A = (double*)malloc(N * N * sizeof(double));
    double* B = (double*)malloc(N * N * sizeof(double));
    double* C = (double*)malloc(N * N * sizeof(double));

    for (i = 0; i < N * N; i++) {
        A[i] = 1.0;
        B[i] = 1.0;
    }

    double checksum = 0.0;

    for (i = 0; i < 3000000; i++) {
        A[i % (N * N)] += 0.0001;
        cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    N, N, N, 1.0, A, N, B, N, 0.0, C, N);
        checksum += C[0];

        printf("Run %d: C[0]=%f\n", i, C[0]);
    }

    printf("Checksum: %f\n", checksum);

    free(A);
    free(B);
    free(C);
    return 0;
}
