#ifndef PEAK_MPI_PROGRESS_CONTRACT_H
#define PEAK_MPI_PROGRESS_CONTRACT_H

#include "internal/mpi_causal_protocol.h"
#include <stddef.h>

typedef enum {
  PEAK_MPI_PROGRESS_UNKNOWN = 0,
  PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC,
  PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED
} PeakMpiProgressProfile;

typedef struct {
  PeakMpiProgressProfile profile;
  unsigned autonomous_p2p : 1;
  unsigned autonomous_blocking_collective : 1;
  unsigned autonomous_selected_nbc : 1;
  unsigned observed_p2p_completion : 1;
  unsigned observed_buffer_detach : 1;
  unsigned observed_collective_completion : 1;
  const char *name;
  const char *reason;
  int effective_async_progress, provided_thread_level, async_verified;
  char requested_profile[96], fabrics[32], ofi_provider[32], fi_provider[32];
  char async_setting[16];
  char ucx_tls[96], ucx_proto[16], ucx_tm[16], pml[96], coll[128];
  int pml_verified, coll_verified, fixed_rules_verified;
} PeakMpiProgressContract;

typedef struct {
  int effective_async_progress, provided_thread_level;
  int pml_verified, coll_verified, fixed_rules_verified;
  char pml[96], coll[128];
} PeakMpiProgressVerification;
PeakMpiProgressContract
peak_mpi_progress_contract_select_public(const char *version,
                                         const PeakMpiProgressVerification *);
int peak_mpi_progress_contract_collective_allowed(
    const PeakMpiProgressContract *, PeakMpiCausalOperation, unsigned flags,
    int size);
unsigned peak_mpi_progress_contract_result_flags(
    const PeakMpiProgressContract *, PeakMpiCausalOperation, unsigned flags,
    int observed_completion, PeakMpiObserverProgressScope *scope);

/* Selection uses a public MPI library version string and documented public
 * configuration. It never probes private symbols or runs a timing inference.
 * No settings are changed. Unknown versions/configurations stay explicit. */
PeakMpiProgressContract peak_mpi_progress_contract_select(const char *version);
/* effective_async_progress is read once through public MPI_T; -1 is unknown.
 * Native provided thread level is recorded, not upgraded or constrained. */
PeakMpiProgressContract
peak_mpi_progress_contract_select_verified(const char *version,
                                           int effective_async_progress,
                                           int provided_thread_level);
int peak_mpi_progress_contract_supports(const PeakMpiProgressContract *,
                                        PeakMpiCausalOperation operation,
                                        unsigned flags);

#endif
