#define _GNU_SOURCE
#include <unistd.h>
static ssize_t scheduled_write(int fd,const void *data,size_t count);
#define write scheduled_write
#include "../../src/global_budget.c"
#undef write
#include <assert.h>
#include <sched.h>
#include <sys/wait.h>
static atomic_bool stopping;
static int cpus[CPU_SETSIZE],cpu_count;
/* Simulate the same-core preemption immediately after the stop wake syscall.
 * This changes neither FINAL nor the normal report cadence. */
static ssize_t scheduled_write(int fd,const void *data,size_t count){ssize_t n=write(fd,data,count);if(fd==wake_fd&&atomic_load(&stopping))usleep(1000);return n;}
typedef struct{unsigned rank;double flush,join;}Receipt;
static void run(unsigned rank,unsigned n,unsigned short port,int report_fd,unsigned delay_us){
 cpu_set_t one;CPU_ZERO(&one);CPU_SET(cpus[rank%(unsigned)cpu_count],&one);assert(!sched_setaffinity(0,sizeof(one),&one));
 assert(!peak_global_budget_start("127.0.0.1",port,rank,n,.05,NULL));if(!rank)peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1};peak_global_budget_publish(&s);double deadline=seconds_now()+2;while(!peak_global_budget_active()&&seconds_now()<deadline)usleep(1000);assert(peak_global_budget_active());
 peak_global_budget_retire();if(rank==n-1&&delay_us)usleep(delay_us);s.sequence=2;s.retiring=true;s.final=true;peak_global_budget_publish(&s);
 double start=seconds_now();assert(peak_global_budget_final_flush(2000));double flushed=seconds_now();atomic_store(&stopping,true);peak_global_budget_stop();double joined=seconds_now();
 Receipt r={rank,flushed-start,joined-flushed};assert(write(report_fd,&r,sizeof(r))==sizeof(r));
}
int main(int argc,char **argv){
 alarm(15);unsigned n=argc>1?(unsigned)atoi(argv[1]):32,delay=argc>2?(unsigned)atoi(argv[2]):0;assert(n>=2&&n<=32);
 cpu_set_t allowed;assert(!sched_getaffinity(0,sizeof(allowed),&allowed));for(int c=0;c<CPU_SETSIZE;c++)if(CPU_ISSET(c,&allowed))cpus[cpu_count++]=c;assert(cpu_count);
 unsigned short port;assert(!peak_global_budget_listen(&port));int fd[2];assert(!pipe(fd));pid_t children[32];for(unsigned r=1;r<n;r++){pid_t p=fork();assert(p>=0);if(!p){close(fd[0]);peak_global_budget_stop();run(r,n,port,fd[1],delay);close(fd[1]);_exit(0);}children[r]=p;}
 run(0,n,port,fd[1],delay);for(unsigned r=1;r<n;r++){int st;assert(waitpid(children[r],&st,0)==children[r]);assert(WIFEXITED(st)&&WEXITSTATUS(st)==0);}close(fd[1]);double max_flush=0,max_join=0,root_flush=0,root_join=0;for(unsigned r=0;r<n;r++){Receipt v;assert(read(fd[0],&v,sizeof(v))==sizeof(v));if(v.flush>max_flush)max_flush=v.flush;if(v.join>max_join)max_join=v.join;if(!v.rank){root_flush=v.flush;root_join=v.join;}}close(fd[0]);printf("many_final ranks=%u delay_us=%u root_flush_ms=%.3f root_join_ms=%.3f max_flush_ms=%.3f max_join_ms=%.3f\n",n,delay,root_flush*1000,root_join*1000,max_flush*1000,max_join*1000);puts("global_budget_many_final_ok");
}
