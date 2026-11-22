#ifndef PEAK_MPI_ACTIVE_CLOCK_H
#define PEAK_MPI_ACTIVE_CLOCK_H

#include <stdbool.h>

/* Application thread, after successful outer MPI initialization and PEAK's
 * runtime activation. No application collective is added. */
void peak_mpi_active_clock_start(double runtime_start);

/* Update the heartbeat's wall cut and return cumulative estimated MPI
 * residence. A negative result leaves the caller's cut unchanged.
 */
double peak_mpi_active_clock_blocked_seconds(double runtime_start, double* now,
                                             unsigned expected_us, bool* saturated);

#endif /* PEAK_MPI_ACTIVE_CLOCK_H */
