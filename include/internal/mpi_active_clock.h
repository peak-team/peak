#ifndef PEAK_MPI_ACTIVE_CLOCK_H
#define PEAK_MPI_ACTIVE_CLOCK_H

/* Application thread, after successful outer MPI initialization and PEAK's
 * runtime activation. No application collective is added. */
void peak_mpi_active_clock_start(double runtime_start);

#define PEAK_MPI_CLOCK_UNAVAILABLE (-2.0)

/* Cumulative residence at the updated wall cut, or a negative result without
 * changing the cut. Unavailable clocks use the ordinary wall policy. Other
 * misses carry the previous allowance.
 */
double peak_mpi_active_clock_blocked_seconds(double runtime_start, double* now);

/* Occupancy at the atomic load. A negative result means the clock is
 * unavailable for this rank or runtime epoch. */
int peak_mpi_active_clock_peek(double runtime_start);

/* The sampled clock's advertised resolution, after a valid observation. */
double peak_mpi_active_clock_quantum_seconds(void);

#endif /* PEAK_MPI_ACTIVE_CLOCK_H */
