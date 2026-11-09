#ifndef PEAK_MPI_CAUSAL_RECOVERY_H
#define PEAK_MPI_CAUSAL_RECOVERY_H
#include <stdint.h>
#include <float.h>
#include <math.h>

typedef struct { uint64_t P_ns, A_ns, L_ns; } PeakMpiCostCut;
static inline double peak_mpi_cost_cut_seconds(uint64_t n) { return (double)n / 1e9; }
/* Each cut sums separately truncated nonnegative components: P, observer,
 * startup, control, and heartbeat. Four/five truncations lose <4/<5 ns;
 * the relative term bounds binary64 summation/conversion rounding. This
 * validates metadata only; it never changes or clamps a paid cost. */
static inline int peak_mpi_cost_cut_valid(PeakMpiCostCut c, double P, double A, double L) {
  return isfinite(P) && isfinite(A) && isfinite(L) && P >= 0 && P <= A && A <= L &&
      c.P_ns <= c.A_ns && c.A_ns <= c.L_ns &&
      fabs(peak_mpi_cost_cut_seconds(c.P_ns)-P) <= 1e-9 + 2*DBL_EPSILON*P &&
      fabs(peak_mpi_cost_cut_seconds(c.A_ns)-A) <= 4e-9 + 8*DBL_EPSILON*A &&
      fabs(peak_mpi_cost_cut_seconds(c.L_ns)-L) <= 5e-9 + 10*DBL_EPSILON*L;
}
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
  double A;
  uint64_t app_cost_known;
  PeakMpiCostCut cut;
} PeakMpiCausalRecovery;
#endif /* PEAK_MPI_CAUSAL_RECOVERY_H */
