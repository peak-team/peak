

void descinit_(int *desc, const int *m, const int *n, const int *mb, const int *nb, const int *irsrc, const int *icsrc, const int *ictxt, const int *lld, int *info)
{
 //printf ("descinit -- matrix size: %dx%d  block size: %dx%d \n", *m, *n, *mb, *nb);
  void (*orig_f)()=NULL;
#include "function_wrapper_body1.c" 
  orig_f(desc, m, n, mb, nb, irsrc, icsrc, ictxt, lld, info);
#include "function_wrapper_body2.c" 
  return;
}

int numroc (const int *n, const int *nb, const int *iproc, const int *srcproc, const int *nprocs)
{
  int (*orig_f)()=NULL;
  int result;
#include "function_wrapper_body1.c" 
  result=orig_f (n, nb, iproc, srcproc, nprocs);
#include "function_wrapper_body2.c" 
  return result;
}
