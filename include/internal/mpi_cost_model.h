#ifndef PEAK_MPI_COST_MODEL_H
#define PEAK_MPI_COST_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend-only, single-owner graph engine. This is not an MPI adapter: the
 * caller must supply complete identities, candidates, progress scopes and
 * local cost groups under an independently verified implementation contract.
 * In particular, PMPI parameters alone do not establish these preconditions.
 *
 * Reserve references before their definitions arrive. Seal a node only after
 * its entire incoming dependency/candidate set is known. A committed node is
 * immutable; later local continuation must use a new node. A cost owner names
 * one original local increment, never an inherited delay. Concurrent aliases
 * of one owner must belong to one conservative component (or be represented
 * by a shared predecessor before submission). Caller-side sequence/UID tables
 * must deduplicate transport records and retain all live references.
 *
 * All storage is allocated at create. Mutations are O(1), except add_edge's
 * explicitly bounded hash probes. run/collect execute at most work_budget
 * node/edge/state transitions and can resume. Mutations return BUSY during a
 * run; only the backend may wait/retry, never an application MPI wrapper.
 */

typedef struct PeakMpiCostModel PeakMpiCostModel;
typedef struct { uint32_t slot; uint64_t generation; } PeakMpiCostRef;
typedef struct { uint32_t slot; uint64_t generation; } PeakMpiCostOwnerRef;
typedef struct { uint32_t slot; uint64_t generation; } PeakMpiCostBranchRef;

#define PEAK_MPI_COST_NO_RANK UINT32_MAX
#ifdef __cplusplus
#define PEAK_MPI_COST_NO_OWNER PeakMpiCostOwnerRef{UINT32_MAX, 0}
#else
#define PEAK_MPI_COST_NO_OWNER ((PeakMpiCostOwnerRef){UINT32_MAX, 0})
#endif

typedef enum {
    PEAK_MPI_COST_OK,
    PEAK_MPI_COST_PENDING,
    PEAK_MPI_COST_YIELDED,
    PEAK_MPI_COST_BUSY,
    PEAK_MPI_COST_DUPLICATE,
    PEAK_MPI_COST_COARSE,
    PEAK_MPI_COST_INVALID
} PeakMpiCostStatus;

typedef enum {
    PEAK_MPI_COST_REASON_NONE,
    PEAK_MPI_COST_EVENT_LOSS,
    PEAK_MPI_COST_MISSING_CAPABILITY,
    PEAK_MPI_COST_UNKNOWN_PROGRESS_SCOPE,
    PEAK_MPI_COST_NODE_CAPACITY,
    PEAK_MPI_COST_EDGE_CAPACITY,
    PEAK_MPI_COST_INDEX_CAPACITY,
    PEAK_MPI_COST_OWNER_CAPACITY,
    PEAK_MPI_COST_BRANCH_CAPACITY,
    PEAK_MPI_COST_INVALID_REFERENCE,
    PEAK_MPI_COST_CONFLICTING_DEFINITION,
    PEAK_MPI_COST_CLOSED_DEPENDENCIES,
    PEAK_MPI_COST_INVALID_COST,
    PEAK_MPI_COST_UNGROUPED_OWNER,
    PEAK_MPI_COST_GENERATION_OVERFLOW,
    PEAK_MPI_COST_INVALID_CLOSURE,
    PEAK_MPI_COST_INTERNAL_ERROR
} PeakMpiCostReason;

typedef struct {
    uint32_t max_nodes;
    uint32_t max_edges;
    uint32_t max_owners;
    uint32_t max_branches;
    uint32_t max_ranks;
    uint32_t max_hash_probes; /* 1..64; bounds each edge insertion */
} PeakMpiCostConfig;

typedef enum { PEAK_MPI_COST_EVENT, PEAK_MPI_COST_COLLECTIVE } PeakMpiCostNodeKind;
typedef struct {
    PeakMpiCostNodeKind kind;
    uint32_t rank;
    PeakMpiCostOwnerRef owner;
    double seed_delay; /* already-accounted immutable external input */
    uint32_t participants; /* collective group only */
    bool single_execution_flow; /* explicit assertion at this entry event */
    bool progress_clear; /* no unsealed progress interval at this entry */
    bool s1; /* reviewed same-operation S1 classification, never inferred */
} PeakMpiCostNodeSpec;

typedef enum {
    PEAK_MPI_COST_GUARANTEED,
    PEAK_MPI_COST_SEND_REVERSE,
    PEAK_MPI_COST_PROGRESS,
    PEAK_MPI_COST_SLACK_INPUT,
    PEAK_MPI_COST_COLLECTIVE_CANDIDATE
} PeakMpiCostEdgeKind;
typedef enum {
    PEAK_MPI_COST_SEND_BRANCH,
    PEAK_MPI_COST_REQUEST_BRANCH,
    PEAK_MPI_COST_PROGRESS_BRANCH
} PeakMpiCostBranchKind;

typedef struct {
    uint64_t generation;
    double L;
    double P;
    double elapsed;
    uint64_t control_generation;
} PeakMpiCostRawCut;

typedef struct {
    uint32_t active_nodes, active_edges, active_owners, active_branches;
    uint32_t peak_pending_nodes, settled_nodes;
    uint32_t scc_count, excluded_candidates, retained_candidates; /* latest pass */
    uint64_t generation, lost_events, scc_settlements; /* lifetime commits */
    bool coarse;
    PeakMpiCostReason reason;
} PeakMpiCostStats;

PeakMpiCostModel *peak_mpi_cost_model_create(const PeakMpiCostConfig *config);
void peak_mpi_cost_model_destroy(PeakMpiCostModel *model);
void peak_mpi_cost_model_reset_begin(PeakMpiCostModel *model);
int peak_mpi_cost_model_reset_work(PeakMpiCostModel *model, size_t budget,
                                   size_t *used);
void peak_mpi_cost_model_reset_base(PeakMpiCostModel *model, uint32_t rank,
                                    double L, double P);
PeakMpiCostStatus peak_mpi_cost_model_reserve_owner(PeakMpiCostModel *, PeakMpiCostOwnerRef *);
PeakMpiCostStatus peak_mpi_cost_model_define_owner(PeakMpiCostModel *, PeakMpiCostOwnerRef,
                                                uint32_t rank, double delta_L, double delta_P);
PeakMpiCostStatus peak_mpi_cost_model_reserve_node(PeakMpiCostModel *, PeakMpiCostRef *);
PeakMpiCostStatus peak_mpi_cost_model_define_node(PeakMpiCostModel *, PeakMpiCostRef,
                                               const PeakMpiCostNodeSpec *);
/* A collective group has no cost owner/seed. Its O(n) SLACK_INPUT edges point
 * from entry events with W as wait_bound. Compensation additionally requires
 * every entry's explicit single-flow/progress-clear assertions, the exact
 * participant count, settled inputs, and an acyclic singleton component.
 * Additional non-participant dependency edges disable credit. Other cases
 * use MAX. Add each rank's inside-call increment on its separate
 * exit node after the group; never subtract it from W or charge it twice.
 * Weak collective groups use COLLECTIVE_CANDIDATE on the star's input/output
 * edges. These are retained conservatively but never prove reachability in G.
 * SLACK_INPUT for a group not classified S1 also cannot prove G. S1 inputs
 * require distinct concrete entry ranks; undefined entries remain pending,
 * while duplicate/unknown ranks are rejected. The caller still certifies the
 * communicator's actual membership and same-operation identity. */
PeakMpiCostStatus peak_mpi_cost_model_add_edge(PeakMpiCostModel *, PeakMpiCostRef source,
                                            PeakMpiCostRef target, PeakMpiCostEdgeKind,
                                            double wait_bound);
PeakMpiCostStatus peak_mpi_cost_model_seal(PeakMpiCostModel *, PeakMpiCostRef);
PeakMpiCostStatus peak_mpi_cost_model_run(PeakMpiCostModel *, size_t work_budget, size_t *work_used);
PeakMpiCostStatus peak_mpi_cost_model_delay(const PeakMpiCostModel *, PeakMpiCostRef, double *);

PeakMpiCostStatus peak_mpi_cost_model_hold_branch(PeakMpiCostModel *, PeakMpiCostRef,
                                               PeakMpiCostBranchKind, PeakMpiCostBranchRef *);
PeakMpiCostStatus peak_mpi_cost_model_release_branch(PeakMpiCostModel *, PeakMpiCostBranchRef,
                                                  bool no_future_consumers);
PeakMpiCostStatus peak_mpi_cost_model_set_continuation(PeakMpiCostModel *, uint32_t rank,
                                                    PeakMpiCostRef);
/* One backend transaction installs the replacement before retiring the token.
 * No frontier read can expose an intermediate deletion. */
PeakMpiCostStatus peak_mpi_cost_model_replace_branch(PeakMpiCostModel *, PeakMpiCostBranchRef,
                                                  uint32_t rank, PeakMpiCostRef continuation);
/* Numerical frontier of registered continuations/live branches only. This
 * does not certify a complete distributed cut: the caller must establish
 * covered producer prefixes, cohort membership and freshness separately. */
PeakMpiCostStatus peak_mpi_cost_model_frontier(const PeakMpiCostModel *, double *);

/* Retirement is an explicit proof that no future edge/reference can enter
 * this node. Collection also requires a committed value, no live pins and no
 * unresolved adjacent edge. Barriers/free/slack are not retirement proofs. */
PeakMpiCostStatus peak_mpi_cost_model_retire(PeakMpiCostModel *, PeakMpiCostRef,
                                          bool future_references_closed);
PeakMpiCostStatus peak_mpi_cost_model_collect(PeakMpiCostModel *, size_t work_budget,
                                           size_t *work_used);

/* Unique owner totals are diagnostic raw local increments, never sum(D).
 * Cumulative producer cuts are separate; do not add them to owner totals.
 * Cuts continue in sticky coarse mode. Missing rank data returns PENDING.
 * They are not automatically a covered graph prefix or a cohort-wide cut. */
PeakMpiCostStatus peak_mpi_cost_model_accounted(const PeakMpiCostModel *, uint32_t rank,
                                             double *L, double *P);
PeakMpiCostStatus peak_mpi_cost_model_raw_cut(PeakMpiCostModel *, uint32_t rank,
                                           const PeakMpiCostRawCut *);
PeakMpiCostStatus peak_mpi_cost_model_raw(const PeakMpiCostModel *, uint32_t rank,
                                       PeakMpiCostRawCut *);
void peak_mpi_cost_model_fail(PeakMpiCostModel *, PeakMpiCostReason, uint64_t lost_events);
void peak_mpi_cost_model_stats(const PeakMpiCostModel *, PeakMpiCostStats *);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_MPI_COST_MODEL_H */
