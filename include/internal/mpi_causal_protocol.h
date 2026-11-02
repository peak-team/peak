#ifndef PEAK_MPI_CAUSAL_PROTOCOL_H
#define PEAK_MPI_CAUSAL_PROTOCOL_H

#include "internal/mpi_observer.h"

/* Logical capture protocol. Record storage is fixed; transport must encode
 * fields explicitly rather than assume this host layout is a wire ABI. */
typedef enum {
  PEAK_MPI_CAUSAL_SEND = 1,
  PEAK_MPI_CAUSAL_SSEND,
  PEAK_MPI_CAUSAL_BSEND,
  PEAK_MPI_CAUSAL_RSEND,
  PEAK_MPI_CAUSAL_RECV,
  PEAK_MPI_CAUSAL_SENDRECV,
  PEAK_MPI_CAUSAL_BARRIER,
  PEAK_MPI_CAUSAL_BCAST,
  PEAK_MPI_CAUSAL_ALLREDUCE,
  PEAK_MPI_CAUSAL_REDUCE,
  PEAK_MPI_CAUSAL_ALLGATHER,
  PEAK_MPI_CAUSAL_ALLTOALL,
  PEAK_MPI_CAUSAL_REDUCE_SCATTER,
  PEAK_MPI_CAUSAL_GATHER,
  PEAK_MPI_CAUSAL_SCATTER,
  PEAK_MPI_CAUSAL_SCAN,
  PEAK_MPI_CAUSAL_EXSCAN,
  PEAK_MPI_CAUSAL_WAIT,
  PEAK_MPI_CAUSAL_TEST,
  PEAK_MPI_CAUSAL_PROBE,
  PEAK_MPI_CAUSAL_MPROBE,
  PEAK_MPI_CAUSAL_MRECV,
  PEAK_MPI_CAUSAL_COMM_DUP,
  PEAK_MPI_CAUSAL_COMM_SPLIT,
  PEAK_MPI_CAUSAL_COMM_IDUP,
  PEAK_MPI_CAUSAL_COMM_FREE,
  PEAK_MPI_CAUSAL_BUFFER_DETACH,
  PEAK_MPI_CAUSAL_REQUEST_FREE,
  PEAK_MPI_CAUSAL_CANCEL,
  PEAK_MPI_CAUSAL_FINALIZE,
  PEAK_MPI_CAUSAL_START
} PeakMpiCausalOperation;

typedef enum {
  PEAK_MPI_CAUSAL_CALL_BEGIN = 1,
  PEAK_MPI_CAUSAL_CALL_END,
  PEAK_MPI_CAUSAL_REQUEST_COMPLETE,
  PEAK_MPI_CAUSAL_GROUP_BEGIN,
  PEAK_MPI_CAUSAL_GROUP_END,
  PEAK_MPI_CAUSAL_REQUEST_ACTIVATE,
  PEAK_MPI_CAUSAL_MESSAGE_BOUND,
  PEAK_MPI_CAUSAL_REQUEST_RETIRED
} PeakMpiCausalPhase;

enum {
  PEAK_MPI_CAUSAL_NONBLOCKING = 1u << 8,
  PEAK_MPI_CAUSAL_PERSISTENT = 1u << 9,
  PEAK_MPI_CAUSAL_RECEIVE = 1u << 10,
  PEAK_MPI_CAUSAL_CONCURRENT = 1u << 11,
  PEAK_MPI_CAUSAL_SUCCESS = 1u << 12,
  PEAK_MPI_CAUSAL_COLLECTIVE = 1u << 13,
  PEAK_MPI_CAUSAL_BUFFERED = 1u << 14,
  PEAK_MPI_CAUSAL_INTERCOMM = 1u << 15,
  PEAK_MPI_CAUSAL_ZERO_DATA = 1u << 16,
  PEAK_MPI_CAUSAL_INACTIVE_REQUEST = 1u << 17,
  PEAK_MPI_CAUSAL_STATUS_OBSERVED = 1u << 18,
  PEAK_MPI_CAUSAL_STATUS_ONLY = 1u << 19,
  PEAK_MPI_CAUSAL_RECLAIMED = 1u << 20,
  PEAK_MPI_CAUSAL_VARIABLE_COUNT = 1u << 21
};
#define PEAK_MPI_CAUSAL_CUT_PRECAPTURED UINT32_C(0x80000000)
#define PEAK_MPI_CAUSAL_ANY_SOURCE (-1)
#define PEAK_MPI_CAUSAL_PROC_NULL (-2)
#define PEAK_MPI_CAUSAL_UNKNOWN_PEER (-3)
#define PEAK_MPI_CAUSAL_ANY_TAG (-1)

enum {
  PEAK_MPI_CAUSAL_OBJECT_COMM = 1,
  PEAK_MPI_CAUSAL_COMM_WORLD = 1,
  PEAK_MPI_CAUSAL_COMM_SELF = 2,
  PEAK_MPI_CAUSAL_COMM_DUPLICATED = 3,
  PEAK_MPI_CAUSAL_COMM_SPLIT_CREATED = 4,
  PEAK_MPI_CAUSAL_COMM_IDUP_CREATED = 5
};

#define PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART 16u
typedef struct {
  uint64_t local_uid, parent_uid, creation_ordinal;
  uint32_t creation_kind, world_size;
  int32_t color, local_rank, local_size;
  uint32_t member_offset, member_count;
  int32_t members[PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART];
} PeakMpiCausalCommPart;

/* CALL_BEGIN/END share op_uid; batch_uid is a conservative concurrent group.
 * input_ref at CALL_BEGIN is the previous completed call on THIS THREAD.
 * Group records use input_ref for the previous group. This group relationship
 * is cost ownership metadata, never a cross-thread MPI matching guarantee.
 * GROUP_BEGIN/END progress_end is the covered local elapsed nanoseconds.
 * GROUP_END completion_count is the exact number of published CALL contexts,
 * excluding suppressed failed polls and request subrecords. END CONCURRENT
 * is authoritative when this count exceeds one; END may arrive before other
 * producer lanes, so the owner waits for that many entry/end pairs.
 * Buffered launch/Buffer_detach progress_begin identifies the attached buffer
 * generation. It is not an application request completion or automatic seal.
 *
 * REQUEST_COMPLETE uses op_uid for its activation's launch, batch_uid for the
 * observing Wait/Test call; request_uid/generation/activation identify its
 * reusable public handle. Actual receive status uses world process ranks.
 * A completing Wait/Test has exactly one CALL_END, owning its inside cost;
 * completion subrecords carry no additional raw-cost charge.
 * STATUS_ONLY preserves the public handle; RECLAIMED retires nonpersistent
 * handles and makes persistent activations inactive. A later reclaim after a
 * status-only observation uses REQUEST_RETIRED with no repeated dependency.
 * Matched receive BEGIN uses request_uid=message UID, progress_begin=Mprobe
 * UID.
 *
 * SENDRECV uses peer/tag for its send and root/actual_tag for its posted
 * receive source/tag at BEGIN; END actual_source/actual_tag are the received
 * status. Collective ordinal is per-context MPI call order. Definitions name
 * local communicator UIDs and lineage; equal member lists do not identify a
 * context. P/L are supplied by runtime_submit in its fixed cost unit, not this
 * frontend.
 */

#endif /* PEAK_MPI_CAUSAL_PROTOCOL_H */
