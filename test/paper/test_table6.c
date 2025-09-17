#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include "test_cblas.h"

#define N 80
int main(void) {
    const int num_threads = 28;
    const long iters_per_thread = 3000000L;
    const size_t NN = (size_t)N * (size_t)N;

    double *B = (double*) malloc(NN * sizeof(double));
    if (!B) { fprintf(stderr, "malloc B failed\n"); return 1; }
    for (size_t i = 0; i < NN; ++i) B[i] = 1.0;

    double global_checksum = 0.0;

    omp_set_num_threads(num_threads);

    #pragma omp parallel reduction(+:global_checksum)
    {
        int tid = omp_get_thread_num();

        double *A = (double*) malloc(NN * sizeof(double));
        double *C = (double*) malloc(NN * sizeof(double));
        if (!A || !C) {
            fprintf(stderr, "Thread %d: malloc A/C failed\n", tid);
            if (A) free(A);
            if (C) free(C);
        } else {
            for (size_t i = 0; i < NN; ++i) {
                A[i] = 1.0;
                C[i] = 0.0;
            }

            double local_checksum = 0.0;

            for (long it = 0; it < iters_per_thread; ++it) {
                size_t idx = (size_t)it % NN;
                A[idx] += 0.0001;

                cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                            N, N, N, 1.0, A, N, B, N, 0.0, C, N);
                local_checksum += C[0];
            }

            printf("Thread %02d done. local_checksum=%f\n", tid, local_checksum);
            global_checksum += local_checksum;

            free(A);
            free(C);
        }
    }

    printf("Global checksum: %f\n", global_checksum);

    free(B);
    return 0;
}
