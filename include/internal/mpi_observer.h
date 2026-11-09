#ifndef PEAK_MPI_OBSERVER_H
#define PEAK_MPI_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local capture ABI, not an MPI capability claim or a portable wire encoding.
 * Adapters supply complete identities/progress scope, or explicitly mark an
 * opt-in progress estimate; unsupported/lost capture still calls fail(). */
#define PEAK_MPI_OBSERVER_RECOVERY_API 1

#define PEAK_MPI_OBSERVER_RECORD_BYTES 256u
#define PEAK_MPI_OBSERVER_FEEDBACK_BYTES 512u
#define PEAK_MPI_OBSERVER_OBJECT_BYTES 128u

typedef struct PeakMpiObserver PeakMpiObserver;
int peak_mpi_observer_recover(PeakMpiObserver *observer);
int peak_mpi_observer_recover_generation(PeakMpiObserver *,
                                         uint64_t generation);
void peak_mpi_observer_discard(PeakMpiObserver *observer);
uint64_t peak_mpi_observer_fault_mask(const PeakMpiObserver *observer);
uint64_t peak_mpi_observer_loss_generation(const PeakMpiObserver *observer);

typedef enum {
    PEAK_MPI_OBSERVER_OK = 0,
    PEAK_MPI_OBSERVER_INACTIVE,
    PEAK_MPI_OBSERVER_COARSE,
    PEAK_MPI_OBSERVER_OVERFLOW,
    PEAK_MPI_OBSERVER_CAPACITY,
    PEAK_MPI_OBSERVER_BAD_PRODUCER,
    PEAK_MPI_OBSERVER_BAD_ARGUMENT,
    PEAK_MPI_OBSERVER_BUSY,
    PEAK_MPI_OBSERVER_EMPTY,
    PEAK_MPI_OBSERVER_OLD_GENERATION
} PeakMpiObserverStatus;

typedef enum {
    PEAK_MPI_OBSERVER_REASON_NONE = 0,
    PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW,
    PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY,
    PEAK_MPI_OBSERVER_REASON_SEQUENCE_LIMIT,
    PEAK_MPI_OBSERVER_REASON_INVALID_COST,
    PEAK_MPI_OBSERVER_REASON_CAPABILITY,
    PEAK_MPI_OBSERVER_REASON_IDENTITY,
    PEAK_MPI_OBSERVER_REASON_METADATA,
    PEAK_MPI_OBSERVER_REASON_PENDING_FAULT
} PeakMpiObserverReason;

typedef enum {
    PEAK_MPI_OBSERVER_EVENT_START = 1,
    PEAK_MPI_OBSERVER_EVENT_COMPLETE,
    PEAK_MPI_OBSERVER_EVENT_LOCAL_COST,
    PEAK_MPI_OBSERVER_EVENT_PROGRESS,
    PEAK_MPI_OBSERVER_EVENT_OBJECT,
    PEAK_MPI_OBSERVER_EVENT_BATCH_END
} PeakMpiObserverEventKind;

typedef enum {
    PEAK_MPI_OBSERVER_SCOPE_UNKNOWN = 0,
    PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS,
    PEAK_MPI_OBSERVER_SCOPE_INTERVAL
} PeakMpiObserverProgressScope;

enum {
    PEAK_MPI_OBSERVER_PROGRESS_SEALED = 1u << 0,
    PEAK_MPI_OBSERVER_SLACK_S1 = 1u << 1,
    PEAK_MPI_OBSERVER_SLACK_SINGLE_FLOW = 1u << 2,
    PEAK_MPI_OBSERVER_SLACK_NO_OPEN_PROGRESS = 1u << 3,
    PEAK_MPI_OBSERVER_CANCELLED = 1u << 4,
    PEAK_MPI_OBSERVER_APP_COST_KNOWN = 1u << 24,
    PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED = 1u << 25
};

typedef struct {
    uint64_t incarnation, thread_uid, thread_generation, sequence;
    uint64_t op_uid, input_ref, result_ref, progress_begin, progress_end;
    uint64_t L_cut, P_cut, control_generation, communicator_uid;
    uint64_t request_uid, request_generation, activation, collective_ordinal;
    uint64_t batch_uid, wait_ns, capability_uid, policy_generation;
    int32_t peer, tag, root, actual_source, actual_tag, mpi_error;
    uint32_t kind, operation, phase, flags, progress_scope;
    uint32_t partition, completion_index, completion_count;
    uint64_t A_cut; /* Billable application cost; valid only with APP_COST_KNOWN. */
    uint64_t reserved[3];
} PeakMpiObserverRecord;

typedef struct {
    size_t slots, records_per_slot, object_capacity;
    uint64_t incarnation;
} PeakMpiObserverConfig;

typedef struct {
    size_t slot;
    uint64_t generation, thread_uid;
} PeakMpiObserverProducer;

typedef struct {
    uint64_t generation, last_attempt_sequence, first_lost_sequence, lost;
    uint64_t published, consumed, L, P;
    bool claimed;
} PeakMpiObserverSlotState;

typedef struct {
    uint64_t uid, generation, parent_uid;
    uint32_t kind, flags, part_index, part_count;
    unsigned char payload[PEAK_MPI_OBSERVER_OBJECT_BYTES];
} PeakMpiObserverObject;

typedef struct {
    PeakMpiObserverReason reason;
    size_t slot;
    uint64_t generation, sequence;
} PeakMpiObserverFault;

typedef struct {
    uint64_t generation;
    size_t size;
    unsigned char payload[PEAK_MPI_OBSERVER_FEEDBACK_BYTES];
} PeakMpiObserverFeedback;

/* Construction allocates all storage. Destroy requires all producers and the
 * single consumer to have quiesced; mode switches never release their storage. */
PeakMpiObserver *peak_mpi_observer_create(const PeakMpiObserverConfig *config);
void peak_mpi_observer_destroy(PeakMpiObserver *observer);

/* Activation enables local recording, including state that precedes the first
 * remote exchange. It neither proves remote communication/contract completeness
 * nor opens a connection. Network/global policy use a separate actual-remote-
 * communication gate. Activation cannot clear a fault. */
void peak_mpi_observer_activate(PeakMpiObserver *observer);
bool peak_mpi_observer_active(const PeakMpiObserver *observer);
void peak_mpi_observer_fail(PeakMpiObserver *observer,
                            PeakMpiObserverReason reason);
PeakMpiObserverReason peak_mpi_observer_coarse_reason(
    const PeakMpiObserver *observer);
bool peak_mpi_observer_fault(const PeakMpiObserver *observer,
                             PeakMpiObserverFault *fault);

/* The existing thread registry must prove old-writer quiescence before release
 * and claim. One live writer per claimed slot is a precondition. Generation
 * strictly increases; queue positions, sequence and costs survive handoff.
 * Old handles are rejected. They do not mutate the new owner's state. An
 * adapter must call fail() if a rejected claim/handle prevents observation of
 * a real operation; a rejected stale cleanup handle alone is not an event. */
PeakMpiObserverStatus peak_mpi_observer_claim(PeakMpiObserver *observer,
    size_t slot, uint64_t generation, uint64_t thread_uid,
    PeakMpiObserverProducer *producer);
PeakMpiObserverStatus peak_mpi_observer_release(PeakMpiObserver *observer,
    const PeakMpiObserverProducer *producer);
/* P and L-P are independently cumulative actual-cost lanes in one consistent
 * caller-selected integer unit. This does no pricing: the writer must accrue
 * completed phases with their immutable calibration prices, never reprice old
 * history or include future reserves. Stores are single-writer relaxed stores. */
PeakMpiObserverStatus peak_mpi_observer_set_cost(PeakMpiObserver *observer,
    const PeakMpiObserverProducer *producer, uint64_t L, uint64_t P);
PeakMpiObserverStatus peak_mpi_observer_emit(PeakMpiObserver *observer,
    const PeakMpiObserverProducer *producer, const PeakMpiObserverRecord *record);

/* Exactly one consumer per slot. Cross-slot drain order is not an MPI order.
 * A cost snapshot reads each cumulative lane once, using relaxed loads. It
 * derives L from the same sampled P plus sampled non-profile cost, but it is
 * not an instantaneous cut across lanes/threads. Adapters/backend must retain
 * the conservative concurrent interval/group and unique raw-cost ownership;
 * they must not invent MPI order from these rank-wide samples. Diagnostic
 * sequence/ownership fields are independently sampled, not a feedback tuple. */
bool peak_mpi_observer_consume(PeakMpiObserver *observer, size_t slot,
                               PeakMpiObserverRecord *record);
bool peak_mpi_observer_slot_state(const PeakMpiObserver *observer, size_t slot,
                                  PeakMpiObserverSlotState *state);
/* MPI-boundary cost-only path: O(configured slots), two relaxed loads per slot,
 * no hook/registry scan, metadata lock, fence or retry. Retired slot totals are
 * included exactly once. Overflow fails closed and leaves outputs unchanged.
 * This remains a noninstantaneous conservative cut, as described above. */
bool peak_mpi_observer_cost_snapshot(PeakMpiObserver *observer,
                                     uint64_t *L, uint64_t *P);

/* Append-only immutable definitions. Only producer append takes a short local
 * metadata mutex; no backend/consumer takes it. This function never invokes
 * external code or retains the mutex across a PMPI/network/control operation.
 * UIDs and returned pointers remain valid until destroy, including coarse mode.
 * Variable definitions use bounded numbered parts, never hot-path allocation. */
PeakMpiObserverStatus peak_mpi_observer_object_add(PeakMpiObserver *observer,
    const PeakMpiObserverObject *definition, uint64_t *uid);
/* Acquire-loaded count of complete immutable definitions in the append-only
 * slab. Zero for NULL; each UID through this prefix is published and readable. */
size_t peak_mpi_observer_object_count(const PeakMpiObserver *observer);
const PeakMpiObserverObject *peak_mpi_observer_object_get(
    const PeakMpiObserver *observer, uint64_t uid);

/* Opaque complete tuple publication, separate from producer rings. Both calls
 * use one try-lock and return BUSY instead of waiting. Policy validation,
 * cohort/incarnation checks and cost interpretation belong to the policy
 * adapter. Read contention must retain/age the previous tuple conservatively.
 * A publisher may retry later; only strictly increasing generations publish. */
PeakMpiObserverStatus peak_mpi_observer_feedback_publish(PeakMpiObserver *observer,
    uint64_t generation, const void *payload, size_t size);
PeakMpiObserverStatus peak_mpi_observer_feedback_read(PeakMpiObserver *observer,
    PeakMpiObserverFeedback *feedback);

#ifdef __cplusplus
}
#endif
#endif /* PEAK_MPI_OBSERVER_H */
