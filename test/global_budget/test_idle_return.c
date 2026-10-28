#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
 alarm(6);unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,1,.5,NULL));peak_global_budget_origin(1);
 uint64_t seq=0;double begin=now();PeakGlobalBudgetView v={0};bool requested=false,spent=false;
 while(now()-begin<1.1){PeakGlobalBudgetSample s={.sequence=++seq,.elapsed_seconds=now()-begin,.attached_targets=(now()-begin>.7)?16:0};peak_global_budget_publish(&s);v=peak_global_budget_view();if(now()-begin>.55&&now()-begin<.65)assert(v.local_credit_ns>=10000000);if(v.active&&now()-begin>.3&&!requested){peak_global_budget_request_credit(10000000);requested=true;}if(now()-begin>.7&&v.local_credit_ns>1000000&&!spent){assert(peak_global_budget_try_spend(1000));spent=true;}usleep(1000);}
 assert(spent&&v.returned_total_ns>9000000);assert(v.local_credit_ns<=200000);assert(v.control_reserve_seconds>=.0035-1e-9);assert(v.wave==0);
 uint64_t returned=v.returned_total_ns;peak_global_budget_request_credit(1000000);begin=now();
 while(now()-begin<.2&&peak_global_budget_view().local_credit_ns<1000000)usleep(1000);
 v=peak_global_budget_view();assert(v.local_credit_ns>=1000000&&v.local_credit_ns<2000000);assert(peak_global_budget_try_spend(500000));assert(peak_global_budget_view().returned_total_ns==returned);
 peak_global_budget_stop();puts("global_budget_idle_return_ok");
}
