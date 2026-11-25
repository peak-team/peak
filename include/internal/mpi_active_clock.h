#ifndef PEAK_MPI_ACTIVE_CLOCK_H
#define PEAK_MPI_ACTIVE_CLOCK_H

#include <stdatomic.h>
#include <stdint.h>

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

enum {
    PEAK_MPI_EPOCH_FAILED,
    PEAK_MPI_EPOCH_BARRIER,
    PEAK_MPI_EPOCH_ALLREDUCE,
    PEAK_MPI_EPOCH_MAX_ATTEMPTS = 5
};

/* Read by the MPI owner. The listener mutex protects the corresponding state. */
extern _Atomic uint64_t peak_mpi_epoch_armed;
void peak_general_listener_mpi_epoch_completed(uint64_t nomination,
                                              unsigned attempt, unsigned kind,
                                              double now);

#endif /* PEAK_MPI_ACTIVE_CLOCK_H */
