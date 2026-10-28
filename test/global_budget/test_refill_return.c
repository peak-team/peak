#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void run(unsigned rank,unsigned short port) {
 assert(!peak_global_budget_start("127.0.0.1",port,rank,2,.5,NULL));
 if(!rank) peak_global_budget_origin(1);
 uint64_t seq=0; double start=now(),cost=0; unsigned spent=0,misses=0; bool returned=false,first=false,second=false,requested_second=false;
 uint64_t returned_value=0; double previous_spent=0;
 while(now()-start<2.2) {
  double elapsed=now()-start; PeakGlobalBudgetView v=peak_global_budget_view();
  assert(v.global_spent_seconds+1e-10>=previous_spent); previous_spent=v.global_spent_seconds;
  if(!rank && elapsed>.1 && elapsed<.8) {
   if(v.local_credit_ns>=10000) { if(peak_global_budget_try_spend(10000)){spent++;cost+=.00001;}else misses++; }
   else if(!spent) peak_global_budget_request_credit(200000);
   else misses++;
  }
  if(!rank && elapsed>.85 && !returned) {
   peak_global_budget_return_idle(); returned_value=peak_global_budget_view().returned_total_ns;
   peak_global_budget_return_idle(); assert(peak_global_budget_view().returned_total_ns==returned_value); returned=true;
  }
  if(!rank && elapsed>1.1 && !first) { cost=10; peak_global_budget_request_credit(200000); }
  if(v.wave==1) { first=true; assert(v.wave_participant==!rank); peak_global_budget_complete(1); }
  if(first && elapsed>1.6 && !requested_second && !rank) { peak_global_budget_return_idle(); peak_global_budget_request_credit(300000); requested_second=true; }
  if(v.wave==2) { second=true; assert(v.wave_participant==!rank); peak_global_budget_complete(2); }
  PeakGlobalBudgetSample sample={.sequence=++seq,.completed_wave=v.wave,.profile_seconds=cost,
   .elapsed_seconds=elapsed,.attached_targets=rank?16:((first&&elapsed<1.6)?0:16)};
  peak_global_budget_publish(&sample); usleep(1000);
 }
 PeakGlobalBudgetView v=peak_global_budget_view();
 fprintf(stderr,"refill rank%u spent%u misses%u returned%llu wave%llu participant%d D%.6f\n",rank,spent,misses,(unsigned long long)returned_value,(unsigned long long)v.wave,v.wave_participant,v.control_reserve_seconds);
 assert(first&&second&&v.wave==2&&v.root_completed_wave==2);
 if(!rank) { assert(spent>400); assert(misses==0); assert(returned_value>0); }
 else { assert(v.granted_total_seconds==0); assert(v.returned_total_ns==0); }
 peak_global_budget_stop();
}
int main(void) { alarm(10); unsigned short port; assert(!peak_global_budget_listen(&port)); pid_t p=fork(); assert(p>=0); if(!p){peak_global_budget_stop();run(1,port);_exit(0);}run(0,port);int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);puts("global_budget_refill_return_ok"); }
