#define _GNU_SOURCE
#define PEAK_GLOBAL_BUDGET_TEST_REQUEST_DELAY_US 200
#include "../../src/global_budget.c"
#include <assert.h>
int main(void){
 alarm(8);unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,1,.5,NULL));peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1,.attached_targets=16};peak_global_budget_publish(&s);usleep(150000);assert(peak_global_budget_active());
 uint64_t grace=500000;unsigned insufficient=0;
 for(unsigned i=0;i<1500;i++){if(!peak_global_budget_try_spend(50000)){insufficient++;assert(grace>=50000);grace-=50000;}usleep(100);}
 PeakGlobalBudgetView v=peak_global_budget_view();fprintf(stderr,"first burst insufficient%u grace_used%llu rate%.6f wave%llu\n",insufficient,(unsigned long long)(500000-grace),v.consumption_rate,(unsigned long long)v.wave);
 assert(v.active&&!v.wave&&v.consumption_rate>.01);peak_global_budget_retire();s.sequence=2;s.retiring=true;s.final=true;peak_global_budget_publish(&s);assert(peak_global_budget_final_flush(2000));peak_global_budget_stop();puts("global_budget_first_burst_ok");
}
