#include "internal/mpi_causal_batches.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  PeakMpiObserverRecord r;
  uint64_t attempted;
  size_t next_free, position;
  unsigned rank;
  int used;
} Entry;
enum { FIND, GATHER, VALIDATE, READY, RETIRE, ERROR };
struct PeakMpiCausalBatches {
  size_t world, capacity, pending, free_head;
  Entry *entries;
  uint64_t *last, version, attempt_version;
  size_t *begins, *ends, *activations, *subrecords, *active;
  size_t scan_limit, retire_epoch;
  size_t nb, ne, na, ns, begin, end, native, scan, find_cursor, retire;
  size_t pair_count, duplicate_count, end_match;
  unsigned rank;
  uint64_t uid;
  int state;
};
#define NONE SIZE_MAX
PeakMpiCausalBatches *peak_mpi_causal_batches_create(size_t world, size_t cap) {
  PeakMpiCausalBatches *b;
  size_t i;
  if (!world || !cap || world > UINT32_MAX || cap > SIZE_MAX / sizeof(Entry) ||
      cap > SIZE_MAX / sizeof(size_t))
    return NULL;
  b = calloc(1, sizeof(*b));
  if (!b)
    return NULL;
  b->world = world;
  b->capacity = cap;
  b->entries = calloc(cap, sizeof(Entry));
  b->last = calloc(world, sizeof(uint64_t));
  b->begins = calloc(cap, sizeof(size_t));
  b->ends = calloc(cap, sizeof(size_t));
  b->activations = calloc(cap, sizeof(size_t));
  b->subrecords = calloc(cap, sizeof(size_t));
  b->active = calloc(cap, sizeof(size_t));
  if (!b->entries || !b->last || !b->begins || !b->ends || !b->activations ||
      !b->subrecords || !b->active) {
    peak_mpi_causal_batches_destroy(b);
    return NULL;
  }
  for (i = 0; i < cap; i++) {
    b->entries[i].next_free = i + 1;
    b->entries[i].attempted = UINT64_MAX;
  }
  b->free_head = 0;
  b->version = 1;
  return b;
}
void peak_mpi_causal_batches_destroy(PeakMpiCausalBatches *b) {
  if (!b)
    return;
  free(b->entries);
  free(b->last);
  free(b->begins);
  free(b->ends);
  free(b->activations);
  free(b->subrecords);
  free(b->active);
  free(b);
}
int peak_mpi_causal_batches_ingest(PeakMpiCausalBatches *b, unsigned rank,
                                   const PeakMpiObserverRecord *r) {
  size_t i;
  if (!b || !r || rank >= b->world || b->free_head >= b->capacity)
    return 0;
  i = b->free_head;
  b->free_head = b->entries[i].next_free;
  b->entries[i].r = *r;
  b->entries[i].rank = rank;
  b->entries[i].used = 1;
  b->entries[i].attempted = UINT64_MAX;
  b->entries[i].position = b->pending;
  b->active[b->pending++] = i;
  b->version++;
  return 1;
}
static void view(const PeakMpiCausalBatches *b, PeakMpiCausalBatchView *v) {
  if (!v)
    return;
  memset(v, 0, sizeof(*v));
  v->rank = b->rank;
  v->group_uid = b->uid;
  v->native_count = b->nb;
  v->record_count = 2 + b->nb + b->ne + b->na + b->ns;
  v->concurrent = b->nb > 1;
  v->begin = &b->entries[b->begin].r;
  v->end = b->end == NONE ? NULL : &b->entries[b->end].r;
}
static void incomplete(PeakMpiCausalBatches *b) {
  b->entries[b->begin].attempted = b->attempt_version;
  b->state = FIND;
  b->scan = 0;
  b->scan_limit = b->pending;
}
int peak_mpi_causal_batches_prepare_next(PeakMpiCausalBatches *b, size_t budget,
                                         PeakMpiCausalBatchView *v) {
  if (!b)
    return -1;
  if (b->state == READY) {
    view(b, v);
    return 1;
  }
  if (b->state == ERROR) {
    view(b, v);
    return -1;
  }
  while (budget) {
    Entry *e;
    const PeakMpiObserverRecord *r;
    size_t i;
    if (b->state == FIND) {
      if (!b->scan)
        b->scan_limit = b->pending;
      if (b->scan == b->scan_limit) {
        budget--;
        b->scan = 0;
        return 0;
      }
      if (b->find_cursor >= b->scan_limit)
        b->find_cursor = 0;
      i = b->active[b->find_cursor];
      b->find_cursor = (b->find_cursor + 1) % b->scan_limit;
      b->scan++;
      budget--;
      e = &b->entries[i];
      r = &e->r;
      if (!e->used || r->phase != PEAK_MPI_CAUSAL_GROUP_BEGIN ||
          r->input_ref != b->last[e->rank] || e->attempted == b->version)
        continue;
      b->rank = e->rank;
      b->uid = r->op_uid;
      b->begin = i;
      b->end = NONE;
      b->nb = b->ne = b->na = b->ns = 0;
      b->native = 0;
      b->scan = 0;
      b->attempt_version = b->version;
      b->scan_limit = b->pending;
      b->state = GATHER;
      continue;
    }
    if (b->state == GATHER) {
      if (b->scan == b->scan_limit) {
        budget--;
        if (b->end == NONE || !b->nb ||
            b->entries[b->end].r.completion_count != b->nb) {
          incomplete(b);
          continue;
        }
        b->state = VALIDATE;
        b->scan = 0;
        b->pair_count = b->duplicate_count = 0;
        b->end_match = NONE;
        continue;
      }
      i = b->active[b->scan++];
      budget--;
      e = &b->entries[i];
      r = &e->r;
      if (!e->used || e->rank != b->rank)
        continue;
      if (r->phase == PEAK_MPI_CAUSAL_GROUP_END && r->op_uid == b->uid) {
        if (b->end != NONE) {
          b->state = ERROR;
          view(b, v);
          return -1;
        }
        b->end = i;
      }
      if (r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN && r->op_uid == b->uid &&
          i != b->begin) {
        b->state = ERROR;
        view(b, v);
        return -1;
      }
      if (r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN && r->batch_uid == b->uid)
        b->begins[b->nb++] = i;
      continue;
    }
    if (b->state == VALIDATE) {
      const PeakMpiObserverRecord *start = &b->entries[b->begins[b->native]].r;
      if (b->scan == b->scan_limit) {
        budget--;
        if (b->pair_count > 1) {
          b->state = ERROR;
          view(b, v);
          return -1;
        }
        if (b->pair_count != 1) {
          incomplete(b);
          continue;
        }
        if (b->duplicate_count != 1) {
          b->state = ERROR;
          view(b, v);
          return -1;
        }
        b->ends[b->ne++] = b->end_match;
        b->native++;
        b->scan = 0;
        b->pair_count = b->duplicate_count = 0;
        b->end_match = NONE;
        if (b->native == b->nb) {
          b->state = READY;
          view(b, v);
          return 1;
        }
        continue;
      }
      i = b->active[b->scan++];
      budget--;
      e = &b->entries[i];
      r = &e->r;
      if (!e->used || e->rank != b->rank)
        continue;
      if (r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN && r->op_uid == start->op_uid)
        b->duplicate_count++;
      if (r->phase == PEAK_MPI_CAUSAL_CALL_END && r->op_uid == start->op_uid &&
          r->batch_uid == b->uid) {
        b->pair_count++;
        b->end_match = i;
      }
      if (r->batch_uid == start->op_uid) {
        if (r->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE)
          b->activations[b->na++] = i;
        else if (r->phase == PEAK_MPI_CAUSAL_REQUEST_COMPLETE ||
                 r->phase == PEAK_MPI_CAUSAL_REQUEST_RETIRED ||
                 r->phase == PEAK_MPI_CAUSAL_MESSAGE_BOUND)
          b->subrecords[b->ns++] = i;
      }
      continue;
    }
    if (b->state == RETIRE) {
      size_t n = 2 + b->nb + b->ne + b->na + b->ns;
      if (b->retire == n) {
        budget--;
        b->last[b->rank] = b->uid;
        b->state = FIND;
        b->scan = 0;
        b->find_cursor = 0;
        b->scan_limit = b->pending;
        continue;
      }
      i = b->retire++;
      if (!i)
        i = b->begin;
      else if (--i < b->nb)
        i = b->begins[i];
      else if ((i -= b->nb) < b->na)
        i = b->activations[i];
      else if ((i -= b->na) < b->ns)
        i = b->subrecords[i];
      else if ((i -= b->ns) < b->ne)
        i = b->ends[i];
      else
        i = b->end;
      budget--;
      e = &b->entries[i];
      e->used = 0;
      e->next_free = b->free_head;
      b->free_head = i;
      b->pending--;
      b->active[e->position] = b->active[b->pending];
      b->entries[b->active[e->position]].position = e->position;
      b->retire_epoch = (b->retire_epoch + 1) % (SIZE_MAX / (b->capacity + 1));
      b->version++;
      continue;
    }
  }
  return 0;
}
const PeakMpiObserverRecord *
peak_mpi_causal_batches_record_at(const PeakMpiCausalBatches *b, size_t i) {
  size_t slot;
  if (!b || b->state != READY)
    return NULL;
  if (!i)
    slot = b->begin;
  else if (--i < b->nb)
    slot = b->begins[i];
  else if ((i -= b->nb) < b->na)
    slot = b->activations[i];
  else if ((i -= b->na) < b->ns)
    slot = b->subrecords[i];
  else if ((i -= b->ns) < b->ne)
    slot = b->ends[i];
  else if (i == b->ne)
    slot = b->end;
  else
    return NULL;
  return &b->entries[slot].r;
}
void peak_mpi_causal_batches_commit(PeakMpiCausalBatches *b) {
  if (b && b->state == READY) {
    b->state = RETIRE;
    b->retire = 0;
  }
}
size_t peak_mpi_causal_batches_pending(const PeakMpiCausalBatches *b) {
  return b ? b->pending : 0;
}
int peak_mpi_causal_batches_prefix_clear(const PeakMpiCausalBatches *b,
                                         unsigned rank,
                                         const uint64_t *prefixes, size_t slots,
                                         size_t *cursor, size_t *budget) {
  if (!b || !prefixes || !cursor || !budget || rank >= b->world)
    return 0;
  /* Cursor carries the retirement epoch: swap-removal invalidates an older
   * partial scan, while append-only ingestion cannot hide an old prefix. */
  size_t base = b->capacity + 1, pos;
  pos = *cursor / base == b->retire_epoch ? *cursor % base : 0;
  while (pos < b->pending && *budget) {
    const Entry *e = &b->entries[b->active[pos++]];
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

uint64_t peak_mpi_causal_batches_last_group(const PeakMpiCausalBatches *b,
                                            unsigned rank) {
  return b && rank < b->world ? b->last[rank] : 0;
}
