#ifndef PEAK_MPI_OVERHEAD_POLICY_H
#define PEAK_MPI_OVERHEAD_POLICY_H
#include <stddef.h>
#include <stdint.h>
/* All times are cumulative seconds. module_cpu is disjoint from P/C/M.
 * Call with a complete rank vector. The admitted cohort reference clock is
 * supplied independently; individual elapsed times still define local rates. */
typedef struct {
    uint64_t sequence;
    double elapsed_seconds, profile_seconds, control_seconds;
    double management_seconds, module_cpu_seconds;
} PeakMpiOverheadPolicySample;
typedef struct {
    double common_elapsed_seconds, raw_cost_seconds, module_cpu_seconds;
    double budget_seconds, debt_seconds, profiling_headroom_seconds;
    double committed_total_seconds, unspent_reservation_seconds;
} PeakMpiOverheadDecision;
typedef struct PeakMpiOverheadPolicy PeakMpiOverheadPolicy;
PeakMpiOverheadPolicy *peak_mpi_overhead_policy_create(size_t count);
void peak_mpi_overhead_policy_free(PeakMpiOverheadPolicy *policy);
/* Full actual P/C/M/T history and debt are retained. Ratios represent weighted
 * CURRENT cumulative allowed P+C quotas after full actual M/T, with no fixed reserve.
 * Paid P+C is charged first; only remaining quota is weighted. Global P+C
 * overdraw reduces local quotas proportionally. Legacy decision fields describe
 * current quota+M/T and actual spare, never hard leases or issuance/reservation.
 * Main compares local risk against the resulting quota.
 * Demand rates and explicit cohort reference retain their existing semantics. */
void peak_mpi_overhead_policy_update(PeakMpiOverheadPolicy *policy,
    const PeakMpiOverheadPolicySample *samples, double q,
    double budget_elapsed_seconds, double *ratios,
    PeakMpiOverheadDecision *decision);
/* Return acknowledged cumulative P+C quota/current elapsed, subtracting fresh
 * local M/T once. P/C are compared by main, not debited again here. F/N remain
 * API-compatible unused inputs: main applies their unchanged risk/hysteresis.
 * No unacknowledged elapsed-time credit; sampled feedback is not a hard lease. */
double peak_mpi_overhead_policy_input(const PeakMpiOverheadPolicySample *current,
    const PeakMpiOverheadPolicySample *acknowledged, double acknowledged_raw_ratio,
    double detach_factor, unsigned local_ranks);
#endif /* PEAK_MPI_OVERHEAD_POLICY_H */
