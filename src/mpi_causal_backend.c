#define _POSIX_C_SOURCE 200809L
#include "internal/mpi_causal_backend.h"
#include "internal/mpi_causal_batches.h"
#include "internal/mpi_causal_match_index.h"
#include "internal/mpi_causal_transport.h"
#include "internal/mpi_progress_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* This parser owns graph references. In particular, a receive never consults
 * a sender's current D: the operation entry/return references survive until
 * the message and request lifecycle have closed. */
typedef struct {
  uint64_t uid, parent, ordinal;
  unsigned kind;
  int color, size, parts, got;
  unsigned canonical;
  size_t offset;
} Comm;
typedef struct {
  unsigned parent, kind;
  uint64_t ordinal;
  int color, size;
  unsigned self;
} Context;
typedef struct {
  unsigned rank;
  int legacy_resolved, legacy_wait;
  int used, ended, completed, matched, sealed, retired, send_matched,
      collective_built;
  uint64_t order, match_nonce, candidate_epoch, visible_nonce, self_nonce,
      self_epoch;
  int visible, candidate_done, ambiguous, ambiguity_closed, send_ambiguous;
  PeakMpiObserverRecord begin, end;
  PeakMpiCostRef entry, exit, result, join, data, launch;
  int has_data, join_s1;
  int result_defined, result_sealed, immediate;
  unsigned observed_count;
  unsigned context;
  size_t peer_op, reserved_by, buffer, dep_head;
  uint64_t dep_gc_stamp;
  double created_at;
  uint64_t group_index, completion_index, consumer_index;
  unsigned consumer_rank;
  size_t analysis_next, analysis_previous;
  int analysis_queued, analysis_waiting;
} Operation;
typedef struct {
  PeakMpiCostRef ref;
  size_t next;
  unsigned rank;
  uint64_t group_index;
} PendingInput;
typedef struct {
  unsigned rank;
  uint64_t generation;
  PeakMpiCostRef seal;
  int closed;
  uint64_t closed_index;
} BufferInterval;
typedef struct {
  PeakMpiCostRef ref;
  PeakMpiPolicySample sample;
  double received, tail;
  uint64_t index;
  size_t next;
} SettledCut;
typedef struct {
  uint64_t requested_nonce, echoed_nonce, verified_nonce;
  uint64_t incarnation, raw_generation, cut_generation, last_event, control,
      mpi_thread, mpi_thread_generation;
  uint64_t *sequence, *anchor, *prefix, *verify_prefix, *verified_prefix,
      *processed;
  uint64_t idle_group, verify_group, verify_nonce, verify_generation,
      last_verify_generation;
  size_t verify_cursor;
  size_t *prefix_slots;
  unsigned char *prefix_seen;
  size_t prefix_slot_count, verify_slot_count;
  int idle, verify_active, verify_idle, verify_stage, has_verified;
  size_t visible_cursor, visible_limit, accept_cursor;
  double verify_L, verify_P, verify_elapsed, verify_received;
  uint64_t verify_control;
  double L, P, elapsed, received_at, covered_at, last_L, last_P;
  uint64_t last_L_cut, last_P_cut;
  PeakMpiCostRef continuation, covered_ref;
  int have_continuation, have_raw, active, coarse, unknown, have_prefix;
  /* Sticky disqualifier: unrelated collectives never enter message lists. */
  int other_point_to_point;
  PeakMpiPolicyReason reason;
  uint64_t lost;
  PeakMpiPolicy *policy;
  PeakMpiPolicyCohortKey key;
  unsigned parent;
  size_t cohort_size;
  PeakMpiPolicySample covered;
  uint64_t parsed_index, covered_index, processed_sum;
  double covered_tail;
  PeakMpiCausalRecovery recovery;
  uint64_t uid_floor, acknowledged_loss, recovery_comm, feedback_epoch;
  size_t cut_head, cut_tail;
  uint64_t root_bound, root_index;
  PeakMpiCostRef root_ref;
  int root_valid;
} Rank;
typedef enum { ROOT_PLAN_IDLE, ROOT_PLAN_RANKS, ROOT_PLAN_OPERATIONS,
  ROOT_PLAN_CUTS, ROOT_PLAN_BEGIN, ROOT_PLAN_CONTINUATIONS,
  ROOT_PLAN_BRANCHES, ROOT_PLAN_READY } RootPlanStage;
struct PeakMpiCausalService {
  PeakMpiCausalServiceConfig config;
  PeakMpiCausalRecovery recovery, recovery_candidate;
  uint64_t recovery_context;
  unsigned recovery_reset, recovery_rank, recovery_prepare, recovery_source;
  size_t recovery_cursor;
  int approximate, hard_fault, recovery_needed, recovery_incomplete,
      recovery_restart;
  double recovery_g, recovery_H;
  PeakMpiCausalTransport *transport;
  PeakMpiCostModel *model;
  PeakMpiCausalBatches *batches;
  PeakMpiCausalMatchIndex *matching;
  PeakMpiCausalBatchView plan;
  size_t plan_index;
  int plan_active, building_concurrent;
  PeakMpiCostRef group_start, group_cost;
  Rank *ranks;
  size_t *prefix_slots;
  unsigned char *prefix_seen;
  uint64_t *sequences, *anchors, *prefixes, *verify_prefixes,
      *verified_prefixes, *processed;
  unsigned visibility_rank, verify_rank_cursor;
  PeakMpiCostRef *nodes;
  size_t node_count;
  Operation *operations;
  BufferInterval *buffers;
  size_t buffer_count, buffer_index_capacity;
  size_t *buffer_index;
  size_t operation_count, scan, index_capacity, free_count;
  size_t *free_ops;
  int gc_stage, gc_dep_keep;
  size_t gc_cursor, gc_kept, gc_dep_cursor, gc_dep_op, gc_cut_cursor;
  PendingInput *deps;
  size_t *free_deps, free_dep_count;
  size_t *index;
  size_t *active;
  uint64_t *keep;
  Comm *comms;
  unsigned *comm_ranks;
  size_t comm_count;
  int *members;
  Context *contexts;
  size_t context_count;
  PeakMpiPolicySample *samples;
  unsigned char *peers;
  uint64_t epoch, generation, order, gc_generation;
  double now;
  unsigned verify_batch_stage, verify_batch_rank, verify_batch_start;
  size_t verify_batch_op, verify_batch_limit;
  RootPlanStage root_plan_stage;
  unsigned root_plan_rank, root_ref_count, root_ref_cursor;
  size_t root_plan_cursor, root_dep_cursor;
  int root_cut_active;
  PeakMpiCostRef root_refs[5];
  int task_stage;
  size_t task_op, task_cursor, task_limit, task_count, task_best, task_bound;
  size_t *candidates;
  size_t task_receive_head[4];
  unsigned task_receive_class;
  int task_ordered, task_was_ambiguous, task_ambiguous, task_ready, task_s1,
      task_all, task_root_only, task_from_root, task_have_data;
  size_t task_receives, task_sends;
  uint64_t task_epoch;
  PeakMpiCostRef task_join, task_data;
  int pub_stage, pub_complete, pub_coarse;
  size_t pub_sample;
  unsigned pub_root, pub_rank;
  size_t pub_op, pub_count, pub_dep;
  double pub_F, pub_age, pub_started, pub_oldest, pub_global_oldest;
  uint64_t pub_epoch;
  size_t last_work_used, last_tick_work;
  unsigned engine_phase;
  uint64_t parsed_records, cycle_records;
  int cycle_idle_progress, parse_finish_only, parse_window_open;
  size_t parse_window_left;
  PeakMpiPolicyFeedback *feedback_staging, *feedback_cache;
  double *feedback_captured;
  unsigned char *feedback_valid;
  unsigned cache_root;
  int cache_dirty;
  SettledCut *cuts;
  size_t *free_cuts, free_cut_count;
  unsigned settle_rank;
  int settle_done;
  size_t analysis_head, analysis_tail;
  int model_running, graph_pending;
  PeakMpiPolicyReason local_fault;
  PeakMpiCausalServiceStats stats;
};
static double sec(uint64_t n) { return (double)n * 1e-9; }
static unsigned root(PeakMpiCausalService *s, unsigned r) {
  while (s->ranks[r].parent != r)
    r = s->ranks[r].parent;
  return r;
}
static void coarse(PeakMpiCausalService *s, unsigned r,
                   PeakMpiPolicyReason reason, uint64_t lost) {
  Rank *a = &s->ranks[r];
  if (reason != PEAK_MPI_POLICY_EVENT_LOSS || !a->recovery.loss)
    s->hard_fault = 1;
  if (!a->coarse &&
      (!s->recovery.state || s->recovery.state == PEAK_MPI_RECOVERY_ACK))
    s->recovery_g =
        s->feedback_valid && s->feedback_valid[r] ? s->feedback_cache[r].g : 1;
  a->coarse = 1;
  a->reason = reason;
  a->lost += lost;
  unsigned cohort = root(s, r);
  for (unsigned j = 0; j < s->config.world_size; j++)
    if (root(s, j) == cohort)
      s->ranks[j].coarse = 1;
  peak_mpi_policy_mark_coarse(s->ranks[root(s, r)].policy, reason, lost,
                              s->now);
}
static int good(PeakMpiCausalService *s, unsigned r, PeakMpiCostStatus st) {
  if (st == PEAK_MPI_COST_OK || st == PEAK_MPI_COST_DUPLICATE) {
    if (st == PEAK_MPI_COST_OK)
      s->graph_pending = 1;
    return 1;
  }
  coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
  return 0;
}
static int edge(PeakMpiCausalService *s, unsigned r, PeakMpiCostRef a,
                PeakMpiCostRef b, PeakMpiCostEdgeKind k, double w) {
  return good(s, r, peak_mpi_cost_model_add_edge(s->model, a, b, k, w));
}
static int reserve(PeakMpiCausalService *s, unsigned r, PeakMpiCostRef *out) {
  if (s->node_count >= s->config.model.max_nodes) {
    coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
    return 0;
  }
  if (!good(s, r, peak_mpi_cost_model_reserve_node(s->model, out)))
    return 0;
  s->nodes[s->node_count++] = *out;
  return 1;
}
static int node(PeakMpiCausalService *s, unsigned r, double L, double P,
                const PeakMpiObserverRecord *rec, PeakMpiCostRef *out) {
  PeakMpiCostOwnerRef owner = PEAK_MPI_COST_NO_OWNER;
  if (L < -1e-12 || P < -1e-12 || P > L + 1e-12) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 0;
  }
  if (L > 0 || P > 0) {
    if (!good(s, r, peak_mpi_cost_model_reserve_owner(s->model, &owner)) ||
        !good(s, r,
              peak_mpi_cost_model_define_owner(s->model, owner, r, fmax(0, L),
                                               fmax(0, P))))
      return 0;
  }
  PeakMpiCostNodeSpec spec = {PEAK_MPI_COST_EVENT, r, owner, 0, 0, 0, 0, 0};
  if (rec) {
    spec.single_execution_flow =
        (rec->flags & PEAK_MPI_OBSERVER_SLACK_SINGLE_FLOW) != 0;
    spec.progress_clear =
        (rec->flags & PEAK_MPI_OBSERVER_SLACK_NO_OPEN_PROGRESS) != 0;
  }
  return reserve(s, r, out) &&
         good(s, r, peak_mpi_cost_model_define_node(s->model, *out, &spec));
}
static size_t op_hash(unsigned rank, uint64_t uid) {
  uint64_t x = uid ^ ((uint64_t)rank << 32);
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  return (size_t)(x ^ (x >> 31));
}
static Operation *find_op(PeakMpiCausalService *s, unsigned rank,
                          uint64_t uid) {
  size_t h = op_hash(rank, uid);
  for (size_t i = 0; i < 64; i++) {
    size_t v = s->index[(h + i) & (s->index_capacity - 1)];
    if (!v)
      return NULL;
    if (v != SIZE_MAX) {
      Operation *o = &s->operations[v - 1];
      if (o->rank == rank && o->begin.op_uid == uid)
        return o;
    }
  }
  return NULL;
}
static int index_insert(PeakMpiCausalService *s, size_t slot) {
  Operation *o = &s->operations[slot];
  size_t h = op_hash(o->rank, o->begin.op_uid), empty = SIZE_MAX;
  for (size_t i = 0; i < 64; i++) {
    size_t at = (h + i) & (s->index_capacity - 1), v = s->index[at];
    if (v == SIZE_MAX && empty == SIZE_MAX)
      empty = at;
    if (!v) {
      if (empty == SIZE_MAX)
        empty = at;
      s->index[empty] = slot + 1;
      return 1;
    }
  }
  if (empty != SIZE_MAX) {
    s->index[empty] = slot + 1;
    return 1;
  }
  coarse(s, o->rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
  return 0;
}
static void index_remove(PeakMpiCausalService *s, size_t slot) {
  Operation *o = &s->operations[slot];
  size_t h = op_hash(o->rank, o->begin.op_uid);
  for (size_t i = 0; i < 64; i++) {
    size_t at = (h + i) & (s->index_capacity - 1);
    if (s->index[at] == slot + 1) {
      s->index[at] = SIZE_MAX;
      return;
    }
    if (!s->index[at])
      return;
  }
}
static BufferInterval *buffer_interval(PeakMpiCausalService *s, unsigned rank,
                                       uint64_t generation, int create) {
  size_t h = op_hash(rank, generation);
  for (size_t i = 0; i < 64; i++) {
    size_t at = (h + i) & (s->buffer_index_capacity - 1),
           v = s->buffer_index[at];
    if (v) {
      BufferInterval *b = &s->buffers[v - 1];
      if (b->rank == rank && b->generation == generation)
        return b;
    } else {
      if (!create)
        return NULL;
      if (!generation || s->buffer_count == s->config.max_context_parts) {
        coarse(s, rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
        return NULL;
      }
      BufferInterval *b = &s->buffers[s->buffer_count++];
      b->rank = rank;
      b->generation = generation;
      if (!reserve(s, rank, &b->seal))
        return NULL;
      s->buffer_index[at] = s->buffer_count;
      return b;
    }
  }
  if (create)
    coarse(s, rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
  return NULL;
}
static PeakMpiCostRef progress_input(PeakMpiCausalService *s, Operation *o) {
  if (o->buffer != SIZE_MAX)
    return s->buffers[o->buffer].seal;
  if (o->end.progress_scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL &&
      (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate)
    return o->result;
  return o->immediate ? o->launch : o->exit;
}
static void remember_completion_input(PeakMpiCausalService *s, Operation *send,
                                      PeakMpiCostRef ref, unsigned rank,
                                      uint64_t index) {
  if (!(send->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) || send->immediate)
    return;
  if (!s->free_dep_count) {
    coarse(s, send->rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
    return;
  }
  size_t at = s->free_deps[--s->free_dep_count];
  s->deps[at] = (PendingInput){ref, send->dep_head, rank, index};
  send->dep_head = at;
}
static int send_op(unsigned o) {
  return o == PEAK_MPI_CAUSAL_SEND || o == PEAK_MPI_CAUSAL_SSEND ||
         o == PEAK_MPI_CAUSAL_BSEND || o == PEAK_MPI_CAUSAL_RSEND ||
         o == PEAK_MPI_CAUSAL_SENDRECV;
}
static int recv_op(unsigned o) {
  return o == PEAK_MPI_CAUSAL_RECV || o == PEAK_MPI_CAUSAL_SENDRECV ||
         o == PEAK_MPI_CAUSAL_MRECV;
}
static int probe_op(unsigned o) {
  return o == PEAK_MPI_CAUSAL_PROBE || o == PEAK_MPI_CAUSAL_MPROBE;
}
static int remote_op(const PeakMpiObserverRecord *r) {
  return send_op(r->operation) || recv_op(r->operation) ||
         probe_op(r->operation) || (r->flags & PEAK_MPI_CAUSAL_COLLECTIVE);
}
static PeakMpiCostRef output(Operation *o) {
  return (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate
             ? o->result
             : o->exit;
}
static int merge(PeakMpiCausalService *s, unsigned a, unsigned b) {
  a = root(s, a);
  b = root(s, b);
  if (a == b)
    return 1;
  if (s->ranks[a].cohort_size < s->ranks[b].cohort_size) {
    unsigned t = a;
    a = b;
    b = t;
  }
  PeakMpiPolicyCohortKey k = {s->config.job_context, a, ++s->epoch};
  if (peak_mpi_policy_merge(s->ranks[a].policy, s->ranks[b].policy, k,
                            s->now) != PEAK_MPI_POLICY_OK) {
    coarse(s, a, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
    return 0;
  }
  s->ranks[a].key = k;
  s->ranks[b].parent = a;
  s->ranks[a].cohort_size += s->ranks[b].cohort_size;
  if (s->ranks[a].coarse || s->ranks[b].coarse)
    for (unsigned j = 0; j < s->config.world_size; j++)
      if (root(s, j) == a)
        s->ranks[j].coarse = 1;
  return 1;
}
static unsigned context_for(PeakMpiCausalService *s, unsigned rank,
                            uint64_t uid) {
  for (size_t i = 0; i < s->comm_count; i++)
    if (s->comm_ranks[i] == rank && s->comms[i].uid == uid)
      return s->comms[i].canonical;
  return 0;
}
static unsigned canonical(PeakMpiCausalService *s, unsigned rank, Comm *c) {
  unsigned parent = 0, self = 0;
  if (c->kind == PEAK_MPI_CAUSAL_COMM_SELF)
    self = rank + 1;
  else if (c->kind != PEAK_MPI_CAUSAL_COMM_WORLD) {
    parent = context_for(s, rank, c->parent);
    if (!parent)
      return 0;
  }
  for (size_t i = 0; i < s->context_count; i++) {
    Context *x = &s->contexts[i];
    if (x->parent == parent && x->kind == c->kind && x->ordinal == c->ordinal &&
        x->color == c->color && x->self == self && x->size == c->size)
      return (unsigned)i + 1;
  }
  if (s->context_count >= s->config.max_context_parts)
    return 0;
  Context *x = &s->contexts[s->context_count++];
  *x = (Context){parent, c->kind, c->ordinal, c->color, c->size, self};
  return (unsigned)s->context_count;
}
int peak_mpi_causal_service_ingest_object(PeakMpiCausalService *s, unsigned r,
                                          uint64_t inc,
                                          const PeakMpiObserverObject *obj) {
  if (!s || !s->ranks || !obj || r >= s->config.world_size)
    return 0;
  Rank *a = &s->ranks[r];
  if (a->incarnation && a->incarnation != inc) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  a->incarnation = inc;
  if (obj->kind != PEAK_MPI_CAUSAL_OBJECT_COMM)
    return 1;
  PeakMpiCausalCommPart p;
  memcpy(&p, obj->payload, sizeof p);
  if (p.local_size < 0 || p.world_size != s->config.world_size ||
      p.member_count > 16 ||
      p.member_offset + p.member_count > (unsigned)p.local_size) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  size_t i;
  for (i = 0; i < s->comm_count; i++)
    if (s->comm_ranks[i] == r && s->comms[i].uid == p.local_uid)
      break;
  if (i == s->comm_count) {
    if (i >= s->config.max_context_parts ||
        (size_t)p.local_size > s->config.max_context_parts * 16) {
      coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
      return 1;
    }
    size_t off = 0;
    for (size_t j = 0; j < i; j++)
      off += (size_t)s->comms[j].size;
    if (off + (size_t)p.local_size > s->config.max_context_parts * 16) {
      coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
      return 1;
    }
    s->comm_ranks[i] = r;
    s->comms[i] =
        (Comm){p.local_uid, p.parent_uid, p.creation_ordinal,   p.creation_kind,
               p.color,     p.local_size, (int)obj->part_count, 0,
               0,           off};
    s->comm_count++;
    for (int j = 0; j < p.local_size; j++)
      s->members[off + (size_t)j] = -1;
  }
  Comm *c = &s->comms[i];
  for (unsigned j = 0; j < p.member_count; j++) {
    int v = p.members[j];
    if (v < 0 || (unsigned)v >= s->config.world_size) {
      coarse(s, r, PEAK_MPI_POLICY_UNKNOWN_MEMBERS, 0);
      continue;
    }
    int *dst = &s->members[c->offset + p.member_offset + j];
    if (*dst < 0) {
      *dst = v;
      c->got++;
    } else if (*dst != v)
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
  }
  if (c->got == c->size)
    c->canonical = canonical(s, r, c);
  return 1;
}
static uint64_t prefix_sum(const uint64_t *prefix, size_t slots) {
  uint64_t sum = 0;
  for (size_t i = 0; i < slots; i++) {
    if (UINT64_MAX - sum < prefix[i])
      return UINT64_MAX;
    sum += prefix[i];
  }
  return sum;
}
static uint64_t prefix_sum_slots(const uint64_t *prefix, const size_t *slots,
                                 size_t count) {
  uint64_t sum = 0;
  for (size_t i = 0; i < count; i++) {
    uint64_t value = prefix[slots[i]];
    if (UINT64_MAX - sum < value)
      return UINT64_MAX;
    sum += value;
  }
  return sum;
}
static void raw_to_policy(PeakMpiCausalService *s, unsigned r) {
  Rank *a = &s->ranks[r];
  PeakMpiPolicySample p = {
      {r, s->config.job_context},
      a->raw_generation,
      a->have_prefix ? prefix_sum(a->prefix, s->config.slots) : a->last_event,
      a->control,
      a->L,
      a->P,
      0,
      a->elapsed,
      0,
      0};
  peak_mpi_policy_record_raw(s->ranks[root(s, r)].policy,
                             s->ranks[root(s, r)].key, &p, s->now);
}
static void freeze_verify_tuple(Rank *a) {
  a->verify_slot_count = a->prefix_slot_count;
  for (size_t i = 0; i < a->verify_slot_count; i++) {
    size_t slot = a->prefix_slots[i];
    a->verify_prefix[slot] = a->prefix[slot];
  }
  a->verify_nonce = a->echoed_nonce;
  a->verify_generation = a->raw_generation;
  a->verify_cursor = 0;
  a->verify_active = 1;
  a->verify_stage = 1;
  a->accept_cursor = 0;
  a->verify_idle = a->idle;
  a->verify_group = a->idle_group;
  a->verify_L = a->L;
  a->verify_P = a->P;
  a->verify_elapsed = a->elapsed;
  a->verify_control = a->control;
  a->verify_received = a->received_at;
}
int peak_mpi_causal_service_ingest_raw(PeakMpiCausalService *s, unsigned r,
                                       const PeakMpiCausalRaw *raw,
                                       double now) {
  if (!s || !s->ranks || !raw || r >= s->config.world_size)
    return 0;
  s->now = now;
  Rank *a = &s->ranks[r];
  if (a->incarnation && a->incarnation != raw->incarnation) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  a->incarnation = raw->incarnation;
  if (!isfinite(raw->L) || !isfinite(raw->P) || !isfinite(raw->elapsed) ||
      raw->L < 0 || raw->P < 0 || raw->P > raw->L || raw->elapsed < 0) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 1;
  }
  if (a->have_raw && raw->generation < a->raw_generation)
    return 1;
  if (a->have_raw &&
      (raw->L < a->L || raw->P < a->P || raw->elapsed < a->elapsed)) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 1;
  }
  if (s->approximate && raw->recovery.epoch < s->recovery.epoch)
    return 1;
  if (s->recovery_reset && raw->recovery.loss > a->recovery.loss)
    s->recovery_restart = 1;
  a->recovery = raw->recovery;
  if (a->recovery.fault_mask &
      ~(UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW))
    s->hard_fault = 1;
  if (a->recovery.loss > a->acknowledged_loss)
    s->recovery_needed = 1;
  a->raw_generation = raw->generation;
  a->echoed_nonce = raw->watermark_nonce;
  a->idle = raw->idle_cut;
  a->idle_group = raw->idle_group_uid;
  a->L = raw->L;
  a->P = raw->P;
  a->elapsed = raw->elapsed;
  a->control = raw->control_generation;
  a->received_at = now;
  a->have_raw = 1;
  a->have_prefix = raw->slot_prefixes && raw->slot_count == s->config.slots;
  if (a->have_prefix) {
    /* The existing RAW prefix copy also discovers slots once. This cumulative
     * list only appends; an in-flight verifier freezes its earlier length. */
    for (size_t i = 0; i < s->config.slots; i++) {
      uint64_t value = raw->slot_prefixes[i];
      if (value && !a->prefix_seen[i]) {
        a->prefix_seen[i] = 1;
        a->prefix_slots[a->prefix_slot_count++] = i;
      }
      a->prefix[i] = value;
    }
  }
  a->active |= raw->active;
  a->unknown |= raw->unknown_members;
  if (raw->known_peers)
    for (size_t j = 0; j < raw->peer_count && j < s->config.world_size; j++)
      if (raw->known_peers[j] && j != r) {
        s->peers[(size_t)r * s->config.world_size + j] = 1;
        merge(s, r, (unsigned)j);
      }
  raw_to_policy(s, r);
  if (raw->unknown_members)
    peak_mpi_policy_set_membership_known(s->ranks[root(s, r)].policy, 0, now);
  if ((raw->coarse_reason != PEAK_MPI_OBSERVER_REASON_NONE &&
       !(raw->coarse_reason == PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW &&
         raw->recovery.epoch == s->recovery.epoch &&
         raw->lost_events <= a->acknowledged_loss)) ||
      raw->lost_events > a->acknowledged_loss) {
    PeakMpiPolicyReason why =
        raw->coarse_reason == PEAK_MPI_OBSERVER_REASON_CAPABILITY
            ? PEAK_MPI_POLICY_MISSING_CONTRACT
            : PEAK_MPI_POLICY_EVENT_LOSS;
    coarse(s, r, why,
           raw->lost_events > a->lost ? raw->lost_events - a->lost : 0);
  }
  if (!a->coarse && !s->approximate && !s->recovery_reset &&
      !s->recovery_needed && a->have_prefix && !a->verify_active &&
      a->requested_nonce > a->verified_nonce &&
      a->echoed_nonce >= a->requested_nonce &&
      a->raw_generation > a->last_verify_generation)
    freeze_verify_tuple(a);
  return 1;
}
static int advance(PeakMpiCausalService *s, unsigned r,
                   const PeakMpiObserverRecord *rec, PeakMpiCostRef *out) {
  Rank *a = &s->ranks[r];
  double L = sec(rec->L_cut), P = sec(rec->P_cut);
  if (rec->L_cut < a->last_L_cut || rec->P_cut < a->last_P_cut ||
      rec->P_cut - a->last_P_cut > rec->L_cut - a->last_L_cut) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 0;
  }
  if (!node(s, r, sec(rec->L_cut - a->last_L_cut),
            sec(rec->P_cut - a->last_P_cut), rec, out))
    return 0;
  int thread_edge = 0;
  if (rec->phase == PEAK_MPI_CAUSAL_CALL_BEGIN && rec->input_ref) {
    Operation *prior = find_op(s, r, rec->input_ref);
    thread_edge = prior && prior->exit.slot == a->continuation.slot &&
                  prior->exit.generation == a->continuation.generation;
  }
  if (a->have_continuation && !thread_edge &&
      !edge(s, r, a->continuation, *out, PEAK_MPI_COST_PROGRESS, 0))
    return 0;
  a->last_L = L;
  a->last_P = P;
  a->last_L_cut = rec->L_cut;
  a->last_P_cut = rec->P_cut;
  a->continuation = *out;
  a->have_continuation = 1;
  return 1;
}
static void seal_output(PeakMpiCausalService *s, Operation *o) {
  if (o->begin.operation == PEAK_MPI_CAUSAL_SENDRECV &&
      (!o->matched || !o->send_matched))
    return;
  if ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate) {
    if (o->result_defined && !o->result_sealed) {
      good(s, o->rank, peak_mpi_cost_model_seal(s->model, o->result));
      o->result_sealed = 1;
    }
  } else if (o->ended && !o->sealed) {
    good(s, o->rank, peak_mpi_cost_model_seal(s->model, o->exit));
    o->sealed = 1;
  }
}
static void legacy_resolve(PeakMpiCausalService *s, Operation *o) {
  double bound = 0;
  for (unsigned r = 0; r < s->config.world_size; r++)
    bound += s->ranks[r].L;
  /* This is an ownerless paid-history upper anchor, never another raw owner. */
  PeakMpiCostRef ref;
  if (!reserve(s, o->rank, &ref))
    return;
  double inside = fmax(0, sec(o->end.L_cut) - sec(o->begin.L_cut));
  PeakMpiCostNodeSpec spec = {PEAK_MPI_COST_EVENT,
                              o->rank,
                              PEAK_MPI_COST_NO_OWNER,
                              fmax(0, bound - inside),
                              0,
                              0,
                              0,
                              0};
  good(s, o->rank, peak_mpi_cost_model_define_node(s->model, ref, &spec));
  good(s, o->rank, peak_mpi_cost_model_seal(s->model, ref));
  edge(s, o->rank, ref, output(o), PEAK_MPI_COST_PROGRESS, 0);
  o->legacy_resolved = 1;
  if (o->begin.operation == PEAK_MPI_CAUSAL_SENDRECV)
    o->send_matched = 1;
  if (recv_op(o->begin.operation) || probe_op(o->begin.operation)) {
    o->matched = o->candidate_done = 1;
    peak_mpi_causal_match_index_remove_role(s->matching,
                                            PEAK_MPI_CAUSAL_MATCH_RECEIVE,
                                            (size_t)(o - s->operations));
  }
  seal_output(s, o);
}
int peak_mpi_causal_service_ingest_record(PeakMpiCausalService *s, unsigned r,
                                          const PeakMpiObserverRecord *rec) {
  if (!s || !s->ranks || !rec || r >= s->config.world_size)
    return 0;
  Rank *a = &s->ranks[r];
  if (a->incarnation && a->incarnation != rec->incarnation) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  a->incarnation = rec->incarnation;
  if (!rec->thread_uid || rec->thread_uid > s->config.slots) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  uint64_t *seq = &a->sequence[rec->thread_uid - 1];
  if (a->uid_floor && rec->kind != PEAK_MPI_OBSERVER_EVENT_OBJECT &&
      rec->op_uid < a->uid_floor) {
    if (rec->sequence > *seq) {
      *seq = rec->sequence;
      uint64_t *done = &a->processed[rec->thread_uid - 1];
      if (rec->sequence > *done) {
        a->processed_sum += rec->sequence - *done;
        *done = rec->sequence;
      }
    }
    return 1;
  }
  if (rec->sequence <= *seq) {
    s->stats.duplicates++;
    return 1;
  }
  if (rec->sequence != *seq + 1) {
    s->hard_fault = 1;
    coarse(s, r, PEAK_MPI_POLICY_EVENT_LOSS, rec->sequence - *seq - 1);
    *seq = rec->sequence;
    return 1;
  }
  if (!a->coarse && rec->kind != PEAK_MPI_OBSERVER_EVENT_OBJECT &&
      !peak_mpi_causal_batches_ingest(s->batches, r, rec))
    coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 1);
  *seq = rec->sequence;
  s->stats.records++;
  a->last_event++;
  return 1;
}
static PeakMpiCausalMatchKey collective_key(Operation *o) {
  PeakMpiCausalMatchKey key = {o->context, o->begin.collective_ordinal, 0, 0,
                               0,          (int32_t)o->begin.operation};
  return key;
}
static void match_index_insert(PeakMpiCausalService *s, Operation *o) {
  size_t slot = (size_t)(o - s->operations);
  int ok = 1;
  if (send_op(o->begin.operation) && o->begin.peer >= 0) {
    PeakMpiCausalMatchKey key = {
        o->context, 0, (int32_t)o->rank, o->begin.peer, o->begin.tag, 0};
    ok &= peak_mpi_causal_match_index_insert(
        s->matching, PEAK_MPI_CAUSAL_MATCH_SEND, slot, &key);
  }
  if (recv_op(o->begin.operation) ||
      o->begin.operation == PEAK_MPI_CAUSAL_MPROBE) {
    int peer = o->begin.operation == PEAK_MPI_CAUSAL_SENDRECV ? o->begin.root
                                                              : o->begin.peer;
    int tag = o->begin.operation == PEAK_MPI_CAUSAL_SENDRECV
                  ? o->begin.actual_tag
                  : o->begin.tag;
    if (peer >= PEAK_MPI_CAUSAL_ANY_SOURCE) {
      PeakMpiCausalMatchKey key = {o->context,       0,   peer,
                                   (int32_t)o->rank, tag, 0};
      ok &= peak_mpi_causal_match_index_insert(
          s->matching, PEAK_MPI_CAUSAL_MATCH_RECEIVE, slot, &key);
    }
  }
  if (o->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE) {
    PeakMpiCausalMatchKey key = collective_key(o);
    ok &= peak_mpi_causal_match_index_insert(
        s->matching, PEAK_MPI_CAUSAL_MATCH_COLLECTIVE, slot, &key);
  }
  if (!ok)
    coarse(s, o->rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
}
static int queue_cut(PeakMpiCausalService *s, unsigned rank, PeakMpiCostRef ref,
                     PeakMpiPolicySample sample, double received) {
  if (!s->free_cut_count) {
    coarse(s, rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
    return 0;
  }
  size_t at = s->free_cuts[--s->free_cut_count];
  Rank *r = &s->ranks[rank];
  s->cuts[at] = (SettledCut){
      ref, sample, received, sample.L - r->last_L, r->parsed_index, SIZE_MAX};
  if (r->cut_tail != SIZE_MAX)
    s->cuts[r->cut_tail].next = at;
  else
    r->cut_head = at;
  r->cut_tail = at;
  /* An idle RAW cut can widen the required frontier without adding a graph
   * edge. Ask for a new rooted solve before moving outside the old scope. */
  if (s->root_cut_active && s->cuts[at].index > r->root_index)
    s->graph_pending = 1;
  return 1;
}
static size_t advance_covered(PeakMpiCausalService *s, size_t allowance) {
  size_t used = 0;
  unsigned idle = 0;
  s->settle_done = 0;
  while (used < allowance && idle < s->config.world_size) {
    unsigned rank = s->settle_rank++ % s->config.world_size;
    Rank *r = &s->ranks[rank];
    used++;
    if (r->cut_head == SIZE_MAX) {
      idle++;
      continue;
    }
    SettledCut *cut = &s->cuts[r->cut_head];
    double d;
    if (s->root_cut_active && cut->index > r->root_index) {
      idle++;
      continue;
    }
    if (peak_mpi_cost_model_delay(s->model, cut->ref, &d) != PEAK_MPI_COST_OK) {
      idle++;
      continue;
    }
    size_t at = r->cut_head;
    r->covered_ref = cut->ref;
    r->covered = cut->sample;
    r->covered_tail = cut->tail;
    r->covered.D = d + cut->tail;
    r->covered.generation = ++r->cut_generation;
    r->covered_at = cut->received;
    r->covered_index = cut->index;
    r->cut_head = cut->next;
    if (r->cut_head == SIZE_MAX)
      r->cut_tail = SIZE_MAX;
    s->free_cuts[s->free_cut_count++] = at;
    idle = 0;
  }
  s->settle_done = idle == s->config.world_size;
  return used;
}
static void analysis_remove(PeakMpiCausalService *s, Operation *o) {
  if (!o->analysis_queued)
    return;
  size_t previous = o->analysis_previous, next = o->analysis_next;
  if (previous != SIZE_MAX)
    s->operations[previous].analysis_next = next;
  else
    s->analysis_head = next;
  if (next != SIZE_MAX)
    s->operations[next].analysis_previous = previous;
  else
    s->analysis_tail = previous;
  o->analysis_queued = 0;
  o->analysis_next = o->analysis_previous = SIZE_MAX;
}
static void analysis_enqueue(PeakMpiCausalService *s, Operation *o, int front) {
  if (!o->used)
    return;
  if (o->analysis_queued) {
    if (!front)
      return;
    analysis_remove(s, o);
  }
  size_t at = (size_t)(o - s->operations);
  o->analysis_waiting = 0;
  o->analysis_queued = 1;
  if (front) {
    o->analysis_previous = SIZE_MAX;
    o->analysis_next = s->analysis_head;
    if (s->analysis_head != SIZE_MAX)
      s->operations[s->analysis_head].analysis_previous = at;
    else
      s->analysis_tail = at;
    s->analysis_head = at;
  } else {
    o->analysis_next = SIZE_MAX;
    o->analysis_previous = s->analysis_tail;
    if (s->analysis_tail != SIZE_MAX)
      s->operations[s->analysis_tail].analysis_next = at;
    else
      s->analysis_head = at;
    s->analysis_tail = at;
  }
}
static void analysis_wake(PeakMpiCausalService *s, Operation *o) {
  if (!o->analysis_waiting || !o->used)
    return;
  if (o->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE) {
    if (o->ended && (!o->collective_built || o->result_defined || o->immediate))
      analysis_enqueue(s, o, 0);
    return;
  }
  int source =
      send_op(o->begin.operation) && !recv_op(o->begin.operation)
          ? o->begin.peer
          : o->end.actual_source;
  if (source < 0 || (unsigned)source >= s->config.world_size)
    return;
  Rank *remote = &s->ranks[(unsigned)source], *local = &s->ranks[o->rank];
  if (remote->verified_nonce >= o->match_nonce &&
      local->verified_nonce >= o->self_nonce)
    analysis_enqueue(s, o, 0);
}
static int parse_record(PeakMpiCausalService *s, unsigned r,
                        const PeakMpiObserverRecord *rec) {
  Rank *a = &s->ranks[r];
  if (a->coarse)
    return 1;
  if (rec->thread_uid && rec->thread_uid <= s->config.slots &&
      rec->sequence > a->processed[rec->thread_uid - 1]) {
    a->processed_sum += rec->sequence - a->processed[rec->thread_uid - 1];
    a->processed[rec->thread_uid - 1] = rec->sequence;
  }
  if (rec->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN) {
    a->parsed_index++;
    if (!s->plan_active || !s->plan.concurrent)
      return 1;
    PeakMpiCostRef ref;
    if (!advance(s, r, rec, &ref))
      return 1;
    good(s, r, peak_mpi_cost_model_seal(s->model, ref));
    return 1;
  }
  if (rec->phase == PEAK_MPI_CAUSAL_GROUP_END) {
    if (!a->have_continuation) {
      coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
      return 1;
    }
    PeakMpiPolicySample sample = {{r, s->config.job_context},
                                  0,
                                  a->processed_sum,
                                  rec->control_generation,
                                  sec(rec->L_cut),
                                  sec(rec->P_cut),
                                  0,
                                  sec(rec->progress_end),
                                  0,
                                  0};
    queue_cut(s, r, a->continuation, sample, s->now);
    return 1;
  }
  if (rec->phase == PEAK_MPI_CAUSAL_REQUEST_RETIRED) {
    Operation *o = find_op(s, r, rec->op_uid);
    if (o)
      o->retired = 1;
    else
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  if (rec->phase == PEAK_MPI_CAUSAL_CALL_BEGIN ||
      rec->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE) {
    if (find_op(s, r, rec->op_uid)) {
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      return 1;
    }
    if (!s->free_count) {
      coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
      return 1;
    }
    size_t i = s->free_ops[--s->free_count];
    Operation *o = &s->operations[i];
    memset(o, 0, sizeof *o);
    o->used = 1;
    o->created_at = s->now;
    o->rank = r;
    o->begin = *rec;
    o->order = ++s->order;
    o->peer_op = SIZE_MAX;
    o->reserved_by = SIZE_MAX;
    o->buffer = SIZE_MAX;
    o->dep_head = SIZE_MAX;
    o->analysis_next = o->analysis_previous = SIZE_MAX;
    o->group_index = a->parsed_index;
    o->context = context_for(s, r, rec->communicator_uid);
    index_insert(s, i);
    match_index_insert(s, o);
    s->active[s->operation_count++] = i;
    if (rec->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE) {
      Operation *start = find_op(s, r, rec->batch_uid);
      if (!start || !node(s, r, 0, 0, rec, &o->entry)) {
        coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
        return 1;
      }
      edge(s, r, start->entry, o->entry, PEAK_MPI_COST_GUARANTEED, 0);
    } else if (s->building_concurrent) {
      if (!node(s, r, 0, 0, rec, &o->entry))
        return 1;
      edge(s, r, s->group_start, o->entry, PEAK_MPI_COST_PROGRESS, 0);
      edge(s, r, s->group_cost, o->entry, PEAK_MPI_COST_PROGRESS, 0);
    } else if (!advance(s, r, rec, &o->entry))
      return 1;
    if (rec->input_ref) {
      Operation *prior = find_op(s, r, rec->input_ref);
      if (!prior) {
        coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
        return 1;
      }
      edge(s, r, prior->exit, o->entry, PEAK_MPI_COST_GUARANTEED, 0);
    }
    good(s, r, peak_mpi_cost_model_seal(s->model, o->entry));
    reserve(s, r, &o->exit);
    if ((rec->flags & PEAK_MPI_CAUSAL_NONBLOCKING) &&
        rec->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE)
      reserve(s, r, &o->result);
    if ((rec->flags & PEAK_MPI_CAUSAL_NONBLOCKING) &&
        rec->phase == PEAK_MPI_CAUSAL_CALL_BEGIN && s->plan_active) {
      const PeakMpiObserverRecord *end =
          peak_mpi_causal_batches_call_end(s->batches, r, rec->op_uid);
      if (!end) {
        coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
        return 1;
      }
      o->end = *end;
      o->immediate = (end->flags & PEAK_MPI_CAUSAL_INACTIVE_REQUEST) != 0;
      if (!o->immediate)
        reserve(s, r, &o->result);
    }

    if (rec->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE) {
      Operation *start = find_op(s, r, rec->batch_uid);
      PeakMpiCostNodeSpec spec = {
          PEAK_MPI_COST_EVENT, r, PEAK_MPI_COST_NO_OWNER, 0, 0, 0, 0, 0};
      good(s, r, peak_mpi_cost_model_define_node(s->model, o->exit, &spec));
      edge(s, r, start->exit, o->exit, PEAK_MPI_COST_GUARANTEED, 0);
      good(s, r, peak_mpi_cost_model_seal(s->model, o->exit));
      o->end = *rec;
      if (rec->progress_scope == PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS)
        o->end.flags |= PEAK_MPI_OBSERVER_PROGRESS_SEALED;
      o->ended = o->sealed = 1;
    }

    if (remote_op(rec)) {
      if ((send_op(rec->operation) || recv_op(rec->operation) ||
           probe_op(rec->operation)) &&
          (rec->operation != PEAK_MPI_CAUSAL_SENDRECV ||
           (rec->flags & PEAK_MPI_CAUSAL_NONBLOCKING)))
        a->other_point_to_point = 1;
      if (a->mpi_thread && (a->mpi_thread != rec->thread_uid ||
                            a->mpi_thread_generation != rec->thread_generation))
        a->mpi_thread = UINT64_MAX;
      else {
        a->mpi_thread = rec->thread_uid;
        a->mpi_thread_generation = rec->thread_generation;
      }
    }
    if ((rec->flags & PEAK_MPI_CAUSAL_BUFFERED) &&
        rec->progress_scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL) {
      BufferInterval *b = buffer_interval(s, r, rec->progress_begin, 1);
      if (b)
        o->buffer = (size_t)(b - s->buffers);
    }
    if (remote_op(rec) && !o->context)
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    if (remote_op(rec) &&
        (rec->progress_scope == PEAK_MPI_OBSERVER_SCOPE_UNKNOWN ||
         (rec->capability_uid != PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC &&
          rec->capability_uid !=
              PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED)))
      coarse(s, r, PEAK_MPI_POLICY_MISSING_CONTRACT, 0);
    return 1;
  }
  if (s->approximate && rec->phase == PEAK_MPI_CAUSAL_REQUEST_COMPLETE &&
      (rec->flags & PEAK_MPI_CAUSAL_LEGACY_INPUT)) {
    Operation *wait = find_op(s, r, rec->batch_uid);
    if (wait) {
      wait->observed_count++;
      double bound = s->ranks[r].last_L;
      for (unsigned other = 0; other < s->config.world_size; other++)
        if (other != r)
          bound += s->ranks[other].L;
      PeakMpiCostRef ref;
      if (reserve(s, r, &ref)) {
        PeakMpiCostNodeSpec spec = {
            PEAK_MPI_COST_EVENT, r, PEAK_MPI_COST_NO_OWNER, bound, 0, 0, 0, 0};
        good(s, r, peak_mpi_cost_model_define_node(s->model, ref, &spec));
        good(s, r, peak_mpi_cost_model_seal(s->model, ref));
        edge(s, r, ref, wait->exit, PEAK_MPI_COST_PROGRESS, 0);
      }
    } else
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  Operation *o = find_op(s, r, rec->op_uid);
  if (!o) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  if (rec->phase == PEAK_MPI_CAUSAL_REQUEST_COMPLETE) {
    if (o->completed) {
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      return 1;
    }
    Operation *wait = find_op(s, r, rec->batch_uid);
    uint64_t request_uid =
        o->begin.request_uid ? o->begin.request_uid : o->end.request_uid;
    uint64_t request_generation = o->begin.request_uid
                                      ? o->begin.request_generation
                                      : o->end.request_generation;
    uint64_t activation =
        o->begin.request_uid ? o->begin.activation : o->end.activation;
    if (!wait || !(o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) ||
        (request_uid && (rec->request_uid != request_uid ||
                         rec->request_generation != request_generation ||
                         rec->activation != activation))) {
      coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      return 1;
    }
    PeakMpiCostNodeSpec result_spec = {
        PEAK_MPI_COST_EVENT, r, PEAK_MPI_COST_NO_OWNER, 0, 0, 0, 0, 0};
    if (!good(
            s, r,
            peak_mpi_cost_model_define_node(s->model, o->result, &result_spec)))
      return 1;
    wait->observed_count++;
    o->completion_index = wait->group_index;
    o->result_defined = 1;
    o->completed = 1;
    o->retired = (rec->flags & PEAK_MPI_CAUSAL_RECLAIMED) != 0;
    o->end.actual_source = rec->actual_source;
    o->end.actual_tag = rec->actual_tag;
    o->end.flags |= rec->flags;
    edge(s, r, o->entry, o->result, PEAK_MPI_COST_GUARANTEED, 0);
    edge(s, r, wait->entry, o->result, PEAK_MPI_COST_GUARANTEED, 0);
    edge(s, r, o->result, wait->exit, PEAK_MPI_COST_GUARANTEED, 0);
    /* The whole observed completion interval is conservative progress input.
     * Its inside cost has one owner on wait.exit, even for Waitall. */
    edge(s, r, wait->exit, o->result, PEAK_MPI_COST_PROGRESS, 0);
    if (rec->flags & PEAK_MPI_OBSERVER_CANCELLED) {
      o->matched = 1;
      if (rec->flags & PEAK_MPI_OBSERVER_CANCELLED)
        o->send_matched = 1;
      seal_output(s, o);
    } else if (send_op(o->begin.operation) &&
               (o->send_matched ||
                o->begin.operation == PEAK_MPI_CAUSAL_BSEND ||
                o->begin.operation == PEAK_MPI_CAUSAL_RSEND)) {
      seal_output(s, o);
    }
    if (recv_op(o->begin.operation) ||
        (s->approximate && send_op(o->begin.operation)) ||
        (o->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE))
      analysis_enqueue(s, o, 0);
    return 1;
  }
  if (rec->phase != PEAK_MPI_CAUSAL_CALL_END)
    return 1;
  if (o->ended) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
    return 1;
  }
  if ((o->begin.operation == PEAK_MPI_CAUSAL_WAIT ||
       o->begin.operation == PEAK_MPI_CAUSAL_TEST) &&
      rec->completion_count != o->observed_count) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 1;
  }
  int actual_source = o->end.actual_source, actual_tag = o->end.actual_tag;
  o->end = *rec;
  if (o->result_defined) {
    o->end.actual_source = actual_source;
    o->end.actual_tag = actual_tag;
  }
  o->ended = 1;
  if (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) {
    o->immediate = (rec->flags & PEAK_MPI_CAUSAL_INACTIVE_REQUEST) != 0;
    if (!o->immediate && !o->result.generation)
      reserve(s, r, &o->result);
    else
      o->retired = 1;
  }
  a->anchor[rec->thread_uid - 1] = rec->op_uid;
  o->completed |=
      o->immediate || !(o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING);
  uint64_t Lcut = s->building_concurrent ? a->last_L_cut : rec->L_cut;
  uint64_t Pcut = s->building_concurrent ? a->last_P_cut : rec->P_cut;
  double L = sec(Lcut), P = sec(Pcut);
  PeakMpiCostOwnerRef own = PEAK_MPI_COST_NO_OWNER;
  if (Lcut < a->last_L_cut || Pcut < a->last_P_cut ||
      Pcut - a->last_P_cut > Lcut - a->last_L_cut) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 1;
  }
  if (Lcut > a->last_L_cut || Pcut > a->last_P_cut) {
    if (!good(s, r, peak_mpi_cost_model_reserve_owner(s->model, &own)) ||
        !good(s, r,
              peak_mpi_cost_model_define_owner(s->model, own, r,
                                               sec(Lcut - a->last_L_cut),
                                               sec(Pcut - a->last_P_cut))))
      return 1;
  }
  PeakMpiCostNodeSpec spec = {PEAK_MPI_COST_EVENT, r, own, 0, 0, 0, 0, 0};
  if (o->immediate) {
    if (!reserve(s, r, &o->launch))
      return 1;
    good(s, r, peak_mpi_cost_model_define_node(s->model, o->launch, &spec));
    edge(s, r, o->entry, o->launch, PEAK_MPI_COST_GUARANTEED, 0);
    good(s, r, peak_mpi_cost_model_seal(s->model, o->launch));
    spec.owner = PEAK_MPI_COST_NO_OWNER;
  }
  good(s, r, peak_mpi_cost_model_define_node(s->model, o->exit, &spec));
  if (o->immediate)
    edge(s, r, o->launch, o->exit, PEAK_MPI_COST_GUARANTEED, 0);
  if (!(o->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE) ||
      (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING))
    edge(s, r, o->entry, o->exit, PEAK_MPI_COST_GUARANTEED, 0);
  if (!s->building_concurrent) {
    a->last_L = L;
    a->last_P = P;
    a->last_L_cut = Lcut;
    a->last_P_cut = Pcut;
    a->continuation = o->exit;
    a->have_continuation = 1;
  }
  if (o->begin.operation == PEAK_MPI_CAUSAL_BUFFER_DETACH && !rec->mpi_error) {
    BufferInterval *b = buffer_interval(s, r, rec->progress_begin, 0);
    if (b && !b->closed) {
      PeakMpiCostNodeSpec spec = {
          PEAK_MPI_COST_EVENT, r, PEAK_MPI_COST_NO_OWNER, 0, 0, 0, 0, 0};
      good(s, r, peak_mpi_cost_model_define_node(s->model, b->seal, &spec));
      edge(s, r, o->exit, b->seal, PEAK_MPI_COST_PROGRESS, 0);
      good(s, r, peak_mpi_cost_model_seal(s->model, b->seal));
      b->closed = 1;
      b->closed_index = o->group_index;
    }
  }
  if ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate) {
    good(s, r, peak_mpi_cost_model_seal(s->model, o->exit));
    o->sealed = 1;
  } else if (!remote_op(&o->begin) || rec->peer == PEAK_MPI_CAUSAL_PROC_NULL ||
             (rec->flags & PEAK_MPI_OBSERVER_CANCELLED) || rec->mpi_error) {
    o->matched = 1;
    o->send_matched = 1;
    seal_output(s, o);
  } else if (o->begin.operation == PEAK_MPI_CAUSAL_BSEND ||
             o->begin.operation == PEAK_MPI_CAUSAL_RSEND) {
    seal_output(s, o);
  }
  if ((s->approximate && send_op(o->begin.operation)) ||
      (o->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE) ||
      ((recv_op(o->begin.operation) || probe_op(o->begin.operation)) &&
       o->completed))
    analysis_enqueue(s, o, 0);
  return 1;
}
static int same_message(Operation *send, Operation *recv) {
  int source = recv->end.actual_source, tag = recv->end.actual_tag;
  return send->context == recv->context && send->rank == (unsigned)source &&
         send->begin.peer == (int)recv->rank && send->begin.tag == tag;
}
static int envelope(const Operation *a, const Operation *b) {
  return a->context == b->context && a->rank == b->rank &&
         a->end.actual_source == b->end.actual_source &&
         a->end.actual_tag == b->end.actual_tag;
}
static int eligible_send(Operation *send, Operation *recv) {
  return send->used && send_op(send->begin.operation) && !send->send_matched &&
         send->ended && same_message(send, recv) && send->visible &&
         send->visible_nonce <= recv->candidate_epoch;
}
static int same_collective(const Operation *a, const Operation *b) {
  return b->used && (b->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE) &&
         a->context == b->context &&
         a->begin.collective_ordinal == b->begin.collective_ordinal &&
         a->begin.operation == b->begin.operation;
}
static PeakMpiCostRef launch_input(Operation *o) {
  if (!(o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING))
    return o->entry;
  if (o->immediate)
    return o->launch;
  return o->end.progress_scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL ? o->result
                                                                   : o->exit;
}
static void collective_output(PeakMpiCausalService *s, Operation *o) {
  if (o->matched || ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) &&
                     !o->immediate && !o->result_defined))
    return;
  edge(s, o->rank, o->join, output(o),
       o->join_s1 ? PEAK_MPI_COST_GUARANTEED
                  : PEAK_MPI_COST_COLLECTIVE_CANDIDATE,
       0);
  if (o->has_data)
    edge(s, o->rank, o->data, output(o), PEAK_MPI_COST_GUARANTEED, 0);
  o->matched = 1;
  seal_output(s, o);
}
enum {
  TASK_NONE,
  TASK_MESSAGES,
  TASK_EARLIER,
  TASK_EDGES,
  TASK_MESSAGE_END,
  TASK_CLOSE_RECEIVES,
  TASK_CLOSE_SENDS,
  TASK_CLOSE_COMMIT,
  TASK_COLLECTIVE_SCAN,
  TASK_COLLECTIVE_INPUT,
  TASK_COLLECTIVE_OUTPUT
};
static void matcher_work(PeakMpiCausalService *s, size_t budget) {
  size_t initial = budget;
  s->last_work_used = 0;
  while (budget) {
    budget--;
    s->last_work_used++;
    if (budget < initial / 2 && !s->task_stage)
      return;
    if (!s->task_stage) {
      if (s->analysis_head == SIZE_MAX)
        return;
      Operation *o = &s->operations[s->analysis_head];
      analysis_remove(s, o);
      if (s->ranks[o->rank].coarse)
        continue;
      s->task_op = (size_t)(o - s->operations);
      s->task_cursor = s->task_count = 0;
      s->task_limit = s->operation_count;
      s->task_best = s->task_bound = SIZE_MAX;
      s->task_ready = s->task_ordered = 1;
      s->task_was_ambiguous = 0;
      if (o->begin.flags & PEAK_MPI_CAUSAL_COLLECTIVE) {
        if (!o->ended)
          continue;
        if (o->collective_built) {
          collective_output(s, o);
          continue;
        }
        s->task_s1 = 1;
        PeakMpiCausalMatchKey key = collective_key(o);
        s->task_cursor = peak_mpi_causal_match_index_first(
            s->matching, PEAK_MPI_CAUSAL_MATCH_COLLECTIVE, &key);
        s->task_stage = TASK_COLLECTIVE_SCAN;
        continue;
      }
      if (s->approximate && send_op(o->begin.operation) &&
          !recv_op(o->begin.operation)) {
        if (!o->ended || !o->completed || o->send_matched ||
            o->legacy_resolved ||
            ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate &&
             !o->result_defined))
          continue;
        int peer = o->begin.peer;
        if (peer < 0 || (unsigned)peer >= s->config.world_size)
          continue;
        Rank *remote = &s->ranks[peer], *local = &s->ranks[o->rank];
        if (!o->match_nonce)
          o->match_nonce = ++remote->requested_nonce;
        if (!o->self_nonce)
          o->self_nonce = ++local->requested_nonce;
        if (remote->verified_nonce < o->match_nonce ||
            local->verified_nonce < o->self_nonce) {
          o->analysis_waiting = 1;
          continue;
        }
        if (!o->legacy_wait) {
          o->legacy_wait = 1;
          o->match_nonce = ++remote->requested_nonce;
          o->self_nonce = ++local->requested_nonce;
          o->analysis_waiting = 1;
          continue;
        }
        legacy_resolve(s, o);
        continue;
      }
      if (!o->ended || !o->completed ||
          (!recv_op(o->begin.operation) && !probe_op(o->begin.operation)))
        continue;
      if (o->candidate_done) {
        if (o->ambiguous && !o->ambiguity_closed) {
          s->task_stage = TASK_CLOSE_RECEIVES;
          s->task_receives = s->task_sends = 0;
          s->task_epoch = 0;
        }
        continue;
      }
      if (o->matched)
        continue;
      if (o->end.actual_source < 0) {
        if (o->end.actual_source == PEAK_MPI_CAUSAL_PROC_NULL) {
          o->matched = 1;
          seal_output(s, o);
        }
        continue;
      }
      unsigned sender = (unsigned)o->end.actual_source;
      if (sender >= s->config.world_size) {
        coarse(s, o->rank, PEAK_MPI_POLICY_UNKNOWN_MEMBERS, 0);
        continue;
      }
      Rank *r = &s->ranks[sender];
      if (!o->match_nonce) {
        if (r->requested_nonce == UINT64_MAX) {
          coarse(s, sender, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
          continue;
        }
        o->match_nonce = ++r->requested_nonce;
      }
      Rank *local = &s->ranks[o->rank];
      if (!o->self_nonce) {
        if (local->requested_nonce == UINT64_MAX) {
          coarse(s, o->rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
          continue;
        }
        o->self_nonce = ++local->requested_nonce;
      }
      if (r->verified_nonce < o->match_nonce ||
          local->verified_nonce < o->self_nonce) {
        o->analysis_waiting = 1;
        continue;
      }
      if (!o->candidate_epoch)
        o->candidate_epoch = r->verified_nonce;
      if (!o->self_epoch)
        o->self_epoch = local->verified_nonce;
      if (o->begin.operation == PEAK_MPI_CAUSAL_MRECV) {
        Operation *p = find_op(s, o->rank, o->begin.progress_begin);
        if (!p || !p->matched) {
          if (s->approximate)
            legacy_resolve(s, o);
          continue;
        }
        if (p->peer_op != SIZE_MAX)
          s->task_bound = p->peer_op;
        else {
          o->candidate_epoch = p->candidate_epoch;
          o->self_epoch = p->self_epoch;
        }
      }
      PeakMpiCausalMatchKey key = {o->context,           0,
                                   o->end.actual_source, (int32_t)o->rank,
                                   o->end.actual_tag,    0};
      for (unsigned k = 0; k < 4; k++) {
        PeakMpiCausalMatchKey key = {
            o->context,
            0,
            k & 1 ? PEAK_MPI_CAUSAL_ANY_SOURCE : o->end.actual_source,
            (int32_t)o->rank,
            k & 2 ? PEAK_MPI_CAUSAL_ANY_TAG : o->end.actual_tag,
            0};
        s->task_receive_head[k] = peak_mpi_causal_match_index_first(
            s->matching, PEAK_MPI_CAUSAL_MATCH_RECEIVE, &key);
      }
      s->task_cursor = peak_mpi_causal_match_index_first(
          s->matching, PEAK_MPI_CAUSAL_MATCH_SEND, &key);
      s->task_stage = TASK_MESSAGES;
      continue;
    }
    Operation *o = &s->operations[s->task_op];
    if (s->task_stage == TASK_MESSAGES) {
      if (s->task_cursor == SIZE_MAX) {
        if (!s->task_count || !s->task_ready) {
          if (s->approximate && !s->task_count) {
            legacy_resolve(s, o);
            s->task_stage = 0;
            continue;
          }
          o->analysis_waiting = 1;
          s->task_stage = 0;
          continue;
        }
        s->task_ambiguous =
            (s->task_count > 1 && !s->task_ordered) || s->task_was_ambiguous;
        s->task_receive_class = 0;
        s->task_cursor = s->task_receive_head[0];
        s->task_stage = TASK_EARLIER;
        continue;
      }
      size_t slot = s->task_cursor;
      s->task_cursor = peak_mpi_causal_match_index_next(
          s->matching, PEAK_MPI_CAUSAL_MATCH_SEND, slot);
      Operation *x = &s->operations[slot];
      if (!eligible_send(x, o) ||
          (s->task_bound != SIZE_MAX && slot != s->task_bound) ||
          (x->reserved_by != SIZE_MAX && s->task_bound == SIZE_MAX))
        continue;
      if (s->task_best != SIZE_MAX) {
        Operation *best = &s->operations[s->task_best];
        if (best->begin.thread_uid != x->begin.thread_uid ||
            best->begin.thread_generation != x->begin.thread_generation)
          s->task_ordered = 0;
        if (x->begin.sequence < best->begin.sequence)
          s->task_best = slot;
      } else
        s->task_best = slot;
      s->candidates[s->task_count++] = slot;
      s->task_was_ambiguous |= x->send_ambiguous;
      /* A single producer/generation containing only blocking Sendrecv P2P
       * calls has FIFO sends. With no earlier receive in any wildcard class,
       * its first eligible send is the ordered match; otherwise keep scanning. */
      if (s->task_count == 1 && !s->task_was_ambiguous && !s->approximate &&
          o->begin.operation == PEAK_MPI_CAUSAL_SENDRECV &&
          x->begin.operation == PEAK_MPI_CAUSAL_SENDRECV &&
          !x->send_ambiguous && !s->ranks[o->rank].other_point_to_point &&
          !s->ranks[x->rank].other_point_to_point &&
          s->ranks[o->rank].mpi_thread == o->begin.thread_uid &&
          s->ranks[o->rank].mpi_thread_generation == o->begin.thread_generation &&
          s->ranks[x->rank].mpi_thread == x->begin.thread_uid &&
          s->ranks[x->rank].mpi_thread_generation == x->begin.thread_generation) {
        int first_receive = 1;
        for (unsigned k = 0; k < 4; k++) {
          size_t head = s->task_receive_head[k];
          if (head != SIZE_MAX) {
            Operation *prior = &s->operations[head];
            if (prior->begin.thread_uid != o->begin.thread_uid ||
                prior->begin.thread_generation != o->begin.thread_generation ||
                prior->begin.sequence < o->begin.sequence)
              first_receive = 0;
          }
        }
        if (first_receive)
          s->task_cursor = SIZE_MAX;
      }
      if (x->buffer == SIZE_MAX &&
          x->end.progress_scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL &&
          (x->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !x->immediate &&
          !x->result_defined)
        s->task_ready = 0;
    } else if (s->task_stage == TASK_EARLIER) {
      if (s->task_cursor == SIZE_MAX && s->task_receive_class < 3) {
        s->task_cursor = s->task_receive_head[++s->task_receive_class];
        continue;
      }
      if (s->task_cursor == SIZE_MAX) {
        s->task_cursor = 0;
        if (!s->task_ambiguous) {
          s->task_count = 1;
          s->candidates[0] = s->task_best;
        }
        s->task_stage = TASK_EDGES;
        continue;
      }
      size_t slot = s->task_cursor;
      s->task_cursor = peak_mpi_causal_match_index_next(
          s->matching, PEAK_MPI_CAUSAL_MATCH_RECEIVE, slot);
      Operation *x = &s->operations[slot];
      Operation *send = &s->operations[s->task_best];
      /* Only one producer/generation makes exact-key insertion order a
       * per-thread sequence order. Never infer an order between threads. */
      Rank *local = &s->ranks[o->rank];
      if (local->mpi_thread == o->begin.thread_uid &&
          local->mpi_thread_generation == o->begin.thread_generation &&
          x->begin.thread_uid == o->begin.thread_uid &&
          x->begin.thread_generation == o->begin.thread_generation &&
          x->begin.sequence >= o->begin.sequence) {
        s->task_cursor = SIZE_MAX;
        continue;
      }
      if (x == o ||
          (o->begin.operation == PEAK_MPI_CAUSAL_MRECV &&
           x->begin.op_uid == o->begin.progress_begin) ||
          x->rank != o->rank || x->context != o->context ||
          (!recv_op(x->begin.operation) &&
           x->begin.operation != PEAK_MPI_CAUSAL_MPROBE) ||
          !x->visible || x->visible_nonce > o->self_epoch)
        continue;
      int same_thread =
          x->begin.thread_uid == o->begin.thread_uid &&
          x->begin.thread_generation == o->begin.thread_generation;
      if (same_thread && x->begin.sequence >= o->begin.sequence)
        continue;
      if (x->matched && (!x->ambiguous || x->ambiguity_closed))
        continue;
      int peer = x->begin.operation == PEAK_MPI_CAUSAL_SENDRECV ? x->begin.root
                                                                : x->begin.peer;
      int tag = x->begin.operation == PEAK_MPI_CAUSAL_SENDRECV
                    ? x->begin.actual_tag
                    : x->begin.tag;
      if (same_thread && x->completed && !x->matched) {
        peer = x->end.actual_source;
        tag = x->end.actual_tag;
      }
      if ((peer == PEAK_MPI_CAUSAL_ANY_SOURCE || peer == (int)send->rank) &&
          (tag == PEAK_MPI_CAUSAL_ANY_TAG || tag == send->begin.tag)) {
        if (same_thread && x->completed && !x->matched) {
          analysis_enqueue(s, x, 1);
          if (x->match_nonce && x->self_nonce) {
            o->match_nonce = o->match_nonce > x->match_nonce ? o->match_nonce
                                                             : x->match_nonce;
            o->self_nonce =
                o->self_nonce > x->self_nonce ? o->self_nonce : x->self_nonce;
            o->analysis_waiting = 1;
          } else
            analysis_enqueue(s, o, 0);
          s->task_stage = 0;
        } else
          s->task_ambiguous = 1;
      }
    } else if (s->task_stage == TASK_EDGES) {
      if (s->task_cursor == s->task_count) {
        s->task_stage = TASK_MESSAGE_END;
        continue;
      }
      Operation *send = &s->operations[s->candidates[s->task_cursor++]];
      PeakMpiCostRef scope = progress_input(s, send);
      merge(s, send->rank, o->rank);
      edge(s, o->rank, send->entry, output(o),
           s->task_ambiguous ? PEAK_MPI_COST_PROGRESS
                             : PEAK_MPI_COST_GUARANTEED,
           0);
      edge(s, o->rank, scope, output(o), PEAK_MPI_COST_PROGRESS, 0);
      if (!probe_op(o->begin.operation)) {
        send->consumer_rank = o->rank;
        uint64_t join =
            (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate
                ? o->completion_index
                : o->group_index;
        if (join > send->consumer_index)
          send->consumer_index = join;
        if (!send->legacy_resolved &&
            (send->begin.operation == PEAK_MPI_CAUSAL_SEND ||
             send->begin.operation == PEAK_MPI_CAUSAL_SENDRECV))
          edge(s, send->rank, o->entry, output(send),
               PEAK_MPI_COST_SEND_REVERSE, 0);
        else if (!send->legacy_resolved &&
                 send->begin.operation == PEAK_MPI_CAUSAL_SSEND)
          edge(s, send->rank, o->entry, output(send),
               s->task_ambiguous ? PEAK_MPI_COST_PROGRESS
                                 : PEAK_MPI_COST_GUARANTEED,
               0);
        if (send->begin.operation == PEAK_MPI_CAUSAL_SEND ||
            send->begin.operation == PEAK_MPI_CAUSAL_SENDRECV ||
            send->begin.operation == PEAK_MPI_CAUSAL_SSEND)
          remember_completion_input(s, send, o->entry, o->rank, o->group_index);
        if (s->task_ambiguous)
          send->send_ambiguous = 1;
      }
    } else if (s->task_stage == TASK_MESSAGE_END) {
      o->candidate_done = o->matched = 1;
      o->ambiguous =
          s->task_ambiguous && (!probe_op(o->begin.operation) ||
                                o->begin.operation == PEAK_MPI_CAUSAL_MPROBE);
      if (!s->task_ambiguous) {
        Operation *send = &s->operations[s->task_best];
        o->peer_op = s->task_best;
        if (!probe_op(o->begin.operation)) {
          send->send_matched = 1;
          peak_mpi_causal_match_index_remove_role(
              s->matching, PEAK_MPI_CAUSAL_MATCH_SEND,
              (size_t)(send - s->operations));
          if (send->begin.operation != PEAK_MPI_CAUSAL_SENDRECV)
            send->matched = 1;
          send->peer_op = s->task_op;
          seal_output(s, send);
        }
        if (o->begin.operation == PEAK_MPI_CAUSAL_MPROBE)
          send->reserved_by = s->task_op;
      }
      seal_output(s, o);
      if (!o->ambiguous)
        peak_mpi_causal_match_index_remove_role(
            s->matching, PEAK_MPI_CAUSAL_MATCH_RECEIVE, s->task_op);
      s->task_stage = 0;
      if (o->ambiguous) {
        s->task_stage = TASK_CLOSE_RECEIVES;
        s->task_cursor = 0;
        s->task_receives = s->task_sends = 0;
        s->task_epoch = 0;
      }
    } else if (s->task_stage == TASK_CLOSE_RECEIVES) {
      if (s->task_cursor == s->task_limit) {
        s->task_cursor = 0;
        s->task_stage = s->task_receives ? TASK_CLOSE_SENDS : TASK_NONE;
        continue;
      }
      Operation *x = &s->operations[s->active[s->task_cursor++]];
      if (x->candidate_done && x->ambiguous && !x->ambiguity_closed &&
          envelope(x, o) && !probe_op(x->begin.operation)) {
        s->task_receives++;
        if (x->candidate_epoch > s->task_epoch)
          s->task_epoch = x->candidate_epoch;
      }
    } else if (s->task_stage == TASK_CLOSE_SENDS) {
      if (s->task_cursor == s->task_limit) {
        s->task_cursor = 0;
        s->task_stage =
            s->task_receives == s->task_sends ? TASK_CLOSE_COMMIT : TASK_NONE;
        continue;
      }
      Operation *x = &s->operations[s->active[s->task_cursor++]];
      if (send_op(x->begin.operation) && !x->send_matched && x->ended &&
          same_message(x, o) && x->visible && x->visible_nonce <= s->task_epoch)
        s->task_sends++;
    } else if (s->task_stage == TASK_CLOSE_COMMIT) {
      if (s->task_cursor == s->task_limit) {
        s->task_stage = 0;
        continue;
      }
      Operation *x = &s->operations[s->active[s->task_cursor++]];
      if (x->candidate_done && x->ambiguous && !x->ambiguity_closed &&
          x->candidate_epoch <= s->task_epoch && envelope(x, o)) {
        x->ambiguity_closed = 1;
        peak_mpi_causal_match_index_remove_role(s->matching,
                                                PEAK_MPI_CAUSAL_MATCH_RECEIVE,
                                                (size_t)(x - s->operations));
      }
      if (send_op(x->begin.operation) && !x->send_matched && x->ended &&
          same_message(x, o) && x->visible &&
          x->visible_nonce <= s->task_epoch) {
        x->send_matched = 1;
        peak_mpi_causal_match_index_remove_role(s->matching,
                                                PEAK_MPI_CAUSAL_MATCH_SEND,
                                                (size_t)(x - s->operations));
        if (x->begin.operation != PEAK_MPI_CAUSAL_SENDRECV)
          x->matched = 1;
        seal_output(s, x);
      }
    } else if (s->task_stage == TASK_COLLECTIVE_SCAN) {
      if (s->task_cursor == SIZE_MAX) {
        if (!s->task_ready || !s->task_count || !o->context ||
            s->task_count != (unsigned)s->contexts[o->context - 1].size) {
          s->task_stage = 0;
          continue;
        }
        PeakMpiCostNodeSpec spec = {PEAK_MPI_COST_COLLECTIVE,
                                    PEAK_MPI_COST_NO_RANK,
                                    PEAK_MPI_COST_NO_OWNER,
                                    0,
                                    (uint32_t)s->task_count,
                                    0,
                                    0,
                                    s->task_s1 != 0};
        if (!reserve(s, o->rank, &s->task_join) ||
            !good(s, o->rank,
                  peak_mpi_cost_model_define_node(s->model, s->task_join,
                                                  &spec))) {
          s->task_stage = 0;
          continue;
        }
        s->task_all = s->task_root_only = s->task_from_root =
            s->task_have_data = 0;
        if (!s->task_s1 && !(o->begin.flags & PEAK_MPI_CAUSAL_ZERO_DATA))
          switch (o->begin.operation) {
          case PEAK_MPI_CAUSAL_ALLREDUCE:
          case PEAK_MPI_CAUSAL_ALLGATHER:
          case PEAK_MPI_CAUSAL_ALLTOALL:
          case PEAK_MPI_CAUSAL_REDUCE_SCATTER:
            s->task_all = 1;
            break;
          case PEAK_MPI_CAUSAL_REDUCE:
          case PEAK_MPI_CAUSAL_GATHER:
            s->task_all = s->task_root_only = 1;
            break;
          case PEAK_MPI_CAUSAL_BCAST:
          case PEAK_MPI_CAUSAL_SCATTER:
            s->task_from_root = 1;
            break;
          default:
            break;
          }
        if (s->task_all) {
          if (!node(s, o->rank, 0, 0, NULL, &s->task_data)) {
            s->task_stage = 0;
            continue;
          }
          s->task_have_data = 1;
        }
        s->task_cursor = 0;
        s->task_stage = TASK_COLLECTIVE_INPUT;
        continue;
      }
      size_t slot = s->task_cursor;
      s->task_cursor = peak_mpi_causal_match_index_next(
          s->matching, PEAK_MPI_CAUSAL_MATCH_COLLECTIVE, slot);
      Operation *x = &s->operations[slot];
      if (!same_collective(o, x))
        continue;
      s->candidates[s->task_count++] = slot;
      if (!x->ended)
        s->task_ready = 0;
      if (!(x->begin.flags & PEAK_MPI_OBSERVER_SLACK_S1))
        s->task_s1 = 0;
    } else if (s->task_stage == TASK_COLLECTIVE_INPUT) {
      if (s->task_cursor == s->task_count) {
        if (s->task_all)
          good(s, o->rank, peak_mpi_cost_model_seal(s->model, s->task_data));
        good(s, o->rank, peak_mpi_cost_model_seal(s->model, s->task_join));
        s->task_cursor = 0;
        s->task_stage = TASK_COLLECTIVE_OUTPUT;
        continue;
      }
      Operation *x = &s->operations[s->candidates[s->task_cursor++]];
      merge(s, o->rank, x->rank);
      x->join = s->task_join;
      x->join_s1 = s->task_s1;
      x->collective_built = 1;
      PeakMpiCostRef input = launch_input(x);
      edge(s, x->rank, input, s->task_join,
           s->task_s1 ? PEAK_MPI_COST_SLACK_INPUT
                      : PEAK_MPI_COST_COLLECTIVE_CANDIDATE,
           sec(x->end.wait_ns));
      if (s->task_all)
        edge(s, x->rank, input, s->task_data, PEAK_MPI_COST_GUARANTEED, 0);
      if (s->task_from_root && (int)x->rank == o->begin.root) {
        s->task_data = input;
        s->task_have_data = 1;
      }
    } else if (s->task_stage == TASK_COLLECTIVE_OUTPUT) {
      if (s->task_cursor == s->task_count) {
        s->task_stage = 0;
        continue;
      }
      Operation *x = &s->operations[s->candidates[s->task_cursor++]];
      if (s->task_have_data &&
          (!s->task_root_only || (int)x->rank == o->begin.root)) {
        x->data = s->task_data;
        x->has_data = 1;
      }
      collective_output(s, x);
    }
  }
}
static uint64_t branch_cut_index(PeakMpiCausalService *s, unsigned rank, int planned) {
  return planned ? s->ranks[rank].root_index : s->ranks[rank].covered_index;
}
static int branch_live_for_indices(PeakMpiCausalService *s, Operation *o, int planned) {
  uint64_t local_cut = branch_cut_index(s, o->rank, planned);
  if (o->group_index > local_cut)
    return 0;
  if ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate &&
      (!o->completed || o->completion_index > local_cut))
    return 1;
  if (send_op(o->begin.operation) && !o->legacy_resolved &&
      (!o->send_matched || o->consumer_index > branch_cut_index(s, o->consumer_rank, planned)))
    return 1;
  if (o->buffer != SIZE_MAX &&
      (!s->buffers[o->buffer].closed || s->buffers[o->buffer].closed_index > local_cut))
    return 1;
  if (o->ambiguous && !o->ambiguity_closed)
    return 1;
  if (o->begin.operation == PEAK_MPI_CAUSAL_MPROBE && o->peer_op != SIZE_MAX &&
      s->operations[o->peer_op].used) {
    Operation *send = &s->operations[o->peer_op];
    if (!send->send_matched ||
        send->consumer_index > branch_cut_index(s, send->consumer_rank, planned))
      return 1;
  }
  return 0;
}
static int branch_live_at_cut(PeakMpiCausalService *s, Operation *o) {
  return branch_live_for_indices(s, o, 0);
}
static int branch_live_at_root(PeakMpiCausalService *s, Operation *o) {
  return branch_live_for_indices(s, o, 1);
}
static void root_ref(PeakMpiCausalService *s, PeakMpiCostRef ref) {
  /* The single producer below adds at most five references: entry, exit,
   * result, join, and buffer seal. Dependency roots are streamed separately. */
  if (ref.generation)
    s->root_refs[s->root_ref_count++] = ref;
}
/* Selection is only a heuristic. Full incoming/owner closure decides readiness. */
static int prepare_root_cut(PeakMpiCausalService *s, size_t budget) {
  s->last_work_used = 0;
  while (budget) {
    if (s->root_plan_stage == ROOT_PLAN_RANKS) {
      if (s->root_plan_rank == s->config.world_size) {
        s->root_plan_stage = ROOT_PLAN_OPERATIONS;
        s->root_plan_cursor = 0;
        continue;
      }
      Rank *r = &s->ranks[s->root_plan_rank++];
      r->root_bound = UINT64_MAX;
      r->root_index = r->covered_index;
      r->root_ref = r->covered_ref;
      r->root_valid = r->covered.generation != 0;
    } else if (s->root_plan_stage == ROOT_PLAN_OPERATIONS) {
      if (s->root_plan_cursor == s->operation_count) {
        s->root_plan_stage = ROOT_PLAN_CUTS;
        s->root_plan_rank = 0;
        s->root_plan_cursor = SIZE_MAX;
        continue;
      }
      Operation *o = &s->operations[s->active[s->root_plan_cursor++]];
      Rank *r = &s->ranks[o->rank];
      if (o->group_index > r->covered_index &&
          !peak_mpi_cost_model_closed_hint(s->model, o->exit) && o->group_index < r->root_bound)
        r->root_bound = o->group_index;
    } else if (s->root_plan_stage == ROOT_PLAN_CUTS) {
      if (s->root_plan_rank == s->config.world_size) {
        s->root_plan_stage = ROOT_PLAN_BEGIN;
        s->root_plan_rank = 0;
        continue;
      }
      Rank *r = &s->ranks[s->root_plan_rank];
      if (s->root_plan_cursor == SIZE_MAX)
        s->root_plan_cursor = r->cut_head;
      if (s->root_plan_cursor == SIZE_MAX) {
        s->root_plan_rank++;
        --budget;
        ++s->last_work_used;
        continue;
      }
      SettledCut *c = &s->cuts[s->root_plan_cursor];
      if (c->index >= r->root_bound) {
        s->root_plan_rank++;
        s->root_plan_cursor = SIZE_MAX;
        --budget;
        ++s->last_work_used;
        continue;
      }
      r->root_index = c->index;
      r->root_ref = c->ref;
      r->root_valid = 1;
      s->root_plan_cursor = c->next;
      if (s->root_plan_cursor == SIZE_MAX)
        s->root_plan_rank++;
    } else if (s->root_plan_stage == ROOT_PLAN_BEGIN) {
      if (good(s, s->config.owner_rank, peak_mpi_cost_model_roots_begin(s->model))) {
        s->root_plan_stage = ROOT_PLAN_CONTINUATIONS;
        s->root_plan_rank = 0;
      } else
        return 1;
    } else if (s->root_plan_stage == ROOT_PLAN_CONTINUATIONS) {
      if (s->root_plan_rank == s->config.world_size) {
        s->root_plan_stage = ROOT_PLAN_BRANCHES;
        s->root_plan_cursor = 0;
        s->root_ref_count = s->root_ref_cursor = 0;
        s->root_dep_cursor = SIZE_MAX;
        continue;
      }
      Rank *r = &s->ranks[s->root_plan_rank++];
      if (r->root_valid)
        good(s, s->config.owner_rank, peak_mpi_cost_model_roots_add(s->model, r->root_ref));
    } else {
      if (s->root_ref_cursor < s->root_ref_count) {
        good(s, s->config.owner_rank,
             peak_mpi_cost_model_roots_add(s->model, s->root_refs[s->root_ref_cursor++]));
      } else if (s->root_dep_cursor != SIZE_MAX) {
        PendingInput *d = &s->deps[s->root_dep_cursor];
        s->root_dep_cursor = d->next;
        if (d->group_index <= s->ranks[d->rank].root_index)
          good(s, s->config.owner_rank, peak_mpi_cost_model_roots_add(s->model, d->ref));
      } else {
        if (s->root_plan_cursor == s->operation_count) {
          s->root_cut_active = 1;
          /* READY survives an exhausted heartbeat allowance. Until the first
           * run starts, it distinguishes a new root set from an old cut cap. */
          s->root_plan_stage = ROOT_PLAN_READY;
          return 1;
        }
        Operation *o = &s->operations[s->active[s->root_plan_cursor++]];
        Rank *r = &s->ranks[o->rank];
        s->root_ref_cursor = s->root_ref_count = 0;
        if (o->group_index <= r->root_index &&
            (o->group_index > r->covered_index || branch_live_at_cut(s, o) ||
             branch_live_at_root(s, o))) {
          if ((!o->result_defined || o->completion_index > r->root_index) &&
              o->dep_head != SIZE_MAX)
            s->root_dep_cursor = o->dep_head;
          root_ref(s, o->entry);
          if (o->ended)
            root_ref(s, o->exit);
          if (o->result_defined && o->completion_index <= r->root_index)
            root_ref(s, o->result);
          if (o->collective_built && peak_mpi_cost_model_closed_hint(s->model, o->join))
            root_ref(s, o->join);
          if (o->buffer != SIZE_MAX && s->buffers[o->buffer].closed &&
              s->buffers[o->buffer].closed_index <= r->root_index)
            root_ref(s, s->buffers[o->buffer].seal);
        }
      }
    }
    --budget;
    ++s->last_work_used;
  }
  return 0;
}
static int publish_work(PeakMpiCausalService *s, size_t budget) {
  s->last_work_used = 0;
  if (!s->pub_stage) {
    s->pub_stage = 1;
    s->pub_root = 0;
    s->pub_epoch = s->epoch;
    s->pub_started = s->now;
    s->pub_global_oldest = 0;
  }
  if (s->pub_epoch != s->epoch) {
    s->pub_stage = 0;
    return 0;
  }
  while (budget) {
    budget--;
    s->last_work_used++;
    if (s->pub_stage == 1) {
      if (s->pub_root == s->config.world_size) {
        s->pub_stage = 0;
        s->stats.oldest_unresolved_age = s->pub_global_oldest;
        return 1;
      }
      if (root(s, s->pub_root) != s->pub_root) {
        s->pub_root++;
        continue;
      }
      s->pub_rank = 0;
      s->pub_op = s->pub_count = 0;
      s->pub_F = s->pub_age = s->pub_oldest = 0;
      s->pub_complete = 1;
      s->pub_coarse = s->ranks[s->pub_root].coarse;
      s->pub_stage = 2;
    } else if (s->pub_stage == 2) {
      if (s->pub_rank == s->config.world_size) {
        s->pub_stage = 3;
        continue;
      }
      unsigned rank = s->pub_rank++;
      if (root(s, rank) != s->pub_root)
        continue;
      Rank *a = &s->ranks[rank];
      if (s->pub_coarse) {
        if (!a->have_raw) {
          s->pub_complete = 0;
          continue;
        }
        PeakMpiPolicySample p = {{rank, s->config.job_context},
                                 a->raw_generation,
                                 a->have_prefix
                                     ? prefix_sum(a->prefix, s->config.slots)
                                     : a->last_event,
                                 a->control,
                                 a->L,
                                 a->P,
                                 0,
                                 a->elapsed,
                                 fmax(0, s->now - a->received_at),
                                 0};
        s->samples[s->pub_count++] = p;
        s->pub_F += p.L;
        s->pub_age = fmax(s->pub_age, p.age_seconds);
        continue;
      }
      double base_delay;
      if (!a->covered.generation ||
          peak_mpi_cost_model_delay(s->model, a->covered_ref, &base_delay) !=
              PEAK_MPI_COST_OK) {
        s->pub_complete = 0;
        continue;
      }
      a->covered.D = base_delay + a->covered_tail;
      PeakMpiPolicySample p = a->covered;
      p.age_seconds = fmax(0, s->now - a->covered_at);
      if (a->have_raw && a->elapsed > p.elapsed)
        p.age_seconds =
            fmax(p.age_seconds,
                 a->elapsed - p.elapsed + fmax(0, s->now - a->received_at));
      p.mixed_control = a->control != p.control_generation;
      s->samples[s->pub_count++] = p;
      s->pub_F = fmax(s->pub_F, p.D);
      s->pub_age = fmax(s->pub_age, p.age_seconds);

    } else if (s->pub_stage == 3) {
      if (s->pub_op == s->operation_count) {
        s->pub_stage = 4;
        continue;
      }
      Operation *o = &s->operations[s->active[s->pub_op++]];
      if (root(s, o->rank) != s->pub_root)
        continue;
      if (branch_live_at_cut(s, o)) {
        s->pub_oldest = fmax(s->pub_oldest, fmax(0, s->now - o->created_at));
        s->pub_global_oldest = fmax(s->pub_global_oldest, s->pub_oldest);
        if (s->pub_coarse)
          continue;
        double d;
        if (peak_mpi_cost_model_delay(s->model, o->entry, &d) !=
            PEAK_MPI_COST_OK) {
          s->pub_complete = 0;
          continue;
        }
        s->pub_F = fmax(s->pub_F, d);
        if (o->ended && peak_mpi_cost_model_delay(s->model, o->exit, &d) ==
                            PEAK_MPI_COST_OK)
          s->pub_F = fmax(s->pub_F, d);
        if (o->result_defined &&
            o->completion_index <= s->ranks[o->rank].covered_index) {
          if (peak_mpi_cost_model_delay(s->model, o->result, &d) !=
              PEAK_MPI_COST_OK)
            s->pub_complete = 0;
          else
            s->pub_F = fmax(s->pub_F, d);
        }
        if (o->begin.progress_scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL &&
            ((o->buffer != SIZE_MAX && (!s->buffers[o->buffer].closed ||
                                        s->buffers[o->buffer].closed_index >
                                            s->ranks[o->rank].covered_index)) ||
             ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->immediate &&
              (!o->completed ||
               o->completion_index > s->ranks[o->rank].covered_index))))
          s->pub_F = fmax(s->pub_F, s->ranks[o->rank].covered.D);
        if (o->collective_built &&
            peak_mpi_cost_model_delay(s->model, o->join, &d) ==
                PEAK_MPI_COST_OK)
          s->pub_F = fmax(s->pub_F, d);
        if (o->buffer != SIZE_MAX && s->buffers[o->buffer].closed &&
            s->buffers[o->buffer].closed_index <=
                s->ranks[o->rank].covered_index) {
          if (peak_mpi_cost_model_delay(s->model, s->buffers[o->buffer].seal,
                                        &d) != PEAK_MPI_COST_OK)
            s->pub_complete = 0;
          else
            s->pub_F = fmax(s->pub_F, d);
        }
        if ((!o->result_defined ||
             o->completion_index > s->ranks[o->rank].covered_index) &&
            o->dep_head != SIZE_MAX) {
          s->pub_dep = o->dep_head;
          s->pub_stage = 31;
        }
      }
    } else if (s->pub_stage == 31) {
      if (s->pub_dep == SIZE_MAX) {
        s->pub_stage = 3;
        continue;
      }
      PendingInput *input = &s->deps[s->pub_dep];
      double d;
      if (input->group_index > s->ranks[input->rank].covered_index) {
        s->pub_dep = input->next;
        continue;
      }
      if (peak_mpi_cost_model_delay(s->model, input->ref, &d) !=
          PEAK_MPI_COST_OK)
        s->pub_complete = 0;
      else
        s->pub_F = fmax(s->pub_F, d);
      s->pub_dep = input->next;
    } else {
      if (s->pub_complete && s->pub_count) {
        /* Policy atomically commits one coherent cohort tuple. Admit its O(n)
         * arithmetic only when this round has the matching work allowance. */
        size_t cut_work = 1 + 4 * s->pub_count;
        if (cut_work > budget)
          return 0;
        budget -= cut_work;
        s->last_work_used += cut_work;
        Rank *leader = &s->ranks[s->pub_root];
        PeakMpiPolicyCut c = {leader->key,
                              ++s->generation,
                              s->generation,
                              s->config.policy.policy_generation,
                              s->pub_F,
                              s->pub_age + fmax(0, s->now - s->pub_started),
                              s->pub_oldest,
                              PEAK_MPI_POLICY_COMPLETE_CUT,
                              1,
                              s->samples,
                              s->pub_count};
        s->cache_dirty = 1;
        PeakMpiPolicyResult result =
            peak_mpi_policy_submit_cut(leader->policy, &c, s->now);
        if (result == PEAK_MPI_POLICY_INVALID ||
            result == PEAK_MPI_POLICY_CAPACITY)
          coarse(s, s->pub_root, PEAK_MPI_POLICY_BAD_CUT, 0);
      }
      s->pub_root++;
      s->pub_stage = 1;
    }
  }
  return 0;
}
/* Only closed operation histories are retired. Live sends, request starts,
 * unsealed progress, matched-message handles and exact per-thread order
 * anchors remain independent of the rank's newer continuation. The model
 * additionally postpones collection across unsettled adjacent edges. */
static int operation_live(PeakMpiCausalService *s, Operation *o) {
  return !o->ended || o->group_index > s->ranks[o->rank].covered_index ||
         branch_live_at_cut(s, o);
}
static void keep_ref(PeakMpiCausalService *s, PeakMpiCostRef ref) {
  if (ref.generation && ref.slot < s->config.model.max_nodes)
    s->keep[ref.slot] = s->gc_generation;
}
static void checkpoint(PeakMpiCausalService *s, size_t budget) {
  s->last_work_used = 0;
  if (!s->gc_stage) {
    if (++s->gc_generation == 0)
      return;
    s->gc_stage = 1;
    s->gc_cursor = 0;
  }
  while (budget && s->gc_stage) {
    budget--;
    s->last_work_used++;
    if (s->gc_stage == 1) {
      if (s->gc_cursor == s->config.world_size) {
        s->gc_stage = 2;
        s->gc_cursor = 0;
        continue;
      }
      unsigned r = (unsigned)s->gc_cursor++;
      Rank *a = &s->ranks[r];
      if (a->have_continuation) {
        peak_mpi_cost_model_set_continuation(s->model, r, a->continuation);
        keep_ref(s, a->continuation);
      }
      if (a->covered.generation)
        keep_ref(s, a->covered_ref);
      if (a->cut_head != SIZE_MAX) {
        s->gc_cut_cursor = a->cut_head;
        s->gc_stage = 12;
      }
    } else if (s->gc_stage == 12) {
      if (s->gc_cut_cursor == SIZE_MAX) {
        s->gc_stage = 1;
        continue;
      }
      SettledCut *cut = &s->cuts[s->gc_cut_cursor];
      keep_ref(s, cut->ref);
      s->gc_cut_cursor = cut->next;
    } else if (s->gc_stage == 2) {
      if (s->gc_cursor == s->operation_count) {
        s->gc_stage = 3;
        s->gc_cursor = s->gc_kept = 0;
        continue;
      }
      Operation *o = &s->operations[s->active[s->gc_cursor]];
      if (o->dep_head != SIZE_MAX && o->dep_gc_stamp != s->gc_generation) {
        double d;
        o->dep_gc_stamp = s->gc_generation;
        s->gc_dep_cursor = o->dep_head;
        s->gc_dep_op = (size_t)(o - s->operations);
        s->gc_dep_keep = operation_live(s, o) &&
                         (!o->result_defined ||
                          peak_mpi_cost_model_delay(s->model, o->result, &d) !=
                              PEAK_MPI_COST_OK);
        s->gc_stage = 22;
        continue;
      }
      s->gc_cursor++;
      if (o->ambiguous && !o->ambiguity_closed)
        analysis_enqueue(s, o, 0);
      if (s->ranks[o->rank].anchor[o->begin.thread_uid - 1] == o->begin.op_uid)
        keep_ref(s, o->exit);
      if (operation_live(s, o)) {
        keep_ref(s, o->entry);
        keep_ref(s, o->exit);
        if (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING)
          keep_ref(s, o->result);
        if (o->collective_built)
          keep_ref(s, o->join);
        if (o->has_data)
          keep_ref(s, o->data);
        if (o->buffer != SIZE_MAX)
          keep_ref(s, s->buffers[o->buffer].seal);
        if (o->immediate)
          keep_ref(s, o->launch);
      }
    } else if (s->gc_stage == 22) {
      if (s->gc_dep_cursor == SIZE_MAX) {
        s->gc_stage = 2;
        continue;
      }
      size_t at = s->gc_dep_cursor;
      PendingInput *dep = &s->deps[at];
      s->gc_dep_cursor = dep->next;
      if (s->gc_dep_keep)
        keep_ref(s, dep->ref);
      else {
        s->operations[s->gc_dep_op].dep_head = dep->next;
        s->free_deps[s->free_dep_count++] = at;
      }
    } else if (s->gc_stage == 3) {
      if (s->gc_cursor == s->node_count) {
        s->node_count = s->gc_kept;
        s->gc_stage = 4;
        s->gc_cursor = s->gc_kept = 0;
        continue;
      }
      PeakMpiCostRef ref = s->nodes[s->gc_cursor++];
      if (s->keep[ref.slot] == s->gc_generation)
        s->nodes[s->gc_kept++] = ref;
      else
        peak_mpi_cost_model_retire(s->model, ref, true);
    } else if (s->gc_stage == 4) {
      if (s->gc_cursor == s->operation_count) {
        s->operation_count = s->gc_kept;
        s->gc_stage = 5;
        continue;
      }
      size_t slot = s->active[s->gc_cursor++];
      Operation *o = &s->operations[slot];
      if (operation_live(s, o) ||
          s->ranks[o->rank].anchor[o->begin.thread_uid - 1] ==
              o->begin.op_uid ||
          ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->retired))
        s->active[s->gc_kept++] = slot;
      else {
        analysis_remove(s, o);
        peak_mpi_causal_match_index_remove_all(s->matching, slot);
        index_remove(s, slot);
        memset(o, 0, sizeof *o);
        s->free_ops[s->free_count++] = slot;
      }
    } else {
      size_t used = 0;
      PeakMpiCostStatus result =
          peak_mpi_cost_model_collect(s->model, budget, &used);
      s->last_work_used += used;
      if (result != PEAK_MPI_COST_YIELDED)
        s->gc_stage = 0;
      return;
    }
  }
}
/* Verify a finite set of prefixes with one shared visibility/wake pass,
 * then tuple commit. The heartbeat may update latest RAW values while a
 * batch yields, but no parser/matcher/model/GC observes a partial batch. */
static int verify_rank_prefix(PeakMpiCausalService *s, unsigned rank,
                              size_t *left) {
  size_t budget = *left;
  size_t used_before = s->last_work_used;
  Rank *a = &s->ranks[rank];
  int result = 0;
  if (!a->have_prefix || a->coarse) {
    budget--;
    s->last_work_used++;
    goto done;
  }
  if (!a->verify_active) {
    if (a->last_verify_generation == a->raw_generation && a->has_verified &&
        a->verified_nonce >= a->echoed_nonce) {
      budget--;
      s->last_work_used++;
      result = 0;
      goto done;
    }
    /* One coherent frozen tuple. Copy only its cumulative nonzero slots;
     * later slot activation cannot alter this frozen membership. Subsequent
     * accepted-prefix, pending-record and visible-operation walks
     * resume at one transition per work unit. */
    if (budget < a->prefix_slot_count) {
      result = -1;
      goto done;
    }
    freeze_verify_tuple(a);
    budget -= a->verify_slot_count;
    s->last_work_used += a->verify_slot_count;
  }
  if (a->verify_stage == 1) {
    while (budget && a->accept_cursor < a->verify_slot_count) {
      size_t at = a->prefix_slots[a->accept_cursor];
      budget--;
      s->last_work_used++;
      if (a->sequence[at] < a->verify_prefix[at]) {
        result = 0;
        goto done;
      }
      a->accept_cursor++;
    }
    if (a->accept_cursor < a->verify_slot_count) {
      result = budget ? 0 : -1;
      goto done;
    }
    a->verify_stage = 2;
  }
  if (a->verify_stage == 2) {
    size_t before = budget;
    int clear = peak_mpi_causal_batches_prefix_clear_slots(
        s->batches, rank, a->verify_prefix, a->prefix_slots,
        a->verify_slot_count, &a->verify_cursor, &budget);
    s->last_work_used += before - budget;
    if (clear != 1) {
      result = clear < 0 ? -1 : 0;
      goto done;
    }
    a->verify_stage = 3;
    a->verified_nonce = a->verify_nonce;
  }

  result = a->verify_stage == 3;
done:
  *left -= s->last_work_used - used_before;
  return result;
}
static void verify_prefixes(PeakMpiCausalService *s, size_t budget) {
  s->last_work_used = 0;
  if (!s->verify_batch_stage) {
    s->verify_batch_stage = 1;
    s->verify_batch_rank = 0;
    s->verify_batch_start = s->verify_rank_cursor++ % s->config.world_size;
    s->visibility_rank = 1;
  }
  while (budget) {
    if (s->verify_batch_stage == 1) {
      if (s->verify_batch_rank < s->config.world_size) {
        unsigned rank = (s->verify_batch_start + s->verify_batch_rank) %
                        s->config.world_size;
        int verified = verify_rank_prefix(s, rank, &budget);
        if (verified < 0)
          return;
        s->verify_batch_rank++;
        continue;
      }
      s->verify_batch_stage = 2;
      s->verify_batch_op = 0;
      s->verify_batch_limit = s->operation_count;
    }
    if (s->verify_batch_stage == 2) {
      while (budget && s->verify_batch_op < s->verify_batch_limit) {
        Operation *o = &s->operations[s->active[s->verify_batch_op++]];
        Rank *owner = &s->ranks[o->rank];
        budget--;
        s->last_work_used++;
        if (owner->verify_stage == 3 && !owner->coarse && !o->visible &&
            o->begin.sequence <=
                owner->verify_prefix[o->begin.thread_uid - 1]) {
          o->visible = 1;
          o->visible_nonce = owner->verify_nonce;
        }
        analysis_wake(s, o);
      }
      if (s->verify_batch_op < s->verify_batch_limit)
        return;
      s->verify_batch_stage = 3;
      s->verify_batch_rank = 0;
    }
    if (s->verify_batch_stage == 3) {
      while (budget && s->verify_batch_rank < s->config.world_size) {
        unsigned rank = (s->verify_batch_start + s->verify_batch_rank) %
                        s->config.world_size;
        Rank *a = &s->ranks[rank];
        if (a->verify_stage != 3 || a->coarse) {
          budget--;
          s->last_work_used++;
          if (a->coarse)
            a->verify_active = a->verify_stage = 0;
          s->verify_batch_rank++;
          continue;
        }
        size_t cost =
            a->verify_idle && a->verify_slot_count ? a->verify_slot_count : 1;
        if (budget < cost)
          return;
        budget -= cost;
        s->last_work_used += cost;
        uint64_t *old = a->verified_prefix;
        a->verified_prefix = a->verify_prefix;
        a->verify_prefix = old;
        a->verified_nonce = a->verify_nonce;
        a->last_verify_generation = a->verify_generation;
        a->has_verified = 1;
        a->verify_active = a->verify_stage = 0;

        if (a->verify_idle && peak_mpi_causal_batches_last_group(
                                  s->batches, rank) == a->verify_group) {
          PeakMpiObserverRecord idle = {0};
          idle.P_cut = (uint64_t)(a->verify_P * 1e9);
          idle.L_cut =
              idle.P_cut + (uint64_t)((a->verify_L - a->verify_P) * 1e9);
          if (sec(idle.L_cut) >= a->last_L && sec(idle.P_cut) >= a->last_P) {
            if (a->have_continuation) {
              s->cycle_idle_progress = 1;
              a->parsed_index++;
              PeakMpiPolicySample sample = {
                  {rank, s->config.job_context},
                  0,
                  prefix_sum_slots(a->verified_prefix, a->prefix_slots,
                                   a->verify_slot_count),
                  a->verify_control,
                  sec(idle.L_cut),
                  sec(idle.P_cut),
                  0,
                  a->verify_elapsed,
                  0,
                  0};
              queue_cut(s, rank, a->continuation, sample, a->verify_received);
            }
          }
        }
        s->verify_batch_rank++;
      }
      if (s->verify_batch_rank < s->config.world_size)
        return;
      s->verify_batch_stage = 0;
      s->visibility_rank = 0;
      return;
    }
  }
}

static void parse_batches(PeakMpiCausalService *s, size_t budget) {
  s->last_work_used = 0;
  while (budget) {
    if (!s->plan_active) {
      size_t spent = 0;
      int ready = peak_mpi_causal_batches_prepare_next_work(s->batches, budget,
                                                            &s->plan, &spent);
      budget -= spent;
      s->last_work_used += spent;
      if (ready < 0) {
        coarse(s, s->plan.rank, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
        return;
      }
      if (!ready)
        return;
      s->plan_active = 1;
      s->plan_index = 0;
    }
    if (s->plan_index == s->plan.record_count) {
      budget--;
      s->last_work_used++;
      s->building_concurrent = 0;
      s->plan_active = 0;
      peak_mpi_causal_batches_commit(s->batches);
      if (s->parse_finish_only)
        return;
      continue;
    }
    const PeakMpiObserverRecord *saved =
        peak_mpi_causal_batches_record_at(s->batches, s->plan_index);
    if (!saved) {
      coarse(s, s->plan.rank, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      return;
    }
    size_t cost = 1;
    if (cost > budget)
      return;
    budget -= cost;
    s->last_work_used += cost;
    PeakMpiObserverRecord rec = *saved;
    unsigned rank = s->plan.rank;
    Rank *a = &s->ranks[rank];
    if (s->plan_index == 0) {
      s->building_concurrent = 0;
      parse_record(s, rank, &rec);
      s->group_start = a->continuation;
      if (s->plan.concurrent) {
        if (s->plan.end->L_cut < s->plan.begin->L_cut ||
            s->plan.end->P_cut < s->plan.begin->P_cut ||
            s->plan.end->P_cut - s->plan.begin->P_cut >
                s->plan.end->L_cut - s->plan.begin->L_cut) {
          coarse(s, rank, PEAK_MPI_POLICY_BAD_CUT, 0);
          return;
        }
        if (!node(s, rank, sec(s->plan.end->L_cut - s->plan.begin->L_cut),
                  sec(s->plan.end->P_cut - s->plan.begin->P_cut), NULL,
                  &s->group_cost))
          return;
        s->building_concurrent = 1;
      }
    } else if (rec.phase == PEAK_MPI_CAUSAL_GROUP_END) {
      if (s->building_concurrent) {
        good(s, rank, peak_mpi_cost_model_seal(s->model, s->group_cost));
        a->continuation = s->group_cost;
        a->have_continuation = 1;
        a->last_L = sec(rec.L_cut);
        a->last_P = sec(rec.P_cut);
        a->last_L_cut = rec.L_cut;
        a->last_P_cut = rec.P_cut;
        s->building_concurrent = 0;
      }
      parse_record(s, rank, &rec);
    } else {
      if (s->building_concurrent)
        rec.flags &= ~(PEAK_MPI_OBSERVER_SLACK_S1 |
                       PEAK_MPI_OBSERVER_SLACK_NO_OPEN_PROGRESS);
      parse_record(s, rank, &rec);
      if (s->building_concurrent && rec.phase == PEAK_MPI_CAUSAL_CALL_BEGIN) {
        Operation *o = find_op(s, rank, rec.op_uid);
        if (o) {
          edge(s, rank, o->exit, s->group_cost, PEAK_MPI_COST_PROGRESS, 0);
          edge(s, rank, s->group_cost, o->exit, PEAK_MPI_COST_PROGRESS, 0);
        }
      }
    }
    s->plan_index++;
    s->parsed_records++;
  }
}
static size_t prepare_feedback_cache(PeakMpiCausalService *s,
                                     size_t allowance) {
  size_t used = 0;
  while (s->cache_root < s->config.world_size && used < allowance) {
    unsigned rank = s->cache_root;
    if (root(s, rank) != rank) {
      s->cache_root++;
      used++;
      continue;
    }
    PeakMpiPolicy *policy = s->ranks[rank].policy;
    size_t batch = peak_mpi_policy_feedback_batch_work(policy);
    size_t members = s->ranks[rank].cohort_size;
    if (members > (SIZE_MAX - batch) / 2 ||
        batch + 2 * members > allowance - used)
      break;
    size_t count = 0, spent = 0;
    PeakMpiPolicyResult result = peak_mpi_policy_feedback_batch(
        policy, s->now, s->feedback_staging, s->config.world_size,
        allowance - used - 2 * members, &count, &spent);
    if (result == PEAK_MPI_POLICY_BUDGET)
      break;
    used += spent;
    if (result != PEAK_MPI_POLICY_OK || count != members) {
      coarse(s, rank, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      s->cache_root++;
      continue;
    }
    int valid = 1;
    for (size_t i = 0; i < count; i++) {
      PeakMpiPolicyFeedback *f = &s->feedback_staging[i];
      used++;
      if (f->member.process >= s->config.world_size ||
          f->member.incarnation != s->config.job_context ||
          f->cohort.epoch != s->ranks[rank].key.epoch ||
          f->cohort.cohort_id != s->ranks[rank].key.cohort_id ||
          f->cohort.job_incarnation != s->config.job_context)
        valid = 0;
    }
    if (!valid) {
      coarse(s, rank, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      s->cache_root++;
      continue;
    }
    /* One admitted single-owner commit. The transport cannot observe a
     * partially copied cohort: no networking or yield occurs in this loop. */
    for (size_t i = 0; i < count; i++) {
      PeakMpiPolicyFeedback *f = &s->feedback_staging[i];
      unsigned member = (unsigned)f->member.process;
      s->feedback_cache[member] = *f;
      s->feedback_captured[member] = s->now;
      s->feedback_valid[member] = 1;
      s->ranks[member].feedback_epoch =
          s->recovery.state == PEAK_MPI_RECOVERY_ACK && !s->recovery_reset
              ? s->recovery.epoch
              : 0;
      used++;
    }
    s->cache_root++;
  }
  if (s->cache_root == s->config.world_size) {
    s->cache_root = 0;
    s->cache_dirty = 0;
  }
  return used;
}
static uint64_t recovery_local_comm(PeakMpiCausalService *s, unsigned rank) {
  return s->ranks[rank].recovery_comm;
}
static int recovery_eligible(uint64_t operation) {
  return operation == PEAK_MPI_CAUSAL_BARRIER ||
         operation == PEAK_MPI_CAUSAL_ALLREDUCE ||
         operation == PEAK_MPI_CAUSAL_ALLGATHER ||
         operation == PEAK_MPI_CAUSAL_ALLTOALL ||
         operation == PEAK_MPI_CAUSAL_REDUCE_SCATTER;
}
static int recovery_work(PeakMpiCausalService *s, size_t budget, size_t *used) {
  *used = 0;
  if (s->hard_fault)
    return s->recovery_reset != 0 || s->recovery_incomplete;
  if (s->recovery_restart) {
    s->recovery_restart = 0;
    s->recovery_incomplete = 1;
    s->recovery_reset = s->recovery_prepare = s->recovery_rank = 0;
    s->recovery.state = PEAK_MPI_RECOVERY_NONE;
    s->recovery_cursor = 0;
  }
  if (!s->recovery_needed)
    return 0;
  /* Query preparation visits each rank/immutable communicator once. The cached
   * native intracomm size plus validated world member IDs establishes global
   * participation; no repeated quadratic membership walk is needed. */
  while ((!s->recovery.state || s->recovery.state == PEAK_MPI_RECOVERY_ACK ||
          s->recovery_prepare) &&
         *used < budget) {
    ++*used;
    if (!s->recovery_prepare) {
      if (s->recovery_rank == s->config.world_size) {
        s->recovery_rank = 0;
        return 1;
      }
      unsigned r = s->recovery_rank++;
      Rank *a = &s->ranks[r];
      if (a->recovery.loss <= a->acknowledged_loss)
        continue;
      if (!a->recovery.comm || !a->recovery.ordinal ||
          !recovery_eligible(a->recovery.operation)) {
        s->recovery_rank = 0;
        return 1;
      }
      s->recovery_candidate = a->recovery;
      s->recovery_source = r;
      s->recovery_prepare = 1;
      s->recovery_cursor = 0;
    } else if (s->recovery_prepare == 1) {
      if (s->recovery_cursor == s->comm_count) {
        s->recovery_prepare = s->recovery_rank = 0;
        return 1;
      }
      size_t at = s->recovery_cursor++;
      Comm *c = &s->comms[at];
      if (s->comm_ranks[at] == s->recovery_source &&
          c->uid == s->recovery_candidate.comm && c->canonical &&
          c->size == (int)s->config.world_size) {
        s->recovery_context = c->canonical;
        s->recovery_prepare = 2;
        s->recovery_rank = 0;
      }
    } else if (s->recovery_prepare == 2) {
      if (s->recovery_rank < s->config.world_size)
        s->ranks[s->recovery_rank++].recovery_comm = 0;
      else {
        s->recovery_prepare = 3;
        s->recovery_cursor = 0;
      }
    } else if (s->recovery_prepare == 3) {
      if (s->recovery_cursor < s->comm_count) {
        size_t at = s->recovery_cursor++;
        Comm *c = &s->comms[at];
        if (c->canonical == s->recovery_context &&
            c->size == (int)s->config.world_size)
          s->ranks[s->comm_ranks[at]].recovery_comm = c->uid;
      } else {
        s->recovery_prepare = 4;
        s->recovery_rank = 0;
      }
    } else if (s->recovery_rank < s->config.world_size) {
      if (!s->ranks[s->recovery_rank++].recovery_comm) {
        s->recovery_prepare = 3;
        s->recovery_cursor = 0;
        return 1;
      }
    } else {
      s->recovery.epoch = s->recovery.epoch ? s->recovery.epoch + 1 : 2;
      s->recovery.state = PEAK_MPI_RECOVERY_QUERY;
      s->recovery.ordinal = s->recovery_candidate.ordinal;
      s->recovery.operation = s->recovery_candidate.operation;
      s->recovery_prepare = s->recovery_rank = 0;
    }
  }
  if (s->recovery.state != PEAK_MPI_RECOVERY_QUERY)
    return 1;
  if (!s->recovery_reset) {
    while (s->recovery_rank < s->config.world_size && *used < budget) {
      ++*used;
      Rank *a = &s->ranks[s->recovery_rank];
      if (a->recovery.epoch != s->recovery.epoch ||
          a->recovery.state != PEAK_MPI_RECOVERY_READY ||
          a->recovery.ordinal < s->recovery.ordinal ||
          a->recovery.comm != a->recovery_comm) {
        s->recovery_rank = 0;
        return 1;
      }
      if (!isfinite(a->recovery.L) || !isfinite(a->recovery.P) ||
          !isfinite(a->recovery.elapsed) || a->recovery.P < 0 ||
          a->recovery.P > a->recovery.L || a->recovery.elapsed < 0 ||
          !a->recovery.uid_floor) {
        coarse(s, s->recovery_rank, PEAK_MPI_POLICY_BAD_CUT, 0);
        return 1;
      }
      ++s->recovery_rank;
    }
    if (s->recovery_rank != s->config.world_size || budget - *used < 1)
      return 1;
    ++*used;
    s->recovery_reset = 1;
    s->recovery_rank = 0;
    s->recovery_cursor = 0;
    s->root_plan_stage = ROOT_PLAN_IDLE;
    s->root_cut_active = 0;
    s->model_running = s->plan_active = s->visibility_rank = 0;
    s->verify_batch_stage = s->verify_batch_rank = 0;
    s->verify_batch_op = s->verify_batch_limit = 0;
    s->pub_stage = s->gc_stage = s->task_stage = 0;
    s->graph_pending = 0;
    s->analysis_head = s->analysis_tail = SIZE_MAX;
    peak_mpi_cost_model_reset_begin(s->model);
    peak_mpi_causal_batches_reset_begin(s->batches);
  }
  while (s->recovery_reset && *used < budget) {
    size_t left = budget - *used, spent = 0;
    if (s->recovery_reset == 1) {
      int done = peak_mpi_cost_model_reset_work(s->model, left, &spent);
      *used += spent;
      if (!done)
        break;
      s->recovery_reset = 2;
    } else if (s->recovery_reset == 2) {
      int done = peak_mpi_causal_batches_reset_work(s->batches, left, &spent);
      *used += spent;
      if (!done)
        break;
      s->recovery_reset = 3;
    } else if (s->recovery_reset == 3) {
      ++*used;
      if (s->operation_count) {
        size_t slot = s->active[s->operation_count - 1];
        Operation *o = &s->operations[slot];
        if (o->dep_head != SIZE_MAX) {
          size_t d = o->dep_head;
          o->dep_head = s->deps[d].next;
          s->free_deps[s->free_dep_count++] = d;
          continue;
        }
        peak_mpi_causal_match_index_remove_all(s->matching, slot);
        index_remove(s, slot);
        o->used = 0;
        s->free_ops[s->free_count++] = slot;
        --s->operation_count;
      } else {
        s->recovery_reset = 4;
        s->recovery_cursor = 0;
      }
    } else if (s->recovery_reset == 4) {
      ++*used;
      if (s->recovery_cursor < s->buffer_index_capacity)
        s->buffer_index[s->recovery_cursor++] = 0;
      else {
        s->buffer_count = s->node_count = 0;
        s->recovery_reset = 5;
      }
    } else if (s->recovery_reset == 5) {
      ++*used;
      if (s->recovery_rank < s->config.world_size) {
        Rank *a = &s->ranks[s->recovery_rank];
        if (a->cut_head != SIZE_MAX) {
          size_t at = a->cut_head;
          a->cut_head = s->cuts[at].next;
          s->free_cuts[s->free_cut_count++] = at;
        } else
          ++s->recovery_rank;
      } else {
        s->recovery_H = 0;
        s->recovery_reset = 6;
        s->recovery_rank = 0;
      }
    } else if (s->recovery_reset == 6) {
      ++*used;
      if (s->recovery_rank < s->config.world_size)
        s->recovery_H += s->ranks[s->recovery_rank++].recovery.L;
      else {
        s->recovery_reset = 7;
        s->recovery_rank = 0;
      }
    } else if (s->recovery_reset == 7) {
      if (s->recovery_rank == s->config.world_size) {
        ++*used;
        s->approximate = 1;
        s->recovery_needed = s->recovery_incomplete = 0;
        s->recovery.state = PEAK_MPI_RECOVERY_ACK;
        s->recovery_reset = s->recovery_rank = 0;
        s->engine_phase = 0;
        s->parse_window_open = s->parse_finish_only = 0;
        s->cache_dirty = 1;
        continue;
      }
      ++*used;
      Rank *a = &s->ranks[s->recovery_rank];
      a->last_L_cut = (uint64_t)(a->recovery.L * 1e9);
      a->last_P_cut = (uint64_t)(a->recovery.P * 1e9);
      a->last_L = sec(a->last_L_cut);
      a->last_P = sec(a->last_P_cut);
      a->uid_floor = a->recovery.uid_floor;
      a->acknowledged_loss = a->recovery.loss;
      a->coarse = 0;
      a->reason = PEAK_MPI_POLICY_NO_REASON;
      a->cut_head = a->cut_tail = SIZE_MAX;
      a->covered.generation = 0;
      a->covered_index = a->parsed_index = 0;
      a->processed_sum = 0;
      a->verify_active = a->verify_stage = a->has_verified = 0;
      s->recovery_cursor = 0;
      s->recovery_reset = 8;
    } else if (s->recovery_reset == 8) {
      ++*used;
      Rank *a = &s->ranks[s->recovery_rank];
      if (s->recovery_cursor < s->config.slots) {
        size_t j = s->recovery_cursor++;
        a->sequence[j] = a->processed[j] = a->prefix[j];
        a->processed_sum += a->prefix[j];
        a->anchor[j] = 0;
      } else
        s->recovery_reset = 9;
    } else if (s->recovery_reset == 9) {
      ++*used;
      Rank *a = &s->ranks[s->recovery_rank];
      if (!reserve(s, s->recovery_rank, &a->continuation))
        return 1;
      a->have_continuation = 1;
      s->recovery_reset = 10;
    } else if (s->recovery_reset == 10) {
      ++*used;
      unsigned r = s->recovery_rank;
      Rank *a = &s->ranks[r];
      PeakMpiCostNodeSpec spec = {PEAK_MPI_COST_EVENT,
                                  r,
                                  PEAK_MPI_COST_NO_OWNER,
                                  s->recovery_H,
                                  0,
                                  0,
                                  0,
                                  0};
      if (!good(s, r,
                peak_mpi_cost_model_define_node(s->model, a->continuation,
                                                &spec)))
        return 1;
      s->recovery_reset = 11;
    } else if (s->recovery_reset == 11) {
      ++*used;
      unsigned r = s->recovery_rank;
      Rank *a = &s->ranks[r];
      if (!good(s, r, peak_mpi_cost_model_seal(s->model, a->continuation)))
        return 1;
      peak_mpi_cost_model_reset_base(s->model, r, a->last_L, a->last_P);
      peak_mpi_policy_recover_begin(a->policy, s->now, s->recovery_g);
      s->recovery_reset = 12;
    } else {
      Rank *a = &s->ranks[s->recovery_rank];
      int done = peak_mpi_policy_recover_work(a->policy, left, &spent);
      *used += spent;
      if (!done)
        break;
      if (!spent)
        ++*used;
      ++s->recovery_rank;
      s->recovery_reset = 7;
    }
  }

  return s->recovery.state != PEAK_MPI_RECOVERY_ACK;
}
static uint64_t sink_nonce(void *v, unsigned r) {
  PeakMpiCausalService *s = v;
  return r < s->config.world_size ? s->ranks[r].requested_nonce : 0;
}
static int sink_object(void *s, unsigned r, uint64_t inc,
                       const PeakMpiObserverObject *o) {
  return peak_mpi_causal_service_ingest_object(s, r, inc, o);
}
static int sink_record(void *s, unsigned r, const PeakMpiObserverRecord *o) {
  return peak_mpi_causal_service_ingest_record(s, r, o);
}
static int sink_raw(void *s, unsigned r, const PeakMpiCausalRaw *raw,
                    double now) {
  return peak_mpi_causal_service_ingest_raw(s, r, raw, now);
}
static int sink_feedback(void *v, unsigned rank, double now,
                         PeakMpiPolicyFeedback *out) {
  PeakMpiCausalService *s = v;
  if (rank >= s->config.world_size || !s->feedback_valid ||
      !s->feedback_valid[rank])
    return 0;
  *out = s->feedback_cache[rank];
  if (s->recovery.state) {
    out->recovery = s->recovery;
    if (out->recovery.state == PEAK_MPI_RECOVERY_ACK &&
        s->ranks[rank].feedback_epoch != s->recovery.epoch)
      out->recovery.state = PEAK_MPI_RECOVERY_QUERY;
    out->recovery.comm = recovery_local_comm(s, rank);
    out->generation += s->recovery.epoch * 4 + s->recovery.state;
  }
  out->recovery.loss = 0;
  for (unsigned r = 0; r < s->config.world_size; r++)
    out->recovery.loss += s->ranks[r].recovery.loss;
  out->recovery.fault_mask =
      s->hard_fault ? UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_METADATA : 0;
  if (s->approximate) {
    out->approximate = 1;
    if (out->mode == PEAK_MPI_POLICY_COMPLETE)
      out->mode = PEAK_MPI_POLICY_APPROX;
    out->coverage_complete = out->risk_known = out->tails_complete = 0;
  }
  double age = fmax(0, now - s->feedback_captured[rank]);
  out->age_seconds += age;
  if (out->oldest_unresolved_age > 0)
    out->oldest_unresolved_age += age;
  return 1;
}
static void sink_fault(void *v, unsigned r, PeakMpiPolicyReason reason) {
  PeakMpiCausalService *s = v;
  s->hard_fault = 1;
  if (!s->ranks) {
    s->local_fault = reason;
    s->stats.unavailable = 1;
  } else if (r < s->config.world_size)
    coarse(s, r, reason, 0);
}
PeakMpiCausalService *
peak_mpi_causal_service_create(const PeakMpiCausalServiceConfig *c) {
  if (!c || !c->world_size || c->rank >= c->world_size ||
      c->owner_rank >= c->world_size || !c->slots || !c->max_operations ||
      !c->max_context_parts)
    return NULL;
  if (c->world_size > SIZE_MAX / c->world_size ||
      c->slots > SIZE_MAX / c->world_size ||
      c->max_context_parts > SIZE_MAX / 16 || c->max_operations > SIZE_MAX / 4)
    return NULL;
  PeakMpiCausalService *s = calloc(1, sizeof *s);
  if (!s)
    return NULL;
  s->config = *c;
  if (c->rank != c->owner_rank) {
    PeakMpiCausalTransportSink sink = {.fault = sink_fault, .context = s};
    s->transport = peak_mpi_causal_transport_create(c, &sink);
    if (!s->transport) {
      free(s);
      return NULL;
    }
    return s;
  }
  s->epoch = 1;
  s->analysis_head = s->analysis_tail = SIZE_MAX;
  s->ranks = calloc(c->world_size, sizeof *s->ranks);
  s->prefix_slots =
      calloc((size_t)c->world_size * c->slots, sizeof *s->prefix_slots);
  s->prefix_seen =
      calloc((size_t)c->world_size * c->slots, sizeof *s->prefix_seen);
  s->sequences = calloc((size_t)c->world_size * c->slots, sizeof *s->sequences);
  s->anchors = calloc((size_t)c->world_size * c->slots, sizeof *s->anchors);
  s->prefixes = calloc((size_t)c->world_size * c->slots, sizeof *s->prefixes);
  s->verify_prefixes =
      calloc((size_t)c->world_size * c->slots, sizeof *s->verify_prefixes);
  s->processed = calloc((size_t)c->world_size * c->slots, sizeof *s->processed);
  s->verified_prefixes =
      calloc((size_t)c->world_size * c->slots, sizeof *s->verified_prefixes);
  s->nodes = calloc(c->model.max_nodes, sizeof *s->nodes);
  s->cuts = calloc(c->model.max_nodes, sizeof *s->cuts);
  s->free_cuts = calloc(c->model.max_nodes, sizeof *s->free_cuts);
  s->free_cut_count = c->model.max_nodes;
  for (size_t i = 0; i < c->model.max_nodes; i++)
    s->free_cuts[i] = c->model.max_nodes - i - 1;
  s->index_capacity = 2;
  while (s->index_capacity < c->max_operations * 2)
    s->index_capacity *= 2;
  s->index = calloc(s->index_capacity, sizeof *s->index);
  s->operations = calloc(c->max_operations, sizeof *s->operations);
  s->active = calloc(c->max_operations, sizeof *s->active);
  s->candidates = calloc(c->max_operations, sizeof *s->candidates);
  s->free_ops = calloc(c->max_operations, sizeof *s->free_ops);
  s->free_count = c->max_operations;
  for (size_t i = 0; i < c->max_operations; i++)
    s->free_ops[i] = c->max_operations - i - 1;
  s->keep = calloc(c->model.max_nodes, sizeof *s->keep);
  s->deps = calloc(c->model.max_edges, sizeof *s->deps);
  s->free_deps = calloc(c->model.max_edges, sizeof *s->free_deps);
  s->free_dep_count = c->model.max_edges;
  for (size_t i = 0; i < c->model.max_edges; i++)
    s->free_deps[i] = c->model.max_edges - i - 1;
  s->buffer_index_capacity = 2;
  while (s->buffer_index_capacity < c->max_context_parts * 2)
    s->buffer_index_capacity *= 2;
  s->buffer_index = calloc(s->buffer_index_capacity, sizeof *s->buffer_index);
  s->buffers = calloc(c->max_context_parts, sizeof *s->buffers);
  s->comms = calloc(c->max_context_parts, sizeof *s->comms);
  s->comm_ranks = calloc(c->max_context_parts, sizeof *s->comm_ranks);
  s->members = calloc(c->max_context_parts * 16, sizeof *s->members);
  s->contexts = calloc(c->max_context_parts, sizeof *s->contexts);
  s->samples = calloc(c->world_size, sizeof *s->samples);
  s->feedback_staging = calloc(c->world_size, sizeof *s->feedback_staging);
  s->feedback_cache = calloc(c->world_size, sizeof *s->feedback_cache);
  s->feedback_captured = calloc(c->world_size, sizeof *s->feedback_captured);
  s->feedback_valid = calloc(c->world_size, sizeof *s->feedback_valid);
  s->cache_dirty = 1;
  s->peers = calloc((size_t)c->world_size * c->world_size, 1);
  s->model = peak_mpi_cost_model_create(&c->model);
  s->matching = peak_mpi_causal_match_index_create(c->max_operations);
  s->batches = peak_mpi_causal_batches_create_with_slots(
      c->world_size, c->slots, c->max_pending_records);
  if (!s->ranks || !s->prefix_slots || !s->prefix_seen || !s->sequences ||
      !s->anchors || !s->prefixes ||
      !s->verify_prefixes || !s->verified_prefixes || !s->processed ||
      !s->nodes || !s->cuts || !s->free_cuts || !s->index || !s->operations ||
      !s->active || !s->candidates || !s->free_ops || !s->keep || !s->deps ||
      !s->free_deps || !s->buffers || !s->buffer_index || !s->comms ||
      !s->comm_ranks || !s->members || !s->contexts || !s->samples ||
      !s->feedback_staging || !s->feedback_cache || !s->feedback_captured ||
      !s->feedback_valid || !s->peers || !s->model || !s->batches ||
      !s->matching)
    goto fail;
  for (unsigned r = 0; r < c->world_size; r++) {
    Rank *a = &s->ranks[r];
    a->cut_head = a->cut_tail = SIZE_MAX;
    a->parent = r;
    a->cohort_size = 1;
    a->key = (PeakMpiPolicyCohortKey){c->job_context, r, 1};
    a->prefix_slots = s->prefix_slots + (size_t)r * c->slots;
    a->prefix_seen = s->prefix_seen + (size_t)r * c->slots;
    a->sequence = s->sequences + (size_t)r * c->slots;
    a->anchor = s->anchors + (size_t)r * c->slots;
    a->prefix = s->prefixes + (size_t)r * c->slots;
    a->verify_prefix = s->verify_prefixes + (size_t)r * c->slots;
    a->processed = s->processed + (size_t)r * c->slots;
    a->verified_prefix = s->verified_prefixes + (size_t)r * c->slots;

    PeakMpiPolicyConfig pc = c->policy;
    pc.member_capacity = c->world_size;
    PeakMpiPolicyMemberId id = {r, c->job_context};
    a->policy = peak_mpi_policy_create(&pc, a->key, &id, 1, 0, 1, 0);
    if (!a->policy)
      goto fail;
  }
  PeakMpiCausalTransportSink sink = {.object = sink_object,
                                     .record = sink_record,
                                     .raw = sink_raw,
                                     .feedback = sink_feedback,
                                     .fault = sink_fault,
                                     .context = s,
                                     .request_nonce = sink_nonce};
  s->transport = peak_mpi_causal_transport_create(c, &sink);
  if (!s->transport)
    goto fail;
  return s;
fail:
  peak_mpi_causal_service_destroy(s);
  return NULL;
}
void peak_mpi_causal_service_destroy(PeakMpiCausalService *s) {
  if (!s)
    return;
  peak_mpi_causal_transport_destroy(s->transport);
  peak_mpi_cost_model_destroy(s->model);
  peak_mpi_causal_batches_destroy(s->batches);
  peak_mpi_causal_match_index_destroy(s->matching);
  if (s->ranks)
    for (unsigned i = 0; i < s->config.world_size; i++)
      peak_mpi_policy_free(s->ranks[i].policy);
  free(s->ranks);
  free(s->prefix_slots);
  free(s->prefix_seen);
  free(s->sequences);
  free(s->anchors);
  free(s->prefixes);
  free(s->verify_prefixes);
  free(s->processed);
  free(s->verified_prefixes);

  free(s->nodes);
  free(s->cuts);
  free(s->free_cuts);
  free(s->operations);
  free(s->index);
  free(s->active);
  free(s->candidates);
  free(s->free_ops);
  free(s->keep);
  free(s->deps);
  free(s->free_deps);
  free(s->comms);
  free(s->buffers);
  free(s->buffer_index);
  free(s->comm_ranks);
  free(s->members);
  free(s->contexts);
  free(s->samples);
  free(s->feedback_staging);
  free(s->feedback_cache);
  free(s->feedback_captured);
  free(s->feedback_valid);
  free(s->peers);
  free(s);
}
void peak_mpi_causal_service_tick(PeakMpiCausalService *s,
                                  PeakMpiObserver *capture,
                                  const PeakMpiCausalRaw *raw, double now,
                                  size_t bytes, size_t work) {
  if (!s || !raw)
    return;
  struct timespec begin, end;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &begin);
  s->now = now;
  PeakMpiCausalRaw local_raw = *raw;
  if (s->local_fault) {
    local_raw.coarse_reason = PEAK_MPI_OBSERVER_REASON_METADATA;
  }
  /* A cut committed on the previous heartbeat must be visible to this
   * transport turn. Charge its atomic cache preparation to the same budget. */
  size_t pre_cache_work = 0;
  if (s->config.rank == s->config.owner_rank && s->cache_dirty)
    pre_cache_work = prepare_feedback_cache(s, work);
  peak_mpi_causal_transport_tick(s->transport, capture, &local_raw, now, bytes);
  if (s->config.rank == s->config.owner_rank) {
    size_t recovery_used = 0;
    int recovering = recovery_work(s, work - pre_cache_work, &recovery_used);
    recovery_used += pre_cache_work;
    if (recovering) {
      recovery_used += prepare_feedback_cache(s, work - recovery_used);
      s->last_tick_work = recovery_used;
      goto accounting;
    }
    size_t left = work - recovery_used, used = prepare_feedback_cache(s, left);
    int parsed_this_tick = 0;
    left -= used;
    enum {
      ENGINE_PARSE,
      ENGINE_VERIFY,
      ENGINE_MATCH,
      ENGINE_MODEL,
      ENGINE_ADVANCE,
      ENGINE_PUBLISH,
      ENGINE_GC
    };
    while (left) {
      if (s->model_running || s->root_plan_stage)
        s->engine_phase = ENGINE_MODEL;
      else if (s->pub_stage)
        s->engine_phase = ENGINE_PUBLISH;
      else if (s->gc_stage)
        s->engine_phase = ENGINE_GC;
      else if (s->visibility_rank)
        s->engine_phase = ENGINE_VERIFY;
      else if (s->plan_active)
        s->engine_phase = ENGINE_PARSE;
      size_t allowance = left;
      if (s->engine_phase == ENGINE_PARSE) {
        parsed_this_tick = 1;
        if (!s->parse_window_open) {
          s->parse_window_left = work / 3 ? work / 3 : work;
          s->parse_window_open = 1;
        }
        s->parse_finish_only = s->parse_window_left == 0;
        allowance =
            s->parse_finish_only
                ? left
                : (left < s->parse_window_left ? left : s->parse_window_left);
        parse_batches(s, allowance);
        used = s->last_work_used;
        left -= used;
        s->parse_window_left =
            used < s->parse_window_left ? s->parse_window_left - used : 0;
        if (s->plan_active) {
          if (!used) {
            s->parse_window_left = 0;
            if (left < s->config.slots + 1)
              break;
          }
          continue;
        }
        s->parse_window_open = 0;
        s->parse_finish_only = 0;
        s->engine_phase = ENGINE_VERIFY;
      } else if (s->engine_phase == ENGINE_VERIFY) {
        if (allowance > work / 8 && work / 8 >= s->config.slots)
          allowance = work / 8;
        verify_prefixes(s, allowance);
        used = s->last_work_used;
        left -= used;
        if (s->visibility_rank) {
          if (!used)
            break;
          continue;
        }
        s->engine_phase = ENGINE_MATCH;
      } else if (s->engine_phase == ENGINE_MATCH) {
        if (allowance > work / 3 && work / 3)
          allowance = work / 3;
        matcher_work(s, allowance);
        used = s->last_work_used;
        left -= used;
        if (s->task_stage || s->analysis_head != SIZE_MAX) {
          if (!used)
            break;
          continue;
        }
        s->engine_phase = ENGINE_MODEL;
      } else if (s->engine_phase == ENGINE_MODEL) {
        if (!s->graph_pending && !s->model_running && !s->root_plan_stage) {
          left--;
          s->engine_phase = ENGINE_ADVANCE;
          continue;
        }
        if (!s->model_running && !s->root_plan_stage) {
          s->root_plan_stage = ROOT_PLAN_RANKS;
          s->root_plan_rank = 0;
        }
        if (s->root_plan_stage && s->root_plan_stage != ROOT_PLAN_READY) {
          int ready = prepare_root_cut(s, left);
          left -= s->last_work_used;
          if (!ready)
            break;
          if (!left)
            break;
        }
        s->root_plan_stage = ROOT_PLAN_IDLE;
        size_t spent = 0;
        PeakMpiCostStatus status =
            peak_mpi_cost_model_run(s->model, left, &spent);
        left -= spent;
        s->model_running = status == PEAK_MPI_COST_YIELDED;
        if (s->model_running)
          break;
        s->graph_pending = 0;
        if (status == PEAK_MPI_COST_COARSE)
          for (unsigned rank = 0; rank < s->config.world_size; rank++)
            if (!s->ranks[rank].coarse)
              coarse(s, rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
        s->engine_phase = s->task_stage ? ENGINE_MATCH : ENGINE_ADVANCE;
      } else if (s->engine_phase == ENGINE_ADVANCE) {
        size_t spent = advance_covered(s, left);
        left -= spent;
        if (!s->settle_done)
          break;
        s->engine_phase = ENGINE_PUBLISH;
      } else if (s->engine_phase == ENGINE_PUBLISH) {
        int complete = publish_work(s, left);
        used = s->last_work_used;
        left -= used;
        if (!complete) {
          if (!used || s->pub_stage)
            break;
          continue;
        }
        s->engine_phase = ENGINE_GC;
      } else {
        checkpoint(s, left);
        used = s->last_work_used;
        left -= used;
        if (s->gc_stage)
          break;
        int progress =
            s->parsed_records != s->cycle_records || s->cycle_idle_progress;
        s->cycle_records = s->parsed_records;
        s->cycle_idle_progress = 0;
        s->engine_phase = ENGINE_PARSE;
        if (!progress && parsed_this_tick)
          break;
      }
    }
    if (s->cache_dirty && left) {
      used = prepare_feedback_cache(s, left);
      left -= used;
    }
    s->last_tick_work = work - left;
  }
accounting: {
  size_t pending = peak_mpi_causal_batches_pending(s->batches);
  if (pending > s->stats.pending_peak)
    s->stats.pending_peak = pending;
}
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
  s->stats.observer_cpu_seconds += (double)(end.tv_sec - begin.tv_sec) +
                                   (double)(end.tv_nsec - begin.tv_nsec) * 1e-9;
}
int peak_mpi_causal_service_feedback(const PeakMpiCausalService *s,
                                     PeakMpiPolicyFeedback *f) {
  if (!s || !f)
    return 0;
  if (s->config.rank == s->config.owner_rank)
    return sink_feedback((void *)s, s->config.rank, s->now, f);
  return peak_mpi_causal_transport_feedback(s->transport, f);
}
uint64_t
peak_mpi_causal_service_requested_nonce(const PeakMpiCausalService *s) {
  return s ? peak_mpi_causal_transport_requested_nonce(s->transport) : 0;
}
PeakMpiPolicyReason
peak_mpi_causal_service_fault_reason(const PeakMpiCausalService *s) {
  return s ? s->local_fault : PEAK_MPI_POLICY_RESOURCE_LIMIT;
}
void peak_mpi_causal_service_stats(const PeakMpiCausalService *s,
                                   PeakMpiCausalServiceStats *out) {
  if (!s || !out)
    return;
  peak_mpi_causal_transport_stats(s->transport, out);
  out->records = s->stats.records;
  out->duplicates = s->stats.duplicates;
  out->observer_cpu_seconds = s->stats.observer_cpu_seconds;
  out->oldest_unresolved_age = s->stats.oldest_unresolved_age;
  out->active_operations = s->operation_count;
  out->pending_records += peak_mpi_causal_batches_pending(s->batches);
  out->pending_peak = s->stats.pending_peak;
  out->unavailable |= s->stats.unavailable;
  for (unsigned r = 0; s->ranks && r < s->config.world_size; r++) {
    out->lost_events += s->ranks[r].lost;
    if (s->ranks[r].unknown)
      out->unavailable = 1;
  }
}
