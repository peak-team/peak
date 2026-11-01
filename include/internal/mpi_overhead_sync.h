#ifndef PEAK_MPI_OVERHEAD_SYNC_H
#define PEAK_MPI_OVERHEAD_SYNC_H

#include <stdint.h>
#include "internal/mpi_overhead_policy.h"

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

/* Existing HB publishes snapshots and reads a coherent coordinator quota.
 * Profile/cadence and reattach risk inputs share ONE validated ACK;
 * actual local rank identity, risk arithmetic and future forecasts are unchanged.
 * No MPI/Gum/controller requests/wake. Fallback retains configured MAIN ratio
 * for both inputs. */
PeakMpiOverheadInput peak_mpi_overhead_sync_update(const PeakMpiOverheadSample* sample,
                                     double configured_ratio,
                                     double detach_factor,
                                     unsigned local_control_ranks);

#endif /* PEAK_MPI_OVERHEAD_SYNC_H */
