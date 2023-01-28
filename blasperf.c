//gcc -shared -fPIC -o libfakeintel.so fakeintel.c
//icc -shared -fPIC -o libfakeintel.so fakeintel.c -qmkl
//


#include "blasperf.h"
#include "hash.h"
#include <stdlib.h>     /* atexit */

// global 
bool blasperf_init_flag=false;
int blasperf_mkl_fake=-1;
int blasperf_debug=0;
double apptime;

#ifdef _OPENMP
  omp_lock_t lock;
#endif

/*
int mkl_serv_intel_cpu_true() {
   int (*fp)()=NULL;
   
   if (blasperf_mkl_fake==1) return 1;
   if (blasperf_mkl_fake==0) return 0; 
   
   fp=dlsym(RTLD_NEXT, __func__);
   return fp();
}
*/



void env_get()
{
   char* myenv;

   myenv = getenv("BLASPERF_MKL_FAKE");
   blasperf_mkl_fake = myenv? atoi(myenv) : -1;        
  
   myenv = getenv("BLASPERF_DEBUG");
   blasperf_debug = myenv? atoi(myenv) : 0 ;        

   return ;
}

void env_show()
{
   fprintf(stdout, "------------------ BLAS Perf ENV -------------------\n");
   fprintf(stdout, "BLASPERF_MKL_FAKE = %d \n",blasperf_mkl_fake); 
   fprintf(stdout, "BLASPERF_DEBUG = %d \n",blasperf_debug);

   //fprintf(stderr,"*******************************\n");

   return ;
}

 void blas_finalize(){
#ifdef _OPENMP
  omp_destroy_lock(&lock);
#pragma omp master
#endif
  {
    apptime = mysecond()-apptime;
    fprintf(stdout,"\n"); 
    fprintf(stdout, "-------------------- BLAS Perf ---------------------\n");
    fprintf(stdout,"Total runtime: %.3f\n",apptime);
    env_show();
    hash_show();
    fprintf(stdout,"\n"); 
  }
  //fclose(bpfile);
}

void blas_init(){
  blasperf_init_flag=true;
  env_get();
  atexit(blas_finalize);
#ifdef _OPENMP
  omp_init_lock(&lock);
#endif
// bpfile = fopen("blasperf.log");
  return;
}

void __attribute__ ((constructor)) premain()
{

   apptime = mysecond();
   fprintf(stdout, "----------------------------------------------------\n");
   fprintf(stdout, "            Using BLAS Perf Library\n");
   fprintf(stdout, "----------------------------------------------------\n");
}
