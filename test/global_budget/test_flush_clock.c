#define _GNU_SOURCE
#include <time.h>
static unsigned clock_mode, clock_calls;
static int controlled_clock_gettime(clockid_t clock,struct timespec *t);
#define clock_gettime controlled_clock_gettime
#include "../../src/global_budget.c"
#undef clock_gettime
#include <assert.h>
static int controlled_clock_gettime(clockid_t clock,struct timespec *t){
 if(clock==CLOCK_MONOTONIC&&clock_mode){
  if(clock_mode==1){errno=EINVAL;return -1;}
  t->tv_sec=clock_mode==2&&clock_calls++?99:100;t->tv_nsec=0;return 0;
 }
 return clock_gettime(clock,t);
}
int main(void){
 alarm(2);atomic_store(&owner,getpid());atomic_store(&running,true);
 local_sample=(PeakGlobalBudgetSample){.sequence=2,.retiring=true,.final=true};
 publish_view((PeakGlobalBudgetView){.acknowledged_sequence=1});
 for(unsigned mode=1;mode<=3;mode++){
  clock_mode=mode;clock_calls=0;assert(!peak_global_budget_flush(3));
  clock_calls=0;assert(!peak_global_budget_final_flush(3));
 }
 atomic_store(&running,false);puts("global_budget_flush_clock_ok");
}
