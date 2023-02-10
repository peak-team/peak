#include "lib_prof.h"
#include "hash.h"
#include <stdlib.h>     /* atexit */

// global 
 bool peakprof_init_flag=false;
 double apptime=0.0;
 double libtime=0.0;
 double layer_time[MAX_LAYER];
 int layer_count;
 int my_rank_id;
 int my_rank_size; 

// environmental variables
 int peakprof_debug=0;
 int peakprof_mkl_fake=-1;
 int peakprof_record_rank=0;

#ifdef _OPENMP
  omp_lock_t lock;
#endif

/*
int mkl_serv_intel_cpu_true() {
   int (*fp)()=NULL;
   
   if (peakprof_mkl_fake==1) return 1;
   if (peakprof_mkl_fake==0) return 0; 
   
   fp=dlsym(RTLD_NEXT, __func__);
   return fp();
}
*/


void env_get()
{
   char* myenv;

   myenv = getenv("PEAKPROF_MKL_FAKE");
   peakprof_mkl_fake = myenv? atoi(myenv) : -1;        
  
   myenv = getenv("PEAKPROF_DEBUG");
   peakprof_debug = myenv? atoi(myenv) : 0 ;        

   myenv = getenv("PEAKPROF_RECORD_RANK");
   peakprof_record_rank = myenv? atoi(myenv) : 0 ;        

   return ;
}

void env_show()
{
   fprintf(OUTFILE, "environmental variables:\n");
//   fprintf(OUTFILE, "PEAKPROF_MKL_FAKE = %d \n",peakprof_mkl_fake); 
   fprintf(OUTFILE, "    PEAKPROF_DEBUG = %d \n",peakprof_debug);


   return ;
}

 void libprof_finalize(){
#ifdef _OPENMP
//  omp_destroy_lock(&lock);
#pragma omp master
#endif
  {
    apptime = mysecond()-apptime;
    fprintf(OUTFILE,"\n"); 

    fprintf(OUTFILE, "        ----------------------------------------------------\n");
    fprintf(OUTFILE, "                         PEAK Prof Library\n");
    fprintf(OUTFILE, "        ----------------------------------------------------\n");
    fprintf(OUTFILE,"----------------------------- PEAK Prof -------------------------------\n");
    fprintf(OUTFILE,"total runtime: %.3fs, library time: %.3fs, percentage of lib: %.1f%\n",apptime, libtime, libtime/apptime*100);
    env_show();
    hash_show_final();
    fprintf(OUTFILE,"\n"); 
  }
  //fclose(bpfile);
}

void libprof_init(){


   apptime = mysecond();
   fprintf(OUTFILE, "        ----------------------------------------------------\n");
   fprintf(OUTFILE, "                    Starting PEAK Prof Library\n");
   fprintf(OUTFILE, "        ----------------------------------------------------\n");

   peakprof_init_flag=true;
   layer_count=0; 
   memset(layer_time, 0, MAX_LAYER*sizeof(layer_time[0]));
   env_get();
  //atexit(peakprof_finalize);
#ifdef _OPENMP
//  omp_init_lock(&lock);
#endif
// bpfile = fopen("peakprof.log");
  return;
}


void libprof_init()     __attribute__((constructor));
void libprof_finalize() __attribute__((destructor));

