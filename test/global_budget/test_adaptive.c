#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void rank_run(unsigned rank,unsigned short port)
{
 assert(!peak_global_budget_start("127.0.0.1",port,rank,2,.5,NULL));
 double start=now(),cost=0,first_grant=0,wave2_seen=0;uint64_t seq=0;bool first=false,resumed=false,second=false;
 while(now()-start<5){
  double elapsed=now()-start;PeakGlobalBudgetView v=peak_global_budget_view();
  if(elapsed>.2)peak_global_budget_pressure();
  if(v.active&&v.allowance_seconds>0&&peak_global_budget_try_spend(1000))cost+=1e-6;
  if(rank==0&&elapsed>.7&&cost<1)cost+=1;
  if(v.wave==1&&!first){first=true;first_grant=v.granted_total_seconds;}
  if(first&&elapsed>2.8&&v.granted_total_seconds>first_grant+1e-7)resumed=true;
  if(rank==0&&elapsed>3.3&&cost<2)cost+=2;
  if(v.wave==2){second=true;if(!wave2_seen)wave2_seen=elapsed;}
  uint64_t ack=v.wave;
  if(rank==1&&v.wave==2&&elapsed-wave2_seen<.2){ack=1;assert(v.root_completed_wave<2);}
  PeakGlobalBudgetSample s={.sequence=++seq,.completed_wave=ack,
    .profile_seconds=cost,.elapsed_seconds=elapsed,
    .attached_targets=(!first||(resumed&&!second))?16:0};
  peak_global_budget_publish(&s);usleep(5000);
 }
 PeakGlobalBudgetView v=peak_global_budget_view();
 fprintf(stderr,"adaptive rank%u first%d resumed%d second%d wave%llu completed%llu\n",rank,first,resumed,second,(unsigned long long)v.wave,(unsigned long long)v.root_completed_wave);
 assert(first&&resumed&&second);assert(v.wave==2&&v.root_completed_wave==2);
 peak_global_budget_stop();
}
int main(void){alarm(12);unsigned short port;assert(!peak_global_budget_listen(&port));pid_t child=fork();assert(child>=0);if(!child){peak_global_budget_stop();rank_run(1,port);_exit(0);}rank_run(0,port);int status;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);puts("global_budget_adaptive_ok");}
