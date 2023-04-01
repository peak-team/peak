

#define _GNU_SOURCE 

#include <stdio.h> 
#include <dlfcn.h>
#include <stdlib.h>


void dgemm_(const char *transa, const char *transb, const int *m, const int *n, const int *k, 
                const double *alpha, const double *a, const int *lda, const double *b, const int *ldb, 
                const double *beta, double *c, const int *ldc) 
{
   void (*orig_f)()=NULL;
   orig_f = dlsym(RTLD_NEXT, __func__);

   orig_f(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
   return;
}

