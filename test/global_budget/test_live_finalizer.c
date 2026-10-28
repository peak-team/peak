#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
static uint64_t cpu(clockid_t c){struct timespec t;assert(!clock_gettime(c,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv){
 (void)argv;alarm(8);unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,1,.5,NULL));peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1,.management_seconds=.002};peak_global_budget_publish(&s);peak_global_budget_retire();
 clockid_t c;assert(!pthread_getcpuclockid(pthread_self(),&c));uint64_t start=cpu(c);
 peak_global_budget_finalizer(argc>1?(clockid_t)INT32_MAX:c,start,.002);
 if(argc>1){usleep(300000);PeakGlobalBudgetView v=peak_global_budget_view();assert(v.accounting_invalid&&!v.active);s.sequence=2;s.retiring=true;s.final=true;peak_global_budget_publish(&s);assert(!peak_global_budget_final_flush(100));peak_global_budget_stop();puts("global_budget_finalizer_fault_ok");return 0;}
 while(cpu(c)-start<40000000){}usleep(250000);
 PeakGlobalBudgetView v=peak_global_budget_view();assert(v.active&&v.global_spent_seconds>=.042);
 double accounted=.002+(double)(cpu(c)-start)/1e9;
 /* Re-registering from a FINAL counter must not replace the frozen base. */
 peak_global_budget_finalizer(c,start,accounted);s.sequence=2;s.management_seconds=accounted;s.retiring=true;s.final=true;peak_global_budget_publish(&s);
 assert(peak_global_budget_final_flush(2000));v=peak_global_budget_view();assert(v.global_spent_seconds>=accounted&&v.global_spent_seconds<accounted+.02);
 fprintf(stderr,"live cpu accounted%.6f spent%.6f\n",accounted,v.global_spent_seconds);peak_global_budget_stop();puts("global_budget_live_finalizer_ok");
}
