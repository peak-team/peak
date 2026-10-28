#define _GNU_SOURCE
#include "internal/global_budget.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
typedef struct{unsigned rank,early,late;uint64_t credit,target;double rtt,reserve;}Receipt;
static void run(unsigned rank,unsigned short port,int fd){
 assert(!peak_global_budget_start("127.0.0.1",port,rank,4,.1,NULL));if(!rank)peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1};peak_global_budget_publish(&s);double begin=now();bool requested=false,captured=false;Receipt out={.rank=rank};
 while(now()-begin<.62){PeakGlobalBudgetView v=peak_global_budget_view();if(v.active&&!requested){peak_global_budget_request_credit(40000000);requested=true;}
  if(requested)peak_global_budget_request_credit(80000000);/* A changed prediction cannot enlarge the pending frozen lease. */
  if(now()-begin>.22&&!captured){out.early=v.local_credit_ns>0;captured=true;if(out.early)assert(v.local_credit_ns>=40000000);else assert(v.allowance_seconds==0);}
  usleep(1000);
 }
 PeakGlobalBudgetView v=peak_global_budget_view();assert(v.active);out.late=v.local_credit_ns>0;out.credit=v.local_credit_ns;out.target=v.recovery_target_ns;out.rtt=v.refill_rtt_seconds;out.reserve=v.control_reserve_seconds;
 assert(out.target==40000000);assert(!out.credit||out.credit>=40000000);assert(out.rtt<.01);assert(write(fd,&out,sizeof(out))==sizeof(out));usleep(100000);peak_global_budget_stop();
}
int main(int argc,char **argv){(void)argv;alarm(8);unsigned short port;assert(!peak_global_budget_listen(&port));
 if(argc>1){assert(!peak_global_budget_start("127.0.0.1",port,0,1,.05,NULL));peak_global_budget_origin(1);PeakGlobalBudgetSample s={.sequence=1,.attached_targets=16};peak_global_budget_publish(&s);double deadline=now()+1;while(!peak_global_budget_active()&&now()<deadline)usleep(1000);peak_global_budget_request_credit(100000000);while((!peak_global_budget_view().local_credit_ns||!peak_global_budget_view().wave)&&now()<deadline)usleep(1000);PeakGlobalBudgetView v=peak_global_budget_view();assert(v.local_credit_ns>0&&v.local_credit_ns<100000000);assert(v.wave==1&&!v.recovery_target_ns);peak_global_budget_stop();puts("global_budget_active_partial_ok");return 0;}int pfd[2];assert(!pipe(pfd));pid_t children[3];for(unsigned r=1;r<4;r++){pid_t p=fork();assert(p>=0);if(!p){close(pfd[0]);peak_global_budget_stop();run(r,port,pfd[1]);close(pfd[1]);_exit(0);}children[r-1]=p;}run(0,port,pfd[1]);for(unsigned r=0;r<3;r++){int st;assert(waitpid(children[r],&st,0)==children[r]);assert(WIFEXITED(st)&&WEXITSTATUS(st)==0);}close(pfd[1]);unsigned early=0,late=0;for(unsigned r=0;r<4;r++){Receipt out;assert(read(pfd[0],&out,sizeof(out))==sizeof(out));early+=out.early;late+=out.late;fprintf(stderr,"whole rank%u early%u late%u credit%llu target%llu rtt%.6f D%.6f\n",out.rank,out.early,out.late,(unsigned long long)out.credit,(unsigned long long)out.target,out.rtt,out.reserve);}close(pfd[0]);assert(early==2&&late==3);puts("global_budget_whole_recovery_ok");}
