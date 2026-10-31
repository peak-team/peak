#ifndef PEAK_GLOBAL_BUDGET_CAUSAL_H
#define PEAK_GLOBAL_BUDGET_CAUSAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A profiling-cost estimate, independent of cash reservations and MPI headers.
 * The coordinator owns this object and serializes all calls. */
typedef struct PeakGlobalBudgetCausal PeakGlobalBudgetCausal;
typedef enum {
    PEAK_CAUSAL_FRONTIER,
    PEAK_CAUSAL_POST,
    PEAK_CAUSAL_WAIT,
    PEAK_CAUSAL_COMPLETE,
    PEAK_CAUSAL_UNKNOWN,
    PEAK_CAUSAL_MPI_ENTER,
    PEAK_CAUSAL_MPI_EXIT
} PeakGlobalBudgetCausalKind;
typedef enum {
    PEAK_CAUSAL_SEND,
    PEAK_CAUSAL_RECV,
    PEAK_CAUSAL_COLLECTIVE,
    PEAK_CAUSAL_AMBIGUOUS_SEND
} PeakGlobalBudgetCausalOperation;
typedef struct {
    size_t rank;
    uint64_t sequence, last_mpi_sequence;
    uint32_t context_owner;
    uint64_t context, round, tag, request;
    PeakGlobalBudgetCausalKind kind;
    PeakGlobalBudgetCausalOperation operation;
    size_t peer;
    /* Measured active+ignored OUTSIDE-MPI profiling plus frontend metadata
     * CPU measured outside native MPI. Inside profiling
     * is billed separately, once, through serialized(). ENTER/EXIT track
     * sequence/quiescence and never reclassify this typed outside counter. */
    uint64_t cumulative_cost_ns;
    const uint64_t *requests;
    size_t request_count;
} PeakGlobalBudgetCausalEvent;
typedef struct {
    uint64_t raw_cost_ns, causal_raw_ns, fallback_raw_ns;
    uint64_t confirmed_ns, unresolved_ns, serialized_ns, effective_ns;
    size_t outstanding_requests;
    bool sum_mode, resource_failed, saturated;
} PeakGlobalBudgetCausalView;

PeakGlobalBudgetCausal *peak_global_budget_causal_new(size_t world_size);
void peak_global_budget_causal_free(PeakGlobalBudgetCausal *model);
/* Register the complete communicator membership using stable world ranks.
 * A context ID must not be reused for a different communicator generation. */
bool peak_global_budget_causal_context(PeakGlobalBudgetCausal *model,
                                      uint32_t owner, uint64_t context,
                                      const size_t *members, size_t count);
/* Events have per-rank contiguous sequence numbers, starting at one. Delivery
 * may be out of order. Raw cumulative highwater is reserved immediately.
 * POST/WAIT snapshots do not block; an unresolved COMPLETE blocks later local
 * frontiers. Request IDs remain stable while unmatched evidence exists,
 * including across a checkpoint. Blocking-send ambiguity uses AMBIGUOUS_SEND
 * with actual POST/WAIT/COMPLETE evidence rather than dropping its message.
 * Singleton receive COMPLETE supplies actual peer/tag/round for a wildcard
 * POST (unresolved peer=SIZE_MAX, tag=UINT64_MAX and/or round=0). Known keys
 * are immutable; different receive resolutions use individual records.
 * Returns false on rejected duplicate/invalid stream records or allocation
 * failure; allocation failure permanently retains conservative SUM mode. */
bool peak_global_budget_causal_submit(PeakGlobalBudgetCausal *model,
                                     const PeakGlobalBudgetCausalEvent *event);
/* Reserve a measured typed outside-cost highwater without inventing a stream
 * sequence. Cost beyond processed events remains U until ordered evidence
 * arrives. An untyped raw-profile tail must NOT be labeled outside here. */
bool peak_global_budget_causal_reserve_outside(PeakGlobalBudgetCausal *model,
                                              size_t rank, uint64_t cumulative_ns);
/* Inside-MPI profiling, ALL physical control, management, transport/broker CPU
 * and other serialized work belong here. These costs never participate in parallel MAX folding. */
void peak_global_budget_causal_serialized(PeakGlobalBudgetCausal *model,
                                         uint64_t cost_ns);
void peak_global_budget_causal_fallback(PeakGlobalBudgetCausal *model);
/* Caller certifies a CLOSED_COST_PREFIX across the world, not an idle timeout
 * or a claim that MPI has no buffered messages. SUM mode first drains already
 * received ordered records; no missing event or completion is invented.
 * Requires no pending records, active MPI intervals or unmatched requests,
 * including completed sends: rendezvous can depend on a post-cut receive.
 * R/D cash accounting is untouched. Saturation/resource loss cannot resume. */
bool peak_global_budget_causal_checkpoint(PeakGlobalBudgetCausal *model);
/* Only with proof from a real all-WORLD Barrier entry fence. Every participant
 * must have entered before any post-cut application event can occur. This
 * permits completed unmatched SEND evidence to carry forward, retaining IDs
 * and matching keys with post/wait rebased to S. RECV/incomplete collective
 * evidence still rejects the cut. An ordinary collective EXIT is not proof. */
bool peak_global_budget_causal_checkpoint_after_entry_fence(PeakGlobalBudgetCausal *model);
PeakGlobalBudgetCausalView peak_global_budget_causal_view(
    const PeakGlobalBudgetCausal *model);

#endif /* PEAK_GLOBAL_BUDGET_CAUSAL_H */
