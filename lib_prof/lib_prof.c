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

// environmental variables
 int peakprof_debug=0;
 int peakprof_mkl_fake=-1;
 int peakprof_record_rank=0;
 double peakprof_record_threshold=-0.001;

//local
 char *argv0;



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

   myenv = getenv("PEAKPROF_RECORD_THRESHOLD");
   peakprof_record_threshold = myenv? atof(myenv) : -0.001 ;        

   return ;
}

void env_show()
{
   fprintf(OUTFILE, "environmental variables:\n");
//   fprintf(OUTFILE, "PEAKPROF_MKL_FAKE = %d \n",peakprof_mkl_fake); 
   fprintf(OUTFILE, "    PEAKPROF_DEBUG=%d \n",peakprof_debug);
   fprintf(OUTFILE, "    PEAKPROF_RECORD_RANK=%d \n",peakprof_record_rank);
   fprintf(OUTFILE, "    PEAKPROF_RECORD_THRESHOLD=%.3f \n",peakprof_record_threshold);

   return ;
}

int MPI_Finalize(void) {
    printf("--- My Final ---\n");
    return 0;
}

void  MPI_Finalize_(int *ierr) {
    printf("--- My Final_ ---\n");
    ierr=0;
    return ;
}

/* somehow causes crash
void  mpi_finalize_f08_(int *ierr) {
    printf("--- my final_f08_ ---\n");
    ierr=0;
    return ;
}
*/

int (*original_pmpi_finalize)(void)=NULL;
int peak_done = 0;
int PMPI_Finalize(void) {
//    printf("--- My PFinal ---\n");
    if (!original_pmpi_finalize) {
        original_pmpi_finalize = dlsym(RTLD_NEXT, "PMPI_Finalize");
    }   
    return 0;
}


void print_result() {

    struct item* farray=NULL;
    int fn = hash_get_size();
    if (fn == 0) return;   //nothing profiled
    farray=hash_to_array();

    fprintf(OUTFILE,"\n"); 
    fprintf(OUTFILE, "        ----------------------------------------------------\n");
    fprintf(OUTFILE, "                         PEAK Prof Library\n");
    fprintf(OUTFILE, "        ----------------------------------------------------\n");
    fprintf(OUTFILE, "for application: %s\n\n",argv0);
//   fprintf(OUTFILE,"----------------------------- PEAK Prof -------------------------------\n");
    fprintf(OUTFILE,"total runtime: %.3fs, library time: %.3fs, percentage of lib: %.1f%\n\n",apptime, libtime, libtime/apptime*100);
//   fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    env_show();
     // hash_show_final();

// direct call 
    qsort(farray, fn, sizeof(struct item), compare_time_di);
    fprintf(OUTFILE,"\n----------------------  function statistics (direct) --------------------\n");
    fprintf(OUTFILE,"    direct call time (in seconds) and counts\n");
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    for (int i=0; i<fn; i++) {
       if(farray[i].value.count_di>0)
         if(farray[i].value.time_di > peakprof_record_threshold)
             fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", farray[i].value.fgroup, farray[i].key, farray[i].value.count_di, farray[i].value.time_di);
    }
    fprintf(OUTFILE,"%62s %10.3f\n","------------------------------------------ total library time:", libtime);
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");

// exlusive time
    qsort(farray, fn, sizeof(struct item), compare_time_ex);
    fprintf(OUTFILE,"\n-------------------  function statistics (exclusive) --------------------\n");
    fprintf(OUTFILE,"    exclusive call time (in seconds) and counts\n");
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    for (int i=0; i<fn; i++) {
         if(farray[i].value.time_ex > peakprof_record_threshold)
             fprintf(OUTFILE,"group: %10s, function: %10s, count: %7d, time: %10.3f\n", farray[i].value.fgroup, farray[i].key, farray[i].value.count, farray[i].value.time_ex);
    }
    fprintf(OUTFILE,"%62s %10.3f\n","------------------------------------------ total library time:", libtime);
    fprintf(OUTFILE,"-------------------------------------------------------------------------\n");
    fprintf(OUTFILE,"\n"); 
   
    fflush(OUTFILE); 
    free(farray);
    return;
}

void reduce_result() {
    int my_rank_id, my_rank_size;
    if (!original_pmpi_finalize) PMPI_Finalize(); //register original_pmpi_finalize

      int init_flag;
      MPI_Initialized(&init_flag);
      if(!init_flag) 
              MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank_id);
    MPI_Comm_size(MPI_COMM_WORLD, &my_rank_size);

//  MPI_Reduce(values, &sum_values, NUM_COUNTERS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
//  MPI_Reduce(values_uc, &sum_values_uc, NUM_COUNTERS_UC*SOCKETS, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    // only prints designated rank. no reduction yet. 

    if (my_rank_id == peakprof_record_rank) {
        print_result();
    }
    original_pmpi_finalize();
    return;
}

void libprof_fini(){
   apptime = mysecond()-apptime;
   if (check_MPI())  {
      reduce_result(); 
   }
   else 
       print_result();
}

void libprof_init(){

/*  need to print in MPI_INIT in MPI, or here for serial
   fprintf(OUTFILE, "        ----------------------------------------------------\n");
   fprintf(OUTFILE, "                    Starting PEAK Prof Library\n");
   fprintf(OUTFILE, "        ----------------------------------------------------\n");
*/
   peakprof_init_flag=true;
   layer_count=0; 
   memset(layer_time, 0, MAX_LAYER*sizeof(layer_time[0]));
   env_get();
   get_argv0(&argv0);

   apptime = mysecond();
  return;
}

//__attribute__((section(".init_array"))) void *__init = libprof_init;
//__attribute__((section(".fini_array"))) void *__fini = libprof_fini;

  void libprof_init() __attribute__((constructor));
  void libprof_fini() __attribute__((destructor));

