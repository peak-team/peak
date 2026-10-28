#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static void wait_credit(uint64_t amount){double deadline=seconds_now()+1;while(peak_global_budget_view().local_credit_ns<amount&&seconds_now()<deadline)usleep(1000);assert(peak_global_budget_view().local_credit_ns>=amount);}
int main(void){
 alarm(8);
 PeakGlobalBudgetSample arithmetic={.profile_seconds=0,.control_seconds=.001,.management_seconds=.001,.settled_control_credit_ns=500000};
 assert(fabs(ledger_cost(&arithmetic,2000000)-.004)<1e-12);
 arithmetic=(PeakGlobalBudgetSample){.profile_seconds=.002};assert(fabs(ledger_cost(&arithmetic,1000000)-.002)<1e-12);
 arithmetic=(PeakGlobalBudgetSample){.control_seconds=.003};assert(fabs(ledger_cost(&arithmetic,0)-.003)<1e-12);
 arithmetic.control_seconds=.006;arithmetic.settled_control_credit_ns=3000000;assert(fabs(ledger_cost(&arithmetic,0)-.006)<1e-12);
 unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,1,.5,NULL));peak_global_budget_origin(1);
 PeakGlobalBudgetSample s={.sequence=1,.attached_targets=16};peak_global_budget_publish(&s);double deadline=seconds_now()+1;while(!peak_global_budget_active()&&seconds_now()<deadline)usleep(1000);
 peak_global_budget_request_credit(100000);wait_credit(3);assert(peak_global_budget_try_spend(3));
 s.sequence=2;s.attached_targets=0;peak_global_budget_publish(&s);peak_global_budget_request_credit(30480001);wait_credit(30480001);
 PeakGlobalBudgetView v=peak_global_budget_view();assert(v.granted_total_ns==30480004&&v.local_credit_ns==30480001);assert(v.recovery_target_ns==30480001);
 assert(peak_global_budget_try_spend(1999997));assert(peak_global_budget_try_spend_control(500000));
 s.sequence=3;s.settled_control_credit_ns=peak_global_budget_control_consumed_ns();assert(s.settled_control_credit_ns==500000);s.attached_targets=16;s.control_seconds=.001;s.management_seconds=.001;peak_global_budget_publish(&s);assert(peak_global_budget_flush(100));v=peak_global_budget_view();assert(v.global_spent_seconds>=.004);
 s.sequence=4;s.management_seconds=.002;peak_global_budget_publish(&s);assert(peak_global_budget_flush(100));assert(peak_global_budget_view().global_spent_seconds>=.005);
 peak_global_budget_retire();clockid_t c;assert(!pthread_getcpuclockid(pthread_self(),&c));uint64_t begin=ns_now(c);peak_global_budget_finalizer(c,begin,.002);while(ns_now(c)-begin<2000000){}deadline=seconds_now()+.6;while(peak_global_budget_view().global_spent_seconds<.007&&seconds_now()<deadline)usleep(1000);assert(peak_global_budget_view().global_spent_seconds>=.007);
 s.sequence=5;s.retiring=true;s.final=true;peak_global_budget_publish(&s);assert(peak_global_budget_final_flush(2000));peak_global_budget_stop();
 /* Exact 1ns increments remain authorized even above binary64's integer precision. */
 atomic_store(&granted_ns,0);atomic_store(&available_ns,0);atomic_store(&debit_ns,0);atomic_store(&consumed_ns,0);atomic_store(&consumed_control_ns,0);atomic_store(&returned_ns,0);atomic_store(&local_retiring,false);atomic_store(&local_attached,16);atomic_store(&low_water_ns,0);
 uint64_t base=UINT64_C(9007199254740992);for(uint64_t i=0;i<=1000;i++){uint64_t total=base+i;publish_view((PeakGlobalBudgetView){.active=true,.granted_total_ns=total,.granted_total_seconds=(double)total/1e9});if(i)assert(peak_global_budget_try_spend(1));}
 assert(atomic_load(&granted_ns)==base+1000);assert(atomic_load(&available_ns)==(int64_t)base);assert(atomic_load(&consumed_ns)==1000);
 assert((uint64_t)atomic_load(&available_ns)+atomic_load(&consumed_ns)+atomic_load(&consumed_control_ns)+atomic_load(&returned_ns)==atomic_load(&granted_ns));puts("global_budget_integer_ledger_ok");
}
