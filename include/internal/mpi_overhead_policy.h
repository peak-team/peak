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
typedef struct {
    double profile_target, control_target;
} PeakMpiOverheadInput;

/* Reexpress acknowledged normal P+C quota, less fresh local M/T once, in
 * MAIN's original P+N*C risk units by adding only (N-1)*current C. No P addback,
 * no P/C second debit or F division. Profile target stays quota/E; only the
 * reattach input adds control risk. True headroom remains quota-(P+C).
 * MAIN keeps actual N, F/a gates and future N-scaled forecasts unchanged.
 * No unacknowledged time credit or hard-lease claim; F remains API-compatible. */
PeakMpiOverheadInput peak_mpi_overhead_policy_input(const PeakMpiOverheadPolicySample *current,
    const PeakMpiOverheadPolicySample *acknowledged, double acknowledged_raw_ratio,
    double detach_factor, unsigned local_ranks);
#endif /* PEAK_MPI_OVERHEAD_POLICY_H */
