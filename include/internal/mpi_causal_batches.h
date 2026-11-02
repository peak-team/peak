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
void peak_mpi_causal_batches_destroy(PeakMpiCausalBatches *);
int peak_mpi_causal_batches_ingest(PeakMpiCausalBatches *, unsigned rank,
                                   const PeakMpiObserverRecord *);
/* Every inspected slot/comparison and retirement uses one work unit.
 * 1: coherent plan ready, 0: incomplete/work exhausted, -1: invalid group.
 * Plan remains immutable until commit; errors remain visible until destroy. */
int peak_mpi_causal_batches_prepare_next(PeakMpiCausalBatches *,
                                         size_t work_budget,
                                         PeakMpiCausalBatchView *);
const PeakMpiObserverRecord *
peak_mpi_causal_batches_record_at(const PeakMpiCausalBatches *, size_t index);
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
#endif
