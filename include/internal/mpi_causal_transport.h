#ifndef PEAK_MPI_CAUSAL_TRANSPORT_H
#define PEAK_MPI_CAUSAL_TRANSPORT_H

#include "internal/mpi_causal_backend.h"

typedef struct PeakMpiCausalTransport PeakMpiCausalTransport;
typedef struct {
  int (*object)(void *, unsigned, uint64_t, const PeakMpiObserverObject *);
  int (*record)(void *, unsigned, const PeakMpiObserverRecord *);
  int (*raw)(void *, unsigned, const PeakMpiCausalRaw *, double);
  int (*feedback)(void *, unsigned, double, PeakMpiPolicyFeedback *);
  void (*fault)(void *, unsigned, PeakMpiPolicyReason);
  void *context;
  /* Monotonic parser request for a subsequent producer-prefix snapshot. */
  uint64_t (*request_nonce)(void *, unsigned);
} PeakMpiCausalTransportSink;

PeakMpiCausalTransport *
peak_mpi_causal_transport_create(const PeakMpiCausalServiceConfig *,
                                 const PeakMpiCausalTransportSink *);
void peak_mpi_causal_transport_destroy(PeakMpiCausalTransport *);
size_t peak_mpi_causal_transport_tick(PeakMpiCausalTransport *, PeakMpiObserver *,
                                    const PeakMpiCausalRaw *, double now,
                                    size_t byte_budget);
const PeakMpiCausalRaw *peak_mpi_causal_transport_observation(const PeakMpiCausalTransport *);
int peak_mpi_causal_transport_feedback(const PeakMpiCausalTransport *,
                                       PeakMpiPolicyFeedback *);
uint64_t
peak_mpi_causal_transport_requested_nonce(const PeakMpiCausalTransport *);
void peak_mpi_causal_transport_stats(const PeakMpiCausalTransport *,
                                     PeakMpiCausalServiceStats *);

#endif /* PEAK_MPI_CAUSAL_TRANSPORT_H */
