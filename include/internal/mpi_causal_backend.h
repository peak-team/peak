#ifndef PEAK_MPI_CAUSAL_BACKEND_H
#define PEAK_MPI_CAUSAL_BACKEND_H

#include "internal/mpi_causal_protocol.h"
#include "internal/mpi_cost_model.h"
#include "internal/mpi_policy_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PeakMpiCausalService PeakMpiCausalService;

typedef struct {
  unsigned rank, world_size, owner_rank;
  uint64_t job_context, incarnation;
  const char *address; /* Numeric IPv4 or IPv6; copied by create. */
  unsigned port;
  size_t slots, queue_frames, max_operations, max_context_parts;
  size_t max_pending_records;
  PeakMpiCostConfig model;
  PeakMpiPolicyConfig policy;
} PeakMpiCausalServiceConfig;

typedef struct {
  uint64_t generation, incarnation, control_generation;
  uint64_t watermark_nonce;
  uint64_t idle_group_uid;
  int idle_cut;
  double L, P, elapsed;
  unsigned contract_profile;
  int active, unknown_members, finalized;
  PeakMpiObserverReason coarse_reason;
  uint64_t lost_events;
  /* One byte per static world rank. Entries only change from zero to one.
   * The caller retains storage; tick copies it before returning. */
  const unsigned char *known_peers;
  size_t peer_count;
  /* Frozen producer prefixes at this raw cut; ownership remains with caller. */
  const uint64_t *slot_prefixes;
  size_t slot_count;
} PeakMpiCausalRaw;

typedef struct {
  uint64_t records, duplicates, lost_events, sent_bytes, received_bytes;
  uint64_t reconnects, feedbacks;
  size_t pending_records, pending_peak, active_operations;
  double observer_cpu_seconds;
  double oldest_unresolved_age;
  int connected, listening, unavailable;
} PeakMpiCausalServiceStats;

/* Single heartbeat owner. All user storage is preallocated at create; it
 * performs no socket operation. Only tick binds/connects/reads/writes.
 * The application wrappers never call this module. */
PeakMpiCausalService *
peak_mpi_causal_service_create(const PeakMpiCausalServiceConfig *config);
void peak_mpi_causal_service_destroy(PeakMpiCausalService *service);

/* Tick has explicit byte and model-transition limits. It never waits for
 * socket readiness or a remote cut. Capture storage remains owned by caller.
 * Raw cuts continue after fine collection stops. A dormant owner can serve
 * peers while its own raw.active is false; other ranks exchange nothing until
 * raw.active becomes true. Costs in records are integer nanoseconds. */
void peak_mpi_causal_service_tick(PeakMpiCausalService *service,
                                  PeakMpiObserver *capture,
                                  const PeakMpiCausalRaw *raw, double now,
                                  size_t byte_budget, size_t work_budget);
int peak_mpi_causal_service_feedback(const PeakMpiCausalService *service,
                                     PeakMpiPolicyFeedback *feedback);
uint64_t
peak_mpi_causal_service_requested_nonce(const PeakMpiCausalService *service);
PeakMpiPolicyReason
peak_mpi_causal_service_fault_reason(const PeakMpiCausalService *service);
void peak_mpi_causal_service_stats(const PeakMpiCausalService *service,
                                   PeakMpiCausalServiceStats *stats);

/* These single-owner ingestion functions are also the independent parser
 * test interface. They do not imply that a transport or MPI contract exists.
 * Incarnation changes, sequence gaps and capacities are sticky uncertainty. */
int peak_mpi_causal_service_ingest_object(PeakMpiCausalService *service,
                                          unsigned rank, uint64_t incarnation,
                                          const PeakMpiObserverObject *object);
int peak_mpi_causal_service_ingest_record(PeakMpiCausalService *service,
                                          unsigned rank,
                                          const PeakMpiObserverRecord *record);
int peak_mpi_causal_service_ingest_raw(PeakMpiCausalService *service,
                                       unsigned rank,
                                       const PeakMpiCausalRaw *raw, double now);

#ifdef __cplusplus
}
#endif
#endif /* PEAK_MPI_CAUSAL_BACKEND_H */
