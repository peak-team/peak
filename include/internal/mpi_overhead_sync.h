#ifndef PEAK_MPI_OVERHEAD_SYNC_H
#define PEAK_MPI_OVERHEAD_SYNC_H

#include <stdint.h>

typedef struct {
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    uint64_t sequence;
} PeakMpiOverheadSample;

/* Startup application thread only, after the original outer MPI initializer.
 * Runtime helpers never call MPI. All world ranks must load PEAK. */
void peak_mpi_overhead_sync_start(void);

/* Existing heartbeat only: coalesce the latest cumulative snapshot and read
 * rank-budget telemetry. The existing local-control multiplier is input only;
 * ACK-priced headroom receives no local elapsed-time credit. No MPI, Gum,
 * controller requests, or helper wake. */
double peak_mpi_overhead_sync_update(const PeakMpiOverheadSample* sample,
                                     double configured_ratio,
                                     double detach_factor,
                                     unsigned local_control_ranks);

#endif
