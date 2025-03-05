#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include "test_cblas.h"

#define N 300
static double A[N][N], B[N][N], C[N][N];

int main(int argc, char** argv)
{
    int rank, size;
    int i, j;
    double* D = (double*)malloc(N * N * sizeof(double));
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Initialize matrices A and B
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            A[i][j] = 1.0;
            B[i][j] = 1.0;
        }
    }

// Perform matrix multiplication
#pragma omp parallel for
    for (i = 0; i < 10000; i++) {
        //C[i][j] += A[i][k] * B[k][j];
        cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, N, N, N, 1.0, &A[0][0], N, &B[0][0], N, 0.0, &C[0][0], N);
    }
    if (rank == 0)
        system("echo hello!");

    // Perform all-reduce operation to sum the C matrix across all processes
    MPI_Allreduce(C, D, N * N, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0)
        printf("Test is done, D[0][0] = %f\n", D[0]);
    free(D);
    MPI_Finalize();
    return 0;
}
