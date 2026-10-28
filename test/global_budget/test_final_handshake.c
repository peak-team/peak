#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static bool all_ready;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void run(unsigned rank,unsigned short port){
 assert(!peak_global_budget_start("127.0.0.1",port,rank,2,.5,NULL));
 PeakGlobalBudgetSample s={.sequence=1,.attached_targets=16};peak_global_budget_publish(&s);
 double deadline=now()+1;while(!peak_global_budget_view().active&&now()<deadline)usleep(1000);assert(peak_global_budget_view().active);
 peak_global_budget_retire();
 if(rank&&!all_ready)usleep(450000);
 s.sequence=2;s.retiring=true;s.final=true;peak_global_budget_publish(&s);
 double before=now();assert(peak_global_budget_final_flush(2000));double waited=now()-before;
 PeakGlobalBudgetView v=peak_global_budget_view();assert(v.cohort_finalized&&v.acknowledged_sequence>=2);
 if(!rank){if(!all_ready)assert(waited>.3);usleep(200000);assert(peak_global_budget_view().active);}
 fprintf(stderr,"final rank%u waited%.6f cohort%d ack%llu\n",rank,waited,v.cohort_finalized,(unsigned long long)v.acknowledged_sequence);
 peak_global_budget_stop();
}
int main(int argc,char **argv){(void)argv;all_ready=argc>1;alarm(8);unsigned short port;assert(!peak_global_budget_listen(&port));pid_t p=fork();assert(p>=0);if(!p){peak_global_budget_stop();run(1,port);_exit(0);}run(0,port);int status;assert(waitpid(p,&status,0)==p);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);puts("global_budget_final_handshake_ok");}
