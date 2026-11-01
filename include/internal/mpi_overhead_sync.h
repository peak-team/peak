#ifndef PEAK_MPI_OVERHEAD_SYNC_H
#define PEAK_MPI_OVERHEAD_SYNC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    uint64_t sequence;
} PeakMpiOverheadSample;

/* Startup application thread only, after the original outer MPI initializer.
 * Runtime helpers never call MPI. Partial loading remains ordinary MAIN;
 * only a validated full cohort enables global feedback. Explicit job endpoint
 * (numeric IPv4:port or [IPv6]:port) and full shared token never add an
 * application collective. Launch setup resolves hostnames before configuration;
 * absent/invalid routing retains MAIN policy without global feedback. */
void peak_mpi_overhead_sync_start(void);

typedef struct {
    double target_ratio;
    bool global_accounting;
} PeakMpiOverheadBudgetInput;

/* Existing HB only. Ratio and accounting units come from ONE coherent ACK
 * decision: validated global accounting uses once-counted control cost; all
 * fallback paths retain configured MAIN ratio and real local multiplicity.
 * No MPI/Gum/controller request/wake; helpers and protocol are unchanged. */
PeakMpiOverheadBudgetInput peak_mpi_overhead_sync_update(const PeakMpiOverheadSample* sample,
                                     double configured_ratio,
                                     double detach_factor,
                                     unsigned local_control_ranks);

#endif /* PEAK_MPI_OVERHEAD_SYNC_H */
