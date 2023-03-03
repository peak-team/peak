#include "lib_prof.h"
#include "hash.h"
#include <stdlib.h>     /* atexit */
#include <mpi.h>

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

int MPI_Finalize(void) {
    //printf("--- My Final ---\n");
    return 0;
}


void print_result() {

    struct item* farray=NULL;
    int fn = hash_get_size();
    if (fn == 0) return;
    farray=hash_to_array();

      fprintf(OUTFILE,"\n"); 
      fprintf(OUTFILE, "        ----------------------------------------------------\n");
      fprintf(OUTFILE, "                         PEAK Prof Library\n");
      fprintf(OUTFILE, "        ----------------------------------------------------\n");
      fprintf(OUTFILE,"----------------------------- PEAK Prof -------------------------------\n");
      fprintf(OUTFILE,"total runtime: %.3fs, library time: %.3fs, percentage of lib: %.1f%\n",apptime, libtime, libtime/apptime*100);
      env_show();
      hash_show_final();
    for (int i=0; i<fn; i++) {
         fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", farray[i].value.fgroup, farray[i].key, farray[i].value.count, farray[i].value.time_in);
    }
      fprintf(OUTFILE,"\n"); 
      return;
}

void reduce_result() {
    int my_rank_id, my_rank_size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if(!init_flag) 
            MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank_id);
    MPI_Comm_size(MPI_COMM_WORLD, &my_rank_size);

//  MPI_Reduce(values, &sum_values, NUM_COUNTERS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
//  MPI_Reduce(values_uc, &sum_values_uc, NUM_COUNTERS_UC*SOCKETS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    struct item* farray=NULL;
    int fn = hash_get_size();
    farray=hash_to_array();

    if (my_rank_id == 0) {


    } 
    else {
      
    }
   
    if (my_rank_id == 0) {
        print_result();
    }
    PMPI_Finalize();
}


void libprof_fini(){
   apptime = mysecond()-apptime;
   if (check_MPI()) 
       reduce_result(); 
   else 
       print_result();
}

void libprof_init(){

   apptime = mysecond();
/*  need to print in MPI_INIT in MPI, or here for serial
   fprintf(OUTFILE, "        ----------------------------------------------------\n");
   fprintf(OUTFILE, "                    Starting PEAK Prof Library\n");
   fprintf(OUTFILE, "        ----------------------------------------------------\n");
*/
   peakprof_init_flag=true;
   layer_count=0; 
   memset(layer_time, 0, MAX_LAYER*sizeof(layer_time[0]));
   env_get();
  return;
}

__attribute__((section(".init_array"))) void *__init = libprof_init;
__attribute__((section(".fini_array"))) void *__fini = libprof_fini;

//void libprof_init() __attribute__((constructor));
//void libprof_fini() __attribute__((destructor));

