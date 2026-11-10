#include "internal/mpi_progress_contract.h"
#include <assert.h>
int main(void) {
  PeakMpiProgressContract c = {0};
  PeakMpiObserverProgressScope scope;
  assert(!peak_mpi_progress_contract_estimated_delivery(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED));
  c.profile = PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED;
  c.observed_p2p_completion = 1;
  assert(peak_mpi_progress_contract_estimated_delivery(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED));
  unsigned f = peak_mpi_progress_contract_result_flags(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED, 1, &scope);
  assert(f == PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED &&
         scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL);
  assert(!peak_mpi_progress_contract_estimated_delivery(
      &c, PEAK_MPI_CAUSAL_SEND, 0));
  c.profile = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
  c.progress_estimate = 1;
  assert(peak_mpi_progress_contract_supports(&c, PEAK_MPI_CAUSAL_BSEND,
                                             PEAK_MPI_CAUSAL_BUFFERED));
  assert(peak_mpi_progress_contract_estimated_delivery(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED));
  f = peak_mpi_progress_contract_result_flags(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED, 1, &scope);
  assert(f == PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED &&
         scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL);
  c.progress_estimate = 0;
  c.profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
  c.autonomous_p2p = 1;
  assert(!peak_mpi_progress_contract_estimated_delivery(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED));
  f = peak_mpi_progress_contract_result_flags(
      &c, PEAK_MPI_CAUSAL_BSEND, PEAK_MPI_CAUSAL_BUFFERED, 1, &scope);
  assert(f == PEAK_MPI_OBSERVER_PROGRESS_SEALED &&
         scope == PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS);
}
