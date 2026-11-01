#include "internal/mpi_overhead_policy.h"
#include <math.h>
#include <stdio.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (0)
static int near(double a, double b) { return fabs(a-b) < 1e-12; }
int main(void)
{
    PeakMpiOverheadDecision d;
    double ratio[2];
    PeakMpiOverheadPolicy *one = peak_mpi_overhead_policy_create(1);
    CHECK(one);
    PeakMpiOverheadPolicySample s = {0};
    peak_mpi_overhead_policy_update(one, &s, .01, 0, ratio, &d);
    CHECK(near(ratio[0], .01));
    s = (PeakMpiOverheadPolicySample){1,10,.02,0,0,0};
    peak_mpi_overhead_policy_update(one, &s, .01, 10, ratio, &d);
    CHECK(near(ratio[0], .01));
    /* Actual P/C history does not become an offset or a second quota debit. */
    s.sequence++; s.profile_seconds = .12; s.control_seconds = .03;
    peak_mpi_overhead_policy_update(one, &s, .01, 10, ratio, &d);
    CHECK(near(ratio[0], .01));
    CHECK(near(d.raw_cost_seconds, .15) && near(d.debt_seconds, .05));
    CHECK(d.profiling_headroom_seconds == 0);
    CHECK(near(d.committed_total_seconds, .1));
    PeakMpiOverheadPolicySample ack = s, current = ack;
    PeakMpiOverheadInput input = peak_mpi_overhead_policy_input(&current,&ack,ratio[0],1.2,1);
    CHECK(near(input.profile_target, .01) && input.profile_target == input.control_target);
    current.profile_seconds += 1; current.control_seconds += 2;
    CHECK(near(peak_mpi_overhead_policy_input(&current,&ack,ratio[0],1.2,1).profile_target, .01));
    /* Fresh local M/T is charged once; advancing local time adds no credit. */
    current.management_seconds += .01; current.module_cpu_seconds += .02;
    current.elapsed_seconds = 20;
    CHECK(near(peak_mpi_overhead_policy_input(&current,&ack,ratio[0],1.2,1).profile_target, .07/20));
    current.module_cpu_seconds += .1;
    CHECK(peak_mpi_overhead_policy_input(&current,&ack,ratio[0],1.2,1).profile_target == 0);
    peak_mpi_overhead_policy_free(one);

    PeakMpiOverheadPolicy *two = peak_mpi_overhead_policy_create(2);
    CHECK(two);
    PeakMpiOverheadPolicySample v[2] = {
        {1,10,.02,.01,.002,.001}, {1,10,.03,.01,.003,.001}};
    peak_mpi_overhead_policy_update(two,v,.01,10,ratio,&d);
    CHECK(near(10*(ratio[0]+ratio[1])+.005+.002, .1));
    CHECK(near(d.raw_cost_seconds,.077) && d.debt_seconds == 0);
    CHECK(near(d.profiling_headroom_seconds,.023));
    CHECK(near(d.committed_total_seconds,.1));
    v[0].module_cpu_seconds = .02;
    peak_mpi_overhead_policy_update(two,v,.01,10,ratio,&d);
    CHECK(near(10*(ratio[0]+ratio[1]),.074));
    CHECK(near(d.raw_cost_seconds,.096));
    /* Fees alone can exhaust quota; no history is clipped to available budget. */
    v[0].module_cpu_seconds = .2;
    peak_mpi_overhead_policy_update(two,v,.01,10,ratio,&d);
    CHECK(ratio[0] == 0 && ratio[1] == 0);
    CHECK(near(d.raw_cost_seconds,.276) && near(d.debt_seconds,.176));
    CHECK(near(d.module_cpu_seconds,.201));
    peak_mpi_overhead_policy_free(two);

    /* Equal cost rates, sampled at different cadences, retain equal weights. */
    PeakMpiOverheadPolicy *cadence = peak_mpi_overhead_policy_create(2);
    CHECK(cadence);
    PeakMpiOverheadPolicySample c[2] = {
        {1,1,.001,0,0,0}, {1,1,.001,0,0,0}};
    peak_mpi_overhead_policy_update(cadence,c,.1,1,ratio,&d);
    c[0] = (PeakMpiOverheadPolicySample){2,2,.002,0,0,0};
    peak_mpi_overhead_policy_update(cadence,c,.1,2,ratio,&d);
    c[0] = (PeakMpiOverheadPolicySample){3,3,.003,0,0,0};
    c[1] = (PeakMpiOverheadPolicySample){2,3,.003,0,0,0};
    peak_mpi_overhead_policy_update(cadence,c,.1,3,ratio,&d);
    CHECK(near(ratio[0],.05) && near(ratio[1],.05));
    CHECK(near(d.raw_cost_seconds,.006));
    /* A real later higher rate changes weights; idle updates keep paid history. */
    c[0].sequence++; c[1].sequence++;
    c[0].elapsed_seconds = c[1].elapsed_seconds = 4;
    c[1].profile_seconds = .013;
    peak_mpi_overhead_policy_update(cadence,c,.1,4,ratio,&d);
    CHECK(ratio[1] > ratio[0]);
    CHECK(near(4*(ratio[0]+ratio[1]),.4));
    CHECK(near(d.raw_cost_seconds,.016));
    c[0].sequence++; c[1].sequence++;
    c[0].elapsed_seconds = c[1].elapsed_seconds = 5;
    peak_mpi_overhead_policy_update(cadence,c,.1,5,ratio,&d);
    CHECK(ratio[1] > ratio[0] && near(d.raw_cost_seconds,.016));
    peak_mpi_overhead_policy_free(cadence);

    /* A stopped peer HB must not freeze the independently supplied cohort clock. */
    PeakMpiOverheadPolicy *clock = peak_mpi_overhead_policy_create(2);
    CHECK(clock);
    PeakMpiOverheadPolicySample waiting[2] = {
        {1,10,.01,0,0,0}, {1,10,.01,0,0,0}};
    peak_mpi_overhead_policy_update(clock,waiting,.01,10,ratio,&d);
    CHECK(near(10*(ratio[0]+ratio[1]),.1));
    waiting[0].sequence++; waiting[0].elapsed_seconds = 11;
    peak_mpi_overhead_policy_update(clock,waiting,.01,11,ratio,&d);
    CHECK(near(d.common_elapsed_seconds,11));
    CHECK(near(ratio[0]*11+ratio[1]*10,.11));
    waiting[0].module_cpu_seconds = .03;
    peak_mpi_overhead_policy_update(clock,waiting,.01,12,ratio,&d);
    CHECK(near(ratio[0]*11+ratio[1]*10+.03,.12));
    CHECK(near(d.raw_cost_seconds,.05) && d.debt_seconds == 0);
    CHECK(near(d.profiling_headroom_seconds,.07));
    peak_mpi_overhead_policy_free(clock);
    /* Paid work cannot be reissued as another rank's unused normal quota. */
    PeakMpiOverheadPolicy *paid = peak_mpi_overhead_policy_create(2);
    CHECK(paid);
    PeakMpiOverheadPolicySample spent[2] = {
        {1,10,.09,0,0,0}, {1,10,.01,0,0,0}};
    peak_mpi_overhead_policy_update(paid,spent,.01,10,ratio,&d);
    CHECK(near(ratio[0]*10,.09) && near(ratio[1]*10,.01));
    CHECK(near(d.raw_cost_seconds,.1) && near(d.debt_seconds,0));
    spent[1].sequence++; spent[1].profile_seconds = .03;
    peak_mpi_overhead_policy_update(paid,spent,.01,10,ratio,&d);
    CHECK(near(ratio[0]*10,.075) && near(ratio[1]*10,.025));
    CHECK(near(d.raw_cost_seconds,.12) && near(d.debt_seconds,.02));
    CHECK(near(10*(ratio[0]+ratio[1]),.1));
    peak_mpi_overhead_policy_free(paid);
    /* Module-only risk conversion preserves true headroom with REAL local N. */
    PeakMpiOverheadPolicySample risk_ack = {1,10,.02,.01,.002,.001};
    PeakMpiOverheadPolicySample risk_now = risk_ack;
    risk_now.profile_seconds += .003; risk_now.control_seconds += .002;
    risk_now.management_seconds += .001; risk_now.module_cpu_seconds += .002;
    risk_now.elapsed_seconds = 11;
    for (unsigned n = 2; n <= 4; n += 2) {
        PeakMpiOverheadInput target = peak_mpi_overhead_policy_input(
            &risk_now,&risk_ack,.01,1.2,n);
        CHECK(near(target.profile_target*11,.097));
        CHECK(near(target.control_target*11-(risk_now.profile_seconds+n*risk_now.control_seconds),
                   .097-(risk_now.profile_seconds+risk_now.control_seconds)));
    }
    PeakMpiOverheadPolicySample many = {1,100,0,.01,0,0};
    PeakMpiOverheadInput separated = peak_mpi_overhead_policy_input(
        &many,&many,.00001,1.2,128);
    CHECK(near(separated.profile_target,.00001));
    CHECK(near(separated.control_target,.01271));
    return 0;
}
