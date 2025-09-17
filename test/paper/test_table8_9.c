#include <stdio.h>
#include <stdlib.h>
#include "test_cblas.h"

#define N 80

__attribute__((noinline, used, visibility("default")))
void dgemm_wrapper(int M, int NN, int K,
                   double alpha, double beta,
                   double *A, double *B, double *C) {
    cblas_dgemm(
        CblasRowMajor,    // Use row-major layout
        CblasNoTrans,     // A not transposed
        CblasNoTrans,     // B not transposed
        M, NN, K,
        alpha,
        A, K,
        B, NN,
        beta,
        C, NN
    );
}

int main() {
    int i;

    double* A = (double*)malloc(N * N * sizeof(double));
    double* B = (double*)malloc(N * N * sizeof(double));
    double* C = (double*)malloc(N * N * sizeof(double));

    // 初始化
    for (i = 0; i < N * N; i++) {
        A[i] = 1.0;
        B[i] = 1.0;
    }

    double checksum = 0.0;

    for (i = 0; i < 2000000; i++) {
        A[i % (N * N)] += 0.0001;
        dgemm_wrapper(N, N, N, 1.0, 0.0, A, B, C);
        checksum += C[0];

        printf("Run %d: C[0]=%f\n", i, C[0]);
    }

    printf("Checksum: %f\n", checksum);

    free(A);
    free(B);
    free(C);
    return 0;
}
