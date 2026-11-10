#include "internal/mpi_causal_batches.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NONE SIZE_MAX
#define TOMBSTONE SIZE_MAX
#define PROBES 64u

typedef struct {
  PeakMpiObserverRecord r;
  size_t free_next, next, op, position, slot_previous, slot_next;
  unsigned rank;
} BatchEntry;
typedef struct {
  uint64_t uid;
  unsigned rank;
  size_t free_next, group, begin, end, next_group;
  size_t activation_head, activation_tail, sub_head, sub_tail;
  size_t activation_count, sub_count;
} BatchOperation;
typedef struct {
  uint64_t uid, previous;
  unsigned rank;
  size_t free_next, begin, end, successor, ready_next;
  size_t operation_head, operation_tail, begin_head, begin_tail;
  size_t operations, begins, ends, pairs, activations, subrecords;
  int queued, exposed, implicit;
} BatchGroup;
enum {
  IDLE,
  BUILD_BEGIN,
  BUILD_ACTIVATIONS,
  BUILD_SUBRECORDS,
  BUILD_END,
  BUILD_MARKER,
  READY,
  RETIRE_RECORDS,
  RETIRE_OPERATIONS,
  RETIRE_GROUP,
  ERROR
};
struct PeakMpiCausalBatches {
  size_t world, slots, capacity, group_capacity, pending;
  BatchEntry *entries;
  BatchOperation *operations;
  BatchGroup *groups;
  size_t *operation_index, *group_index;
  size_t operation_index_capacity, group_index_capacity;
  size_t free_entry, free_operation, free_group;
  size_t *active, *plan, *slot_head, *slot_tail, *rank_next;
  uint64_t *last;
  size_t ready_head, ready_tail, current, plan_count;
  size_t cursor, parent_cursor, retire, retire_operation, retire_epoch;
  int state, compact_plan;
  PeakMpiObserverRecord implicit_begin, implicit_end;
  unsigned reset_stage;
  size_t reset_cursor;
  unsigned error_rank;
  uint64_t error_uid;
};

static size_t hash(unsigned rank, uint64_t uid) {
  uint64_t x = uid ^ ((uint64_t)rank * UINT64_C(0x9e3779b97f4a7c15));
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  return (size_t)(x ^ (x >> 31));
}
static void invalid(PeakMpiCausalBatches *b, unsigned rank, uint64_t uid) {
  b->state = ERROR;
  b->error_rank = rank;
  b->error_uid = uid;
}
static size_t group_find(PeakMpiCausalBatches *b, unsigned rank, uint64_t uid,
                         int create) {
  size_t h = hash(rank, uid), empty = NONE;
  for (size_t i = 0; i < PROBES; i++) {
    size_t at = (h + i) & (b->group_index_capacity - 1);
    size_t v = b->group_index[at];
    if (v == TOMBSTONE) {
      if (empty == NONE)
        empty = at;
    } else if (!v) {
      if (empty == NONE)
        empty = at;
      break;
    } else {
      BatchGroup *g = &b->groups[v - 1];
      if (g->rank == rank && g->uid == uid)
        return v - 1;
    }
  }
  if (!create || empty == NONE || b->free_group == NONE)
    return NONE;
  size_t at = b->free_group;
  BatchGroup *g = &b->groups[at];
  b->free_group = g->free_next;
  memset(g, 0, sizeof(*g));
  g->rank = rank;
  g->uid = uid;
  g->begin = g->end = g->successor = g->ready_next = NONE;
  g->operation_head = g->operation_tail = g->begin_head = g->begin_tail = NONE;
  b->group_index[empty] = at + 1;
  return at;
}
static size_t operation_find(PeakMpiCausalBatches *b, unsigned rank,
                             uint64_t uid, int create) {
  size_t h = hash(rank, uid), empty = NONE;
  for (size_t i = 0; i < PROBES; i++) {
    size_t at = (h + i) & (b->operation_index_capacity - 1);
    size_t v = b->operation_index[at];
    if (v == TOMBSTONE) {
      if (empty == NONE)
        empty = at;
    } else if (!v) {
      if (empty == NONE)
        empty = at;
      break;
    } else {
      BatchOperation *o = &b->operations[v - 1];
      if (o->rank == rank && o->uid == uid)
        return v - 1;
    }
  }
  if (!create || empty == NONE || b->free_operation == NONE)
    return NONE;
  size_t at = b->free_operation;
  BatchOperation *o = &b->operations[at];
  b->free_operation = o->free_next;
  memset(o, 0, sizeof(*o));
  o->rank = rank;
  o->uid = uid;
  o->group = o->begin = o->end = o->next_group = NONE;
  o->activation_head = o->activation_tail = o->sub_head = o->sub_tail = NONE;
  b->operation_index[empty] = at + 1;
  return at;
}
static void group_remove(PeakMpiCausalBatches *b, size_t index) {
  BatchGroup *g = &b->groups[index];
  size_t h = hash(g->rank, g->uid);
  for (size_t i = 0; i < PROBES; i++) {
    size_t at = (h + i) & (b->group_index_capacity - 1);
    if (b->group_index[at] == index + 1) {
      b->group_index[at] = TOMBSTONE;
      break;
    }
  }
  g->free_next = b->free_group;
  b->free_group = index;
}
static void operation_remove(PeakMpiCausalBatches *b, size_t index) {
  BatchOperation *o = &b->operations[index];
  size_t h = hash(o->rank, o->uid);
  for (size_t i = 0; i < PROBES; i++) {
    size_t at = (h + i) & (b->operation_index_capacity - 1);
    if (b->operation_index[at] == index + 1) {
      b->operation_index[at] = TOMBSTONE;
      break;
    }
  }
  o->free_next = b->free_operation;
  b->free_operation = index;
}
static void maybe_ready(PeakMpiCausalBatches *b, size_t index) {
  BatchGroup *g = &b->groups[index];
  if (b->state == ERROR || g->begin == NONE || g->end == NONE)
    return;
  size_t expected = g->implicit ? 1 : b->entries[g->end].r.completion_count;
  if (!expected || g->begins > expected || g->ends > expected ||
      g->operations > expected) {
    invalid(b, g->rank, g->uid);
    return;
  }
  if (g->queued || g->exposed)
    return;
  if (g->begins != expected || g->ends != expected || g->pairs != expected ||
      g->previous != b->last[g->rank])
    return;
  if (b->rank_next[g->rank] != index) {
    invalid(b, g->rank, g->uid);
    return;
  }
  g->queued = 1;
  g->ready_next = NONE;
  if (b->ready_tail == NONE)
    b->ready_head = index;
  else
    b->groups[b->ready_tail].ready_next = index;
  b->ready_tail = index;
}
static int attach(PeakMpiCausalBatches *b, size_t op, size_t group) {
  BatchOperation *o = &b->operations[op];
  BatchGroup *g = &b->groups[group];
  if (o->group != NONE) {
    if (o->group != group) {
      invalid(b, g->rank, g->uid);
      return 0;
    }
    return 1;
  }
  o->group = group;
  if (g->operation_tail == NONE)
    g->operation_head = op;
  else
    b->operations[g->operation_tail].next_group = op;
  g->operation_tail = op;
  g->operations++;
  g->activations += o->activation_count;
  g->subrecords += o->sub_count;
  return 1;
}
static void append(PeakMpiCausalBatches *b, size_t *head, size_t *tail,
                   size_t record) {
  if (*tail == NONE)
    *head = record;
  else
    b->entries[*tail].next = record;
  *tail = record;
}

PeakMpiCausalBatches *peak_mpi_causal_batches_create_with_slots(size_t world,
                                                                size_t slots,
                                                                size_t cap) {
  if (!world || !cap || world > UINT32_MAX ||
      world > SIZE_MAX / sizeof(size_t) || cap > SIZE_MAX / 16 ||
      cap > SIZE_MAX / sizeof(BatchEntry) ||
      cap > SIZE_MAX / sizeof(BatchOperation) ||
      cap > SIZE_MAX / (2 * sizeof(BatchGroup)) ||
      (slots &&
       (world > SIZE_MAX / slots || world * slots > SIZE_MAX / sizeof(size_t))))
    return NULL;
  PeakMpiCausalBatches *b = calloc(1, sizeof(*b));
  if (!b)
    return NULL;
  b->world = world;
  b->slots = slots;
  b->capacity = cap;
  b->group_capacity = cap * 2;
  b->operation_index_capacity = b->group_index_capacity = 2;
  while (b->operation_index_capacity < cap * 2)
    b->operation_index_capacity *= 2;
  while (b->group_index_capacity < cap * 4)
    b->group_index_capacity *= 2;
  b->entries = calloc(cap, sizeof(*b->entries));
  b->operations = calloc(cap, sizeof(*b->operations));
  b->groups = calloc(b->group_capacity, sizeof(*b->groups));
  b->operation_index = calloc(b->operation_index_capacity, sizeof(size_t));
  b->group_index = calloc(b->group_index_capacity, sizeof(size_t));
  b->active = calloc(cap, sizeof(size_t));
  b->plan = calloc(cap, sizeof(size_t));
  b->last = calloc(world, sizeof(uint64_t));
  b->rank_next = malloc(world * sizeof(size_t));
  if (slots) {
    b->slot_head = malloc(world * slots * sizeof(size_t));
    b->slot_tail = malloc(world * slots * sizeof(size_t));
  }
  if (!b->entries || !b->operations || !b->groups || !b->operation_index ||
      !b->group_index || !b->active || !b->plan || !b->last || !b->rank_next ||
      (slots && (!b->slot_head || !b->slot_tail))) {
    peak_mpi_causal_batches_destroy(b);
    return NULL;
  }
  for (size_t i = 0; i < cap; i++) {
    b->entries[i].free_next = b->operations[i].free_next = i + 1;
  }
  b->entries[cap - 1].free_next = b->operations[cap - 1].free_next = NONE;
  for (size_t i = 0; i < b->group_capacity; i++)
    b->groups[i].free_next = i + 1;
  b->groups[b->group_capacity - 1].free_next = NONE;
  for (size_t i = 0; i < world; i++)
    b->rank_next[i] = NONE;
  for (size_t i = 0; slots && i < world * slots; i++)
    b->slot_head[i] = b->slot_tail[i] = NONE;
  b->ready_head = b->ready_tail = b->current = NONE;
  return b;
}
PeakMpiCausalBatches *peak_mpi_causal_batches_create(size_t world, size_t cap) {
  return peak_mpi_causal_batches_create_with_slots(world, 0, cap);
}
void peak_mpi_causal_batches_destroy(PeakMpiCausalBatches *b) {
  if (!b)
    return;
  free(b->entries);
  free(b->operations);
  free(b->groups);
  free(b->operation_index);
  free(b->group_index);
  free(b->active);
  free(b->plan);
  free(b->last);
  free(b->rank_next);
  free(b->slot_head);
  free(b->slot_tail);
  free(b);
}
int peak_mpi_causal_batches_ingest(PeakMpiCausalBatches *b, unsigned rank,
                                   const PeakMpiObserverRecord *r) {
  if (!b || !r || rank >= b->world || b->free_entry == NONE)
    return 0;
  if (b->state == ERROR)
    return 1;
  if (!r->thread_uid || (b->slots && r->thread_uid > b->slots)) {
    invalid(b, rank, r->op_uid);
    return 1;
  }
  int implicit = (r->flags & PEAK_MPI_CAUSAL_COMPACT_GROUP) != 0;
  if (implicit && (!(r->flags & PEAK_MPI_CAUSAL_ORDERED_CALLER) || !r->batch_uid || !r->op_uid ||
      r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN || r->phase == PEAK_MPI_CAUSAL_GROUP_END ||
      r->group_P_cut > r->group_L_cut ||
      ((r->flags & PEAK_MPI_CAUSAL_APP_COST_KNOWN) &&
       (r->group_P_cut > r->group_A_cut || r->group_A_cut > r->group_L_cut)))) {
    invalid(b, rank, r->op_uid);
    return 1;
  }
  if (implicit &&
      ((r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN &&
        (r->group_L_cut > r->L_cut || r->group_P_cut > r->P_cut ||
         ((r->flags & PEAK_MPI_CAUSAL_APP_COST_KNOWN) && r->group_A_cut > r->A_cut) ||
         r->policy_generation > r->control_generation)) ||
       (r->phase == PEAK_MPI_CAUSAL_CALL_END &&
        (r->group_L_cut < r->L_cut || r->group_P_cut < r->P_cut ||
         ((r->flags & PEAK_MPI_CAUSAL_APP_COST_KNOWN) && r->group_A_cut < r->A_cut) ||
         r->policy_generation < r->control_generation)))) {
    invalid(b, rank, r->op_uid);
    return 1;
  }
  size_t group = NONE, op = NONE;
  if (r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN ||
      r->phase == PEAK_MPI_CAUSAL_GROUP_END)
    group = group_find(b, rank, r->op_uid, 1);
  else if (r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN ||
           r->phase == PEAK_MPI_CAUSAL_CALL_END) {
    group = group_find(b, rank, r->batch_uid, 1);
    op = operation_find(b, rank, r->op_uid, 1);
  } else if (r->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE ||
             r->phase == PEAK_MPI_CAUSAL_REQUEST_COMPLETE ||
             r->phase == PEAK_MPI_CAUSAL_REQUEST_RETIRED ||
             r->phase == PEAK_MPI_CAUSAL_MESSAGE_BOUND) {
    op = operation_find(b, rank, r->batch_uid, 1);
    if (op != NONE)
      group = b->operations[op].group;
  } else {
    invalid(b, rank, r->op_uid);
    return 1;
  }
  if ((r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN ||
       r->phase == PEAK_MPI_CAUSAL_GROUP_END ||
       r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN ||
       r->phase == PEAK_MPI_CAUSAL_CALL_END) &&
      group == NONE)
    return 0;
  if (r->phase != PEAK_MPI_CAUSAL_GROUP_BEGIN &&
      r->phase != PEAK_MPI_CAUSAL_GROUP_END && op == NONE)
    return 0;
  if (group != NONE && b->groups[group].exposed) {
    invalid(b, rank, b->groups[group].uid);
    return 1;
  }
  if (implicit && group == NONE) {
    invalid(b, rank, r->op_uid);
    return 1;
  }
  if (group != NONE && b->groups[group].implicit) {
    BatchGroup *g = &b->groups[group];
    const PeakMpiObserverRecord *first = &b->entries[g->begin].r;
    if (!implicit || g->end != NONE || first->thread_uid != r->thread_uid ||
        first->thread_generation != r->thread_generation ||
        first->incarnation != r->incarnation || first->sequence >= r->sequence ||
        ((r->phase == PEAK_MPI_CAUSAL_CALL_END || r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN) &&
         (first->op_uid != r->op_uid || first->result_ref != r->result_ref))) {
      invalid(b, rank, g->uid);
      return 1;
    }
  }
  int implicit_begin = implicit && r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN;
  uint64_t previous = implicit_begin ? r->result_ref : r->input_ref;
  size_t predecessor = NONE;
  if ((r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN || implicit_begin) &&
      previous != b->last[rank]) {
    predecessor = group_find(b, rank, previous, 1);
    if (predecessor == NONE)
      return 0;
  }
  size_t record = b->free_entry;
  BatchEntry *e = &b->entries[record];
  b->free_entry = e->free_next;
  e->r = *r;
  e->rank = rank;
  e->next = e->slot_previous = e->slot_next = NONE;
  e->op = op;
  e->position = b->pending;
  b->active[b->pending++] = record;
  if (b->slots) {
    size_t at = (size_t)rank * b->slots + r->thread_uid - 1;
    e->slot_previous = b->slot_tail[at];
    if (e->slot_previous == NONE)
      b->slot_head[at] = record;
    else
      b->entries[e->slot_previous].slot_next = record;
    b->slot_tail[at] = record;
  }
  if (implicit_begin) {
    BatchGroup *g = &b->groups[group];
    if (g->begin != NONE || g->end != NONE || g->operations || previous == g->uid) {
      invalid(b, rank, g->uid);
      return 1;
    }
    g->implicit = 1;
    g->begin = record;
    g->previous = previous;
    if (previous == b->last[rank]) {
      if (b->rank_next[rank] != NONE && b->rank_next[rank] != group)
        invalid(b, rank, g->uid);
      else b->rank_next[rank] = group;
    } else {
      if (b->groups[predecessor].successor != NONE &&
          b->groups[predecessor].successor != group)
        invalid(b, rank, g->uid);
      else b->groups[predecessor].successor = group;
    }
  } else if (implicit && r->phase == PEAK_MPI_CAUSAL_CALL_END) {
    BatchGroup *g = &b->groups[group];
    if (!g->implicit || g->end != NONE) {
      invalid(b, rank, g->uid);
      return 1;
    }
    g->end = record;
  }
  if (r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN) {
    BatchGroup *g = &b->groups[group];
    if (g->begin != NONE || r->input_ref == r->op_uid) {
      invalid(b, rank, g->uid);
      return 1;
    }
    g->begin = record;
    g->previous = r->input_ref;
    if (g->previous == b->last[rank]) {
      if (b->rank_next[rank] != NONE && b->rank_next[rank] != group)
        invalid(b, rank, g->uid);
      else
        b->rank_next[rank] = group;
    } else {
      if (b->groups[predecessor].successor != NONE &&
          b->groups[predecessor].successor != group)
        invalid(b, rank, g->uid);
      else
        b->groups[predecessor].successor = group;
    }
  } else if (r->phase == PEAK_MPI_CAUSAL_GROUP_END) {
    BatchGroup *g = &b->groups[group];
    if (g->end != NONE) {
      invalid(b, rank, g->uid);
      return 1;
    }
    g->end = record;
  } else if (r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN ||
             r->phase == PEAK_MPI_CAUSAL_CALL_END) {
    BatchOperation *o = &b->operations[op];
    BatchGroup *g = &b->groups[group];
    if (!attach(b, op, group))
      return 1;
    if (r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN) {
      if (o->begin != NONE) {
        invalid(b, rank, g->uid);
        return 1;
      }
      o->begin = record;
      append(b, &g->begin_head, &g->begin_tail, record);
      g->begins++;
      if (o->end != NONE)
        g->pairs++;
    } else {
      if (o->end != NONE) {
        invalid(b, rank, g->uid);
        return 1;
      }
      o->end = record;
      g->ends++;
      if (o->begin != NONE)
        g->pairs++;
    }
  } else {
    BatchOperation *o = &b->operations[op];
    if (r->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE) {
      append(b, &o->activation_head, &o->activation_tail, record);
      o->activation_count++;
      if (group != NONE)
        b->groups[group].activations++;
    } else {
      append(b, &o->sub_head, &o->sub_tail, record);
      o->sub_count++;
      if (group != NONE)
        b->groups[group].subrecords++;
    }
  }
  if (group != NONE)
    maybe_ready(b, group);
  return 1;
}
static void view(PeakMpiCausalBatches *b, PeakMpiCausalBatchView *v) {
  if (!v)
    return;
  memset(v, 0, sizeof(*v));
  if (b->state == ERROR) {
    v->rank = b->error_rank;
    v->group_uid = b->error_uid;
    return;
  }
  const BatchGroup *g = &b->groups[b->current];
  v->rank = g->rank;
  v->group_uid = g->uid;
  v->native_count = g->begins;
  v->record_count = b->plan_count;
  v->concurrent = g->begins > 1;
  v->begin = &b->entries[g->begin].r;
  v->end = &b->entries[g->end].r;
  if (g->implicit) {
    PeakMpiCausalBatches *mutable = b;
    mutable->implicit_begin = *v->begin;
    mutable->implicit_end = *v->end;
    PeakMpiObserverRecord *begin = &mutable->implicit_begin, *end = &mutable->implicit_end;
    begin->kind = PEAK_MPI_OBSERVER_EVENT_LOCAL_COST;
    begin->phase = PEAK_MPI_CAUSAL_GROUP_BEGIN;
    begin->operation = 0;
    begin->op_uid = g->uid;
    begin->input_ref = begin->result_ref;
    begin->result_ref = 0;
    begin->flags &= ~PEAK_MPI_CAUSAL_COMPACT_GROUP;
    const PeakMpiObserverRecord *physical_end = v->end;
    *end = (PeakMpiObserverRecord){
      .incarnation = physical_end->incarnation,
      .thread_uid = physical_end->thread_uid,
      .thread_generation = physical_end->thread_generation,
      .sequence = physical_end->sequence,
      .kind = PEAK_MPI_OBSERVER_EVENT_BATCH_END,
      .phase = PEAK_MPI_CAUSAL_GROUP_END,
      .op_uid = g->uid, .batch_uid = g->uid, .completion_count = 1,
      .flags = physical_end->flags & PEAK_MPI_CAUSAL_APP_COST_KNOWN,
      .L_cut = physical_end->group_L_cut, .P_cut = physical_end->group_P_cut,
      .A_cut = physical_end->group_A_cut,
      .control_generation = physical_end->policy_generation,
      .progress_end = (uint64_t)physical_end->partition |
                      ((uint64_t)physical_end->completion_index << 32)
    };
    begin->L_cut = begin->group_L_cut; begin->P_cut = begin->group_P_cut; begin->A_cut = begin->group_A_cut;
    begin->control_generation = begin->policy_generation;
    begin->progress_end = (uint64_t)begin->partition |
                          ((uint64_t)begin->completion_index << 32);
    begin->partition = begin->completion_index = 0;
    begin->group_L_cut = begin->group_P_cut = begin->group_A_cut = 0;
    begin->policy_generation = 0;
    v->begin = begin; v->end = end;
  }
}
static void free_record(PeakMpiCausalBatches *b, size_t index) {
  BatchEntry *e = &b->entries[index];
  if (b->slots) {
    size_t at = (size_t)e->rank * b->slots + e->r.thread_uid - 1;
    if (e->slot_previous == NONE)
      b->slot_head[at] = e->slot_next;
    else
      b->entries[e->slot_previous].slot_next = e->slot_next;
    if (e->slot_next == NONE)
      b->slot_tail[at] = e->slot_previous;
    else
      b->entries[e->slot_next].slot_previous = e->slot_previous;
  }
  b->pending--;
  b->active[e->position] = b->active[b->pending];
  b->entries[b->active[e->position]].position = e->position;
  e->free_next = b->free_entry;
  b->free_entry = index;
  b->retire_epoch = (b->retire_epoch + 1) % (SIZE_MAX / (b->capacity + 1));
}
/* The common four-record shape already has direct immutable references.
 * Avoid walking empty phase lists and copying those same references into a
 * second plan. Generic/concurrent groups still use the materialized plan. */
static size_t record_index_at(const PeakMpiCausalBatches *b, size_t index) {
  if (!b->compact_plan)
    return b->plan[index];
  const BatchGroup *g = &b->groups[b->current];
  const BatchOperation *o = &b->operations[g->operation_head];
  switch (index) {
  case 0:
    return g->begin;
  case 1:
    return o->begin;
  case 2:
    return o->end;
  default:
    return g->end;
  }
}
int peak_mpi_causal_batches_prepare_next_work(PeakMpiCausalBatches *b,
                                              size_t budget,
                                              PeakMpiCausalBatchView *v,
                                              size_t *work_used) {
  size_t used = 0;
  if (work_used)
    *work_used = 0;
  if (!b)
    return -1;
  if (b->state == READY || b->state == ERROR) {
    view(b, v);
    return b->state == READY ? 1 : -1;
  }
  while (used < budget) {
    used++;
    if (b->state == IDLE) {
      if (b->ready_head == NONE)
        break;
      b->current = b->ready_head;
      BatchGroup *g = &b->groups[b->current];
      b->ready_head = g->ready_next;
      if (b->ready_head == NONE)
        b->ready_tail = NONE;
      g->queued = 0;
      g->exposed = 1;
      b->compact_plan = g->begins == 1 && !g->activations && !g->subrecords;
      if (b->compact_plan) {
        b->plan_count = 4;
        b->state = READY;
        view(b, v);
        if (work_used)
          *work_used = used;
        return 1;
      }
      b->plan_count = 1;
      b->plan[0] = g->begin;
      b->cursor = g->begin_head;
      b->state = BUILD_BEGIN;
    } else if (b->state == BUILD_BEGIN) {
      if (b->cursor == NONE) {
        b->parent_cursor = b->groups[b->current].begin_head;
        b->cursor = NONE;
        b->state = BUILD_ACTIVATIONS;
      } else {
        size_t at = b->cursor;
        b->plan[b->plan_count++] = at;
        b->cursor = b->entries[at].next;
      }
    } else if (b->state == BUILD_ACTIVATIONS || b->state == BUILD_SUBRECORDS) {
      if (b->cursor != NONE) {
        size_t at = b->cursor;
        b->plan[b->plan_count++] = at;
        b->cursor = b->entries[at].next;
      } else if (b->parent_cursor != NONE) {
        BatchEntry *parent = &b->entries[b->parent_cursor];
        BatchOperation *o = &b->operations[parent->op];
        b->cursor =
            b->state == BUILD_ACTIVATIONS ? o->activation_head : o->sub_head;
        b->parent_cursor = parent->next;
      } else if (b->state == BUILD_ACTIVATIONS) {
        b->parent_cursor = b->groups[b->current].begin_head;
        b->state = BUILD_SUBRECORDS;
      } else {
        b->cursor = b->groups[b->current].begin_head;
        b->state = BUILD_END;
      }
    } else if (b->state == BUILD_END) {
      if (b->cursor == NONE)
        b->state = BUILD_MARKER;
      else {
        BatchEntry *parent = &b->entries[b->cursor];
        b->plan[b->plan_count++] = b->operations[parent->op].end;
        b->cursor = parent->next;
      }
    } else if (b->state == BUILD_MARKER) {
      b->plan[b->plan_count++] = b->groups[b->current].end;
      b->state = READY;
      view(b, v);
      if (work_used)
        *work_used = used;
      return 1;
    } else if (b->state == RETIRE_RECORDS) {
      if (b->retire == b->plan_count)
        b->state = RETIRE_OPERATIONS;
      else {
        size_t index = b->retire++;
        if (!b->groups[b->current].implicit || (index && index + 1 < b->plan_count))
          free_record(b, record_index_at(b, index));
      }
    } else if (b->state == RETIRE_OPERATIONS) {
      if (b->retire_operation == NONE)
        b->state = RETIRE_GROUP;
      else {
        size_t at = b->retire_operation;
        b->retire_operation = b->operations[at].next_group;
        operation_remove(b, at);
      }
    } else if (b->state == RETIRE_GROUP) {
      BatchGroup *g = &b->groups[b->current];
      unsigned rank = g->rank;
      uint64_t uid = g->uid;
      size_t next = g->successor;
      b->last[rank] = uid;
      b->rank_next[rank] = next;
      group_remove(b, b->current);
      b->current = NONE;
      b->state = IDLE;
      if (next != NONE)
        maybe_ready(b, next);
    }
    if (b->state == ERROR) {
      view(b, v);
      if (work_used)
        *work_used = used;
      return -1;
    }
  }
  if (work_used)
    *work_used = used;
  return 0;
}
int peak_mpi_causal_batches_prepare_next(PeakMpiCausalBatches *b, size_t budget,
                                         PeakMpiCausalBatchView *v) {
  return peak_mpi_causal_batches_prepare_next_work(b, budget, v, NULL);
}
const PeakMpiObserverRecord *
peak_mpi_causal_batches_record_at(const PeakMpiCausalBatches *b, size_t index) {
  if (!b || b->state != READY || index >= b->plan_count) return NULL;
  if (b->groups[b->current].implicit) {
    if (!index) return &b->implicit_begin;
    if (index + 1 == b->plan_count) return &b->implicit_end;
  }
  return &b->entries[record_index_at(b, index)].r;
}
const PeakMpiObserverRecord *
peak_mpi_causal_batches_call_end(const PeakMpiCausalBatches *b, unsigned rank,
                                 uint64_t uid) {
  if (!b || b->state != READY || rank >= b->world)
    return NULL;
  size_t h = hash(rank, uid);
  for (size_t i = 0; i < PROBES; i++) {
    size_t v = b->operation_index[(h + i) & (b->operation_index_capacity - 1)];
    if (!v)
      return NULL;
    if (v != TOMBSTONE) {
      const BatchOperation *o = &b->operations[v - 1];
      if (o->rank == rank && o->uid == uid)
        return o->group == b->current && o->end != NONE ? &b->entries[o->end].r
                                                        : NULL;
    }
  }
  return NULL;
}
void peak_mpi_causal_batches_commit(PeakMpiCausalBatches *b) {
  if (b && b->state == READY) {
    b->state = RETIRE_RECORDS;
    b->retire = 0;
    b->retire_operation = b->groups[b->current].operation_head;
  }
}
size_t peak_mpi_causal_batches_pending(const PeakMpiCausalBatches *b) {
  return b ? b->pending : 0;
}
uint64_t peak_mpi_causal_batches_last_group(const PeakMpiCausalBatches *b,
                                            unsigned rank) {
  return b && rank < b->world ? b->last[rank] : 0;
}
int peak_mpi_causal_batches_prefix_clear(const PeakMpiCausalBatches *b,
                                         unsigned rank,
                                         const uint64_t *prefixes, size_t slots,
                                         size_t *cursor, size_t *budget) {
  if (!b || !prefixes || !cursor || !budget || rank >= b->world)
    return 0;
  if (b->slots) {
    if (slots != b->slots)
      return 0;
    while (*cursor < slots && *budget) {
      size_t slot = (*cursor)++;
      size_t head = b->slot_head[(size_t)rank * slots + slot];
      (*budget)--;
      if (head != NONE && b->entries[head].r.sequence <= prefixes[slot]) {
        *cursor = 0;
        return 0;
      }
    }
    if (*cursor == slots) {
      *cursor = 0;
      return 1;
    }
    return -1;
  }
  /* Compatibility for independent callers without a configured slot count.
   * Production uses the per-slot oldest-pending index above. */
  size_t base = b->capacity + 1;
  size_t pos = *cursor / base == b->retire_epoch ? *cursor % base : 0;
  while (pos < b->pending && *budget) {
    const BatchEntry *e = &b->entries[b->active[pos++]];
    (*budget)--;
    if (e->rank == rank && (!e->r.thread_uid || e->r.thread_uid > slots ||
                            e->r.sequence <= prefixes[e->r.thread_uid - 1])) {
      *cursor = 0;
      return 0;
    }
  }
  if (pos == b->pending) {
    *cursor = 0;
    return 1;
  }
  *cursor = b->retire_epoch * base + pos;
  return -1;
}

int peak_mpi_causal_batches_prefix_clear_slots(
    const PeakMpiCausalBatches *b, unsigned rank, const uint64_t *prefixes,
    const size_t *slots, size_t count, size_t *cursor, size_t *budget) {
  if (!b || !b->slots || !prefixes || !slots || !cursor || !budget ||
      rank >= b->world || count > b->slots)
    return 0;
  while (*cursor < count && *budget) {
    size_t slot = slots[(*cursor)++];
    (*budget)--;
    if (slot >= b->slots)
      return 0;
    size_t head = b->slot_head[(size_t)rank * b->slots + slot];
    if (head != NONE && b->entries[head].r.sequence <= prefixes[slot]) {
      *cursor = 0;
      return 0;
    }
  }
  if (*cursor == count) {
    *cursor = 0;
    return 1;
  }
  return -1;
}

void peak_mpi_causal_batches_reset_begin(PeakMpiCausalBatches *b) {
  if (b) {
    b->reset_stage = 1;
    b->reset_cursor = 0;
  }
}
int peak_mpi_causal_batches_reset_work(PeakMpiCausalBatches *b, size_t budget,
                                       size_t *used) {
  *used = 0;
  while (b->reset_stage && *used < budget) {
    ++*used;
    size_t i = b->reset_cursor++;
    if (b->reset_stage == 1) {
      if (i < b->capacity) {
        b->entries[i].free_next = b->operations[i].free_next =
            i + 1 < b->capacity ? i + 1 : NONE;
      } else {
        b->reset_stage++;
        b->reset_cursor = 0;
      }
    } else if (b->reset_stage == 2) {
      if (i < b->group_capacity)
        b->groups[i].free_next = i + 1 < b->group_capacity ? i + 1 : NONE;
      else {
        b->reset_stage++;
        b->reset_cursor = 0;
      }
    } else if (b->reset_stage == 3) {
      if (i < b->operation_index_capacity)
        b->operation_index[i] = 0;
      else {
        b->reset_stage++;
        b->reset_cursor = 0;
      }
    } else if (b->reset_stage == 4) {
      if (i < b->group_index_capacity)
        b->group_index[i] = 0;
      else {
        b->reset_stage++;
        b->reset_cursor = 0;
      }
    } else if (b->reset_stage == 5) {
      if (i < b->world * b->slots) {
        b->slot_head[i] = b->slot_tail[i] = NONE;
      } else {
        b->reset_stage++;
        b->reset_cursor = 0;
      }
    } else if (i < b->world) {
      b->last[i] = 0;
      b->rank_next[i] = NONE;
    } else {
      b->free_entry = b->free_operation = b->free_group = 0;
      b->pending = 0;
      b->ready_head = b->ready_tail = b->current = NONE;
      b->plan_count = 0;
      b->state = IDLE;
      b->compact_plan = 0;
      b->reset_stage = 0;
    }
  }
  return !b->reset_stage;
}
