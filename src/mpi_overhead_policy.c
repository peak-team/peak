#include "internal/mpi_overhead_policy.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
    uint64_t sequence;
    double previous_profile_control, previous_elapsed, demand_rate_high_water;
} RankPolicy;
struct PeakMpiOverheadPolicy { size_t count; RankPolicy ranks[]; };
PeakMpiOverheadPolicy *peak_mpi_overhead_policy_create(size_t count)
{
    if (!count || count > (SIZE_MAX-sizeof(PeakMpiOverheadPolicy))/sizeof(RankPolicy)) return NULL;
    PeakMpiOverheadPolicy *p=calloc(1,sizeof(*p)+count*sizeof(RankPolicy));
    if (p) p->count=count;
    return p;
}
void peak_mpi_overhead_policy_free(PeakMpiOverheadPolicy *policy) { free(policy); }

void peak_mpi_overhead_policy_update(PeakMpiOverheadPolicy *policy,
    const PeakMpiOverheadPolicySample *samples, double q,
    double budget_elapsed_seconds, double *ratios,
    PeakMpiOverheadDecision *decision)
{
    size_t count=policy->count;
    RankPolicy *ranks=policy->ranks;
    memset(decision, 0, sizeof(*decision));
    double elapsed = budget_elapsed_seconds, cost = 0, module = 0, demand = 0, management = 0, paid_profile_control = 0;
    size_t active = 0;
    double active_demand = 0;
    for (size_t i = 0; i < count; ++i) {
        const PeakMpiOverheadPolicySample *s = &samples[i];
        RankPolicy *r = &ranks[i];
        double pc = s->profile_seconds + s->control_seconds;
        if (s->sequence > r->sequence) {
            double increment = pc - r->previous_profile_control;
            double interval = s->elapsed_seconds - r->previous_elapsed;
            if (interval > 0) {
                double rate = fmax(0, increment) / interval;
                if (rate > r->demand_rate_high_water) r->demand_rate_high_water = rate;
            }
            r->previous_profile_control = pc;
            r->previous_elapsed = s->elapsed_seconds;
            r->sequence = s->sequence;
        }
        if (pc > 0) {
            ++active;
            active_demand += r->demand_rate_high_water;
        }
        demand += r->demand_rate_high_water;
        double local_cost = pc + s->management_seconds;
        cost += local_cost;
        paid_profile_control += pc;
        management += s->management_seconds;
        module += s->module_cpu_seconds;
    }
    decision->common_elapsed_seconds = elapsed;
    decision->raw_cost_seconds = cost + module;
    decision->module_cpu_seconds = module;
    decision->budget_seconds = q * elapsed;
    decision->debt_seconds = fmax(0, cost + module - q * elapsed);
    /* Current cumulative P+C quota; every actual M/T cost remains charged. */
    double quota = fmax(0, q * elapsed - management - module);
    /* Normalize the existing weights only for NEW spare; paid history stays. */
    double active_weight = demand > 0
        ? .5 * active / count + .5 * active_demand / demand
        : (double)active / count;
    decision->profiling_headroom_seconds = fmax(0, quota - paid_profile_control);
    for (size_t i = 0; i < count; ++i) {
        double weight = demand > 0 ? .5 / count + .5 * ranks[i].demand_rate_high_water / demand
                                   : 1.0 / count;
        double paid = samples[i].profile_seconds + samples[i].control_seconds;
        if (active) weight = paid > 0 ? weight / active_weight : 0;
        double proposal = quota >= paid_profile_control
            ? paid + weight * (quota - paid_profile_control)
            : paid * (quota / paid_profile_control);
        ratios[i] = elapsed <= 0 ? q / count :
            proposal / samples[i].elapsed_seconds;
    }
    /* Legacy field names report current quota+M/T and current actual spare. */
    decision->committed_total_seconds = quota + management + module;
    decision->unspent_reservation_seconds = decision->profiling_headroom_seconds;
}

PeakMpiOverheadInput peak_mpi_overhead_policy_input(const PeakMpiOverheadPolicySample *current,
    const PeakMpiOverheadPolicySample *acknowledged, double acknowledged_raw_ratio,
    double detach_factor, unsigned local_ranks)
{
    /* Main applies its own P+N*C risk, F/a hysteresis and quote deductions. */
    (void)detach_factor;
    double fresh_runtime = fmax(0,
        current->management_seconds + current->module_cpu_seconds -
        acknowledged->management_seconds - acknowledged->module_cpu_seconds);
    double quota = fmax(0, acknowledged_raw_ratio * acknowledged->elapsed_seconds -
                           fresh_runtime);
    if (quota <= 0 || current->elapsed_seconds <= 0)
        return (PeakMpiOverheadInput){0, 0};
    double extra_control_risk = local_ranks > 1
        ? (double)(local_ranks - 1) * current->control_seconds : 0;
    return (PeakMpiOverheadInput){quota / current->elapsed_seconds,
        (quota + extra_control_risk) / current->elapsed_seconds};
}

/* Ordinary MAIN fallback returns its exact ratio. Full observation leases
 * remain unscaled; for K >= 1, existing deductions fund the unscaled forecast
 * within normal headroom. */
double peak_mpi_overhead_next_window_target(double normal_target,
    double elapsed_seconds, double fresh_control_seconds,
    double last_stop_seconds, unsigned local_ranks, int coordinated)
{
    if (!coordinated || normal_target == 0 || local_ranks == 1)
        return normal_target;
    return normal_target + (double)(local_ranks - 1) *
        (fresh_control_seconds + last_stop_seconds) / elapsed_seconds;
}
