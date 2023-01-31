//gcc -shared -fPIC -o libfakeintel.so fakeintel.c
//icc -shared -fPIC -o libfakeintel.so fakeintel.c -qmkl
//


#include "libperf.h"
#include "hash.h"
#include <stdlib.h>     /* atexit */

// global 
bool libperf_init_flag=false;
int libperf_mkl_fake=-1;
int libperf_debug=0;
double apptime;

#ifdef _OPENMP
  omp_lock_t lock;
#endif

/*
int mkl_serv_intel_cpu_true() {
   int (*fp)()=NULL;
   
   if (libperf_mkl_fake==1) return 1;
   if (libperf_mkl_fake==0) return 0; 
   
   fp=dlsym(RTLD_NEXT, __func__);
   return fp();
}
*/



void env_get()
{
   char* myenv;

   myenv = getenv("LIBPERF_MKL_FAKE");
   libperf_mkl_fake = myenv? atoi(myenv) : -1;        
  
   myenv = getenv("LIBPERF_DEBUG");
   libperf_debug = myenv? atoi(myenv) : 0 ;        

   return ;
}

void env_show()
{
   fprintf(stdout, "------------------ BLAS/LAPACK Perf ENV -------------------\n");
   fprintf(stdout, "LIBPERF_MKL_FAKE = %d \n",libperf_mkl_fake); 
   fprintf(stdout, "LIBPERF_DEBUG = %d \n",libperf_debug);

   //fprintf(stderr,"*******************************\n");

   return ;
}

 void libperf_finalize(){
#ifdef _OPENMP
  omp_destroy_lock(&lock);
#pragma omp master
#endif
  {
    apptime = mysecond()-apptime;
    fprintf(stdout,"\n"); 
    fprintf(stdout, "-------------------- BLAS/LAPACK Perf ---------------------\n");
    fprintf(stdout,"Total runtime: %.3f\n",apptime);
    env_show();
    hash_show();
    fprintf(stdout,"\n"); 
  }
  //fclose(bpfile);
}

void blas_init(){
  libperf_init_flag=true;
  env_get();
  atexit(libperf_finalize);
#ifdef _OPENMP
  omp_init_lock(&lock);
#endif
// bpfile = fopen("libperf.log");
  return;
}

void __attribute__ ((constructor)) premain()
{

   apptime = mysecond();
   fprintf(stdout, "----------------------------------------------------\n");
   fprintf(stdout, "            Using BLAS/LAPACK Perf Library\n");
   fprintf(stdout, "----------------------------------------------------\n");
}
