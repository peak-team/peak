#ifndef PEAK_MPI_OVERHEAD_POLICY_H
#define PEAK_MPI_OVERHEAD_POLICY_H
#include <stddef.h>
#include <stdint.h>
/* All times are cumulative seconds. module_cpu is disjoint from P/C/M.
 * Call with a complete rank vector; stale ranks hold back common elapsed. */
typedef struct {
    uint64_t sequence;
    double elapsed_seconds, profile_seconds, control_seconds;
    double management_seconds, module_cpu_seconds;
} PeakMpiOverheadPolicySample;
typedef struct {
    double common_elapsed_seconds, raw_cost_seconds, module_cpu_seconds;
    double budget_seconds, debt_seconds, profiling_headroom_seconds;
} PeakMpiOverheadDecision;
typedef struct PeakMpiOverheadPolicy PeakMpiOverheadPolicy;
PeakMpiOverheadPolicy *peak_mpi_overhead_policy_create(size_t count);
void peak_mpi_overhead_policy_free(PeakMpiOverheadPolicy *policy);
/* Samples must be cumulative and nondecreasing. Ratios are cumulative local
 * P/C/M targets. Before all ranks have elapsed data use .9*q/count. The policy
 * retains demand across idle phases; caller retains cumulative cost history.
 * Conservative prototype only: no claim of parallel coverage or slowdown. */
void peak_mpi_overhead_policy_update(PeakMpiOverheadPolicy *policy,
    const PeakMpiOverheadPolicySample *samples, double q, double *ratios,
    PeakMpiOverheadDecision *decision);
/* Translate an acknowledged raw P/C/M allowance into the existing listener's
 * P+local_ranks*C risk units. Only acknowledged unused spend is available:
 * later elapsed time creates no credit until acknowledged. Current cost growth,
 * including disjoint module CPU, consumes that allowance once. detach_factor
 * discounts only NEW spend; already-paid risk is not divided by it. This scalar
 * also affects detach decisions and requires measured duty/coverage validation. */
double peak_mpi_overhead_policy_input(const PeakMpiOverheadPolicySample *current,
    const PeakMpiOverheadPolicySample *acknowledged, double acknowledged_raw_ratio,
    double detach_factor, unsigned local_ranks);
#endif /* PEAK_MPI_OVERHEAD_POLICY_H */
