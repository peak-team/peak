#define _GNU_SOURCE
#include <mpi.h>
#include <omp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

/* All arithmetic intentionally modulo 2^64. The compiler must retain each
 * externally visible, independently patchable function and indirect call. */
#define A UINT64_C(6364136223846793005)
#define B UINT64_C(1442695040888963407)
#define TARGET(name,id) __attribute__((noinline,noclone,noipa,used,visibility("default"),patchable_function_entry(16,0))) uint64_t pressure_##name(uint64_t x, unsigned work) { for(unsigned j=0;j<work;j++) x=A*x+B+(id); return x; }
#include "targets.inc"
#undef TARGET
static uint64_t (*const targets[256])(uint64_t,unsigned)={
#define TARGET(name,id) pressure_##name,
#include "targets.inc"
#undef TARGET
};
typedef struct { uint64_t a,b; } Aff;
/* compose f after g */
static Aff compose(Aff f,Aff g) { return (Aff){f.a*g.a,f.a*g.b+f.b}; }
static Aff power(Aff f,uint64_t n) { Aff r={1,0}; while(n){if(n&1)r=compose(f,r);f=compose(f,f);n>>=1;}return r; }
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+1e-9*t.tv_nsec;}
static double thread_cpu(void){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec+1e-9*t.tv_nsec;}
static double process_cpu(void){struct rusage r;getrusage(RUSAGE_SELF,&r);return r.ru_utime.tv_sec+1e-6*r.ru_utime.tv_usec+r.ru_stime.tv_sec+1e-6*r.ru_stime.tv_usec;}
static void stamp(const char *s,int rank){if(!rank){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);printf("timestamp %s unix=%lld.%09ld monotonic=%.9f\n",s,(long long)t.tv_sec,t.tv_nsec,now());fflush(stdout);}}
/* Twins 128..143 have EXACTLY the same affine map as 000..015. Only the
 * selected rank/thread uses the hooked address. Function IDs rotate per sweep. */
static uint64_t oracle(uint64_t x,unsigned id,uint64_t calls,unsigned work) {
 Aff f=power((Aff){A,B+id},calls*(uint64_t)work);return f.a*x+f.b;
}
int main(int argc,char **argv) {
 double startup=now();int rank=0,size,provided;stamp("before_mpi_init",0);
 MPI_Init_thread(&argc,&argv,MPI_THREAD_FUNNELED,&provided);
 MPI_Comm_rank(MPI_COMM_WORLD,&rank);MPI_Comm_size(MPI_COMM_WORLD,&size);stamp("after_mpi_init",rank);
 if(provided<MPI_THREAD_FUNNELED)MPI_Abort(MPI_COMM_WORLD,2);
 uint64_t before=200000,between=150000,rounds=250000;unsigned work=131072,burst_work=512,profile_threads=0;
 for(int i=1;i<argc;i++){
  if(i+1>=argc)MPI_Abort(MPI_COMM_WORLD,2);
  const char *key=argv[i++];char *end;uint64_t n=strtoull(argv[i],&end,10);
  if(!n||*end||argv[i][0]=='-')MPI_Abort(MPI_COMM_WORLD,2);
  if(!strcmp(key,"--native-before-calls"))before=n;
  else if(!strcmp(key,"--native-between-calls"))between=n;
  else if(!strcmp(key,"--burst-rounds"))rounds=n;
  else if(!strcmp(key,"--native-work")&&n<=UINT32_MAX)work=n;
  else if(!strcmp(key,"--burst-work")&&n<=UINT32_MAX)burst_work=n;
  else if(!strcmp(key,"--threads")&&n<=65536)omp_set_num_threads(n);
  else if(!strcmp(key,"--profile-threads")&&n<=65536)profile_threads=n;
  else MPI_Abort(MPI_COMM_WORLD,2);
 }
 if(before>UINT64_MAX/work||between>UINT64_MAX/work||rounds>UINT64_MAX/16)MPI_Abort(MPI_COMM_WORLD,2);
 omp_set_dynamic(0);int nt=omp_get_max_threads();if(!profile_threads)profile_threads=nt;if(profile_threads>(unsigned)nt)MPI_Abort(MPI_COMM_WORLD,2);
 if(before>UINT64_MAX/work/(unsigned)nt/(unsigned)size||between>UINT64_MAX/work/(unsigned)nt/(unsigned)size||rounds>UINT64_MAX/16/burst_work/(unsigned)nt/(unsigned)size)MPI_Abort(MPI_COMM_WORLD,2);
 uint64_t *state=calloc(nt,sizeof(uint64_t)),*expected=calloc(nt,sizeof(uint64_t));cpu_set_t *masks=calloc(nt,sizeof(cpu_set_t));
 if(!state||!expected||!masks)MPI_Abort(MPI_COMM_WORLD,2);
 for(int t=0;t<nt;t++)state[t]=expected[t]=UINT64_C(0x12345678)+(unsigned)rank*100003u+t;
 int affinity_bad=0;
 #pragma omp parallel
 {int tid=omp_get_thread_num();CPU_ZERO(&masks[tid]);if(sched_getaffinity(0,sizeof(cpu_set_t),&masks[tid])){
 #pragma omp atomic write
 affinity_bad=1;}}
 if(affinity_bad)MPI_Abort(MPI_COMM_WORLD,2);
 char hostname[256];gethostname(hostname,sizeof(hostname));
 char binding[8192];int used=snprintf(binding,sizeof(binding),"affinity rank=%d host=%s threads=%d",rank,hostname,nt);
 for(int t=0;t<nt;t++){used+=snprintf(binding+used,sizeof(binding)-used," t%d=[",t);int first=1;for(int c=0;c<CPU_SETSIZE;c++)if(CPU_ISSET(c,&masks[t])){if(used>8000)MPI_Abort(MPI_COMM_WORLD,2);used+=snprintf(binding+used,sizeof(binding)-used,"%s%d",first?"":",",c);first=0;}used+=snprintf(binding+used,sizeof(binding)-used,"]");}
 char *bindings=rank==0?calloc(size,sizeof(binding)):NULL;MPI_Gather(binding,sizeof(binding),MPI_CHAR,bindings,sizeof(binding),MPI_CHAR,0,MPI_COMM_WORLD);
 if(!rank){for(int r=0;r<size;r++)printf("%s\n",bindings+r*sizeof(binding));free(bindings);printf("recovery_scenario name=earned-recovery-long sync=barrier layout=all-ranks\n");}free(masks);

 if(!rank){printf("recovery_config ranks=%d threads=%d profile_threads=%u native_before_calls=%llu native_between_calls=%llu native_work=%u burst_rounds=%llu burst_work=%u targets=16 exported_targets=256\n",size,nt,profile_threads,(unsigned long long)before,(unsigned long long)between,work,(unsigned long long)rounds,burst_work);fflush(stdout);}
 const char *names[]={"native_before","burst_before","native_between","burst_after"};int failures=0;
 for(unsigned stage=0;stage<4;stage++){
  int burst=stage%2;uint64_t calls=burst?rounds*16:(stage==0?before:between);unsigned w=burst?burst_work:work;
  double *compute=calloc(nt,sizeof(double)),*cpu=calloc(nt,sizeof(double));uint64_t *records=calloc(nt*5,sizeof(uint64_t));
  if(!compute||!cpu||!records)MPI_Abort(MPI_COMM_WORLD,2);
  MPI_Barrier(MPI_COMM_WORLD);double begin=now(),pc=process_cpu();
  #pragma omp parallel
  {int tid=omp_get_thread_num();uint64_t x=state[tid];double c0=thread_cpu(),w0=now();
   if(burst){unsigned offset=(unsigned)tid<profile_threads?0:128;for(uint64_t j=0;j<rounds;j++)for(unsigned id=0;id<16;id++)x=targets[offset+id](x,w);}
   else for(uint64_t j=0;j<calls;j++)x=targets[128](x,w);
   compute[tid]=now()-w0;cpu[tid]=thread_cpu()-c0;state[tid]=x;
  }
  double wait_begin=now();MPI_Barrier(MPI_COMM_WORLD);double completion_wait=now()-wait_begin,wall=now()-begin,pc_delta=process_cpu()-pc;
  int bad=0,totalbad;uint64_t checksum=0,totalcheck;
  Aff cycle={1,0};if(burst)for(unsigned id=0;id<16;id++)cycle=compose(power((Aff){A,B+id},w),cycle);
  for(int t=0;t<nt;t++){
   if(burst){Aff f=power(cycle,rounds);expected[t]=f.a*expected[t]+f.b;}else expected[t]=oracle(expected[t],0,calls,w);
   bad+=state[t]!=expected[t];checksum^=state[t];records[t*5]=state[t];records[t*5+1]=expected[t];records[t*5+2]=calls;records[t*5+3]=calls*w;records[t*5+4]=burst&&(unsigned)t<profile_threads?calls:0;
  }
  MPI_Allreduce(&bad,&totalbad,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);MPI_Reduce(&checksum,&totalcheck,1,MPI_UINT64_T,MPI_BXOR,0,MPI_COMM_WORLD);failures+=totalbad;
  uint64_t *all=!rank?calloc((size_t)size*nt*5,sizeof(uint64_t)):NULL;double *all_compute=!rank?calloc((size_t)size*nt,sizeof(double)):NULL,*all_cpu=!rank?calloc((size_t)size*nt,sizeof(double)):NULL;
  if(!rank&&(!all||!all_compute||!all_cpu))MPI_Abort(MPI_COMM_WORLD,2);
  MPI_Gather(records,nt*5,MPI_UINT64_T,all,nt*5,MPI_UINT64_T,0,MPI_COMM_WORLD);MPI_Gather(compute,nt,MPI_DOUBLE,all_compute,nt,MPI_DOUBLE,0,MPI_COMM_WORLD);MPI_Gather(cpu,nt,MPI_DOUBLE,all_cpu,nt,MPI_DOUBLE,0,MPI_COMM_WORLD);
  if(!rank){printf("recovery_stage stage=%u name=%s root_wall=%.9f oracle_bad=%d checksum=%016llx total_calls=%llu total_steps=%llu potential_profiled_calls=%llu\n",stage,names[stage],wall,totalbad,(unsigned long long)totalcheck,(unsigned long long)(calls*nt*size),(unsigned long long)(calls*w*nt*size),(unsigned long long)(burst?calls*profile_threads*size:0));
   for(int r=0;r<size;r++)for(int t=0;t<nt;t++){size_t i=(size_t)r*nt+t;printf("recovery_thread stage=%u rank=%d tid=%d checksum=%016llx oracle=%016llx calls=%llu lcg_steps=%llu potential_profiled_calls=%llu compute=%.9f cpu=%.9f\n",stage,r,t,(unsigned long long)all[i*5],(unsigned long long)all[i*5+1],(unsigned long long)all[i*5+2],(unsigned long long)all[i*5+3],(unsigned long long)all[i*5+4],all_compute[i],all_cpu[i]);}
  }
  double metric[]={wall,completion_wait,pc_delta};double *metrics=!rank?calloc(size*3,sizeof(double)):NULL;MPI_Gather(metric,3,MPI_DOUBLE,metrics,3,MPI_DOUBLE,0,MPI_COMM_WORLD);
  if(!rank){for(int r=0;r<size;r++)printf("recovery_rank stage=%u rank=%d wall=%.9f completion_wait=%.9f process_cpu=%.9f\n",stage,r,metrics[r*3],metrics[r*3+1],metrics[r*3+2]);fflush(stdout);}
  free(metrics);free(all);free(all_compute);free(all_cpu);free(records);free(compute);free(cpu);
 }
 free(state);free(expected);stamp("before_mpi_finalize",rank);MPI_Finalize();stamp("after_mpi_finalize",rank);
 struct timespec end={0};if(clock_gettime(CLOCK_MONOTONIC,&end))return 2;double finish=end.tv_sec+1e-9*end.tv_nsec;
 if(!rank)printf("process_seconds=%.6f oracle=%s\n",finish-startup,failures?"FAIL":"PASS");
 const char *value=getenv("PEAK_BENCH_EXEC_BEGIN_NS");if(value){char *tail;unsigned long long start=strtoull(value,&tail,10);uint64_t stop=(uint64_t)end.tv_sec*UINT64_C(1000000000)+(uint64_t)end.tv_nsec;if(!*value||*tail||*value=='-'||!start||start>stop)return 2;printf("exec_interval rank=%d begin_ns=%llu end_ns=%llu seconds=%.9f\n",rank,start,(unsigned long long)stop,(double)(stop-start)/1e9);}
 return failures?1:0;
}
