#include <mkl.h>
#include <stdio.h>

int main() {
    int m = 2000, n = 3000, k = 400;
    double alpha = 1.0, beta = 0.0;
    double A[m * k], B[k * n], C[m * n];

    // Initialize matrices A and B with some values
    for (int i = 0; i < m * k; i++) A[i] = i;
    for (int i = 0; i < k * n; i++) B[i] = i;

    // Perform matrix multiplication C = alpha * A * B + beta * C
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 
                m, n, k, alpha, A, k, B, n, beta, C, n);

    // Print the result matrix C
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            printf("%f ", C[i * n + j]);
        }
        printf("\n");
    }

    return 0;
}

