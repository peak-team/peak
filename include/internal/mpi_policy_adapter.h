#ifndef PEAK_MPI_POLICY_ADAPTER_H
#define PEAK_MPI_POLICY_ADAPTER_H
#include "internal/mpi_causal_recovery.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure policy arithmetic. The observer is the sole owner of each backend
 * object; a rank heartbeat is the sole owner of its receiver. No operation
 * here calls MPI, changes hooks, waits for another owner, or allocates after
 * create(). Feedback is a value, not a wire format: the observer must publish
 * its complete bytes coherently before the receiver copies it. */
typedef struct { uint64_t process, incarnation; } PeakMpiPolicyMemberId;
typedef struct {
    uint64_t job_incarnation, cohort_id, epoch;
} PeakMpiPolicyCohortKey;
/* Identity and generation fields are opaque unsigned values: zero is valid.
 * The caller supplies stable unique identities; equality uses every field. */

typedef enum {
    PEAK_MPI_POLICY_LOCAL = 0,
    PEAK_MPI_POLICY_COMPLETE,
    PEAK_MPI_POLICY_STALE,
    PEAK_MPI_POLICY_COARSE,
    PEAK_MPI_POLICY_APPROX
} PeakMpiPolicyMode;

typedef enum {
    PEAK_MPI_POLICY_NO_REASON = 0,
    PEAK_MPI_POLICY_FEEDBACK_OLD,
    PEAK_MPI_POLICY_UNKNOWN_MEMBERS,
    PEAK_MPI_POLICY_MISSING_CONTRACT,
    PEAK_MPI_POLICY_EVENT_LOSS,
    PEAK_MPI_POLICY_RESOURCE_LIMIT,
    PEAK_MPI_POLICY_BAD_IDENTITY,
    PEAK_MPI_POLICY_BAD_CUT
} PeakMpiPolicyReason;

typedef enum {
    PEAK_MPI_POLICY_OK = 0,
    PEAK_MPI_POLICY_IGNORED,
    PEAK_MPI_POLICY_INVALID,
    PEAK_MPI_POLICY_CAPACITY,
    PEAK_MPI_POLICY_NOT_MEMBER,
    PEAK_MPI_POLICY_BUDGET
} PeakMpiPolicyResult;

enum {
    PEAK_MPI_POLICY_PREFIX_COMPLETE = 1u << 0,
    PEAK_MPI_POLICY_FRONTIER_COMPLETE = 1u << 1,
    PEAK_MPI_POLICY_BACKLOG_COMPLETE = 1u << 2,
    PEAK_MPI_POLICY_COMPLETE_CUT = (1u << 0) | (1u << 1) | (1u << 2)
};

typedef struct {
    size_t member_capacity;
    double q, heartbeat_max_seconds, ema_alpha, min_observation_seconds;
    double initial_multiplier; /* 0 selects 1; otherwise an inherited g. */
    uint64_t policy_generation;
} PeakMpiPolicyConfig;

typedef struct {
    PeakMpiPolicyMemberId member;
    /* generation belongs to this sample's stream. RAW heartbeat generations
     * and covered GROUP generations are independent namespaces. Cut.generation
     * deduplicates covered submissions; only record_raw compares RAW IDs. */
    uint64_t generation, event_cut, control_generation;
    double L, P, D, elapsed, age_seconds;
    int mixed_control;
    double A;
    int app_cost_known;
    PeakMpiCostCut cut;
} PeakMpiPolicySample;

/* Each member sample belongs to this one committed cut. F includes all live
 * continuation/send/request/progress branches. Only an adapter/model with a
 * reviewed complete capability contract may set COMPLETE_CUT. PMPI parameter
 * logs and an async-progress switch do not establish that precondition.
 * Ages are measured durations, never cross-node absolute timestamps. */
typedef struct {
    PeakMpiPolicyCohortKey cohort;
    uint64_t generation, cut_id, policy_generation;
    double F, age_seconds, oldest_unresolved_age;
    unsigned quality;
    int stable;
    const PeakMpiPolicySample *samples;
    size_t sample_count;
} PeakMpiPolicyCut;

typedef struct {
    PeakMpiCausalRecovery recovery;
    int approximate;
    PeakMpiPolicyCohortKey cohort;
    PeakMpiPolicyMemberId member;
    uint64_t generation, event_cut, control_generation, policy_generation;
    PeakMpiPolicyMode mode;
    PeakMpiPolicyReason reason;
    size_t member_count; /* Exact when membership_complete; otherwise a lower bound. */
    double q, g, L_cut, P_cut, D_cut, local_elapsed_at_cut;
    double F, risk, U_tail, T_safe, local_latest_L;
    /* received_raw_sum sums each member's latest known actual L: the maximum
     * of a received RAW cost and a covered cost, never their sum. It is only a
     * partial diagnostic unless its complete flag is set. risk/U_tail/T_safe
     * must likewise be gated by validity flags; zero-filled unknown payload
     * fields never establish zero cost. */
    /* Age includes both covered events and usable stable pressure observations;
     * fresh mixed cuts alone cannot keep feedback usable indefinitely. */
    double received_raw_sum, age_seconds, oldest_unresolved_age;
    uint64_t lost_events;
    /* Fine coverage means one complete prefix/frontier/backlog cut. Coarse
     * coverage instead means a complete application-cost cut (legacy raw L
     * without an annotation); no fine graph is claimed. */
    int membership_complete, coverage_complete, risk_known, tails_complete;
    int received_raw_complete, rate_baseline_valid;
    /* Known cut values use the canonical ns conversion; local_latest_L and
     * received_raw_sum above remain raw-cost diagnostics, never A aliases. */
    double A_cut, local_latest_A, received_app_sum;
    int app_cost_known;
    PeakMpiCostCut cut;
    uint64_t local_latest_A_ns, local_latest_L_ns;
} PeakMpiPolicyFeedback;

typedef struct PeakMpiPolicy PeakMpiPolicy;
void peak_mpi_policy_recover(PeakMpiPolicy *policy, double now, double g);
void peak_mpi_policy_recover_begin(PeakMpiPolicy *, double now, double g);
int peak_mpi_policy_recover_work(PeakMpiPolicy *, size_t budget, size_t *used);

/* Members must be the actual communication-connected cohort, not an inferred
 * world/peer-list replacement for dependency analysis. Unknown membership is
 * represented explicitly: count is then only a lower bound, and one known
 * local member may communicate with an unmanaged/unknown remote participant.
 * Empty membership is allowed only without actual communication; a complete
 * singleton stays local. All sample costs/ages are finite nonnegative seconds,
 * P<=A<=L when annotated; legacy samples conservatively use A=L. Zero costs/cuts/generations are valid. now is one owner's monotonic
 * clock. No absolute timestamps from different ranks are compared. */
PeakMpiPolicy *peak_mpi_policy_create(const PeakMpiPolicyConfig *config,
    PeakMpiPolicyCohortKey cohort, const PeakMpiPolicyMemberId *members,
    size_t member_count, int actual_communication, int membership_complete,
    double now);
void peak_mpi_policy_free(PeakMpiPolicy *policy);
PeakMpiPolicyResult peak_mpi_policy_record_raw(PeakMpiPolicy *policy,
    PeakMpiPolicyCohortKey cohort, const PeakMpiPolicySample *sample, double now);
/* Covered samples retain their own actual cost/elapsed observations and do
 * not overwrite the RAW stream. A fresh covered cut supplies a zero-tail
 * basis even without RAW, or when its actual L is ahead of older RAW data. */
PeakMpiPolicyResult peak_mpi_policy_submit_cut(PeakMpiPolicy *policy,
    const PeakMpiPolicyCut *cut, double now);
/* Merge requires disjoint same-job cohorts and a new epoch greater than both
 * old epochs. Source is not mutated; its owner must retire it after success.
 * All paid history is copied; a new coherent rate baseline is required. */
PeakMpiPolicyResult peak_mpi_policy_merge(PeakMpiPolicy *destination,
    const PeakMpiPolicy *source, PeakMpiPolicyCohortKey merged, double now);
PeakMpiPolicyResult peak_mpi_policy_mark_coarse(PeakMpiPolicy *policy,
    PeakMpiPolicyReason reason, uint64_t lost_events, double now);
PeakMpiPolicyResult peak_mpi_policy_set_membership_known(PeakMpiPolicy *policy,
    int complete, double now);
PeakMpiPolicyResult peak_mpi_policy_tick(PeakMpiPolicy *policy, double now);
/* Single-member convenience query is O(n); do not call once per transport
 * peer. The batch API below aggregates one cohort once. */
PeakMpiPolicyResult peak_mpi_policy_feedback(PeakMpiPolicy *policy,
    PeakMpiPolicyMemberId member, double now, PeakMpiPolicyFeedback *feedback);
/* O(1) admission cost for n stored members: one common aggregate visit per
 * member, one constant-time tuple fill per member, plus one state step. */
size_t peak_mpi_policy_feedback_batch_work(const PeakMpiPolicy *policy);
/* Single-owner atomic O(n) snapshot; output storage is owned/preallocated by
 * the backend. No allocation or partial batch. Insufficient work returns
 * BUDGET and insufficient output capacity returns CAPACITY, with no policy or
 * tuple mutation and *written=*work_used=0. An admitted batch uses at most the
 * reported work allowance. Rows are values, independent of later RAW updates;
 * they include each stored member once in unspecified order. Transport must
 * cache a completed batch, age its original capture time, and never create a
 * new generation merely by retransmitting. */
PeakMpiPolicyResult peak_mpi_policy_feedback_batch(PeakMpiPolicy *policy,
    double now, PeakMpiPolicyFeedback *feedbacks, size_t feedback_capacity,
    size_t work_allowance, size_t *written, size_t *work_used);

typedef struct {
    PeakMpiPolicyConfig config;
    PeakMpiPolicyCohortKey cohort;
    PeakMpiPolicyMemberId member;
    PeakMpiPolicyFeedback feedback;
    double created_at, received_at, last_now, conservative_g;
    size_t known_member_count;
    uint64_t last_generation;
    int active, have_feedback, membership_complete, permanent_coarse;
    int have_history, membership_hint_pending;
    int feedback_invalid;
    PeakMpiPolicyReason coarse_reason;
} PeakMpiPolicyReceiver;

typedef struct {
    PeakMpiPolicyMode mode;
    PeakMpiPolicyReason reason;
    double g, q_eff;
    double detach_target, candidate_stop_target, cadence_target;
    double reattach_future_rate_target;
    double global_headroom, available_headroom, local_D_estimate;
    double feedback_age_seconds;
    /* q_eff remains the conservative future target even when global history
     * is unavailable. Do not fall back to a larger q on this flag alone. */
    int global_available, headroom_known, reattach_relaxation;
} PeakMpiPolicyDecision;

/* A new receiver is purely local until an actual communication activates it
 * through rebind or a matching feedback tuple. Rebind invalidates all old
 * cohort packets, even if their packet generation is numerically larger.
 * Cohorts only merge: rebind cannot reduce the previous member-count bound. */
PeakMpiPolicyResult peak_mpi_policy_receiver_init(PeakMpiPolicyReceiver *receiver,
    const PeakMpiPolicyConfig *config, PeakMpiPolicyCohortKey cohort,
    PeakMpiPolicyMemberId member, double now);
PeakMpiPolicyResult peak_mpi_policy_receiver_rebind(PeakMpiPolicyReceiver *receiver,
    PeakMpiPolicyCohortKey cohort, size_t known_member_count,
    int membership_complete, double now);
/* A strictly larger locally observed lower bound invalidates global
 * membership/headroom while retaining local/learned g until feedback is stale.
 * Stale/coarse pressure uses the bound. No epoch or raw history/packet
 * generation is reset. Older/equal hints are ignored. Once this key
 * had feedback, its complete tuples cannot confirm the growth: a new backend
 * epoch plus covered feedback is required. Before the first feedback, an
 * initial matching-key complete tuple may confirm a sufficient bound. */
PeakMpiPolicyResult peak_mpi_policy_receiver_note_membership_hint(PeakMpiPolicyReceiver *receiver,
    size_t known_member_lower_bound, double now);
PeakMpiPolicyResult peak_mpi_policy_receiver_accept(PeakMpiPolicyReceiver *receiver,
    const PeakMpiPolicyFeedback *feedback, double now);
/* A local sticky capture/contract failure must tighten the next heartbeat
 * even if the channel cannot yet deliver that failure to the observer. */
PeakMpiPolicyResult peak_mpi_policy_receiver_mark_coarse(PeakMpiPolicyReceiver *receiver,
    PeakMpiPolicyReason reason, double now);
/* This fixes q_eff once for all four future-demand inputs. Actual history uses
 * original q. U_tail is extended only by local A newer than local_latest_A;
 * already covered profile/control/observer cost is never charged again.
 * Unknown/stale global headroom is NaN and forbids reattach relaxation; caller
 * retains all existing local lease, forecast and safety checks. */
PeakMpiPolicyResult peak_mpi_policy_receiver_snapshot(PeakMpiPolicyReceiver *receiver,
    double now, double local_elapsed_now, double local_A_now,
    double existing_local_headroom, PeakMpiPolicyDecision *decision);

PeakMpiPolicyResult peak_mpi_policy_receiver_snapshot_cut(PeakMpiPolicyReceiver *,
    double now, double local_elapsed_now, PeakMpiCostCut cut,
    double existing_local_headroom, PeakMpiPolicyDecision *);
int peak_mpi_policy_receiver_recover(PeakMpiPolicyReceiver *receiver,
                                     double now);

#ifdef __cplusplus
}
#endif
#endif /* PEAK_MPI_POLICY_ADAPTER_H */
