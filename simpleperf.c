//gcc -shared -fPIC -o libfakeintel.so fakeintel.c
//icc -shared -fPIC -o libfakeintel.so fakeintel.c -qmkl
//


#include "simpleperf.h"
#include "hash.h"
#include <stdlib.h>     /* atexit */

// global 
bool simpleperf_init_flag=false;
int simpleperf_mkl_fake=-1;
int simpleperf_debug=0;
double apptime;

#ifdef _OPENMP
  omp_lock_t lock;
#endif

/*
int mkl_serv_intel_cpu_true() {
   int (*fp)()=NULL;
   
   if (simpleperf_mkl_fake==1) return 1;
   if (simpleperf_mkl_fake==0) return 0; 
   
   fp=dlsym(RTLD_NEXT, __func__);
   return fp();
}
*/



void env_get()
{
   char* myenv;

   myenv = getenv("SIMPLEPERF_MKL_FAKE");
   simpleperf_mkl_fake = myenv? atoi(myenv) : -1;        
  
   myenv = getenv("SIMPLEPERF_DEBUG");
   simpleperf_debug = myenv? atoi(myenv) : 0 ;        

   return ;
}

void env_show()
{
   fprintf(stdout, "------------------ Simple Perf ENV -------------------\n");
   fprintf(stdout, "SIMPLEPERF_MKL_FAKE = %d \n",simpleperf_mkl_fake); 
   fprintf(stdout, "SIMPLEPERF_DEBUG = %d \n",simpleperf_debug);

   //fprintf(stderr,"*******************************\n");

   return ;
}

 void simpleperf_finalize(){
#ifdef _OPENMP
  omp_destroy_lock(&lock);
#pragma omp master
#endif
  {
    apptime = mysecond()-apptime;
    fprintf(stdout,"\n"); 
    fprintf(stdout, "-------------------- Simple Perf ---------------------\n");
    fprintf(stdout,"Total runtime: %.3f\n",apptime);
    env_show();
    hash_show();
    fprintf(stdout,"\n"); 
  }
  //fclose(bpfile);
}

void blas_init(){
  simpleperf_init_flag=true;
  env_get();
  atexit(simpleperf_finalize);
#ifdef _OPENMP
  omp_init_lock(&lock);
#endif
// bpfile = fopen("simpleperf.log");
  return;
}

void __attribute__ ((constructor)) premain()
{

   apptime = mysecond();
   fprintf(stdout, "----------------------------------------------------\n");
   fprintf(stdout, "            Using Simple Perf Library\n");
   fprintf(stdout, "----------------------------------------------------\n");
}
