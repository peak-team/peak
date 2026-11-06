#ifndef PEAK_MPI_CAUSAL_MATCH_INDEX_H
#define PEAK_MPI_CAUSAL_MATCH_INDEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PEAK_MPI_CAUSAL_MATCH_SEND = 0,
  PEAK_MPI_CAUSAL_MATCH_RECEIVE,
  PEAK_MPI_CAUSAL_MATCH_COLLECTIVE,
  PEAK_MPI_CAUSAL_MATCH_ROLE_COUNT
} PeakMpiCausalMatchRole;

typedef struct {
  uint64_t context, ordinal;
  int32_t source, destination, tag, operation;
} PeakMpiCausalMatchKey;

typedef struct PeakMpiCausalMatchIndex PeakMpiCausalMatchIndex;

/* Single backend owner. All storage is fixed at construction; no hot
 * primitive allocates, performs MPI or waits. Keys are opaque full identities;
 * a list's insertion order does not establish cross-thread MPI order. */
PeakMpiCausalMatchIndex *
peak_mpi_causal_match_index_create(size_t max_operations);
void peak_mpi_causal_match_index_destroy(PeakMpiCausalMatchIndex *);

/* One membership per (role, operation slot). Identical insertion is idempotent;
 * a conflicting membership, invalid slot or bounded-index capacity returns 0.
 * A new membership appends to its exact-key list. Hash lookup/insertion has a
 * fixed 64-probe limit; slot membership/list operations are direct indexed. */
int peak_mpi_causal_match_index_insert(PeakMpiCausalMatchIndex *,
                                       PeakMpiCausalMatchRole, size_t op_slot,
                                       const PeakMpiCausalMatchKey *);
void peak_mpi_causal_match_index_remove_role(PeakMpiCausalMatchIndex *,
                                             PeakMpiCausalMatchRole,
                                             size_t op_slot);
void peak_mpi_causal_match_index_remove_all(PeakMpiCausalMatchIndex *,
                                            size_t op_slot);
int peak_mpi_causal_match_index_contains(const PeakMpiCausalMatchIndex *,
                                         PeakMpiCausalMatchRole,
                                         size_t op_slot);
/* SIZE_MAX denotes no membership/result. The caller keeps operation slots
 * stable while iterating and removes all memberships before reusing a slot.
 * Mutation/removal of the current membership invalidates that iterator. */
size_t peak_mpi_causal_match_index_first(const PeakMpiCausalMatchIndex *,
                                         PeakMpiCausalMatchRole,
                                         const PeakMpiCausalMatchKey *);
size_t peak_mpi_causal_match_index_next(const PeakMpiCausalMatchIndex *,
                                        PeakMpiCausalMatchRole, size_t op_slot);

#ifdef __cplusplus
}
#endif
#endif /* PEAK_MPI_CAUSAL_MATCH_INDEX_H */
