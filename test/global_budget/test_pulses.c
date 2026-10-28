#define _GNU_SOURCE
#define PEAK_GLOBAL_BUDGET_TEST_REQUEST_DELAY_US 2000
#include "../../src/global_budget.c"
#include <assert.h>
static unsigned burst(bool warmup){unsigned misses=0;for(unsigned i=0;i<300;i++){if(!peak_global_budget_try_spend(50000))misses++;if(warmup&&i%10==0)poke();usleep(100);}return misses;}
int main(void){
 alarm(10);unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,1,.5,NULL));peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1,.attached_targets=16};peak_global_budget_publish(&s);
 double deadline=seconds_now()+1;while(!peak_global_budget_view().active&&seconds_now()<deadline)usleep(1000);
 peak_global_budget_request_credit(10000000);deadline=seconds_now()+1;while(peak_global_budget_view().local_credit_ns<10000000&&seconds_now()<deadline)usleep(1000);
 assert(peak_global_budget_view().local_credit_ns>=10000000);unsigned misses=burst(true);
 for(unsigned pulse=0;pulse<5;pulse++){
  for(unsigned tick=0;tick<15;tick++){poke();usleep(10000);}
  PeakGlobalBudgetView v=peak_global_budget_view();assert(v.active&&v.local_credit_ns>100000);
  misses+=burst(false);
 }
 PeakGlobalBudgetView v=peak_global_budget_view();fprintf(stderr,"pulses misses%u returned%.6f retained%llu rate%.6f\n",misses,(double)v.returned_total_ns/1e9,(unsigned long long)v.local_credit_ns,v.consumption_rate);
 /* No grace is available in this fixture: every failed credit spend would
  * immediately be an ordinary emergency in the listener. */
 peak_global_budget_stop();assert(!misses&&v.returned_total_ns>0&&v.wave==0);puts("global_budget_pulses_ok");
}
