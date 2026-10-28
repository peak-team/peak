#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void run_rank(unsigned rank,unsigned n,unsigned short port)
{
 assert(!peak_global_budget_start("127.0.0.1",port,rank,n,.5,NULL));
 double begin=now();uint64_t seq=0;bool granted=false,ack=false,shock=false;double charged=0;
 while(now()-begin<2.0){
  PeakGlobalBudgetView v=peak_global_budget_view();
  if(!granted&&now()-begin>.15)peak_global_budget_pressure();
  if(v.active&&v.granted_total_seconds>0){granted=true;if(!charged&&peak_global_budget_try_spend(1000))charged=1e-6;}
  if(rank==0&&!shock&&now()-begin>1){charged+=2;shock=true;}
  if(rank==0&&now()-begin>.8){peak_global_budget_pressure();if(peak_global_budget_try_spend(10000000))charged+=.01;}
  if(v.wave){ack=true;peak_global_budget_complete(v.wave);}
  if(v.active&&v.remaining_seconds>0)
   assert(v.global_spent_seconds+v.reserved_seconds+v.control_reserve_seconds+v.remaining_seconds<=v.budget_seconds+1e-9);
  PeakGlobalBudgetSample s={.sequence=++seq,.completed_wave=v.wave,.attached_targets=v.wave?0:16,.profile_seconds=charged,.elapsed_seconds=now()-begin,.pressure=(rank==0&&now()-begin>.8)?1000:0};
  peak_global_budget_publish(&s);
  /* Out-of-order updates must not replace the newer sample. */
  s.sequence=seq-1;s.profile_seconds=NAN;peak_global_budget_publish(&s);
  usleep(5000);
 }
 fprintf(stderr,"rank%u grant%d ack%d wave%llu active%d remain%g span%g\n",rank,granted,ack,(unsigned long long)peak_global_budget_view().wave,peak_global_budget_view().active,peak_global_budget_view().remaining_seconds,peak_global_budget_view().wave_span_seconds);assert(granted);assert(ack);
 PeakGlobalBudgetView v=peak_global_budget_view();
 assert(v.wave==1);assert(v.global_spent_seconds>=charged);
 assert(!peak_global_budget_try_spend(UINT64_MAX));
 peak_global_budget_stop();
 assert(!peak_global_budget_view().active);
}
int main(int argc,char **argv)
{
 alarm(12);unsigned n=argc>1?(unsigned)atoi(argv[1]):4;assert(n==2||n==4);
 unsigned short port;assert(!peak_global_budget_listen(&port));pid_t children[4];
 for(unsigned r=1;r<n;r++){pid_t p=fork();assert(p>=0);if(!p){peak_global_budget_stop();run_rank(r,n,port);_exit(0);}children[r]=p;}
 run_rank(0,n,port);
 for(unsigned r=1;r<n;r++){int status;assert(waitpid(children[r],&status,0)==children[r]);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 puts("global_budget_protocol_ok");return 0;
}
