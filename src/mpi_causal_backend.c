#define _POSIX_C_SOURCE 200809L
#include "internal/mpi_causal_backend.h"
#include "internal/mpi_causal_batches.h"
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
} Operation;
typedef struct {
  PeakMpiCostRef ref;
  size_t next;
} PendingInput;
typedef struct {
  unsigned rank;
  uint64_t generation;
  PeakMpiCostRef seal;
  int closed;
} BufferInterval;
typedef struct {
  uint64_t requested_nonce, echoed_nonce, verified_nonce;
  uint64_t incarnation, raw_generation, cut_generation, last_event, control,
      mpi_thread, mpi_thread_generation;
  uint64_t *sequence, *anchor, *prefix, *verify_prefix, *verified_prefix,
      *processed;
  uint64_t idle_group, verify_group, verify_nonce, verify_generation,
      last_verify_generation;
  size_t verify_cursor;
  int idle, verify_active, verify_idle, verify_stage, has_verified;
  size_t visible_cursor, visible_limit, accept_cursor;
  double verify_L, verify_P, verify_elapsed, verify_received;
  uint64_t verify_control;
  double L, P, elapsed, received_at, covered_at, last_L, last_P;
  PeakMpiCostRef continuation, covered_ref;
  int have_continuation, have_raw, active, coarse, unknown, have_prefix;
  PeakMpiPolicyReason reason;
  uint64_t lost;
  PeakMpiPolicy *policy;
  PeakMpiPolicyCohortKey key;
  unsigned parent;
  size_t cohort_size;
  PeakMpiPolicySample covered;
} Rank;
struct PeakMpiCausalService {
  PeakMpiCausalServiceConfig config;
  PeakMpiCausalTransport *transport;
  PeakMpiCostModel *model;
  PeakMpiCausalBatches *batches;
  PeakMpiCausalBatchView plan;
  size_t plan_index;
  int plan_active, building_concurrent;
  PeakMpiCostRef group_start, group_cost;
  Rank *ranks;
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
  size_t gc_cursor, gc_kept, gc_dep_cursor, gc_dep_op;
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
  int task_stage;
  size_t task_op, task_cursor, task_limit, task_count, task_best, task_bound;
  size_t *candidates;
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
  int model_running;
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
  if (st == PEAK_MPI_COST_OK || st == PEAK_MPI_COST_DUPLICATE)
    return 1;
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
                                      PeakMpiCostRef ref) {
  if (!(send->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) || send->immediate)
    return;
  if (!s->free_dep_count) {
    coarse(s, send->rank, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
    return;
  }
  size_t at = s->free_deps[--s->free_dep_count];
  s->deps[at] = (PendingInput){ref, send->dep_head};
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
  if (a->have_prefix)
    memcpy(a->prefix, raw->slot_prefixes, s->config.slots * sizeof(uint64_t));
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
  if (raw->coarse_reason != PEAK_MPI_OBSERVER_REASON_NONE || raw->lost_events) {
    PeakMpiPolicyReason why =
        raw->coarse_reason == PEAK_MPI_OBSERVER_REASON_CAPABILITY
            ? PEAK_MPI_POLICY_MISSING_CONTRACT
            : PEAK_MPI_POLICY_EVENT_LOSS;
    coarse(s, r, why,
           raw->lost_events > a->lost ? raw->lost_events - a->lost : 0);
  }
  return 1;
}
static int advance(PeakMpiCausalService *s, unsigned r,
                   const PeakMpiObserverRecord *rec, PeakMpiCostRef *out) {
  Rank *a = &s->ranks[r];
  double L = sec(rec->L_cut), P = sec(rec->P_cut);
  if (!node(s, r, L - a->last_L, P - a->last_P, rec, out))
    return 0;
  if (a->have_continuation &&
      !edge(s, r, a->continuation, *out, PEAK_MPI_COST_PROGRESS, 0))
    return 0;
  a->last_L = L;
  a->last_P = P;
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
  if (rec->sequence <= *seq) {
    s->stats.duplicates++;
    return 1;
  }
  if (rec->sequence != *seq + 1) {
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
static int parse_record(PeakMpiCausalService *s, unsigned r,
                        const PeakMpiObserverRecord *rec) {
  Rank *a = &s->ranks[r];
  if (a->coarse)
    return 1;
  if (rec->thread_uid && rec->thread_uid <= s->config.slots &&
      rec->sequence > a->processed[rec->thread_uid - 1])
    a->processed[rec->thread_uid - 1] = rec->sequence;
  if (rec->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN ||
      rec->phase == PEAK_MPI_CAUSAL_GROUP_END) {
    PeakMpiCostRef ref;
    if (!advance(s, r, rec, &ref))
      return 1;
    good(s, r, peak_mpi_cost_model_seal(s->model, ref));
    if (rec->phase == PEAK_MPI_CAUSAL_GROUP_END) {
      a->covered_ref = ref;
      a->covered_at = s->now;
      a->covered =
          (PeakMpiPolicySample){{r, s->config.job_context},
                                ++a->cut_generation,
                                prefix_sum(a->processed, s->config.slots),
                                rec->control_generation,
                                sec(rec->L_cut),
                                sec(rec->P_cut),
                                0,
                                sec(rec->progress_end),
                                0,
                                0};
    }
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
    o->context = context_for(s, r, rec->communicator_uid);
    index_insert(s, i);
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
  o->end = *rec;
  o->ended = 1;
  if (o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) {
    o->immediate = (rec->flags & PEAK_MPI_CAUSAL_INACTIVE_REQUEST) != 0;
    if (!o->immediate)
      reserve(s, r, &o->result);
    else
      o->retired = 1;
  }
  a->anchor[rec->thread_uid - 1] = rec->op_uid;
  o->completed =
      o->immediate || !(o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING);
  double L = s->building_concurrent ? a->last_L : sec(rec->L_cut),
         P = s->building_concurrent ? a->last_P : sec(rec->P_cut);
  PeakMpiCostOwnerRef own = PEAK_MPI_COST_NO_OWNER;
  if (L < a->last_L || P < a->last_P || P - a->last_P > L - a->last_L + 1e-12) {
    coarse(s, r, PEAK_MPI_POLICY_BAD_CUT, 0);
    return 1;
  }
  if (L > a->last_L || P > a->last_P) {
    if (!good(s, r, peak_mpi_cost_model_reserve_owner(s->model, &own)) ||
        !good(s, r,
              peak_mpi_cost_model_define_owner(s->model, own, r, L - a->last_L,
                                               P - a->last_P)))
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
  while (budget--) {
    if (budget < initial / 2 && !s->task_stage)
      return;
    if (!s->task_stage) {
      if (!s->operation_count)
        return;
      Operation *o = &s->operations[s->active[s->scan++ % s->operation_count]];
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
        s->task_stage = TASK_COLLECTIVE_SCAN;
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
          local->verified_nonce < o->self_nonce)
        continue;
      if (!o->candidate_epoch)
        o->candidate_epoch = r->verified_nonce;
      if (!o->self_epoch)
        o->self_epoch = local->verified_nonce;
      if (o->begin.operation == PEAK_MPI_CAUSAL_MRECV) {
        Operation *p = find_op(s, o->rank, o->begin.progress_begin);
        if (!p || !p->matched)
          continue;
        if (p->peer_op != SIZE_MAX)
          s->task_bound = p->peer_op;
        else {
          o->candidate_epoch = p->candidate_epoch;
          o->self_epoch = p->self_epoch;
        }
      }
      s->task_stage = TASK_MESSAGES;
      continue;
    }
    Operation *o = &s->operations[s->task_op];
    if (s->task_stage == TASK_MESSAGES) {
      if (s->task_cursor == s->task_limit) {
        if (!s->task_count || !s->task_ready) {
          s->task_stage = 0;
          continue;
        }
        s->task_ambiguous =
            (s->task_count > 1 && !s->task_ordered) || s->task_was_ambiguous;
        s->task_cursor = 0;
        s->task_stage = TASK_EARLIER;
        continue;
      }
      size_t slot = s->active[s->task_cursor++];
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
      if (x->buffer == SIZE_MAX &&
          x->end.progress_scope == PEAK_MPI_OBSERVER_SCOPE_INTERVAL &&
          (x->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !x->immediate &&
          !x->result_defined)
        s->task_ready = 0;
    } else if (s->task_stage == TASK_EARLIER) {
      if (s->task_cursor == s->task_limit) {
        s->task_cursor = 0;
        if (!s->task_ambiguous) {
          s->task_count = 1;
          s->candidates[0] = s->task_best;
        }
        s->task_stage = TASK_EDGES;
        continue;
      }
      Operation *x = &s->operations[s->active[s->task_cursor++]];
      Operation *send = &s->operations[s->task_best];
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
        if (same_thread && x->completed && !x->matched)
          s->task_stage = 0;
        else
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
        if (send->begin.operation == PEAK_MPI_CAUSAL_SEND ||
            send->begin.operation == PEAK_MPI_CAUSAL_SENDRECV)
          edge(s, send->rank, o->entry, output(send),
               PEAK_MPI_COST_SEND_REVERSE, 0);
        else if (send->begin.operation == PEAK_MPI_CAUSAL_SSEND)
          edge(s, send->rank, o->entry, output(send),
               s->task_ambiguous ? PEAK_MPI_COST_PROGRESS
                                 : PEAK_MPI_COST_GUARANTEED,
               0);
        if (send->begin.operation == PEAK_MPI_CAUSAL_SEND ||
            send->begin.operation == PEAK_MPI_CAUSAL_SENDRECV ||
            send->begin.operation == PEAK_MPI_CAUSAL_SSEND)
          remember_completion_input(s, send, o->entry);
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
          if (send->begin.operation != PEAK_MPI_CAUSAL_SENDRECV)
            send->matched = 1;
          send->peer_op = s->task_op;
          seal_output(s, send);
        }
        if (o->begin.operation == PEAK_MPI_CAUSAL_MPROBE)
          send->reserved_by = s->task_op;
      }
      seal_output(s, o);
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
          x->candidate_epoch <= s->task_epoch && envelope(x, o))
        x->ambiguity_closed = 1;
      if (send_op(x->begin.operation) && !x->send_matched && x->ended &&
          same_message(x, o) && x->visible &&
          x->visible_nonce <= s->task_epoch) {
        x->send_matched = 1;
        if (x->begin.operation != PEAK_MPI_CAUSAL_SENDRECV)
          x->matched = 1;
        seal_output(s, x);
      }
    } else if (s->task_stage == TASK_COLLECTIVE_SCAN) {
      if (s->task_cursor == s->task_limit) {
        if (!s->task_ready || !s->task_count ||
            s->candidates[0] != s->task_op || !o->context ||
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
      size_t slot = s->active[s->task_cursor++];
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
static int publish_work(PeakMpiCausalService *s, size_t budget) {
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
      peak_mpi_policy_tick(s->ranks[s->pub_root].policy, s->now);
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
      if (!a->covered.generation ||
          peak_mpi_cost_model_delay(s->model, a->covered_ref, &a->covered.D) !=
              PEAK_MPI_COST_OK) {
        s->pub_complete = 0;
        continue;
      }
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
      if (!o->matched || (send_op(o->begin.operation) && !o->send_matched) ||
          (o->ambiguous && !o->ambiguity_closed) ||
          ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->completed) ||
          (o->buffer != SIZE_MAX && !s->buffers[o->buffer].closed)) {
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
        if (o->result_defined) {
          if (peak_mpi_cost_model_delay(s->model, o->result, &d) !=
              PEAK_MPI_COST_OK)
            s->pub_complete = 0;
          else
            s->pub_F = fmax(s->pub_F, d);
        }
        if (o->collective_built) {
          if (peak_mpi_cost_model_delay(s->model, o->join, &d) !=
              PEAK_MPI_COST_OK)
            s->pub_complete = 0;
          else
            s->pub_F = fmax(s->pub_F, d);
        }
        if (o->buffer != SIZE_MAX && s->buffers[o->buffer].closed) {
          if (peak_mpi_cost_model_delay(s->model, s->buffers[o->buffer].seal,
                                        &d) != PEAK_MPI_COST_OK)
            s->pub_complete = 0;
          else
            s->pub_F = fmax(s->pub_F, d);
        }
        if (!o->result_defined && o->dep_head != SIZE_MAX) {
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
        if (s->pub_count > budget + 1)
          return 0;
        budget = budget + 1 - s->pub_count;
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
  if (o->buffer != SIZE_MAX && !s->buffers[o->buffer].closed)
    return 1;
  if (!o->ended || !o->matched ||
      (send_op(o->begin.operation) && !o->send_matched) ||
      (o->ambiguous && !o->ambiguity_closed) ||
      ((o->begin.flags & PEAK_MPI_CAUSAL_NONBLOCKING) && !o->completed))
    return 1;
  if (o->begin.operation == PEAK_MPI_CAUSAL_MPROBE && o->peer_op != SIZE_MAX &&
      s->operations[o->peer_op].used && !s->operations[o->peer_op].send_matched)
    return 1;
  return 0;
}
static void keep_ref(PeakMpiCausalService *s, PeakMpiCostRef ref) {
  if (ref.generation && ref.slot < s->config.model.max_nodes)
    s->keep[ref.slot] = s->gc_generation;
}
static void checkpoint(PeakMpiCausalService *s, size_t budget) {
  if (!s->gc_stage) {
    if (++s->gc_generation == 0)
      return;
    s->gc_stage = 1;
    s->gc_cursor = 0;
  }
  while (budget && s->gc_stage) {
    budget--;
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
        index_remove(s, slot);
        memset(o, 0, sizeof *o);
        s->free_ops[s->free_count++] = slot;
      }
    } else {
      size_t used = 0;
      PeakMpiCostStatus result =
          peak_mpi_cost_model_collect(s->model, budget + 1, &used);
      if (result != PEAK_MPI_COST_YIELDED)
        s->gc_stage = 0;
      return;
    }
  }
}
static void verify_prefixes(PeakMpiCausalService *s, size_t budget) {
  unsigned visits = 0;
  while (budget && visits < s->config.world_size) {
    unsigned rank = s->visibility_rank
                        ? s->visibility_rank - 1
                        : s->verify_rank_cursor++ % s->config.world_size;
    Rank *a = &s->ranks[rank];
    visits++;
    if (!a->have_prefix || a->coarse) {
      budget--;
      continue;
    }
    if (!a->verify_active) {
      if (a->last_verify_generation == a->raw_generation && a->has_verified &&
          a->verified_nonce >= a->echoed_nonce) {
        budget--;
        continue;
      }
      /* One coherent frozen tuple. Prefix copy is one fixed O(slots) quantum;
       * subsequent accepted-prefix, pending-record and visible-operation walks
       * resume at one transition per work unit. */
      memcpy(a->verify_prefix, a->prefix, s->config.slots * sizeof(uint64_t));
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
      budget = budget > s->config.slots ? budget - s->config.slots : 0;
    }
    if (a->verify_stage == 1) {
      while (budget && a->accept_cursor < s->config.slots) {
        size_t at = a->accept_cursor;
        budget--;
        if (a->sequence[at] < a->verify_prefix[at])
          break;
        a->accept_cursor++;
      }
      if (a->accept_cursor < s->config.slots)
        continue;
      a->verify_stage = 2;
    }
    if (a->verify_stage == 2) {
      if (peak_mpi_causal_batches_prefix_clear(
              s->batches, rank, a->verify_prefix, s->config.slots,
              &a->verify_cursor, &budget) != 1)
        continue;
      a->verify_stage = 3;
      a->visible_cursor = 0;
      a->visible_limit = s->operation_count;
      s->visibility_rank = rank + 1;
    }
    if (a->verify_stage == 3) {
      while (budget && a->visible_cursor < a->visible_limit) {
        Operation *o = &s->operations[s->active[a->visible_cursor++]];
        budget--;
        if (o->rank == rank && !o->visible &&
            o->begin.sequence <= a->verify_prefix[o->begin.thread_uid - 1]) {
          o->visible = 1;
          o->visible_nonce = a->verify_nonce;
        }
      }
      if (a->visible_cursor < a->visible_limit)
        return;
      uint64_t *old = a->verified_prefix;
      a->verified_prefix = a->verify_prefix;
      a->verify_prefix = old;
      a->verified_nonce = a->verify_nonce;
      a->last_verify_generation = a->verify_generation;
      a->has_verified = 1;
      a->verify_active = a->verify_stage = 0;
      s->visibility_rank = 0;
      if (a->verify_idle && peak_mpi_causal_batches_last_group(
                                s->batches, rank) == a->verify_group) {
        PeakMpiObserverRecord idle = {0};
        idle.P_cut = (uint64_t)(a->verify_P * 1e9);
        idle.L_cut = idle.P_cut + (uint64_t)((a->verify_L - a->verify_P) * 1e9);
        if (sec(idle.L_cut) >= a->last_L && sec(idle.P_cut) >= a->last_P) {
          PeakMpiCostRef ref;
          if (advance(s, rank, &idle, &ref)) {
            good(s, rank, peak_mpi_cost_model_seal(s->model, ref));
            a->covered_ref = ref;
            a->covered_at = a->verify_received;
            a->covered = (PeakMpiPolicySample){
                {rank, s->config.job_context},
                ++a->cut_generation,
                prefix_sum(a->verified_prefix, s->config.slots),
                a->verify_control,
                sec(idle.L_cut),
                sec(idle.P_cut),
                0,
                a->verify_elapsed,
                0,
                0};
          }
        }
      }
    }
  }
}
static void parse_batches(PeakMpiCausalService *s, size_t budget) {
  if (!s->plan_active) {
    int ready =
        peak_mpi_causal_batches_prepare_next(s->batches, budget / 2, &s->plan);
    budget -= budget / 2;
    if (ready < 0) {
      coarse(s, s->plan.rank, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      return;
    }
    if (!ready)
      return;
    s->plan_active = 1;
    s->plan_index = 0;
  }
  while (s->plan_index < s->plan.record_count && budget) {
    const PeakMpiObserverRecord *saved =
        peak_mpi_causal_batches_record_at(s->batches, s->plan_index);
    if (!saved) {
      coarse(s, s->plan.rank, PEAK_MPI_POLICY_BAD_IDENTITY, 0);
      return;
    }
    PeakMpiObserverRecord rec = *saved;
    unsigned rank = s->plan.rank;
    Rank *a = &s->ranks[rank];
    budget--;
    if (s->plan_index == 0) {
      s->building_concurrent = 0;
      parse_record(s, rank, &rec);
      s->group_start = a->continuation;
      if (s->plan.concurrent) {
        if (!node(s, rank, sec(s->plan.end->L_cut) - sec(s->plan.begin->L_cut),
                  sec(s->plan.end->P_cut) - sec(s->plan.begin->P_cut), NULL,
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
  }
  if (s->plan_index == s->plan.record_count) {
    s->building_concurrent = 0;
    s->plan_active = 0;
    peak_mpi_causal_batches_commit(s->batches);
  }
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
static int sink_feedback(void *v, unsigned r, double now,
                         PeakMpiPolicyFeedback *f) {
  PeakMpiCausalService *s = v;
  if (r >= s->config.world_size)
    return 0;
  return peak_mpi_policy_feedback(
             s->ranks[root(s, r)].policy,
             (PeakMpiPolicyMemberId){r, s->config.job_context}, now,
             f) == PEAK_MPI_POLICY_OK;
}
static void sink_fault(void *v, unsigned r, PeakMpiPolicyReason reason) {
  PeakMpiCausalService *s = v;
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
  s->ranks = calloc(c->world_size, sizeof *s->ranks);
  s->sequences = calloc((size_t)c->world_size * c->slots, sizeof *s->sequences);
  s->anchors = calloc((size_t)c->world_size * c->slots, sizeof *s->anchors);
  s->prefixes = calloc((size_t)c->world_size * c->slots, sizeof *s->prefixes);
  s->verify_prefixes =
      calloc((size_t)c->world_size * c->slots, sizeof *s->verify_prefixes);
  s->processed = calloc((size_t)c->world_size * c->slots, sizeof *s->processed);
  s->verified_prefixes =
      calloc((size_t)c->world_size * c->slots, sizeof *s->verified_prefixes);
  s->nodes = calloc(c->model.max_nodes, sizeof *s->nodes);
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
  s->peers = calloc((size_t)c->world_size * c->world_size, 1);
  s->model = peak_mpi_cost_model_create(&c->model);
  s->batches =
      peak_mpi_causal_batches_create(c->world_size, c->max_pending_records);
  if (!s->ranks || !s->sequences || !s->anchors || !s->prefixes ||
      !s->verify_prefixes || !s->verified_prefixes || !s->processed ||
      !s->nodes || !s->index || !s->operations || !s->active ||
      !s->candidates || !s->free_ops || !s->keep || !s->deps || !s->free_deps ||
      !s->buffers || !s->buffer_index || !s->comms || !s->comm_ranks ||
      !s->members || !s->contexts || !s->samples || !s->peers || !s->model ||
      !s->batches)
    goto fail;
  for (unsigned r = 0; r < c->world_size; r++) {
    Rank *a = &s->ranks[r];
    a->parent = r;
    a->cohort_size = 1;
    a->key = (PeakMpiPolicyCohortKey){c->job_context, r, 1};
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
  if (s->ranks)
    for (unsigned i = 0; i < s->config.world_size; i++)
      peak_mpi_policy_free(s->ranks[i].policy);
  free(s->ranks);
  free(s->sequences);
  free(s->anchors);
  free(s->prefixes);
  free(s->verify_prefixes);
  free(s->processed);
  free(s->verified_prefixes);

  free(s->nodes);
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
  peak_mpi_causal_transport_tick(s->transport, capture, &local_raw, now, bytes);
  if (s->config.rank == s->config.owner_rank) {
    if (s->pub_stage) {
      if (publish_work(s, work))
        checkpoint(s, 0);
      goto finished;
    }
    if (s->gc_stage) {
      checkpoint(s, work);
      goto finished;
    }
    if (!s->model_running && s->visibility_rank) {
      verify_prefixes(s, work / 4);
      if (s->visibility_rank)
        goto finished;
    }
    if (!s->model_running && !s->task_stage)
      parse_batches(s, work / 4);
    if (!s->model_running && !s->plan_active) {
      if (!s->task_stage)
        verify_prefixes(s, work / 8);
      if (!s->visibility_rank)
        matcher_work(s, work / 8);
    }
    size_t used = 0;
    PeakMpiCostStatus status =
        s->plan_active ? PEAK_MPI_COST_PENDING
                       : peak_mpi_cost_model_run(s->model, work / 4, &used);
    s->model_running = status == PEAK_MPI_COST_YIELDED;
    if (status == PEAK_MPI_COST_COARSE)
      for (unsigned r = 0; r < s->config.world_size; r++)
        if (!s->ranks[r].coarse)
          coarse(s, r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0);
    if (!s->model_running && !s->plan_active && !s->task_stage &&
        !s->visibility_rank) {
      if (publish_work(s, work / 8))
        checkpoint(s, 0);
    }
  }
finished: {
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
