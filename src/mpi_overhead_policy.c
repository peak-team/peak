#include "internal/mpi_overhead_policy.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
    uint64_t sequence;
    double previous_profile_control, previous_elapsed, demand_rate_high_water, cumulative_cap;
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
    const PeakMpiOverheadPolicySample *samples, double q, double *ratios,
    PeakMpiOverheadDecision *decision)
{
    size_t count=policy->count;
    RankPolicy *ranks=policy->ranks;
    memset(decision, 0, sizeof(*decision));
    double elapsed = samples[0].elapsed_seconds, cost = 0, module = 0, demand = 0, committed = 0;
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
        demand += r->demand_rate_high_water;
        elapsed = fmin(elapsed, s->elapsed_seconds);
        double local_cost = pc + s->management_seconds;
        cost += local_cost;
        r->cumulative_cap = fmax(r->cumulative_cap, local_cost);
        committed += r->cumulative_cap;
        module += s->module_cpu_seconds;
    }
    decision->common_elapsed_seconds = elapsed;
    decision->raw_cost_seconds = cost + module;
    decision->module_cpu_seconds = module;
    decision->budget_seconds = q * elapsed;
    decision->debt_seconds = fmax(0, cost + module - q * elapsed);
    /* Actual transport costs always count, including costs beyond its 10% share. */
    double headroom = fmax(0, fmin(.9 * q * elapsed - committed,
                                 q * elapsed - committed - module));
    bool suspended = committed + module > q * elapsed;
    decision->profiling_headroom_seconds = headroom;
    for (size_t i = 0; i < count; ++i) {
        double weight = demand > 0 ? .5 / count + .5 * ranks[i].demand_rate_high_water / demand
                                   : 1.0 / count;
        double local_cost = samples[i].profile_seconds + samples[i].control_seconds +
                            samples[i].management_seconds;
        ranks[i].cumulative_cap += headroom * weight;
        ratios[i] = elapsed <= 0 ? .9 * q / count :
            suspended || ranks[i].cumulative_cap <= local_cost ? 0 :
            ranks[i].cumulative_cap / samples[i].elapsed_seconds;
    }
    decision->committed_total_seconds = committed + headroom + module;
    decision->unspent_reservation_seconds = committed + headroom - cost;
}

double peak_mpi_overhead_policy_input(const PeakMpiOverheadPolicySample *current,
    const PeakMpiOverheadPolicySample *acknowledged, double acknowledged_raw_ratio,
    double detach_factor, unsigned local_ranks)
{
    double acknowledged_cost = acknowledged->profile_seconds +
        acknowledged->control_seconds + acknowledged->management_seconds;
    double current_cost = current->profile_seconds + current->control_seconds +
        current->management_seconds;
    double allowance = fmax(0, acknowledged_raw_ratio * acknowledged->elapsed_seconds -
        acknowledged_cost);
    double spent_delta = fmax(0, current_cost + current->module_cpu_seconds -
        acknowledged_cost - acknowledged->module_cpu_seconds);
    double remaining = fmax(0, allowance - spent_delta);
    if (remaining <= 0 || current->elapsed_seconds <= 0) return 0;
    double risk = current->profile_seconds + local_ranks * current->control_seconds;
    return (risk + remaining / fmax(1, detach_factor)) / current->elapsed_seconds;
}
