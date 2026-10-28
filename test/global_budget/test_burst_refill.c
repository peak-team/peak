#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
 alarm(8);unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,1,.5,NULL));peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1,.attached_targets=16};peak_global_budget_publish(&s);
 double deadline=now()+1;while(!peak_global_budget_view().active&&now()<deadline)usleep(1000);
 peak_global_budget_request_credit(10000000);deadline=now()+1;
 while(peak_global_budget_view().local_credit_ns<10000000&&now()<deadline)usleep(1000);
 assert(peak_global_budget_view().local_credit_ns>=10000000);
 unsigned misses=0;for(unsigned i=0;i<2000;i++){if(!peak_global_budget_try_spend(50000))misses++;usleep(100);}
 PeakGlobalBudgetView v=peak_global_budget_view();fprintf(stderr,"burst misses=%u credit=%llu rate=%.6f RTT=%.6f wave=%llu\n",misses,(unsigned long long)v.local_credit_ns,v.consumption_rate,v.refill_rtt_seconds,(unsigned long long)v.wave);
 assert(misses==0&&v.wave==0);peak_global_budget_stop();puts("global_budget_burst_refill_ok");
}
