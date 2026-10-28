#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void run(unsigned rank,unsigned size,unsigned short port)
{
 assert(!peak_global_budget_start("127.0.0.1",port,rank,size,.05,NULL));
 double begin=now();uint64_t seq=0;PeakGlobalBudgetView last={0};
 while(now()-begin<3){PeakGlobalBudgetSample s={.sequence=++seq,.elapsed_seconds=now()-begin};peak_global_budget_publish(&s);last=peak_global_budget_view();usleep(100000);}
 assert(last.active);
 fprintf(stderr,"rank%u transport_cpu %.9f\n",rank,last.transport_seconds);
 if(!rank)printf("idle ranks=%u elapsed=%.6f aggregate_transport_cpu=%.9f cpu_seconds_per_job_second=%.9f\n",size,now()-begin,last.global_spent_seconds,last.global_spent_seconds/(now()-begin));
 usleep(200000);peak_global_budget_stop();
}
int main(int argc,char **argv)
{
 alarm(15);unsigned n=argc>1?(unsigned)atoi(argv[1]):8;assert(n>=2&&n<=32);unsigned short port;assert(!peak_global_budget_listen(&port));pid_t children[32];
 for(unsigned r=1;r<n;r++){pid_t p=fork();assert(p>=0);if(!p){peak_global_budget_stop();run(r,n,port);_exit(0);}children[r]=p;}
 run(0,n,port);for(unsigned r=1;r<n;r++){int status;assert(waitpid(children[r],&status,0)==children[r]);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);}return 0;
}
