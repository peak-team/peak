#ifndef PEAK_MPI_CAUSAL_COST_H
#define PEAK_MPI_CAUSAL_COST_H

#include <stddef.h>

/* Startup-only allocation; callback slots are retained until process exit. */
int peak_mpi_causal_cost_prepare(size_t slots);
void peak_mpi_causal_cost_observer_add(size_t slot, double seconds);
size_t peak_mpi_causal_cost_used_slots(void);
/* Startup calibration only; does not publish or change the scan bound. */
int peak_mpi_causal_cost_read_prefix(size_t count, double *profile,
                                     double *observer);
int peak_mpi_causal_cost_read(double *profile_seconds,
                              double *observer_seconds);

#endif /* PEAK_MPI_CAUSAL_COST_H */
