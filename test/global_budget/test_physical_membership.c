#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
int main(void)
{
 atomic_store(&owner,getpid()); atomic_store(&running,true);
 local_sample=(PeakGlobalBudgetSample){.sequence=1,.attached_targets=0};
 Peer p={0};
 assert(request_snapshot().sample.attached_targets==0);
 peak_global_budget_physical_targets(3);
 Frame f=request_snapshot(); assert(f.sample.sequence==1 && f.sample.attached_targets==3);
 assert(service_poll_timeout(&p,0,false,false)==0);
 p.pending=true; assert(service_poll_timeout(&p,0,false,false)==100);
 p.pending=false; assert(service_poll_timeout(&p,0,false,false)==0);
 p.outgoing=f; assert(service_poll_timeout(&p,0,false,false)==100);
 /* The root's existing STOP eligibility now sees a positive live member,
  * even without a newer heartbeat or cost watermark. */
 Rank r={.latest=f.sample,.wants_credit=true,.demand_generation=1};
 assert(r.wants_credit && r.latest.attached_targets>0 && r.demand_generation>r.actioned_generation);
 peak_global_budget_physical_targets(0);
 local_sample.attached_targets=9; atomic_store(&local_attached,9);
 assert(request_snapshot().sample.attached_targets==0);
 assert(service_poll_timeout(&p,0,false,false)==0);
 atomic_store(&local_retiring,true);peak_global_budget_physical_targets(4);
 assert(request_snapshot().sample.attached_targets==0);
 atomic_store(&local_retiring,false);atomic_store(&physical_targets,0);
 assert(request_snapshot().sample.attached_targets==9); /* legacy fallback */
 atomic_store(&running,false);
 /* Real broker: a stale-zero HB cannot suppress a needed root STOP. */
 unsigned short port=0; assert(peak_global_budget_listen(&port)==0);
 assert(peak_global_budget_start("127.0.0.1",port,0,1,.05,NULL)==0);
 PeakGlobalBudgetSample sample={.sequence=1,.profile_seconds=.1};
 peak_global_budget_publish(&sample);
 peak_global_budget_physical_targets(1);
 peak_global_budget_request_credit(6000000);
 uint64_t deadline=ns_now(CLOCK_MONOTONIC)+UINT64_C(1000000000);
 PeakGlobalBudgetView view={0};
 do { view=peak_global_budget_view(); if(view.wave)break; struct timespec pause={0,1000000};nanosleep(&pause,NULL); } while(ns_now(CLOCK_MONOTONIC)<deadline);
 assert(view.wave==1 && view.wave_participant);
 peak_global_budget_physical_targets(0);
 sample.sequence=2;sample.attached_targets=7;peak_global_budget_publish(&sample);
 assert(request_snapshot().sample.attached_targets==0);
 peak_global_budget_stop();
 puts("global_budget_physical_membership_ok");
}
