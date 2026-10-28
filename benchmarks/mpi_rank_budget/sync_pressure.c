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
static void stats(const char *name,double value,int rank,int size){double lo,hi,sum;MPI_Reduce(&value,&lo,1,MPI_DOUBLE,MPI_MIN,0,MPI_COMM_WORLD);MPI_Reduce(&value,&sum,1,MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);MPI_Reduce(&value,&hi,1,MPI_DOUBLE,MPI_MAX,0,MPI_COMM_WORLD);if(!rank)printf(" %s rank_min=%.6f rank_mean=%.6f rank_max=%.6f\n",name,lo,sum/size,hi);}
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
 const char *sync="allreduce",*layout="rotating";
 unsigned profile_threads=1;
 unsigned epochs=2048,warmup_work=131072,main_work=1;uint64_t calls=2000,warmup_calls=10000;
 for(int i=1;i<argc;i++) {
  if(i+1>=argc)MPI_Abort(MPI_COMM_WORLD,2);
  const char *key=argv[i++]; if(!strcmp(key,"--sync")){sync=argv[i];continue;} if(!strcmp(key,"--layout")){layout=argv[i];continue;} char *end;uint64_t n=strtoull(argv[i],&end,10);
  if(!n||*end||argv[i][0]=='-')MPI_Abort(MPI_COMM_WORLD,2);
  if(!strcmp(key,"--epochs")&&n<=UINT32_MAX)epochs=n;
  else if(!strcmp(key,"--calls"))calls=n;
  else if(!strcmp(key,"--main-work")&&n<=UINT32_MAX)main_work=n;
  else if(!strcmp(key,"--warmup-calls"))warmup_calls=n;
  else if(!strcmp(key,"--warmup-work")&&n<=UINT32_MAX)warmup_work=n;
  else if(!strcmp(key,"--profile-threads")&&n<=65536)profile_threads=n;
  else if(!strcmp(key,"--threads")&&n<=65536)omp_set_num_threads(n);
  else MPI_Abort(MPI_COMM_WORLD,2);
 }
 if(strcmp(sync,"allreduce")&&strcmp(sync,"barrier")&&strcmp(sync,"bcast")&&strcmp(sync,"ring")&&strcmp(sync,"iallreduce"))MPI_Abort(MPI_COMM_WORLD,2);
 if(strcmp(layout,"rotating")&&strcmp(layout,"slack")&&strcmp(layout,"critical")&&strcmp(layout,"aligned"))MPI_Abort(MPI_COMM_WORLD,2);
 if(warmup_calls>UINT64_MAX/warmup_work||calls>UINT64_MAX/main_work)MPI_Abort(MPI_COMM_WORLD,2);
 omp_set_dynamic(0);int nt=omp_get_max_threads();if(profile_threads>(unsigned)nt)MPI_Abort(MPI_COMM_WORLD,2);
 double *compute=calloc(nt,sizeof(double)),*cpu=calloc(nt,sizeof(double)),*pre=calloc(nt,sizeof(double)),*release=calloc(nt,sizeof(double));
 uint64_t *state=calloc(nt,sizeof(uint64_t)),*expected=calloc(nt,sizeof(uint64_t));cpu_set_t *masks=calloc(nt,sizeof(cpu_set_t));
 if(!compute||!cpu||!pre||!release||!state||!expected||!masks)MPI_Abort(MPI_COMM_WORLD,2);
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
 if(!rank){for(int r=0;r<size;r++)printf("%s\n",bindings+r*sizeof(binding));free(bindings);printf("scenario sync=%s layout=%s\n",sync,layout);}free(masks);
 if(!rank){printf("config ranks=%d threads=%d profile_threads=%u epochs=%u calls_per_thread_epoch=%llu tiny_work=%u medium_work=%u rotate_epochs=%d sparse_cadence=1 layout=3 targets=16 hotset=1 warmup_calls=%llu warmup_work=%u exported_targets=256 expected_potential_calls=%.0Lf expected_profiled_calls=%.0Lf expected_warmup_calls=%.0Lf\n",size,nt,profile_threads,epochs,(unsigned long long)calls,main_work,main_work,size,(unsigned long long)warmup_calls,warmup_work,(long double)size*nt*epochs*calls,(long double)profile_threads*epochs*calls,(long double)size*nt*warmup_calls);fflush(stdout);}
 int failures=0;
 for(unsigned phase=0;phase<2;phase++) {
  memset(compute,0,nt*sizeof(double));memset(cpu,0,nt*sizeof(double));memset(pre,0,nt*sizeof(double));memset(release,0,nt*sizeof(double));
  double mpi=0,post=0,overlap_time=0,wait_time=0;int mpibad=0;unsigned n_epoch=phase?epochs:1;
  MPI_Barrier(MPI_COMM_WORLD);double process_start=process_cpu(),begin=now();
  #pragma omp parallel shared(mpi,mpibad)
  {
   int tid=omp_get_thread_num();uint64_t x=state[tid];
   for(unsigned e=0;e<n_epoch;e++) {
    unsigned id=phase?(e/(unsigned)size)%16:0;
    int hot=(!strcmp(layout,"slack")||!strcmp(layout,"aligned"))?0:(int)(e%(unsigned)size);
    int slow=!strcmp(layout,"slack")?size-1:hot;
    unsigned target=phase&&rank==hot&&(unsigned)tid<profile_threads?id:id+128;
    uint64_t n=phase?calls:warmup_calls;unsigned work=phase?main_work:warmup_work;
    if(phase&&strcmp(layout,"rotating")&&rank==slow)work=512;
    double tc=thread_cpu(),t=now();
    for(uint64_t c=0;c<n;c++)x=targets[target](x,work);
    compute[tid]+=now()-t;cpu[tid]+=thread_cpu()-tc;t=now();
    #pragma omp barrier
    pre[tid]+=now()-t;t=now();
    #pragma omp master
    {double a=now();uint64_t local=(uint64_t)e+1,global;if(!phase||!strcmp(sync,"allreduce")){MPI_Allreduce(&local,&global,1,MPI_UINT64_T,MPI_SUM,MPI_COMM_WORLD);if(global!=local*(uint64_t)size)mpibad++;}
     else if(!strcmp(sync,"barrier")){MPI_Barrier(MPI_COMM_WORLD);}
     else if(!strcmp(sync,"bcast")){int root=e%(unsigned)size;global=rank==root?local:0;MPI_Bcast(&global,1,MPI_UINT64_T,root,MPI_COMM_WORLD);if(global!=local)mpibad++;}
     else if(!strcmp(sync,"ring")){MPI_Sendrecv(&local,1,MPI_UINT64_T,(rank+1)%size,17,&global,1,MPI_UINT64_T,(rank+size-1)%size,17,MPI_COMM_WORLD,MPI_STATUS_IGNORE);if(global!=local)mpibad++;}
     else {MPI_Request req;double post_start=now();MPI_Iallreduce(&local,&global,1,MPI_UINT64_T,MPI_SUM,MPI_COMM_WORLD,&req);post+=now()-post_start;double overlap_start=now();volatile uint64_t overlap=local;for(unsigned k=0;k<256;k++)overlap=A*overlap+B;if(overlap!=oracle(local,0,256,1))mpibad++;overlap_time+=now()-overlap_start;double wait_start=now();MPI_Wait(&req,MPI_STATUS_IGNORE);wait_time+=now()-wait_start;if(global!=local*(uint64_t)size)mpibad++;}
     mpi+=now()-a;}
    #pragma omp barrier
    release[tid]+=now()-t;
   }state[tid]=x;
  }
  double completion_start=now();MPI_Barrier(MPI_COMM_WORLD);double completion_wait=now()-completion_start;
  double wall=now()-begin,process_delta=process_cpu()-process_start;
  int bad=0,totalbad,totalmpibad;uint64_t checksum=0,totalcheck;double cs=0,cpus=0,ps=0,rs=0;
  for(int t=0;t<nt;t++) {
   for(unsigned e=0;e<n_epoch;e++){int hot=(!strcmp(layout,"slack")||!strcmp(layout,"aligned"))?0:(int)(e%(unsigned)size);int slow=!strcmp(layout,"slack")?size-1:hot;unsigned w=phase?main_work:warmup_work;if(phase&&strcmp(layout,"rotating")&&rank==slow)w=512;expected[t]=oracle(expected[t],phase?(e/(unsigned)size)%16:0,phase?calls:warmup_calls,w);}
   if(state[t]!=expected[t]){bad++;}
   checksum^=state[t];cs+=compute[t];cpus+=cpu[t];ps+=pre[t];rs+=release[t];
  }
  MPI_Allreduce(&bad,&totalbad,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);MPI_Allreduce(&mpibad,&totalmpibad,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);MPI_Reduce(&checksum,&totalcheck,1,MPI_UINT64_T,MPI_BXOR,0,MPI_COMM_WORLD);failures+=totalbad+totalmpibad;
  uint64_t local_steps=0,total_steps=0;for(unsigned e=0;e<n_epoch;e++){int hot=(!strcmp(layout,"slack")||!strcmp(layout,"aligned"))?0:(int)(e%(unsigned)size);int slow=!strcmp(layout,"slack")?size-1:hot;unsigned w=phase?main_work:warmup_work;if(phase&&strcmp(layout,"rotating")&&rank==slow)w=512;local_steps+=(uint64_t)nt*(phase?calls:warmup_calls)*w;}MPI_Reduce(&local_steps,&total_steps,1,MPI_UINT64_T,MPI_SUM,0,MPI_COMM_WORLD);
  if(!rank)printf("%s work=%u cadence=1 oracle_bad=%d mpi_bad=%d checksum=%016llx potential_target_calls=%.0Lf profiled_target_calls=%.0Lf lcg_steps=%.0Lf collectives_per_rank=%u\n",phase?"phase":"warmup",phase?main_work:warmup_work,totalbad,totalmpibad,(unsigned long long)totalcheck,(long double)size*nt*(phase?(long double)epochs*calls:warmup_calls),phase?(long double)profile_threads*epochs*calls:0,(long double)total_steps,n_epoch);
  uint64_t rank_calls=0;for(unsigned e=0;e<n_epoch;e++){int hot=(!strcmp(layout,"slack")||!strcmp(layout,"aligned"))?0:(int)(e%(unsigned)size);if(phase&&rank==hot)rank_calls+=(uint64_t)profile_threads*calls;}
  double row[11]={wall,cs/nt,cpus/nt,mpi,process_delta,(double)local_steps,(double)rank_calls,post,overlap_time,wait_time,completion_wait};double *rows=!rank?calloc(size*11,sizeof(double)):NULL;MPI_Gather(row,11,MPI_DOUBLE,rows,11,MPI_DOUBLE,0,MPI_COMM_WORLD);if(!rank){for(int r=0;r<size;r++)printf("rank_metric phase=%u rank=%d wall=%.9f compute=%.9f cpu=%.9f sync=%.9f process_cpu=%.9f lcg_steps=%.0f potential_profiled_calls=%.0f post=%.9f overlap=%.9f wait=%.9f completion_wait=%.9f\n",phase,r,rows[r*11],rows[r*11+1],rows[r*11+2],rows[r*11+3],rows[r*11+4],rows[r*11+5],rows[r*11+6],rows[r*11+7],rows[r*11+8],rows[r*11+9],rows[r*11+10]);printf("phase_root_wall phase=%u seconds=%.9f\n",phase,wall);free(rows);}
  stats("wall_seconds",wall,rank,size);stats("thread_mean_compute_seconds",cs/nt,rank,size);stats("thread_mean_compute_cpu_seconds",cpus/nt,rank,size);stats("process_user_plus_system_cpu_seconds",process_delta,rank,size);stats("thread_mean_pre_collective_barrier_seconds",ps/nt,rank,size);stats("thread_mean_collective_release_barrier_seconds_OVERLAPS_MPI",rs/nt,rank,size);stats("master_mpi_allreduce_seconds",mpi,rank,size);fflush(stdout);
 }
 free(compute);free(cpu);free(pre);free(release);free(state);free(expected);stamp("before_mpi_finalize",rank);MPI_Finalize();stamp("after_mpi_finalize",rank);struct timespec exec_end={0};int exec_clock_valid=clock_gettime(CLOCK_MONOTONIC,&exec_end)==0;
 if(!exec_clock_valid)return 2;
 double interval_end=exec_end.tv_sec+1e-9*exec_end.tv_nsec;
 if(!rank){printf("process_seconds=%.6f oracle=%s\n",interval_end-startup,failures?"FAIL":"PASS");}
 const char *exec_begin=getenv("PEAK_BENCH_EXEC_BEGIN_NS");
 if(exec_begin){char *end;unsigned long long begin=strtoull(exec_begin,&end,10);uint64_t finish=(uint64_t)exec_end.tv_sec*UINT64_C(1000000000)+(uint64_t)exec_end.tv_nsec;
  if(!exec_clock_valid||!*exec_begin||*end||*exec_begin=='-'||!begin||begin>finish){fprintf(stderr,"invalid PEAK_BENCH_EXEC_BEGIN_NS\n");return 2;}
  printf("exec_interval rank=%d begin_ns=%llu end_ns=%llu seconds=%.9f\n",rank,begin,(unsigned long long)finish,(double)(finish-begin)/1e9);
 }
 return failures?1:0;
}
