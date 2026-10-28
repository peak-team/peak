#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static void receive(Rank *r,PeakGlobalBudgetSample s)
{
 assert(s.settled_control_credit_ns<=r->control_ns);assert(valid_sample(&s,r));
 r->latest=s;r->seen=true;
 r->previous_cost=fmax(r->previous_cost,ledger_cost(&s,r->profile_ns));
}
static void close_to(double a,double b){assert(fabs(a-b)<1e-12);}
int main(void)
{
 Rank r={.credit_total_ns=6000000,.consumed_ns=3000000,.control_ns=3000000,.reservation=.003};
 receive(&r,(PeakGlobalBudgetSample){.sequence=1});
 close_to(r.previous_cost,0);close_to(r.reservation+control_escrow(&r),.006);
 /* HB mark captured before completion; actual C read after completion. */
 receive(&r,(PeakGlobalBudgetSample){.sequence=2,.control_seconds=.003});
 close_to(r.previous_cost,.003);close_to(r.reservation+control_escrow(&r),.006);
 receive(&r,(PeakGlobalBudgetSample){.sequence=3,.control_seconds=.003,.settled_control_credit_ns=3000000});
 close_to(r.previous_cost,.003);close_to(r.reservation+control_escrow(&r),.003);
 assert(r.credit_total_ns-r.consumed_ns-r.returned_ns==3000000);
 PeakGlobalBudgetSample old=r.latest;old.sequence++;old.settled_control_credit_ns=0;
 assert(!valid_sample(&old,&r)); /* An old mark cannot release or rewrite escrow. */
 /* Uncredited legacy C plus a new pending paid operation are additive. */
 r=(Rank){.control_ns=3000000};
 receive(&r,(PeakGlobalBudgetSample){.sequence=1,.control_seconds=.003});
 close_to(r.previous_cost+control_escrow(&r),.006);
 receive(&r,(PeakGlobalBudgetSample){.sequence=2,.control_seconds=.006,.settled_control_credit_ns=3000000});
 close_to(r.previous_cost+control_escrow(&r),.006);
 /* Proven cancelled/no-op control settles at zero actual cost. */
 r=(Rank){.credit_total_ns=3000000,.consumed_ns=3000000,.control_ns=3000000};
 receive(&r,(PeakGlobalBudgetSample){.sequence=1});close_to(control_escrow(&r),.003);
 receive(&r,(PeakGlobalBudgetSample){.sequence=2,.settled_control_credit_ns=3000000});
 close_to(r.previous_cost,0);close_to(control_escrow(&r),0);
 puts("global_budget_control_escrow_ok");
}
