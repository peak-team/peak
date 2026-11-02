#ifndef PEAK_MPI_CAUSAL_EVENTS_H
#define PEAK_MPI_CAUSAL_EVENTS_H

#include "internal/mpi_causal_protocol.h"
#include "internal/mpi_progress_contract.h"
#include <stddef.h>

/* Called by existing PEAK MPI lifecycle after successful native init; only
 * public, local object/version queries are performed. No transport here.
 * Startup fixed capacities: PEAK_MPI_REQUEST_CAPACITY
 * default4096/range16..65536; PEAK_MPI_COMPLETION_CAPACITY
 * default64/range1..1024 per registered thread. Larger completion calls still
 * execute unchanged and explicitly coarsen. */
void peak_mpi_causal_events_init(void);
void peak_mpi_causal_events_finalize_begin(void);
typedef int (*PeakMpiCausalIdleCapture)(uint64_t previous_closed_group,
                                        void *data);
/* One trylock; callback may only freeze local cuts/acquire producer prefixes
 * into caller-owned fixed storage. No MPI/network/control. No progress seal. */
int peak_mpi_causal_events_idle_cut(PeakMpiCausalIdleCapture, void *data);
const PeakMpiProgressContract *peak_mpi_causal_events_contract(void);

/* Implemented by the runtime bridge. submit fills cost/control/thread cuts.
 * Object definitions contain a frontend-local UID in CommPart.local_uid;
 * observer slab UIDs are separate immutable transport definition identities. */
int peak_mpi_causal_runtime_enabled(void);
/* Sticky-coarse retains cached communicator/actual peer discovery only. New
 * wildcard requests or untracked activations mark membership unknown. */
int peak_mpi_causal_runtime_capture_enabled(void);
typedef enum {
  PEAK_MPI_CAUSAL_WORK_LOOKUP,
  PEAK_MPI_CAUSAL_WORK_POLL,
  PEAK_MPI_CAUSAL_WORK_COMPLETION,
  PEAK_MPI_CAUSAL_WORK_COUNT
} PeakMpiCausalWorkKind;
typedef struct {
  double seconds[PEAK_MPI_CAUSAL_WORK_COUNT];
  double lookup_fixed_seconds;
} PeakMpiCausalWorkPrices;
/* Startup only: local representative work including callback dispatch; no
 * application communication or synthetic contract/event is generated. */
void peak_mpi_causal_events_calibrate_work(PeakMpiCausalWorkPrices *);
void peak_mpi_causal_runtime_observer_cost(double actual_seconds);
void peak_mpi_causal_runtime_observer_work(PeakMpiCausalWorkKind,
                                           size_t actual_count);
/* Sample before an MPI call without publishing a semantic event. Required for
 * failed polling calls, which must add cost but no dependency. */
void peak_mpi_causal_runtime_boundary(PeakMpiObserverRecord *);
void peak_mpi_causal_runtime_submit(PeakMpiObserverRecord *,
                                    int actual_interprocess);
void peak_mpi_causal_runtime_define(const PeakMpiObserverObject *);
void peak_mpi_causal_runtime_fail(PeakMpiObserverReason);
uint64_t peak_mpi_causal_runtime_operation_id(void);
size_t peak_mpi_causal_runtime_thread_capacity(void);
int peak_mpi_causal_runtime_thread_token(size_t *slot, uint64_t *generation);
void peak_mpi_causal_runtime_note_peer(int world_rank);
void peak_mpi_causal_runtime_note_members(const int32_t *members, size_t count);
int peak_mpi_causal_runtime_internal_enter(void);
void peak_mpi_causal_runtime_internal_leave(int previous);

#endif
