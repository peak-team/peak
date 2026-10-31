#include "internal/mpi_overhead_policy.h"
#include <stdio.h>
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (0)
#include <math.h>
static int near(double a, double b) { return fabs(a-b) < 1e-12; }
int main(void)
{
    PeakMpiOverheadPolicy *state=peak_mpi_overhead_policy_create(2);
    CHECK(state);
    PeakMpiOverheadPolicySample s[2] = {0};
    PeakMpiOverheadDecision d;
    double ratio[2];
    peak_mpi_overhead_policy_update(state,s,.01,ratio,&d);
    CHECK(near(ratio[0],.0045) && near(ratio[1],.0045));
    s[0]=(PeakMpiOverheadPolicySample){1,10,.02,0,0,.001};
    s[1]=(PeakMpiOverheadPolicySample){1,10,.002,0,0,.001};
    peak_mpi_overhead_policy_update(state,s,.01,ratio,&d);
    CHECK(ratio[0]>ratio[1] && ratio[1]>0);
    CHECK(near(10*(ratio[0]+ratio[1]),.09));
    /* A later phase moves expensive work to rank1; old rank0 cost remains paid. */
    s[0].sequence=s[1].sequence=2;
    s[0].elapsed_seconds=s[1].elapsed_seconds=20;
    s[1].profile_seconds=.052;
    peak_mpi_overhead_policy_update(state,s,.01,ratio,&d);
    CHECK(ratio[1]>ratio[0]);
    CHECK(near(d.raw_cost_seconds,.074));
    double before=d.profiling_headroom_seconds;
    s[0].module_cpu_seconds=.15;
    peak_mpi_overhead_policy_update(state,s,.01,ratio,&d);
    CHECK(d.profiling_headroom_seconds<before);
    CHECK(near(d.raw_cost_seconds,.223) && near(d.debt_seconds,.023));
    CHECK(ratio[0]==0 && ratio[1]==0);
    /* Idle time repays debt without resetting cumulative history or demand. */
    s[0].elapsed_seconds=40; s[1].elapsed_seconds=20;
    peak_mpi_overhead_policy_update(state,s,.01,ratio,&d);
    CHECK(near(d.common_elapsed_seconds,20) && near(d.debt_seconds,.023));
    s[1].elapsed_seconds=40;
    peak_mpi_overhead_policy_update(state,s,.01,ratio,&d);
    CHECK(near(d.raw_cost_seconds,.223) && d.debt_seconds==0);
    CHECK(ratio[0]>0 && ratio[1]>ratio[0]);
    /* Measured rank0 from the positive-global-headroom/default-cooldown run.
     * A raw allowance divided wholesale by F rejected already-paid control risk. */
    PeakMpiOverheadPolicySample ack = {509,100.219711633,.033194604,
        .089991024,.003757457,.019064261};
    PeakMpiOverheadPolicySample current = ack;
    double raw_ratio=.00237390694, factor=1.2;
    double risk=ack.profile_seconds+2*ack.control_seconds;
    double allowance=raw_ratio*ack.elapsed_seconds-ack.profile_seconds-
        ack.control_seconds-ack.management_seconds;
    CHECK(allowance>0);
    CHECK(risk>.85*(raw_ratio/factor)*ack.elapsed_seconds);
    double input=peak_mpi_overhead_policy_input(&current,&ack,raw_ratio,factor,2);
    CHECK(near(input*current.elapsed_seconds-risk,allowance/factor));
    CHECK(risk<=.85*input*current.elapsed_seconds);
    /* Local helper CPU consumes acknowledged headroom once, not again in risk. */
    current.module_cpu_seconds+=allowance/2;
    current.elapsed_seconds+=10;
    input=peak_mpi_overhead_policy_input(&current,&ack,raw_ratio,factor,2);
    CHECK(near(input*current.elapsed_seconds-risk,allowance/(2*factor)));
    current.module_cpu_seconds+=allowance;
    CHECK(peak_mpi_overhead_policy_input(&current,&ack,raw_ratio,factor,2)==0);
    /* Equal stationary rates must receive equal NEW headroom despite records
     * arriving at different cadences. Both final clocks and costs are equal. */
    PeakMpiOverheadPolicy *cadence=peak_mpi_overhead_policy_create(2);
    CHECK(cadence);
    PeakMpiOverheadPolicySample constant[2] = {
        {1,1,.001,0,0,0}, {1,1,.001,0,0,0}};
    peak_mpi_overhead_policy_update(cadence,constant,.1,ratio,&d);
    constant[0]=(PeakMpiOverheadPolicySample){2,2,.002,0,0,0};
    peak_mpi_overhead_policy_update(cadence,constant,.1,ratio,&d);
    constant[0]=(PeakMpiOverheadPolicySample){3,3,.003,0,0,0};
    constant[1]=(PeakMpiOverheadPolicySample){2,3,.003,0,0,0};
    peak_mpi_overhead_policy_update(cadence,constant,.1,ratio,&d);
    CHECK(near(ratio[0]*3-.003,d.profiling_headroom_seconds/2));
    CHECK(near(ratio[1]*3-.003,d.profiling_headroom_seconds/2));
    CHECK(near(d.raw_cost_seconds,.006) && d.debt_seconds==0);
    peak_mpi_overhead_policy_free(cadence);
    peak_mpi_overhead_policy_free(state);
    return 0;
}
