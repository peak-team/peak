#ifndef PEAK_MPI_CAUSAL_RECOVERY_H
#define PEAK_MPI_CAUSAL_RECOVERY_H
#include <stdint.h>
enum {
  PEAK_MPI_RECOVERY_NONE,
  PEAK_MPI_RECOVERY_QUERY,
  PEAK_MPI_RECOVERY_READY,
  PEAK_MPI_RECOVERY_ACK
};
/* Passive heartbeat metadata; never an MPI/application control operation. */
typedef struct {
  uint64_t epoch, state, comm, ordinal, operation, uid_floor;
  uint64_t loss, fault_mask;
  double L, P, elapsed;
} PeakMpiCausalRecovery;
#endif /* PEAK_MPI_CAUSAL_RECOVERY_H */
