#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void run(unsigned rank,unsigned short port){
 assert(!peak_global_budget_start("127.0.0.1",port,rank,2,.5,NULL));
 if(!rank)peak_global_budget_origin(1);
 double start=now(),cost=0,first_grant=0,last_spent=0;uint64_t seq=0;bool retired=false,first=false,resumed=false,second=false,requested=false,debt1=false,debt2=false;
 while(now()-start<4){
  double elapsed=now()-start;PeakGlobalBudgetView v=peak_global_budget_view();
  if(v.active){assert(v.global_spent_seconds+1e-9>=last_spent);last_spent=v.global_spent_seconds;}
  if(!rank&&!retired&&elapsed>.1){PeakGlobalBudgetSample s={.sequence=++seq,.elapsed_seconds=elapsed,.attached_targets=16};peak_global_budget_publish(&s);peak_global_budget_retire();retired=true;}
  if(!rank&&retired){assert(!peak_global_budget_try_spend(1));peak_global_budget_request_credit(1000);assert(v.granted_total_seconds==0);if(elapsed>1.2)assert(v.active);}
  if(rank){
   if(elapsed>.2&&!requested){peak_global_budget_request_credit(1000000);requested=true;}
   if(v.active&&v.local_credit_ns>1000&&peak_global_budget_try_spend(1000))cost+=.000001;
   if(elapsed>1.4&&!debt1){cost=1.5;peak_global_budget_request_credit(10000000);debt1=true;}
   if(v.wave==1&&!first){first=true;first_grant=v.granted_total_seconds;}
   if(first&&elapsed>2.7&&v.granted_total_seconds>first_grant+.00001)resumed=true;
   if(elapsed>3.2&&resumed&&!debt2){cost=3;peak_global_budget_request_credit(20000000);debt2=true;}
   if(v.wave>=2)second=true;
   if(v.wave){assert(v.wave_participant);peak_global_budget_complete(v.wave);}
   PeakGlobalBudgetSample s={.sequence=++seq,.elapsed_seconds=elapsed,.profile_seconds=cost,.completed_wave=v.wave,.attached_targets=16};peak_global_budget_publish(&s);
  }
  usleep(2000);
 }
 PeakGlobalBudgetView v=peak_global_budget_view();fprintf(stderr,"retire rank%u wave%llu done%llu active%d grant%.6f\n",rank,(unsigned long long)v.wave,(unsigned long long)v.root_completed_wave,v.active,v.granted_total_seconds);
 assert(v.active&&v.wave>=2&&v.root_completed_wave==v.wave);
 if(rank){assert(first&&resumed&&second);peak_global_budget_retire();}else assert(!v.wave_participant);
 usleep(100000);assert(peak_global_budget_view().local_credit_ns==0);assert(peak_global_budget_flush(100));peak_global_budget_stop();
}
int main(void){alarm(12);unsigned short port;assert(!peak_global_budget_listen(&port));pid_t p=fork();assert(p>=0);if(!p){peak_global_budget_stop();run(1,port);_exit(0);}run(0,port);int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);puts("global_budget_retirement_ok");}
