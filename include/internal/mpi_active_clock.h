#ifndef PEAK_MPI_ACTIVE_CLOCK_H
#define PEAK_MPI_ACTIVE_CLOCK_H

/* Application thread, after successful outer MPI initialization and PEAK's
 * runtime activation. No application collective is added. */
void peak_mpi_active_clock_start(double runtime_start);

/* Existing heartbeat, using PEAK's monotonic clock. A negative reading means
 * the snapshot raced a publication or the process has no eligible rank. */
double peak_mpi_active_clock_blocked_seconds(double runtime_start, double now);

#endif
