#ifndef PEAK_MPI_CAUSAL_BATCHES_H
#define PEAK_MPI_CAUSAL_BATCHES_H
#include "internal/mpi_causal_protocol.h"
typedef struct PeakMpiCausalBatches PeakMpiCausalBatches;
typedef struct {
  unsigned rank;
  uint64_t group_uid;
  size_t native_count, record_count;
  int concurrent;
  const PeakMpiObserverRecord *begin, *end;
} PeakMpiCausalBatchView;
/* Single parser owner. Ingest follows contiguous per-slot sequence dedup;
 * native END therefore proves its earlier same-slot subrecords are present.
 * All storage allocated at create; capacity full returns backpressure. */
PeakMpiCausalBatches *peak_mpi_causal_batches_create(size_t world,
                                                     size_t capacity);
/* Configured producer slots enable O(slots) oldest-pending prefix queries. */
PeakMpiCausalBatches *
peak_mpi_causal_batches_create_with_slots(size_t world, size_t slots,
                                          size_t capacity);
void peak_mpi_causal_batches_destroy(PeakMpiCausalBatches *);
void peak_mpi_causal_batches_reset_begin(PeakMpiCausalBatches *);
int peak_mpi_causal_batches_reset_work(PeakMpiCausalBatches *, size_t,
                                       size_t *);
int peak_mpi_causal_batches_ingest(PeakMpiCausalBatches *, unsigned rank,
                                   const PeakMpiObserverRecord *);
/* Every plan-list and retirement transition uses one work unit; metadata
 * index operations have a fixed 64-probe bound.
 * 1: coherent plan ready, 0: incomplete/work exhausted, -1: invalid group.
 * Plan remains immutable until commit; errors remain visible until destroy. */
int peak_mpi_causal_batches_prepare_next(PeakMpiCausalBatches *,
                                         size_t work_budget,
                                         PeakMpiCausalBatchView *);
/* Reports every build/retirement transition actually consumed. A ready view
 * is immutable and returns with used=0 until commit. */
int peak_mpi_causal_batches_prepare_next_work(PeakMpiCausalBatches *,
                                              size_t work_budget,
                                              PeakMpiCausalBatchView *,
                                              size_t *work_used);
const PeakMpiObserverRecord *
peak_mpi_causal_batches_record_at(const PeakMpiCausalBatches *, size_t index);
/* Native END metadata is already immutable when a closed plan is READY.
 * This bounded lookup does not advance cost or alter phase ordering. */
const PeakMpiObserverRecord *
peak_mpi_causal_batches_call_end(const PeakMpiCausalBatches *, unsigned rank,
                                 uint64_t op_uid);
/* O(1): starts group-owned retirement; the next prepare call pays its work. */
void peak_mpi_causal_batches_commit(PeakMpiCausalBatches *);
size_t peak_mpi_causal_batches_pending(const PeakMpiCausalBatches *);
uint64_t peak_mpi_causal_batches_last_group(const PeakMpiCausalBatches *,
                                            unsigned rank);
/* Caller carries an opaque cursor across quota-limited scans of an unchanged
 * target; reset it when prefixes change. 1 clear, 0 pending, -1 quota
 * exhausted. Ingestion must not introduce records at/below an already accepted
 * prefix during this scan. That follows the caller's contiguous-prefix
 * precondition.
 */
int peak_mpi_causal_batches_prefix_clear(const PeakMpiCausalBatches *,
                                         unsigned rank,
                                         const uint64_t *prefixes, size_t slots,
                                         size_t *cursor, size_t *work_budget);
#endif /* PEAK_MPI_CAUSAL_BATCHES_H */
