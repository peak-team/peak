#define _GNU_SOURCE
#include "internal/mpi_causal_events.h"
#include <dlfcn.h>
#include <mpi.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))
#define COMM_CAP 256u
#define TYPE_CAP 512u
#define COMPLETE_CAP 1024u
#define MESSAGE_CAP 1024u

typedef struct {
  MPI_Comm handle;
  uint64_t uid, generation, ordinal, creation_ordinal;
  unsigned recovery_operation;
  int size, rank, inter, notified, retired;
  unsigned references, active_collectives;
  int32_t *members;
} Comm;
typedef struct {
  MPI_Request handle;
  uint64_t graph_epoch;
  uint64_t uid, generation, activation, start_uid, comm_uid, ordinal,
      lineage_ordinal;
  unsigned flags;
  PeakMpiCausalOperation operation;
  int peer, tag, root, persistent, active, observed, cancelled, occupied;
  unsigned comm_slot;
  MPI_Comm idup_comm;
} Request;
typedef struct {
  MPI_Message handle;
  uint64_t uid, comm_uid, probe_uid;
  int peer, tag, occupied;
} Message;
typedef struct {
  MPI_Datatype handle;
  int size, occupied, supported_family;
} Type;
typedef struct {
  uint64_t generation, previous, capture_epoch;
  PeakMpiObserverRecord begin;
  Request *requests;
  MPI_Status *statuses;
  unsigned count;
  int observing, emitted;
  uint64_t detach_open_snapshot;
} Context;

static pthread_mutex_t metadata = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int ready;
static _Thread_local unsigned depth;
static _Thread_local int internal_previous, internal_active;
static Context *contexts;
static size_t context_count, request_capacity, completion_capacity;
static Request *completion_requests;
static MPI_Status *completion_statuses;
static Request *requests;
static Comm comms[COMM_CAP];
static Type types[TYPE_CAP];
static Message messages[MESSAGE_CAP];
static int32_t *comm_members;
static int world_rank, world_size;
#define RECOVERY_COMM_CAP 4096u
typedef struct {
  uint64_t ordinal, ticket;
} RecoveryWitness;
static RecoveryWitness recovery_witnesses[RECOVERY_COMM_CAP];
static _Thread_local Context *current_call;
static void submit_event(PeakMpiObserverRecord *r, int actual) {
  if (current_call && current_call->observing &&
      (!current_call->capture_epoch ||
       current_call->capture_epoch != peak_mpi_causal_runtime_capture_epoch()))
    return;
  peak_mpi_causal_runtime_submit(r, actual);
}
static uint64_t group_capture_epoch = 1;
static uint64_t object_uid = 2, request_generation, buffer_generation;
static uint64_t group_uid, previous_group;
static uint64_t observed_open_requests, observed_open_buffered;
static unsigned group_active, group_published;
static int group_emitted;
static PeakMpiObserverRecord group_begin;
static PeakMpiProgressContract contract = {
    .profile = PEAK_MPI_PROGRESS_UNKNOWN,
    .name = "unknown",
    .reason = "MPI capability initialization incomplete",
    .effective_async_progress = -1,
    .provided_thread_level = -1};
static int single_flow;
static unsigned ordered_caller;

static uint64_t monotonic_ns(void) {
  struct timespec t;
  return clock_gettime(CLOCK_MONOTONIC, &t) == 0
             ? (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec
             : 0;
}
/* One-time object metadata queries only; normal wrappers use fixed prices. */
static double cold_cpu_seconds(void) {
  struct timespec value;
  return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0
             ? (double)value.tv_sec + (double)value.tv_nsec * 1e-9
             : 0.;
}
static void charge_cold_since(double begin) {
  double end = cold_cpu_seconds();
  if (begin > 0. && end >= begin)
    peak_mpi_causal_runtime_observer_cost(end - begin);
}
static int wrapper_enter(void) {
  ++depth;
  int observe = depth == 1 &&
                atomic_load_explicit(&ready, memory_order_acquire) &&
                peak_mpi_causal_runtime_enabled();
  if (observe) {
    internal_previous = peak_mpi_causal_runtime_internal_enter();
    internal_active = 1;
    peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, 0);
  }
  return observe;
}
static void wrapper_leave(void) {
  if (depth == 1 && internal_active) {
    peak_mpi_causal_runtime_internal_leave(internal_previous);
    internal_active = 0;
  }
  --depth;
}
#define NATIVE(result, call)                                                   \
  do {                                                                         \
    if (depth == 1 && internal_active)                                         \
      peak_mpi_causal_runtime_internal_leave(internal_previous);               \
    result = (call);                                                           \
    if (depth == 1 && internal_active)                                         \
      internal_previous = peak_mpi_causal_runtime_internal_enter();            \
    if (depth == 1 && internal_active && result != MPI_SUCCESS)                \
      peak_mpi_causal_runtime_fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);       \
  } while (0)
static void fail(PeakMpiObserverReason reason) {
  peak_mpi_causal_runtime_fail(reason);
}

static Context *context(void) {
  size_t slot;
  uint64_t generation;
  if (!peak_mpi_causal_runtime_thread_token(&slot, &generation) ||
      slot >= context_count) {
    fail(PEAK_MPI_OBSERVER_REASON_METADATA);
    return NULL;
  }
  Context *c = &contexts[slot];
  if (c->generation != generation) {
    /* Registry handoff is the authority for old-writer quiescence. */
    Request *saved_requests = c->requests;
    MPI_Status *saved_statuses = c->statuses;
    memset(c, 0, sizeof(*c));
    c->requests = saved_requests;
    c->statuses = saved_statuses;
    c->generation = generation;
  }
  return c;
}
static int comm_index(MPI_Comm handle) {
  unsigned inspected = 0;
  for (unsigned i = 0; i < COMM_CAP; ++i) {
    ++inspected;
    if (comms[i].uid && !comms[i].retired && comms[i].handle == handle) {
      peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP,
                                            inspected);
      return (int)i;
    }
  }
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, inspected);
  return -1;
}
static int world_peer(const Comm *c, int peer) {
  if (peer == MPI_PROC_NULL)
    return PEAK_MPI_CAUSAL_PROC_NULL;
  if (peer == MPI_ANY_SOURCE)
    return PEAK_MPI_CAUSAL_ANY_SOURCE;
  return c && !c->inter && peer >= 0 && peer < c->size
             ? c->members[peer]
             : PEAK_MPI_CAUSAL_UNKNOWN_PEER;
}
static int normalized_tag(int tag) {
  return tag == MPI_ANY_TAG ? PEAK_MPI_CAUSAL_ANY_TAG : tag;
}
static int type_nonzero(MPI_Datatype type) {
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return 0;
  int yes = 0;
  unsigned inspected = 0;
  pthread_mutex_lock(&metadata);
  for (unsigned i = 0; i < TYPE_CAP; ++i) {
    ++inspected;
    if (types[i].occupied && types[i].handle == type) {
      yes = types[i].size > 0;
      break;
    }
  }
  pthread_mutex_unlock(&metadata);
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, inspected);
  return yes;
}
/* Cold, public datatype-object inspection; no query occurs at an MPI boundary.
 */
static int datatype_family(MPI_Datatype type, unsigned depth, int *named) {
  int combiner = 0;
  MPI_Datatype base = MPI_DATATYPE_NULL;
  int integers[1] = {0};
  MPI_Aint addresses[1] = {0};
#if MPI_VERSION >= 4
  MPI_Count ni = 0, na = 0, nl = 0, nd = 0, large[1] = {0};
  if (PMPI_Type_get_envelope_c(type, &ni, &na, &nl, &nd, &combiner) !=
      MPI_SUCCESS)
    return 0;
  *named = combiner == MPI_COMBINER_NAMED;
  if (*named)
    return 1;
  if (depth > 16)
    return 0;
  if (combiner != MPI_COMBINER_CONTIGUOUS || nd != 1 || na ||
      !((ni == 1 && nl == 0) || (ni == 0 && nl == 1)))
    return 0;
  if (PMPI_Type_get_contents_c(type, ni, 0, nl, 1, integers, addresses, large,
                               &base) != MPI_SUCCESS)
    return 0;
#else
  int ni = 0, na = 0, nd = 0;
  if (PMPI_Type_get_envelope(type, &ni, &na, &nd, &combiner) != MPI_SUCCESS)
    return 0;
  *named = combiner == MPI_COMBINER_NAMED;
  if (*named)
    return 1;
  if (depth > 16)
    return 0;
  if (combiner != MPI_COMBINER_CONTIGUOUS || ni != 1 || na || nd != 1)
    return 0;
  if (PMPI_Type_get_contents(type, 1, 0, 1, integers, addresses, &base) !=
      MPI_SUCCESS)
    return 0;
#endif
  int base_named = 0;
  int supported = datatype_family(base, depth + 1, &base_named);
  /* Public contents returns a reference for derived input types. */
  if (!base_named && base != MPI_DATATYPE_NULL)
    (void)PMPI_Type_free(&base);
  return supported;
}
static int validate_type(MPI_Datatype type) {
  if (contract.profile !=
          PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED ||
      !peak_mpi_causal_runtime_metadata_enabled())
    return 1;
  unsigned inspected = 0;
  int supported = 0;
  pthread_mutex_lock(&metadata);
  for (unsigned i = 0; i < TYPE_CAP; ++i) {
    ++inspected;
    if (types[i].occupied && types[i].handle == type) {
      supported = types[i].supported_family;
      break;
    }
  }
  pthread_mutex_unlock(&metadata);
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, inspected);
  if (!supported)
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  return supported;
}
static void remember_type(MPI_Datatype type) {
  if (type == MPI_DATATYPE_NULL)
    return;
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return;
  int size = -1;
  double cold_begin = cold_cpu_seconds();
  int result = PMPI_Type_size(type, &size);
  int named = 0,
      supported =
          contract.profile ==
                  PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED
              ? datatype_family(type, 0, &named)
              : 1;
  charge_cold_since(cold_begin);
  if (result != MPI_SUCCESS)
    return;
  pthread_mutex_lock(&metadata);
  unsigned empty = TYPE_CAP, inspected = 0;
  for (unsigned i = 0; i < TYPE_CAP; ++i) {
    ++inspected;
    if (types[i].occupied && types[i].handle == type) {
      types[i].size = size;
      types[i].supported_family = supported;
      empty = TYPE_CAP;
      break;
    }
    if (!types[i].occupied && empty == TYPE_CAP)
      empty = i;
  }
  if (empty != TYPE_CAP)
    types[empty] = (Type){.handle = type,
                          .size = size,
                          .occupied = 1,
                          .supported_family = supported};
  pthread_mutex_unlock(&metadata);
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, inspected);
  /* Missing size only disables optional slack; it loses no message identity. */
}

static int get_comm(MPI_Comm handle, Comm *copy, unsigned *slot, int notify) {
  int index, send_members = 0;
  pthread_mutex_lock(&metadata);
  index = comm_index(handle);
  if (index >= 0) {
    Comm *c = &comms[index];
    ++c->references;
    *copy = *c;
    *slot = (unsigned)index;
    if (notify && !c->notified && c->size > 1) {
      c->notified = 1;
      send_members = 1;
    }
  }
  pthread_mutex_unlock(&metadata);
  if (index < 0) {
    fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
    peak_mpi_causal_runtime_note_peer(-1);
    return 0;
  }
  if (copy->inter) {
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  if (send_members)
    peak_mpi_causal_runtime_note_members(copy->members, (size_t)copy->size);
  return 1;
}
static void put_comm(unsigned slot) {
  pthread_mutex_lock(&metadata);
  Comm *c = &comms[slot];
  if (c->references)
    --c->references;
  if (c->retired && !c->references)
    c->uid = 0;
  pthread_mutex_unlock(&metadata);
}
static void define_comm(MPI_Comm handle, uint64_t parent_uid, uint64_t ordinal,
                        unsigned kind, int color) {
  if (handle == MPI_COMM_NULL)
    return;
  int n = 0, rank = 0, inter = 0;
  double cold_begin = cold_cpu_seconds();
  int query_ok = !(PMPI_Comm_test_inter(handle, &inter) != MPI_SUCCESS ||
                   PMPI_Comm_size(handle, &n) != MPI_SUCCESS ||
                   PMPI_Comm_rank(handle, &rank) != MPI_SUCCESS || n < 1 ||
                   n > world_size);
  charge_cold_since(cold_begin);
  if (!query_ok) {
    fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
    return;
  }
  unsigned slot = COMM_CAP;
  uint64_t uid;
  pthread_mutex_lock(&metadata);
  if (comm_index(handle) >= 0) {
    pthread_mutex_unlock(&metadata);
    return;
  }
  for (unsigned i = 0; i < COMM_CAP; ++i)
    if (!comms[i].uid) {
      slot = i;
      break;
    }
  if (slot == COMM_CAP) {
    pthread_mutex_unlock(&metadata);
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    return;
  }
  uid = kind == PEAK_MPI_CAUSAL_COMM_WORLD  ? 1
        : kind == PEAK_MPI_CAUSAL_COMM_SELF ? 2
                                            : ++object_uid;
  Comm *c = &comms[slot];
  uint64_t generation = c->generation + 1;
  *c = (Comm){.handle = handle,
              .uid = uid,
              .generation = generation,
              .size = n,
              .rank = rank,
              .inter = inter,
              .references = 1,
              .members = comm_members + (size_t)slot * (size_t)world_size};
  pthread_mutex_unlock(&metadata);
  /* Only local metadata queries, once at object creation. No lock crosses MPI.
   */
  MPI_Group group = MPI_GROUP_NULL, world = MPI_GROUP_NULL;
  cold_begin = cold_cpu_seconds();
  int valid = !inter && PMPI_Comm_group(handle, &group) == MPI_SUCCESS &&
              PMPI_Comm_group(MPI_COMM_WORLD, &world) == MPI_SUCCESS;
  if (valid) {
    for (int offset = 0; valid && offset < n; offset += 16) {
      int local[16];
      int count = n - offset < 16 ? n - offset : 16;
      for (int i = 0; i < count; ++i)
        local[i] = offset + i;
      valid = PMPI_Group_translate_ranks(group, count, local, world,
                                         c->members + offset) == MPI_SUCCESS;
    }
    for (int i = 0; valid && i < n; ++i)
      if (c->members[i] == MPI_UNDEFINED)
        valid = 0;
  }
  if (group != MPI_GROUP_NULL)
    (void)PMPI_Group_free(&group);
  if (world != MPI_GROUP_NULL)
    (void)PMPI_Group_free(&world);
  charge_cold_since(cold_begin);
  if (!valid) {
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  }
  for (unsigned offset = 0; offset < (unsigned)n;
       offset += PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART) {
    PeakMpiCausalCommPart part;
    memset(&part, 0, sizeof(part));
    part.local_uid = uid;
    part.parent_uid = parent_uid;
    part.creation_ordinal = ordinal;
    part.creation_kind = kind;
    part.world_size = (uint32_t)world_size;
    part.color = color;
    part.local_rank = rank;
    part.local_size = n;
    part.member_offset = offset;
    part.member_count = (unsigned)n - offset;
    if (part.member_count > PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART)
      part.member_count = PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART;
    for (unsigned j = 0; j < part.member_count; ++j)
      part.members[j] = valid ? c->members[offset + j] : -1;
    PeakMpiObserverObject def;
    memset(&def, 0, sizeof(def));
    def.generation = generation;
    def.parent_uid = parent_uid;
    def.kind = PEAK_MPI_CAUSAL_OBJECT_COMM;
    def.flags = inter ? PEAK_MPI_CAUSAL_INTERCOMM : 0;
    def.part_index = offset / PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART;
    def.part_count = ((unsigned)n + PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART - 1) /
                     PEAK_MPI_CAUSAL_COMM_MEMBERS_PER_PART;
    _Static_assert(sizeof(part) <= sizeof(def.payload),
                   "communicator definition part too large");
    memcpy(def.payload, &part, sizeof(part));
    peak_mpi_causal_runtime_define(&def);
  }
  put_comm(slot);
}

static void publish_begin(Context *c) {
  if (!c || !c->capture_epoch ||
      c->capture_epoch != peak_mpi_causal_runtime_capture_epoch() ||
      !peak_mpi_causal_runtime_capture_enabled())
    return;
  if (!c || c->emitted)
    return;
  PeakMpiObserverRecord group;
  int emit_group = 0;
  pthread_mutex_lock(&metadata);
  if (c->capture_epoch != group_capture_epoch) {
    pthread_mutex_unlock(&metadata);
    return;
  }
  if (group_published == UINT32_MAX)
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
  else
    ++group_published;
  if (!group_emitted) {
    group_emitted = 1;
    group = group_begin;
    emit_group = 1;
  }
  pthread_mutex_unlock(&metadata);
  if (emit_group)
    submit_event(&group, 0);
  submit_event(&c->begin, 0);
  c->emitted = 1;
}
static Context *begin_call(PeakMpiCausalOperation op, const Comm *comm,
                           unsigned flags, int peer, int tag, int root,
                           int deferred) {
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return NULL;
  Context *c = context();
  if (!c)
    return NULL;
  uint64_t epoch = peak_mpi_causal_runtime_capture_enabled()
                       ? peak_mpi_causal_runtime_capture_epoch()
                       : 0;
  if (c->capture_epoch != epoch)
    c->previous = 0;
  c->capture_epoch = epoch;
  current_call = c;
  c->count = 0;
  c->emitted = 0;
  c->observing = 1;
  PeakMpiObserverRecord *r = &c->begin;
  memset(r, 0, sizeof(*r));
  r->kind = PEAK_MPI_OBSERVER_EVENT_START;
  r->operation = (uint32_t)op;
  r->phase = PEAK_MPI_CAUSAL_CALL_BEGIN;
  r->op_uid = 0; /* allocated together with its group under metadata below */
  r->input_ref = c->previous;
  r->communicator_uid = comm ? comm->uid : 0;
  r->peer = peer;
  r->tag = tag;
  r->root = root;
  r->flags = flags | ordered_caller;
  r->capability_uid = (uint64_t)contract.profile;
  r->progress_scope = PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
  if (op == PEAK_MPI_CAUSAL_BSEND)
    r->flags |= PEAK_MPI_CAUSAL_BUFFERED;
  pthread_mutex_lock(&metadata);
  uint64_t locked_epoch = peak_mpi_causal_runtime_capture_enabled()
                              ? peak_mpi_causal_runtime_capture_epoch()
                              : 0;
  if (c->capture_epoch != locked_epoch) {
    c->previous = 0;
    r->input_ref = 0;
  }
  c->capture_epoch = locked_epoch;
  r->op_uid = peak_mpi_causal_runtime_operation_id();
  if (op == PEAK_MPI_CAUSAL_BSEND || op == PEAK_MPI_CAUSAL_BUFFER_DETACH)
    r->progress_begin = buffer_generation;
  if (contract.observed_buffer_detach && op == PEAK_MPI_CAUSAL_BSEND)
    ++observed_open_buffered;
  c->detach_open_snapshot =
      op == PEAK_MPI_CAUSAL_BUFFER_DETACH ? observed_open_buffered : 0;
  /* Only bounded local atomic reads under this short metadata lock. This
   * orders cost-group cuts without claiming MPI order between threads. */
  peak_mpi_causal_runtime_boundary(r);
  r->flags |= PEAK_MPI_CAUSAL_CUT_PRECAPTURED;
  if (!c->capture_epoch) {
    pthread_mutex_unlock(&metadata);
    return c;
  }
  if (group_capture_epoch != c->capture_epoch) {
    group_capture_epoch = c->capture_epoch;
    group_active = group_emitted = group_published = 0;
    previous_group = 0;
  }
  if (!group_active) {
    group_uid = peak_mpi_causal_runtime_operation_id();
    group_emitted = 0;
    group_published = 0;
    group_begin = *r;
    group_begin.kind = PEAK_MPI_OBSERVER_EVENT_LOCAL_COST;
    group_begin.operation = 0;
    group_begin.phase = PEAK_MPI_CAUSAL_GROUP_BEGIN;
    group_begin.op_uid = group_uid;
    group_begin.input_ref = previous_group;
    group_begin.batch_uid = group_uid;
  }
  ++group_active;
  r->batch_uid = group_uid;
  pthread_mutex_unlock(&metadata);
  if (!deferred) {
    r->flags &= ~PEAK_MPI_CAUSAL_CUT_PRECAPTURED;
    publish_begin(c);
  }
  return c;
}
static void end_call(Context *c, int rc, PeakMpiObserverRecord *details,
                     int actual, unsigned completed) {
  if (!c)
    return;
  if (!c->capture_epoch ||
      c->capture_epoch != peak_mpi_causal_runtime_capture_epoch() ||
      !peak_mpi_causal_runtime_capture_enabled()) {
    if (rc == MPI_SUCCESS &&
        c->begin.operation == PEAK_MPI_CAUSAL_BUFFER_DETACH &&
        contract.observed_buffer_detach) {
      pthread_mutex_lock(&metadata);
      uint64_t closed = c->detach_open_snapshot;
      observed_open_buffered = observed_open_buffered >= closed
                                   ? observed_open_buffered - closed
                                   : 0;
      pthread_mutex_unlock(&metadata);
    }
    c->observing = 0;
    if (current_call == c)
      current_call = NULL;
    return;
  }
  if (!c->emitted)
    publish_begin(c);
  PeakMpiObserverRecord r = c->begin;
  r.kind = PEAK_MPI_OBSERVER_EVENT_COMPLETE;
  r.phase = PEAK_MPI_CAUSAL_CALL_END;
  r.flags &= ~PEAK_MPI_CAUSAL_CUT_PRECAPTURED;
  r.mpi_error = rc;
  r.completion_count = completed;
  if (rc == MPI_SUCCESS)
    r.flags |= PEAK_MPI_CAUSAL_SUCCESS;
  if (details) {
    r.request_uid = details->request_uid;
    r.request_generation = details->request_generation;
    r.activation = details->activation;
    r.collective_ordinal = details->collective_ordinal;
    r.actual_source = details->actual_source;
    r.actual_tag = details->actual_tag;
    r.flags |= details->flags;
    r.wait_ns = details->wait_ns;
  }
  if (rc == MPI_SUCCESS) {
    PeakMpiObserverProgressScope scope;
    r.flags |= peak_mpi_progress_contract_result_flags(
        &contract, (PeakMpiCausalOperation)r.operation, r.flags, 0, &scope);
    r.progress_scope = scope;
  }
  submit_event(&r, actual);
  c->previous = r.op_uid;
  PeakMpiObserverRecord end;
  int emit_end = 0;
  pthread_mutex_lock(&metadata);
  if (rc == MPI_SUCCESS && r.operation == PEAK_MPI_CAUSAL_BUFFER_DETACH &&
      contract.observed_buffer_detach) {
    uint64_t closed = c->detach_open_snapshot;
    observed_open_buffered =
        observed_open_buffered >= closed ? observed_open_buffered - closed : 0;
  }
  if (c->capture_epoch != group_capture_epoch) {
    pthread_mutex_unlock(&metadata);
    c->observing = 0;
    if (current_call == c)
      current_call = NULL;
    return;
  }
  if (group_active == 1) {
    previous_group = group_uid;
    if (group_emitted) {
      memset(&end, 0, sizeof(end));
      end.kind = PEAK_MPI_OBSERVER_EVENT_BATCH_END;
      end.phase = PEAK_MPI_CAUSAL_GROUP_END;
      end.op_uid = end.batch_uid = group_uid;
      end.completion_count = group_published;
      if (group_published > 1)
        end.flags |= PEAK_MPI_CAUSAL_CONCURRENT;
      peak_mpi_causal_runtime_boundary(&end);
      end.flags |= PEAK_MPI_CAUSAL_CUT_PRECAPTURED;
      emit_end = 1;
    }
  }
  if (group_active)
    --group_active;
  pthread_mutex_unlock(&metadata);
  if (emit_end)
    submit_event(&end, actual);
  c->observing = 0;
  if (current_call == c)
    current_call = NULL;
}
static void poll_empty(Context *c) {
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_POLL, 1);
  if (!c)
    return;
  if (!c->capture_epoch ||
      c->capture_epoch != peak_mpi_causal_runtime_capture_epoch() ||
      !peak_mpi_causal_runtime_capture_enabled()) {
    c->observing = 0;
    if (current_call == c)
      current_call = NULL;
    return;
  }
  PeakMpiObserverRecord end;
  int emit = 0;
  pthread_mutex_lock(&metadata);
  if (c->capture_epoch != group_capture_epoch) {
    pthread_mutex_unlock(&metadata);
    c->observing = 0;
    if (current_call == c)
      current_call = NULL;
    return;
  }
  if (group_active == 1) {
    if (group_emitted) {
      previous_group = group_uid;
      memset(&end, 0, sizeof(end));
      end.kind = PEAK_MPI_OBSERVER_EVENT_BATCH_END;
      end.phase = PEAK_MPI_CAUSAL_GROUP_END;
      end.op_uid = end.batch_uid = group_uid;
      end.completion_count = group_published;
      if (group_published > 1)
        end.flags |= PEAK_MPI_CAUSAL_CONCURRENT;
      peak_mpi_causal_runtime_boundary(&end);
      end.flags |= PEAK_MPI_CAUSAL_CUT_PRECAPTURED;
      emit = 1;
    }
  }
  if (group_active)
    --group_active;
  pthread_mutex_unlock(&metadata);
  if (emit)
    submit_event(&end, 0);
  c->observing = 0;
}

static void record_capability(PeakMpiCausalOperation op, unsigned flags,
                              int actual) {
  if (actual && !peak_mpi_progress_contract_supports(&contract, op, flags))
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
}

const PeakMpiProgressContract *peak_mpi_causal_events_contract(void) {
  return &contract;
}
/* Public MPI_T control inspection is startup-only and performs no MPI data
 * communication. It verifies configuration, never infers progress from timing.
 */
static int public_scalar(MPI_T_cvar_handle handle, MPI_Datatype datatype,
                         int count, int *out) {
  int size = 0;
  if (count != 1 || PMPI_Type_size(datatype, &size) != MPI_SUCCESS)
    return 0;
  if (datatype == MPI_INT && size == (int)sizeof(int))
    return MPI_T_cvar_read(handle, out) == MPI_SUCCESS;
#ifdef MPI_C_BOOL
  if (datatype == MPI_C_BOOL && size == (int)sizeof(bool)) {
    union {
      max_align_t alignment;
      unsigned char bytes[sizeof(bool)];
    } storage;
    memset(&storage, 0, sizeof(storage));
    if (MPI_T_cvar_read(handle, storage.bytes) != MPI_SUCCESS)
      return 0;
    bool yes = true, no = false, value;
    unsigned char positive[sizeof(bool)], negative[sizeof(bool)];
    memcpy(positive, &yes, sizeof(yes));
    memcpy(negative, &no, sizeof(no));
    if (memcmp(storage.bytes, positive, sizeof(yes)) &&
        memcmp(storage.bytes, negative, sizeof(no)))
      return 0;
    memcpy(&value, storage.bytes, sizeof(value));
    *out = value ? 1 : 0;
    return 1;
  }
#endif
  return 0;
}
static int public_string(MPI_T_cvar_handle handle, MPI_Datatype datatype,
                         int count, char *out, size_t capacity) {
  /* MPI_T count is storage capacity, not the current string length. Vista's
   * public PML/coll handles each report2048 bytes for short selected values. */
  char storage[4096];
  int size = 0;
  if (datatype != MPI_CHAR || count < 1 || (size_t)count > sizeof(storage) ||
      PMPI_Type_size(datatype, &size) != MPI_SUCCESS || size != 1)
    return 0;
  memset(storage, 0, sizeof(storage));
  if (MPI_T_cvar_read(handle, storage) != MPI_SUCCESS)
    return 0;
  const char *end = memchr(storage, 0, (size_t)count);
  if (!end || (size_t)(end - storage) >= capacity)
    return 0;
  memcpy(out, storage, (size_t)(end - storage) + 1);
  return 1;
}
/* Read a required public control by name. Unrelated inventory entries are
 * deliberately outside the capability predicate. 0 means absent; -1 means
 * the named control was found but its metadata/value could not be verified. */
static int public_control(const char *requested, char *text, size_t capacity,
                          int *value) {
  int index = -1;
  if (MPI_T_cvar_get_index(requested, &index) != MPI_SUCCESS)
    return 0;
  char name[192];
  int length = (int)sizeof(name), verbosity = 0, description_length = 0,
      binding = 0, scope = 0;
  MPI_Datatype datatype;
  MPI_T_enum enumeration;
  if (MPI_T_cvar_get_info(index, name, &length, &verbosity, &datatype,
                          &enumeration, NULL, &description_length, &binding,
                          &scope) != MPI_SUCCESS ||
      binding != MPI_T_BIND_NO_OBJECT)
    return -1;
  MPI_T_cvar_handle handle;
  int count = 0;
  if (MPI_T_cvar_handle_alloc(index, NULL, &handle, &count) != MPI_SUCCESS)
    return -1;
  int okay = value ? public_scalar(handle, datatype, count, value)
                   : public_string(handle, datatype, count, text, capacity);
  if (MPI_T_cvar_handle_free(&handle) != MPI_SUCCESS)
    okay = 0;
  return okay ? 1 : -1;
}
static void public_verification(PeakMpiProgressVerification *result) {
  const char *profile = getenv("PEAK_MPI_PROGRESS_PROFILE");
  int intel =
      profile && !strcmp(profile, "intel-2021.11-ofi-opx-host-async-v1");
  int ompi = profile &&
             !strcmp(profile, "ompi-5.0.5-ucx-1.17-rc-proto2-host-observed-v1");
  if (!intel && !ompi)
    return;
  int tools_provided = 0;
  if (MPI_T_init_thread(MPI_THREAD_SINGLE, &tools_provided) != MPI_SUCCESS)
    return;
  int valid = 1;
  if (ompi) {
    int dynamic = -1;
    result->pml_verified =
        public_control("pml", result->pml, sizeof(result->pml), NULL) == 1;
    result->coll_verified =
        public_control("coll", result->coll, sizeof(result->coll), NULL) == 1;
    result->fixed_rules_verified =
        public_control("coll_tuned_use_dynamic_rules", NULL, 0, &dynamic) ==
            1 &&
        dynamic == 0;
    valid = result->pml_verified && result->coll_verified &&
            result->fixed_rules_verified;
  } else {
    const char *names[] = {"MPIR_CVAR_ASYNC_PROGRESS", "I_MPI_ASYNC_PROGRESS"};
    int found = 0, async = -1;
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i) {
      int value = -1, status = public_control(names[i], NULL, 0, &value);
      if (status < 0) {
        valid = 0;
        break;
      }
      if (status == 1) {
        if (found && async != value) {
          valid = 0;
          break;
        }
        found = 1;
        async = value;
      }
    }
    valid = valid && found;
    result->effective_async_progress = valid ? async : -1;
  }
  if (MPI_T_finalize() != MPI_SUCCESS)
    valid = 0;
  if (!valid) {
    result->effective_async_progress = -1;
    result->pml_verified = result->coll_verified =
        result->fixed_rules_verified = 0;
  }
}
void peak_mpi_causal_events_init(void) {
  if (!peak_mpi_causal_runtime_enabled() ||
      atomic_load_explicit(&ready, memory_order_acquire))
    return;
  int init_previous = peak_mpi_causal_runtime_internal_enter();
  ++depth;
  if (PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
      PMPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS ||
      world_size < 1) {
    fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
    --depth;
    peak_mpi_causal_runtime_internal_leave(init_previous);
    return;
  }
  context_count = peak_mpi_causal_runtime_thread_capacity();
  request_capacity = 4096;
  completion_capacity = 64;
  const char *scratch = getenv("PEAK_MPI_COMPLETION_CAPACITY");
  if (scratch) {
    char *end;
    unsigned long n = strtoul(scratch, &end, 10);
    if (*scratch && !*end && n >= 1 && n <= COMPLETE_CAP)
      completion_capacity = n;
  }
  const char *v = getenv("PEAK_MPI_REQUEST_CAPACITY");
  if (v) {
    char *end;
    unsigned long n = strtoul(v, &end, 10);
    if (*v && !*end && n >= 16 && n <= 65536)
      request_capacity = n;
  }
  if (!context_count || context_count > SIZE_MAX / sizeof(*contexts) ||
      context_count >
          SIZE_MAX / completion_capacity / sizeof(*completion_requests) ||
      context_count >
          SIZE_MAX / completion_capacity / sizeof(*completion_statuses) ||
      (size_t)world_size > SIZE_MAX / COMM_CAP / sizeof(*comm_members)) {
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    --depth;
    peak_mpi_causal_runtime_internal_leave(init_previous);
    return;
  }
  contexts = calloc(context_count, sizeof(*contexts));
  requests = calloc(request_capacity, sizeof(*requests));
  completion_requests =
      calloc(context_count * completion_capacity, sizeof(*completion_requests));
  completion_statuses =
      calloc(context_count * completion_capacity, sizeof(*completion_statuses));
  comm_members = calloc(COMM_CAP * (size_t)world_size, sizeof(*comm_members));
  if (!contexts || !requests || !comm_members || !completion_requests ||
      !completion_statuses) {
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    --depth;
    peak_mpi_causal_runtime_internal_leave(init_previous);
    return;
  }
  for (size_t i = 0; i < context_count; ++i) {
    contexts[i].requests = completion_requests + i * completion_capacity;
    contexts[i].statuses = completion_statuses + i * completion_capacity;
  }
  char version[MPI_MAX_LIBRARY_VERSION_STRING];
  int length = 0;
  if (PMPI_Get_library_version(version, &length) != MPI_SUCCESS)
    version[0] = 0;
  else
    version[length < (int)sizeof(version) ? length : (int)sizeof(version) - 1] =
        0;
  int native_thread = -1;
  if (PMPI_Query_thread(&native_thread) != MPI_SUCCESS)
    native_thread = -1;
  ordered_caller = (native_thread == MPI_THREAD_SINGLE ||
                    native_thread == MPI_THREAD_FUNNELED)
                       ? PEAK_MPI_CAUSAL_ORDERED_CALLER
                       : 0;
  PeakMpiProgressVerification verification = {
      .effective_async_progress = -1, .provided_thread_level = native_thread};
  const char *requested = getenv("PEAK_MPI_PROGRESS_PROFILE");
  if (requested &&
      (!strcmp(requested, "intel-2021.11-ofi-opx-host-async-v1") ||
       !strcmp(requested, "ompi-5.0.5-ucx-1.17-rc-proto2-host-observed-v1")))
    public_verification(&verification);
  contract = peak_mpi_progress_contract_select_public(version, &verification);
  single_flow = getenv("PEAK_MPI_SINGLE_EXECUTION_FLOW") &&
                !strcmp(getenv("PEAK_MPI_SINGLE_EXECUTION_FLOW"), "1");
  define_comm(MPI_COMM_WORLD, 0, 0, PEAK_MPI_CAUSAL_COMM_WORLD, 0);
  define_comm(MPI_COMM_SELF, 0, 0, PEAK_MPI_CAUSAL_COMM_SELF, 0);
  MPI_Datatype builtins[] = {
      MPI_BYTE,
      MPI_CHAR,
      MPI_SIGNED_CHAR,
      MPI_UNSIGNED_CHAR,
      MPI_SHORT,
      MPI_UNSIGNED_SHORT,
      MPI_INT,
      MPI_UNSIGNED,
      MPI_LONG,
      MPI_UNSIGNED_LONG,
      MPI_LONG_LONG_INT,
#ifdef MPI_UNSIGNED_LONG_LONG
      MPI_UNSIGNED_LONG_LONG,
#endif
#ifdef MPI_INT8_T
      MPI_INT8_T,
#endif
#ifdef MPI_UINT8_T
      MPI_UINT8_T,
#endif
#ifdef MPI_INT16_T
      MPI_INT16_T,
#endif
#ifdef MPI_UINT16_T
      MPI_UINT16_T,
#endif
#ifdef MPI_INT32_T
      MPI_INT32_T,
#endif
#ifdef MPI_UINT32_T
      MPI_UINT32_T,
#endif
#ifdef MPI_INT64_T
      MPI_INT64_T,
#endif
#ifdef MPI_UINT64_T
      MPI_UINT64_T,
#endif
      MPI_FLOAT,
      MPI_DOUBLE,
      MPI_LONG_DOUBLE,
#ifdef MPI_PACKED
      MPI_PACKED,
#endif
#ifdef MPI_WCHAR
      MPI_WCHAR,
#endif
#ifdef MPI_C_BOOL
      MPI_C_BOOL,
#endif
#ifdef MPI_AINT
      MPI_AINT,
#endif
#ifdef MPI_COUNT
      MPI_COUNT,
#endif
#ifdef MPI_OFFSET
      MPI_OFFSET,
#endif
#ifdef MPI_2INT
      MPI_2INT,
#endif
#ifdef MPI_FLOAT_INT
      MPI_FLOAT_INT,
#endif
#ifdef MPI_DOUBLE_INT
      MPI_DOUBLE_INT,
#endif
#ifdef MPI_LONG_INT
      MPI_LONG_INT,
#endif
#ifdef MPI_SHORT_INT
      MPI_SHORT_INT,
#endif
#ifdef MPI_LONG_DOUBLE_INT
      MPI_LONG_DOUBLE_INT,
#endif
#ifdef MPI_C_FLOAT_COMPLEX
      MPI_C_FLOAT_COMPLEX,
#endif
#ifdef MPI_C_DOUBLE_COMPLEX
      MPI_C_DOUBLE_COMPLEX,
#endif
#ifdef MPI_C_LONG_DOUBLE_COMPLEX
      MPI_C_LONG_DOUBLE_COMPLEX,
#endif
#ifdef MPI_INTEGER
      MPI_INTEGER,
#endif
#ifdef MPI_REAL
      MPI_REAL,
#endif
#ifdef MPI_DOUBLE_PRECISION
      MPI_DOUBLE_PRECISION,
#endif
#ifdef MPI_COMPLEX
      MPI_COMPLEX,
#endif
#ifdef MPI_DOUBLE_COMPLEX
      MPI_DOUBLE_COMPLEX,
#endif
#ifdef MPI_LOGICAL
      MPI_LOGICAL,
#endif
#ifdef MPI_CHARACTER
      MPI_CHARACTER,
#endif
#ifdef MPI_INTEGER1
      MPI_INTEGER1,
#endif
#ifdef MPI_INTEGER2
      MPI_INTEGER2,
#endif
#ifdef MPI_INTEGER4
      MPI_INTEGER4,
#endif
#ifdef MPI_INTEGER8
      MPI_INTEGER8,
#endif
#ifdef MPI_REAL4
      MPI_REAL4,
#endif
#ifdef MPI_REAL8
      MPI_REAL8,
#endif
#ifdef MPI_REAL16
      MPI_REAL16,
#endif
#ifdef MPI_COMPLEX8
      MPI_COMPLEX8,
#endif
#ifdef MPI_COMPLEX16
      MPI_COMPLEX16,
#endif
#ifdef MPI_COMPLEX32
      MPI_COMPLEX32,
#endif
  };
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
    remember_type(builtins[i]);
  atomic_store_explicit(&ready, 1, memory_order_release);
  --depth;
  peak_mpi_causal_runtime_internal_leave(init_previous);
}
int peak_mpi_causal_events_idle_cut(PeakMpiCausalIdleCapture capture,
                                    void *data) {
  if (!capture || !atomic_load_explicit(&ready, memory_order_acquire))
    return 0;
  if (pthread_mutex_trylock(&metadata) != 0)
    return 0;
  int result = group_active ? 0 : capture(previous_group, data);
  pthread_mutex_unlock(&metadata);
  return result;
}
void peak_mpi_causal_events_finalize_begin(void) {
  if (!wrapper_enter()) {
    wrapper_leave();
    return;
  }
  Context *c = begin_call(PEAK_MPI_CAUSAL_FINALIZE, NULL, 0, -1, -1, -1, 0);
  end_call(c, MPI_SUCCESS, NULL, 0, 0);
  wrapper_leave();
  /* Pools stay allocated until process exit; native calls can still hold IDs.
   */
}

/* MPI and PMPI aliases forward to their original corresponding public entry.
 * One TLS guard suppresses nested semantic duplication across native aliases.
 */
#define DECLARE_REAL(name, params)                                             \
  typedef int(*Fn_##name) params;                                              \
  static Fn_##name real_MPI_##name, real_PMPI_##name;                          \
  static pthread_once_t once_MPI_##name = PTHREAD_ONCE_INIT,                   \
                        once_PMPI_##name = PTHREAD_ONCE_INIT;                  \
  static void resolve_MPI_##name(void) {                                       \
    int guard = peak_mpi_causal_runtime_internal_enter();                      \
    void *p = dlsym(RTLD_NEXT, "MPI_" #name);                                  \
    memcpy(&real_MPI_##name, &p, sizeof(p));                                   \
    peak_mpi_causal_runtime_internal_leave(guard);                             \
  }                                                                            \
  static void resolve_PMPI_##name(void) {                                      \
    int guard = peak_mpi_causal_runtime_internal_enter();                      \
    void *p = dlsym(RTLD_NEXT, "PMPI_" #name);                                 \
    memcpy(&real_PMPI_##name, &p, sizeof(p));                                  \
    peak_mpi_causal_runtime_internal_leave(guard);                             \
  }
#define FORWARD_BOTH(name, params, ...)                                        \
  EXPORT int MPI_##name params {                                               \
    pthread_once(&once_MPI_##name, resolve_MPI_##name);                        \
    return real_MPI_##name ? wrap_##name(real_MPI_##name, __VA_ARGS__)         \
                           : MPI_ERR_OTHER;                                    \
  }                                                                            \
  EXPORT int PMPI_##name params {                                              \
    pthread_once(&once_PMPI_##name, resolve_PMPI_##name);                      \
    return real_PMPI_##name ? wrap_##name(real_PMPI_##name, __VA_ARGS__)       \
                            : MPI_ERR_OTHER;                                   \
  }

static int request_index(MPI_Request handle) {
  if (handle == MPI_REQUEST_NULL)
    return -1;
  size_t inspected = 0;
  for (size_t i = 0; i < request_capacity; ++i) {
    ++inspected;
    if (requests[i].occupied && requests[i].handle == handle) {
      peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP,
                                            inspected);
      return (int)i;
    }
  }
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, inspected);
  return -1;
}
static int snapshot_request(MPI_Request handle, Request *out) {
  if (!peak_mpi_causal_runtime_metadata_enabled()) {
    memset(out, 0, sizeof(*out));
    if (handle != MPI_REQUEST_NULL)
      peak_mpi_causal_runtime_note_peer(-1);
    return 0;
  }
  memset(out, 0, sizeof(*out));
  if (handle == MPI_REQUEST_NULL)
    return 1;
  out->handle = handle;
  pthread_mutex_lock(&metadata);
  int i = request_index(handle);
  if (i >= 0)
    *out = requests[i];
  pthread_mutex_unlock(&metadata);
  if (i < 0)
    fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
  return i >= 0;
}
static void forget_request(const Request *old) {
  unsigned slot = COMM_CAP;
  pthread_mutex_lock(&metadata);
  int i = request_index(old->handle);
  if (i >= 0 && requests[i].generation == old->generation) {
    slot = requests[i].comm_slot;
    requests[i].occupied = 0;
  }
  pthread_mutex_unlock(&metadata);
  if (slot != COMM_CAP)
    put_comm(slot);
}
static void install_request(MPI_Request handle, const Comm *comm,
                            unsigned comm_slot, PeakMpiCausalOperation op,
                            unsigned flags, int peer, int tag, int root,
                            uint64_t ordinal, uint64_t start, int persistent,
                            PeakMpiObserverRecord *details) {
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return;
  if (handle == MPI_REQUEST_NULL) {
    details->flags |= PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
    return;
  }
  size_t empty = request_capacity;
  pthread_mutex_lock(&metadata);
  for (size_t i = 0; i < request_capacity; ++i)
    if (!requests[i].occupied) {
      empty = i;
      break;
    }
  peak_mpi_causal_runtime_observer_work(
      PEAK_MPI_CAUSAL_WORK_LOOKUP,
      empty < request_capacity ? empty + 1 : request_capacity);
  if (empty == request_capacity || request_generation == UINT64_MAX) {
    pthread_mutex_unlock(&metadata);
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    return;
  }
  Request *r = &requests[empty];
  memset(r, 0, sizeof(*r));
  r->handle = handle;
  r->uid = peak_mpi_causal_runtime_operation_id();
  r->generation = ++request_generation;
  r->activation = persistent ? 0 : 1;
  r->start_uid = start;
  r->graph_epoch =
      current_call && current_call->observing ? current_call->capture_epoch : 0;
  r->comm_uid = comm->uid;
  r->ordinal = ordinal;
  r->comm_slot = comm_slot;
  r->operation = op;
  r->flags = flags;
  if (op == PEAK_MPI_CAUSAL_BSEND)
    r->flags |= PEAK_MPI_CAUSAL_BUFFERED;
  r->peer = peer;
  r->tag = tag;
  r->root = root;
  r->persistent = persistent;
  r->active = !persistent;
  r->occupied = 1;
  if (contract.observed_p2p_completion && !persistent &&
      !(r->flags & PEAK_MPI_CAUSAL_BUFFERED))
    ++observed_open_requests;
  ++comms[comm_slot].references;
  details->request_uid = r->uid;
  details->request_generation = r->generation;
  details->activation = r->activation;
  pthread_mutex_unlock(&metadata);
}
static int actual_peer(int peer) { return peer >= 0 && peer != world_rank; }
static void note_peer(int peer) {
  if (actual_peer(peer))
    peak_mpi_causal_runtime_note_peer(peer);
  else if (peer == PEAK_MPI_CAUSAL_UNKNOWN_PEER)
    peak_mpi_causal_runtime_note_peer(-1);
}
static int receive_status(const Comm *comm, MPI_Status *status,
                          PeakMpiObserverRecord *r) {
  r->actual_source = world_peer(comm, status->MPI_SOURCE);
  r->actual_tag = normalized_tag(status->MPI_TAG);
  note_peer(r->actual_source);
  record_capability(PEAK_MPI_CAUSAL_RECV, 0, actual_peer(r->actual_source));
  return actual_peer(r->actual_source);
}
static unsigned complete_request(Context *c, const Request *old,
                                 MPI_Status *status, int error, int reclaim,
                                 unsigned index) {
  if (!old->occupied)
    return 0;
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_COMPLETION, 1);
  int observed = old->observed;
  pthread_mutex_lock(&metadata);
  int claim = request_index(old->handle);
  if (claim >= 0 && requests[claim].generation == old->generation) {
    observed = requests[claim].observed;
    if (old->active && !observed) {
      requests[claim].observed = 1;
      if (error == MPI_SUCCESS && contract.observed_p2p_completion &&
          !(old->flags & PEAK_MPI_CAUSAL_BUFFERED) && observed_open_requests)
        --observed_open_requests;
    }
  }
  pthread_mutex_unlock(&metadata);
  unsigned slot = old->comm_slot;
  int cancelled = 0;
  if (old->cancelled && status && error == MPI_SUCCESS)
    (void)PMPI_Test_cancelled(status, &cancelled);
  if (old->active && !observed) {
    PeakMpiObserverRecord r;
    memset(&r, 0, sizeof(r));
    r.kind = PEAK_MPI_OBSERVER_EVENT_COMPLETE;
    r.phase = PEAK_MPI_CAUSAL_REQUEST_COMPLETE;
    r.operation = old->operation;
    r.op_uid = old->start_uid;
    r.batch_uid = c ? c->begin.op_uid : 0;
    int legacy = c && c->capture_epoch && old->graph_epoch != c->capture_epoch;
    if (legacy)
      r.op_uid = c->begin.op_uid;
    r.request_uid = old->uid;
    r.request_generation = old->generation;
    r.activation = old->activation;
    r.communicator_uid = old->comm_uid;
    r.collective_ordinal = old->ordinal;
    r.peer = old->peer;
    r.tag = old->tag;
    r.root = old->root;
    r.flags = old->flags | PEAK_MPI_CAUSAL_STATUS_OBSERVED;
    if (legacy)
      r.flags |= PEAK_MPI_CAUSAL_LEGACY_INPUT;
    r.flags |=
        reclaim ? PEAK_MPI_CAUSAL_RECLAIMED : PEAK_MPI_CAUSAL_STATUS_ONLY;
    if (reclaim && old->persistent)
      r.flags |= PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
    r.mpi_error = error;
    r.completion_index = index;
    r.capability_uid = (uint64_t)contract.profile;
    if (error == MPI_SUCCESS)
      r.flags |= PEAK_MPI_CAUSAL_SUCCESS;
    if (cancelled)
      r.flags |= PEAK_MPI_OBSERVER_CANCELLED;
    if (error == MPI_SUCCESS) {
      PeakMpiObserverProgressScope scope;
      r.flags |= peak_mpi_progress_contract_result_flags(
          &contract, old->operation, r.flags, 1, &scope);
      r.progress_scope = scope;
    }
    int actual = 0;
    if ((old->flags & PEAK_MPI_CAUSAL_RECEIVE) && status &&
        error == MPI_SUCCESS && !cancelled)
      actual = receive_status(&comms[slot], status, &r);
    if (c)
      submit_event(&r, actual);
    if (error != MPI_SUCCESS)
      fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    if (old->operation == PEAK_MPI_CAUSAL_COMM_IDUP &&
        old->idup_comm != MPI_COMM_NULL && error == MPI_SUCCESS)
      define_comm(old->idup_comm, old->comm_uid, old->lineage_ordinal,
                  PEAK_MPI_CAUSAL_COMM_IDUP_CREATED, 0);
  } else if (c && old->observed && reclaim && old->start_uid &&
             old->graph_epoch == c->capture_epoch) {
    PeakMpiObserverRecord r = {0};
    r.kind = PEAK_MPI_OBSERVER_EVENT_LOCAL_COST;
    r.phase = PEAK_MPI_CAUSAL_REQUEST_RETIRED;
    r.op_uid = old->start_uid;
    r.batch_uid = c->begin.op_uid;
    r.request_uid = old->uid;
    r.request_generation = old->generation;
    r.activation = old->activation;
    r.flags = old->flags | PEAK_MPI_CAUSAL_RECLAIMED;
    if (old->persistent)
      r.flags |= PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
    submit_event(&r, 0);
  }
  pthread_mutex_lock(&metadata);
  int i = request_index(old->handle);
  if (i >= 0 && requests[i].generation == old->generation) {
    if (old->active)
      requests[i].observed = 1;
    if (reclaim && old->persistent)
      requests[i].active = 0;
  }
  pthread_mutex_unlock(&metadata);
  if (reclaim && !old->persistent)
    forget_request(old);
  return old->active && !observed ? 1u : 0u;
}

#define BLOCK_SEND_TYPED(name, operation, count_type)                          \
  DECLARE_REAL(name, (const void *buf, count_type count, MPI_Datatype type,    \
                      int dest, int tag, MPI_Comm comm))                       \
  static int wrap_##name(Fn_##name real, const void *buf, count_type count,    \
                         MPI_Datatype type, int dest, int tag,                 \
                         MPI_Comm comm) {                                      \
    int observe = wrapper_enter();                                             \
    Comm m = {0};                                                              \
    unsigned slot = 0;                                                         \
    Context *c = NULL;                                                         \
    int peer = PEAK_MPI_CAUSAL_UNKNOWN_PEER;                                   \
    if (observe && get_comm(comm, &m, &slot, 0)) {                             \
      if (dest != MPI_PROC_NULL)                                               \
        validate_type(type);                                                   \
      peer = world_peer(&m, dest);                                             \
      note_peer(peer);                                                         \
      record_capability(operation, 0, actual_peer(peer));                      \
      c = begin_call(operation, &m, 0, peer, normalized_tag(tag), -1, 0);      \
    }                                                                          \
    int rc;                                                                    \
    NATIVE(rc, real(buf, count, type, dest, tag, comm));                       \
    if (c)                                                                     \
      end_call(c, rc, NULL, actual_peer(peer), 0);                             \
    if (observe && m.uid)                                                      \
      put_comm(slot);                                                          \
    wrapper_leave();                                                           \
    return rc;                                                                 \
  }                                                                            \
  FORWARD_BOTH(name,                                                           \
               (const void *buf, count_type count, MPI_Datatype type,          \
                int dest, int tag, MPI_Comm comm),                             \
               buf, count, type, dest, tag, comm)

#define BLOCK_SEND(name, operation) BLOCK_SEND_TYPED(name, operation, int)
BLOCK_SEND(Send, PEAK_MPI_CAUSAL_SEND)
BLOCK_SEND(Ssend, PEAK_MPI_CAUSAL_SSEND)
BLOCK_SEND(Bsend, PEAK_MPI_CAUSAL_BSEND)
BLOCK_SEND(Rsend, PEAK_MPI_CAUSAL_RSEND)

#define NB_SEND_TYPED(name, operation, count_type)                             \
  DECLARE_REAL(name, (const void *buf, count_type count, MPI_Datatype type,    \
                      int dest, int tag, MPI_Comm comm, MPI_Request *request)) \
  static int wrap_##name(Fn_##name real, const void *buf, count_type count,    \
                         MPI_Datatype type, int dest, int tag, MPI_Comm comm,  \
                         MPI_Request *request) {                               \
    int observe = wrapper_enter();                                             \
    Comm m = {0};                                                              \
    unsigned slot = 0;                                                         \
    Context *c = NULL;                                                         \
    int peer = PEAK_MPI_CAUSAL_UNKNOWN_PEER;                                   \
    if (observe && get_comm(comm, &m, &slot, 0)) {                             \
      if (dest != MPI_PROC_NULL)                                               \
        validate_type(type);                                                   \
      peer = world_peer(&m, dest);                                             \
      note_peer(peer);                                                         \
      record_capability(operation, PEAK_MPI_CAUSAL_NONBLOCKING,                \
                        actual_peer(peer));                                    \
      c = begin_call(operation, &m, PEAK_MPI_CAUSAL_NONBLOCKING, peer,         \
                     normalized_tag(tag), -1, 0);                              \
    }                                                                          \
    int rc;                                                                    \
    NATIVE(rc, real(buf, count, type, dest, tag, comm, request));              \
    PeakMpiObserverRecord d = {0};                                             \
    if (c && rc == MPI_SUCCESS && request)                                     \
      install_request(*request, &m, slot, operation,                           \
                      PEAK_MPI_CAUSAL_NONBLOCKING, peer, normalized_tag(tag),  \
                      -1, 0, c->begin.op_uid, 0, &d);                          \
    if (c)                                                                     \
      end_call(c, rc, &d, actual_peer(peer), 0);                               \
    if (observe && m.uid)                                                      \
      put_comm(slot);                                                          \
    wrapper_leave();                                                           \
    return rc;                                                                 \
  }                                                                            \
  FORWARD_BOTH(name,                                                           \
               (const void *buf, count_type count, MPI_Datatype type,          \
                int dest, int tag, MPI_Comm comm, MPI_Request *request),       \
               buf, count, type, dest, tag, comm, request)

#define NB_SEND(name, operation) NB_SEND_TYPED(name, operation, int)
NB_SEND(Isend, PEAK_MPI_CAUSAL_SEND)
NB_SEND(Issend, PEAK_MPI_CAUSAL_SSEND)
NB_SEND(Ibsend, PEAK_MPI_CAUSAL_BSEND)
NB_SEND(Irsend, PEAK_MPI_CAUSAL_RSEND)

DECLARE_REAL(Recv, (void *buf, int count, MPI_Datatype type, int source,
                    int tag, MPI_Comm comm, MPI_Status *status))
static int wrap_Recv(Fn_Recv real, void *buf, int count, MPI_Datatype type,
                     int source, int tag, MPI_Comm comm, MPI_Status *status) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    if (source != MPI_PROC_NULL)
      validate_type(type);
    int peer = world_peer(&m, source);
    note_peer(peer);
    record_capability(PEAK_MPI_CAUSAL_RECV, 0, actual_peer(peer));
    c = begin_call(PEAK_MPI_CAUSAL_RECV, &m, PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(tag), -1, 0);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(buf, count, type, source, tag, comm, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS) {
    actual = receive_status(&m, native, &d);
    record_capability(PEAK_MPI_CAUSAL_RECV, 0, actual);
  }
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Recv,
             (void *buf, int count, MPI_Datatype type, int source, int tag,
              MPI_Comm comm, MPI_Status *status),
             buf, count, type, source, tag, comm, status)

DECLARE_REAL(Irecv, (void *buf, int count, MPI_Datatype type, int source,
                     int tag, MPI_Comm comm, MPI_Request *request))
static int wrap_Irecv(Fn_Irecv real, void *buf, int count, MPI_Datatype type,
                      int source, int tag, MPI_Comm comm,
                      MPI_Request *request) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  int peer = PEAK_MPI_CAUSAL_UNKNOWN_PEER;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    if (source != MPI_PROC_NULL)
      validate_type(type);
    peer = world_peer(&m, source);
    note_peer(peer);
    if (peer == PEAK_MPI_CAUSAL_ANY_SOURCE &&
        !peak_mpi_causal_runtime_metadata_enabled())
      peak_mpi_causal_runtime_note_peer(-1);
    record_capability(PEAK_MPI_CAUSAL_RECV, PEAK_MPI_CAUSAL_NONBLOCKING,
                      actual_peer(peer));
    c = begin_call(PEAK_MPI_CAUSAL_RECV, &m,
                   PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(tag), -1, 0);
  }
  int rc;
  NATIVE(rc, real(buf, count, type, source, tag, comm, request));
  PeakMpiObserverRecord d = {0};
  if (c && rc == MPI_SUCCESS && request)
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_RECV,
                    PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_RECEIVE, peer,
                    normalized_tag(tag), -1, 0, c->begin.op_uid, 0, &d);
  if (c)
    end_call(c, rc, &d, actual_peer(peer), 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Irecv,
             (void *buf, int count, MPI_Datatype type, int source, int tag,
              MPI_Comm comm, MPI_Request *request),
             buf, count, type, source, tag, comm, request)

DECLARE_REAL(Sendrecv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              int dest, int sendtag, void *recvbuf, int recvcount,
              MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm,
              MPI_Status *status))
static int wrap_Sendrecv(Fn_Sendrecv real, const void *sendbuf, int sendcount,
                         MPI_Datatype sendtype, int dest, int sendtag,
                         void *recvbuf, int recvcount, MPI_Datatype recvtype,
                         int source, int recvtag, MPI_Comm comm,
                         MPI_Status *status) {
  int observe = wrapper_enter();
  if (observe) {
    if (dest != MPI_PROC_NULL)
      validate_type(sendtype);
    if (source != MPI_PROC_NULL)
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  int actual = 0;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    int peer = world_peer(&m, dest);
    note_peer(peer);
    actual = actual_peer(peer);
    record_capability(PEAK_MPI_CAUSAL_SENDRECV, 0, actual);
    c = begin_call(PEAK_MPI_CAUSAL_SENDRECV, &m, PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(sendtag), world_peer(&m, source), 1);
    if (c) {
      c->begin.actual_tag = normalized_tag(recvtag);
      publish_begin(c);
    }
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, dest, sendtag, recvbuf,
                  recvcount, recvtype, source, recvtag, comm, native));
  PeakMpiObserverRecord d = {0};
  if (observe && m.uid && rc == MPI_SUCCESS)
    actual |= receive_status(&m, native, &d);
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Sendrecv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              int dest, int sendtag, void *recvbuf, int recvcount,
              MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm,
              MPI_Status *status),
             sendbuf, sendcount, sendtype, dest, sendtag, recvbuf, recvcount,
             recvtype, source, recvtag, comm, status)

DECLARE_REAL(Wait, (MPI_Request * request, MPI_Status *status))
static int wrap_Wait(Fn_Wait real, MPI_Request *request, MPI_Status *status) {
  int observe = wrapper_enter();
  Request old = {0};
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && request) {
    snapshot_request(*request, &old);
    c = begin_call(PEAK_MPI_CAUSAL_WAIT, NULL, 0, -1, -1, -1, 0);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(request, native));
  unsigned done = 0;
  if (c && old.occupied)
    done = complete_request(c, &old, native, rc, rc == MPI_SUCCESS, 0);
  if (c)
    end_call(c, rc, NULL, 0, done);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Wait, (MPI_Request * request, MPI_Status *status), request, status)

DECLARE_REAL(Test, (MPI_Request * request, int *flag, MPI_Status *status))
static int wrap_Test(Fn_Test real, MPI_Request *request, int *flag,
                     MPI_Status *status) {
  int observe = wrapper_enter();
  Request old = {0};
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && request) {
    snapshot_request(*request, &old);
    c = begin_call(PEAK_MPI_CAUSAL_TEST, NULL, 0, -1, -1, -1, 1);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(request, flag, native));
  unsigned done = 0;
  if (c && rc == MPI_SUCCESS && flag && *flag && old.occupied) {
    publish_begin(c);
    done = complete_request(c, &old, native, rc, 1, 0);
    end_call(c, rc, NULL, 0, done);
  } else if (observe)
    poll_empty(c);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Test, (MPI_Request * request, int *flag, MPI_Status *status),
             request, flag, status)

static Context *prepare_many(int count, MPI_Request array[],
                             PeakMpiCausalOperation op, int deferred) {
  if (!peak_mpi_causal_runtime_metadata_enabled()) {
    if (count > 0)
      peak_mpi_causal_runtime_note_peer(-1);
    return NULL;
  }
  Context *c = context();
  if (!c)
    return NULL;
  if (count < 0 || count > (int)completion_capacity || (count && !array)) {
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    return NULL;
  }
  for (int i = 0; i < count; ++i)
    snapshot_request(array[i], &c->requests[i]);
  c = begin_call(op, NULL, 0, -1, -1, -1, deferred);
  if (c)
    c->count = (unsigned)count;
  return c;
}
static int completion_error(int rc, const MPI_Status *s) {
  int cls = rc;
  if (rc != MPI_SUCCESS)
    (void)PMPI_Error_class(rc, &cls);
  if (rc != MPI_SUCCESS && cls != MPI_ERR_IN_STATUS) {
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    return MPI_ERR_PENDING;
  }
  return cls == MPI_ERR_IN_STATUS ? s->MPI_ERROR : rc;
}
DECLARE_REAL(Waitall, (int count, MPI_Request array[], MPI_Status statuses[]))
static int wrap_Waitall(Fn_Waitall real, int count, MPI_Request array[],
                        MPI_Status statuses[]) {
  int observe = wrapper_enter();
  Context *c =
      observe ? prepare_many(count, array, PEAK_MPI_CAUSAL_WAIT, 0) : NULL;
  MPI_Status *native =
      c && statuses == MPI_STATUSES_IGNORE ? c->statuses : statuses;
  int rc;
  NATIVE(rc, real(count, array, native));
  unsigned done = 0;
  if (c)
    for (int i = 0; i < count; ++i) {
      int error = completion_error(rc, &native[i]);
      int cls = error;
      (void)PMPI_Error_class(error, &cls);
      if (cls != MPI_ERR_PENDING)
        done += complete_request(c, &c->requests[i], &native[i], error, 1,
                                 (unsigned)i);
    }
  if (c)
    end_call(c, rc, NULL, 0, done);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Waitall, (int count, MPI_Request array[], MPI_Status statuses[]),
             count, array, statuses)

DECLARE_REAL(Testall,
             (int count, MPI_Request array[], int *flag, MPI_Status statuses[]))
static int wrap_Testall(Fn_Testall real, int count, MPI_Request array[],
                        int *flag, MPI_Status statuses[]) {
  int observe = wrapper_enter();
  Context *c =
      observe ? prepare_many(count, array, PEAK_MPI_CAUSAL_TEST, 1) : NULL;
  MPI_Status *native =
      c && statuses == MPI_STATUSES_IGNORE ? c->statuses : statuses;
  int rc;
  NATIVE(rc, real(count, array, flag, native));
  unsigned done = 0;
  if (c && ((rc == MPI_SUCCESS && flag && *flag) || rc != MPI_SUCCESS)) {
    publish_begin(c);
    for (int i = 0; i < count; ++i) {
      int error = completion_error(rc, &native[i]);
      int cls = error;
      (void)PMPI_Error_class(error, &cls);
      if (cls != MPI_ERR_PENDING)
        done += complete_request(c, &c->requests[i], &native[i], error, 1,
                                 (unsigned)i);
    }
    end_call(c, rc, NULL, 0, done);
  } else if (observe)
    poll_empty(c);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Testall,
             (int count, MPI_Request array[], int *flag, MPI_Status statuses[]),
             count, array, flag, statuses)

DECLARE_REAL(Waitany,
             (int count, MPI_Request array[], int *index, MPI_Status *status))
static int wrap_Waitany(Fn_Waitany real, int count, MPI_Request array[],
                        int *index, MPI_Status *status) {
  int observe = wrapper_enter();
  Context *c =
      observe ? prepare_many(count, array, PEAK_MPI_CAUSAL_WAIT, 0) : NULL;
  MPI_Status *native = c && status == MPI_STATUS_IGNORE ? c->statuses : status;
  int rc;
  NATIVE(rc, real(count, array, index, native));
  unsigned done = 0;
  if (c && index && *index != MPI_UNDEFINED && *index >= 0 && *index < count)
    done = complete_request(c, &c->requests[*index], native, rc,
                            rc == MPI_SUCCESS, (unsigned)*index);
  if (c)
    end_call(c, rc, NULL, 0, done);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Waitany,
             (int count, MPI_Request array[], int *index, MPI_Status *status),
             count, array, index, status)

DECLARE_REAL(Testany, (int count, MPI_Request array[], int *index, int *flag,
                       MPI_Status *status))
static int wrap_Testany(Fn_Testany real, int count, MPI_Request array[],
                        int *index, int *flag, MPI_Status *status) {
  int observe = wrapper_enter();
  Context *c =
      observe ? prepare_many(count, array, PEAK_MPI_CAUSAL_TEST, 1) : NULL;
  MPI_Status *native = c && status == MPI_STATUS_IGNORE ? c->statuses : status;
  int rc;
  NATIVE(rc, real(count, array, index, flag, native));
  unsigned done = 0;
  if (c && rc == MPI_SUCCESS && flag && *flag && index && *index >= 0 &&
      *index < count) {
    publish_begin(c);
    done = complete_request(c, &c->requests[*index], native, rc, 1,
                            (unsigned)*index);
    end_call(c, rc, NULL, 0, done);
  } else if (observe)
    poll_empty(c);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Testany,
             (int count, MPI_Request array[], int *index, int *flag,
              MPI_Status *status),
             count, array, index, flag, status)

#define SOME(name, op, deferred)                                               \
  DECLARE_REAL(name, (int count, MPI_Request array[], int *outcount,           \
                      int indices[], MPI_Status statuses[]))                   \
  static int wrap_##name(Fn_##name real, int count, MPI_Request array[],       \
                         int *outcount, int indices[],                         \
                         MPI_Status statuses[]) {                              \
    int observe = wrapper_enter();                                             \
    Context *c = observe ? prepare_many(count, array, op, deferred) : NULL;    \
    MPI_Status *native =                                                       \
        c && statuses == MPI_STATUSES_IGNORE ? c->statuses : statuses;         \
    int rc;                                                                    \
    NATIVE(rc, real(count, array, outcount, indices, native));                 \
    unsigned done = 0;                                                         \
    if (c && outcount && *outcount > 0 && *outcount <= count) {                \
      if (!c->emitted)                                                         \
        publish_begin(c);                                                      \
      for (int k = 0; k < *outcount; ++k) {                                    \
        int i = indices[k];                                                    \
        if (i < 0 || i >= count) {                                             \
          fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);                             \
          continue;                                                            \
        }                                                                      \
        int error = completion_error(rc, &native[k]);                          \
        int cls = error;                                                       \
        (void)PMPI_Error_class(error, &cls);                                   \
        if (cls != MPI_ERR_PENDING)                                            \
          done += complete_request(c, &c->requests[i], &native[k], error, 1,   \
                                   (unsigned)i);                               \
      }                                                                        \
      end_call(c, rc, NULL, 0, done);                                          \
    } else if (c) {                                                            \
      if (deferred)                                                            \
        poll_empty(c);                                                         \
      else                                                                     \
        end_call(c, rc, NULL, 0, 0);                                           \
    }                                                                          \
    wrapper_leave();                                                           \
    return rc;                                                                 \
  }                                                                            \
  FORWARD_BOTH(name,                                                           \
               (int count, MPI_Request array[], int *outcount, int indices[],  \
                MPI_Status statuses[]),                                        \
               count, array, outcount, indices, statuses)
SOME(Waitsome, PEAK_MPI_CAUSAL_WAIT, 0)
SOME(Testsome, PEAK_MPI_CAUSAL_TEST, 1)

DECLARE_REAL(Request_get_status,
             (MPI_Request request, int *flag, MPI_Status *status))
static int wrap_Request_get_status(Fn_Request_get_status real,
                                   MPI_Request request, int *flag,
                                   MPI_Status *status) {
  int observe = wrapper_enter();
  Request old = {0};
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe) {
    snapshot_request(request, &old);
    c = begin_call(PEAK_MPI_CAUSAL_TEST, NULL, 0, -1, -1, -1, 1);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(request, flag, native));
  if (c && rc == MPI_SUCCESS && flag && *flag && old.active && !old.observed) {
    publish_begin(c);
    unsigned done = complete_request(c, &old, native, rc, 0, 0);
    end_call(c, rc, NULL, 0, done);
  } else if (observe)
    poll_empty(c);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Request_get_status,
             (MPI_Request request, int *flag, MPI_Status *status), request,
             flag, status)

DECLARE_REAL(Cancel, (MPI_Request * request))
static int wrap_Cancel(Fn_Cancel real, MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe &&
      contract.profile ==
          PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED)
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  Request old = {0};
  if (observe && request)
    snapshot_request(*request, &old);
  int rc;
  NATIVE(rc, real(request));
  if (observe && rc == MPI_SUCCESS && old.occupied) {
    pthread_mutex_lock(&metadata);
    int i = request_index(old.handle);
    if (i >= 0 && requests[i].generation == old.generation)
      requests[i].cancelled = 1;
    pthread_mutex_unlock(&metadata);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Cancel, (MPI_Request * request), request)

DECLARE_REAL(Request_free, (MPI_Request * request))
static int wrap_Request_free(Fn_Request_free real, MPI_Request *request) {
  int observe = wrapper_enter();
  Request old = {0};
  Context *c = NULL;
  if (observe && request) {
    snapshot_request(*request, &old);
    c = begin_call(PEAK_MPI_CAUSAL_REQUEST_FREE, NULL, 0, -1, -1, -1, 0);
    if (old.active && !old.observed)
      fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  }
  int rc;
  NATIVE(rc, real(request));
  PeakMpiObserverRecord d = {0};
  d.request_uid = old.uid;
  d.request_generation = old.generation;
  d.activation = old.activation;
  if (!old.active)
    d.flags |= PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
  if (observe && rc == MPI_SUCCESS && old.occupied) {
    if (old.active && old.observed && c)
      (void)complete_request(c, &old, NULL, MPI_SUCCESS, 1, 0);
    forget_request(&old);
  }
  if (c)
    end_call(c, rc, &d, 0, 0);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Request_free, (MPI_Request * request), request)

#define PERSISTENT_SEND_TYPED(name, operation, count_type)                     \
  DECLARE_REAL(name, (const void *buf, count_type count, MPI_Datatype type,    \
                      int dest, int tag, MPI_Comm comm, MPI_Request *request)) \
  static int wrap_##name(Fn_##name real, const void *buf, count_type count,    \
                         MPI_Datatype type, int dest, int tag, MPI_Comm comm,  \
                         MPI_Request *request) {                               \
    int observe = wrapper_enter();                                             \
    Comm m = {0};                                                              \
    unsigned slot = 0;                                                         \
    int known = observe && get_comm(comm, &m, &slot, 0);                       \
    if (known && dest != MPI_PROC_NULL)                                        \
      validate_type(type);                                                     \
    int rc;                                                                    \
    NATIVE(rc, real(buf, count, type, dest, tag, comm, request));              \
    if (known && peak_mpi_causal_runtime_metadata_enabled() &&                 \
        rc == MPI_SUCCESS && request) {                                        \
      PeakMpiObserverRecord d = {0};                                           \
      install_request(                                                         \
          *request, &m, slot, operation,                                       \
          PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_PERSISTENT,            \
          world_peer(&m, dest), normalized_tag(tag), -1, 0, 0, 1, &d);         \
    }                                                                          \
    if (known)                                                                 \
      put_comm(slot);                                                          \
    wrapper_leave();                                                           \
    return rc;                                                                 \
  }                                                                            \
  FORWARD_BOTH(name,                                                           \
               (const void *buf, count_type count, MPI_Datatype type,          \
                int dest, int tag, MPI_Comm comm, MPI_Request *request),       \
               buf, count, type, dest, tag, comm, request)
#define PERSISTENT_SEND(name, operation)                                       \
  PERSISTENT_SEND_TYPED(name, operation, int)
PERSISTENT_SEND(Send_init, PEAK_MPI_CAUSAL_SEND)
PERSISTENT_SEND(Ssend_init, PEAK_MPI_CAUSAL_SSEND)
PERSISTENT_SEND(Bsend_init, PEAK_MPI_CAUSAL_BSEND)
PERSISTENT_SEND(Rsend_init, PEAK_MPI_CAUSAL_RSEND)

DECLARE_REAL(Recv_init, (void *buf, int count, MPI_Datatype type, int source,
                         int tag, MPI_Comm comm, MPI_Request *request))
static int wrap_Recv_init(Fn_Recv_init real, void *buf, int count,
                          MPI_Datatype type, int source, int tag, MPI_Comm comm,
                          MPI_Request *request) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  int known = observe && get_comm(comm, &m, &slot, 0);
  if (known && source != MPI_PROC_NULL)
    validate_type(type);
  int rc;
  NATIVE(rc, real(buf, count, type, source, tag, comm, request));
  if (known && peak_mpi_causal_runtime_metadata_enabled() &&
      rc == MPI_SUCCESS && request) {
    PeakMpiObserverRecord d = {0};
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_RECV,
                    PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_PERSISTENT |
                        PEAK_MPI_CAUSAL_RECEIVE,
                    world_peer(&m, source), normalized_tag(tag), -1, 0, 0, 1,
                    &d);
  }
  if (known)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Recv_init,
             (void *buf, int count, MPI_Datatype type, int source, int tag,
              MPI_Comm comm, MPI_Request *request),
             buf, count, type, source, tag, comm, request)

static unsigned activate_requests(Context *c, unsigned count) {
  unsigned actual = 0;
  for (unsigned k = 0; k < count; ++k) {
    Request *old = &c->requests[k];
    if (!old->occupied)
      continue;
    if (!old->persistent || old->active) {
      fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
      continue;
    }
    uint64_t start = peak_mpi_causal_runtime_operation_id();
    pthread_mutex_lock(&metadata);
    int i = request_index(old->handle);
    if (i >= 0 && requests[i].generation == old->generation) {
      requests[i].active = 1;
      requests[i].observed = 0;
      requests[i].cancelled = 0;
      requests[i].start_uid = start;
      requests[i].graph_epoch = c->capture_epoch;
      ++requests[i].activation;
      if (contract.observed_p2p_completion) {
        if (requests[i].flags & PEAK_MPI_CAUSAL_BUFFERED)
          ++observed_open_buffered;
        else
          ++observed_open_requests;
      }
      *old = requests[i];
    }
    pthread_mutex_unlock(&metadata);
    note_peer(old->peer);
    actual |= (unsigned)actual_peer(old->peer);
    record_capability(old->operation, old->flags, actual_peer(old->peer));
    PeakMpiObserverRecord r = c->begin;
    r.op_uid = start;
    r.batch_uid = c->begin.op_uid;
    r.phase = PEAK_MPI_CAUSAL_REQUEST_ACTIVATE;
    r.operation = old->operation;
    r.flags |= old->flags;
    r.communicator_uid = old->comm_uid;
    r.request_uid = old->uid;
    r.request_generation = old->generation;
    r.activation = old->activation;
    r.peer = old->peer;
    r.tag = old->tag;
    r.root = old->root;
    r.collective_ordinal = old->ordinal;
    if (old->operation == PEAK_MPI_CAUSAL_BSEND) {
      pthread_mutex_lock(&metadata);
      r.progress_begin = buffer_generation;
      pthread_mutex_unlock(&metadata);
    }
    submit_event(&r, actual_peer(old->peer));
  }
  return actual;
}
DECLARE_REAL(Start, (MPI_Request * request))
static int wrap_Start(Fn_Start real, MPI_Request *request) {
  int observe = wrapper_enter();
  Context *c = observe && request
                   ? prepare_many(1, request, PEAK_MPI_CAUSAL_START, 0)
                   : NULL;
  unsigned actual = c ? activate_requests(c, 1) : 0;
  if (observe && !peak_mpi_causal_runtime_metadata_enabled())
    peak_mpi_causal_runtime_note_peer(-1);
  int rc;
  NATIVE(rc, real(request));
  if (c) {
    if (rc != MPI_SUCCESS)
      fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    end_call(c, rc, NULL, (int)actual, 0);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Start, (MPI_Request * request), request)
DECLARE_REAL(Startall, (int count, MPI_Request array[]))
static int wrap_Startall(Fn_Startall real, int count, MPI_Request array[]) {
  int observe = wrapper_enter();
  Context *c =
      observe ? prepare_many(count, array, PEAK_MPI_CAUSAL_START, 0) : NULL;
  unsigned actual = c ? activate_requests(c, (unsigned)count) : 0;
  if (observe && count > 0 && !peak_mpi_causal_runtime_metadata_enabled())
    peak_mpi_causal_runtime_note_peer(-1);
  int rc;
  NATIVE(rc, real(count, array));
  if (c) {
    if (rc != MPI_SUCCESS)
      fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    end_call(c, rc, NULL, (int)actual, 0);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Startall, (int count, MPI_Request array[]), count, array)

DECLARE_REAL(Probe, (int source, int tag, MPI_Comm comm, MPI_Status *status))
static int wrap_Probe(Fn_Probe real, int source, int tag, MPI_Comm comm,
                      MPI_Status *status) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    c = begin_call(PEAK_MPI_CAUSAL_PROBE, &m, 0, world_peer(&m, source),
                   normalized_tag(tag), -1, 1);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(source, tag, comm, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS) {
    actual = receive_status(&m, native, &d);
    record_capability(PEAK_MPI_CAUSAL_PROBE, 0, actual);
  }
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Probe, (int source, int tag, MPI_Comm comm, MPI_Status *status),
             source, tag, comm, status)
DECLARE_REAL(Iprobe, (int source, int tag, MPI_Comm comm, int *flag,
                      MPI_Status *status))
static int wrap_Iprobe(Fn_Iprobe real, int source, int tag, MPI_Comm comm,
                       int *flag, MPI_Status *status) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    c = begin_call(PEAK_MPI_CAUSAL_PROBE, &m, 0, world_peer(&m, source),
                   normalized_tag(tag), -1, 1);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(source, tag, comm, flag, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS && flag && *flag) {
    actual = receive_status(&m, native, &d);
    record_capability(PEAK_MPI_CAUSAL_PROBE, 0, actual);
    end_call(c, rc, &d, actual, 0);
  } else if (observe)
    poll_empty(c);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iprobe,
             (int source, int tag, MPI_Comm comm, int *flag,
              MPI_Status *status),
             source, tag, comm, flag, status)

static uint64_t bind_message(MPI_Message handle, const Comm *comm,
                             unsigned slot, const MPI_Status *status,
                             uint64_t probe) {
  uint64_t uid = 0;
  pthread_mutex_lock(&metadata);
  for (unsigned i = 0; i < MESSAGE_CAP; ++i)
    if (!messages[i].occupied) {
      uid = peak_mpi_causal_runtime_operation_id();
      messages[i] = (Message){handle,
                              uid,
                              comm->uid,
                              probe,
                              world_peer(comm, status->MPI_SOURCE),
                              normalized_tag(status->MPI_TAG),
                              1};
      ++comms[slot].references;
      break;
    }
  pthread_mutex_unlock(&metadata);
  if (!uid)
    fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
  return uid;
}
DECLARE_REAL(Mprobe, (int source, int tag, MPI_Comm comm, MPI_Message *message,
                      MPI_Status *status))
static int wrap_Mprobe(Fn_Mprobe real, int source, int tag, MPI_Comm comm,
                       MPI_Message *message, MPI_Status *status) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    c = begin_call(PEAK_MPI_CAUSAL_MPROBE, &m, 0, world_peer(&m, source),
                   normalized_tag(tag), -1, 1);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(source, tag, comm, message, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS && message) {
    actual = receive_status(&m, native, &d);
    if (c)
      d.request_uid = bind_message(*message, &m, slot, native, c->begin.op_uid);
    record_capability(PEAK_MPI_CAUSAL_MPROBE, 0, actual);
  }
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Mprobe,
             (int source, int tag, MPI_Comm comm, MPI_Message *message,
              MPI_Status *status),
             source, tag, comm, message, status)
DECLARE_REAL(Improbe, (int source, int tag, MPI_Comm comm, int *flag,
                       MPI_Message *message, MPI_Status *status))
static int wrap_Improbe(Fn_Improbe real, int source, int tag, MPI_Comm comm,
                        int *flag, MPI_Message *message, MPI_Status *status) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    c = begin_call(PEAK_MPI_CAUSAL_MPROBE, &m, 0, world_peer(&m, source),
                   normalized_tag(tag), -1, 1);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(source, tag, comm, flag, message, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS && flag && *flag && message) {
    actual = receive_status(&m, native, &d);
    if (c)
      d.request_uid = bind_message(*message, &m, slot, native, c->begin.op_uid);
    record_capability(PEAK_MPI_CAUSAL_MPROBE, 0, actual);
    end_call(c, rc, &d, actual, 0);
  } else if (observe)
    poll_empty(c);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Improbe,
             (int source, int tag, MPI_Comm comm, int *flag,
              MPI_Message *message, MPI_Status *status),
             source, tag, comm, flag, message, status)

static int take_message(MPI_Message handle, Message *out, Comm *comm,
                        unsigned *slot) {
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return 0;
  int found = 0;
  pthread_mutex_lock(&metadata);
  for (unsigned i = 0; i < MESSAGE_CAP; ++i)
    if (messages[i].occupied && messages[i].handle == handle) {
      *out = messages[i];
      messages[i].occupied = 0;
      for (unsigned j = 0; j < COMM_CAP; ++j)
        if (comms[j].uid == out->comm_uid) {
          *comm = comms[j];
          *slot = j;
          found = 1;
          break;
        }
      break;
    }
  pthread_mutex_unlock(&metadata);
  if (!found)
    fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
  return found;
}
DECLARE_REAL(Mrecv, (void *buf, int count, MPI_Datatype type,
                     MPI_Message *message, MPI_Status *status))
static int wrap_Mrecv(Fn_Mrecv real, void *buf, int count, MPI_Datatype type,
                      MPI_Message *message, MPI_Status *status) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Message msg = {0};
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && message && take_message(*message, &msg, &m, &slot)) {
    c = begin_call(PEAK_MPI_CAUSAL_MRECV, &m, PEAK_MPI_CAUSAL_RECEIVE, msg.peer,
                   msg.tag, -1, 1);
    if (c) {
      c->begin.request_uid = msg.uid;
      c->begin.progress_begin = msg.probe_uid;
      publish_begin(c);
    }
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(buf, count, type, message, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS)
    actual = receive_status(&m, native, &d);
  d.request_uid = msg.uid;
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Mrecv,
             (void *buf, int count, MPI_Datatype type, MPI_Message *message,
              MPI_Status *status),
             buf, count, type, message, status)
DECLARE_REAL(Imrecv, (void *buf, int count, MPI_Datatype type,
                      MPI_Message *message, MPI_Request *request))
static int wrap_Imrecv(Fn_Imrecv real, void *buf, int count, MPI_Datatype type,
                       MPI_Message *message, MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Message msg = {0};
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe && message && take_message(*message, &msg, &m, &slot)) {
    c = begin_call(PEAK_MPI_CAUSAL_MRECV, &m,
                   PEAK_MPI_CAUSAL_RECEIVE | PEAK_MPI_CAUSAL_NONBLOCKING,
                   msg.peer, msg.tag, -1, 1);
    if (c) {
      c->begin.request_uid = msg.uid;
      c->begin.progress_begin = msg.probe_uid;
      publish_begin(c);
    }
  }
  int rc;
  NATIVE(rc, real(buf, count, type, message, request));
  PeakMpiObserverRecord d = {0};
  if (c && rc == MPI_SUCCESS && request)
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_MRECV,
                    PEAK_MPI_CAUSAL_RECEIVE | PEAK_MPI_CAUSAL_NONBLOCKING,
                    msg.peer, msg.tag, -1, 0, c->begin.op_uid, 0, &d);
  if (c)
    end_call(c, rc, &d, actual_peer(msg.peer), 0);
  if (m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Imrecv,
             (void *buf, int count, MPI_Datatype type, MPI_Message *message,
              MPI_Request *request),
             buf, count, type, message, request)

DECLARE_REAL(Buffer_attach, (void *buffer, int size))
static int wrap_Buffer_attach(Fn_Buffer_attach real, void *buffer, int size) {
  int observe = wrapper_enter();
  int rc;
  NATIVE(rc, real(buffer, size));
  if (observe && rc == MPI_SUCCESS) {
    pthread_mutex_lock(&metadata);
    if (buffer_generation == UINT64_MAX)
      fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    else
      ++buffer_generation;
    pthread_mutex_unlock(&metadata);
  }
  if (observe) {
    PeakMpiObserverRecord cut = {0};
    peak_mpi_causal_runtime_boundary(&cut);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Buffer_attach, (void *buffer, int size), buffer, size)
DECLARE_REAL(Buffer_detach, (void *buffer, int *size))
static int wrap_Buffer_detach(Fn_Buffer_detach real, void *buffer, int *size) {
  int observe = wrapper_enter();
  Context *c = observe ? begin_call(PEAK_MPI_CAUSAL_BUFFER_DETACH, NULL, 0, -1,
                                    -1, -1, 0)
                       : NULL;
  int rc;
  NATIVE(rc, real(buffer, size));
  if (c)
    end_call(c, rc, NULL, 0, 0);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Buffer_detach, (void *buffer, int *size), buffer, size)

static Context *collective_begin(PeakMpiCausalOperation op, MPI_Comm handle,
                                 unsigned flags, int qualified, int root,
                                 Comm *comm, unsigned *slot) {
  if (!get_comm(handle, comm, slot, 1))
    return NULL;
  unsigned actual = comm->size > 1 || comm->inter;
  record_capability(op, flags, (int)actual);
  if (actual && !peak_mpi_progress_contract_collective_allowed(
                    &contract, op, flags, comm->size))
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  uint64_t ordinal;
  pthread_mutex_lock(&metadata);
  ordinal = ++comms[*slot].ordinal;
  int overlap = comms[*slot].active_collectives++ != 0;
  int no_open = !observed_open_requests && !observed_open_buffered;
  pthread_mutex_unlock(&metadata);
  comm->ordinal = ordinal;
  comm->recovery_operation = 0;
  if (qualified && !overlap && !comm->inter &&
      !(flags &
        (PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_VARIABLE_COUNT)) &&
      comm->size == peak_mpi_causal_runtime_world_size())
    comm->recovery_operation = (unsigned)op;
  if (overlap)
    fail(PEAK_MPI_OBSERVER_REASON_IDENTITY);
  flags |= PEAK_MPI_CAUSAL_COLLECTIVE;
  if (comm->inter)
    flags |= PEAK_MPI_CAUSAL_INTERCOMM;
  if (qualified && !comm->inter && !(flags & PEAK_MPI_CAUSAL_NONBLOCKING))
    flags |= PEAK_MPI_OBSERVER_SLACK_S1;
  if (single_flow)
    flags |= PEAK_MPI_OBSERVER_SLACK_SINGLE_FLOW;
  if (contract.profile == PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC ||
      (contract.observed_collective_completion && no_open))
    flags |= PEAK_MPI_OBSERVER_SLACK_NO_OPEN_PROGRESS;
  Context *c = begin_call(op, comm, flags, -1, -1, world_peer(comm, root), 1);
  if (c) {
    c->begin.collective_ordinal = ordinal;
    publish_begin(c);
  }
  return c;
}
static uint64_t collective_timer(const Context *c) {
  unsigned mask = PEAK_MPI_OBSERVER_SLACK_S1 |
                  PEAK_MPI_OBSERVER_SLACK_SINGLE_FLOW |
                  PEAK_MPI_OBSERVER_SLACK_NO_OPEN_PROGRESS;
  return c && (c->begin.flags & mask) == mask ? monotonic_ns() : 0;
}
static void collective_end(Context *c, Comm *comm, unsigned slot, int rc,
                           uint64_t started, MPI_Request *request) {
  PeakMpiObserverRecord d = {0};
  if (started) {
    uint64_t now = monotonic_ns();
    if (now >= started)
      d.wait_ns = now - started;
  }
  if (c) {
    d.collective_ordinal = c->begin.collective_ordinal;
    if (rc == MPI_SUCCESS && request)
      install_request(
          *request, comm, slot, (PeakMpiCausalOperation)c->begin.operation,
          c->begin.flags & ~PEAK_MPI_CAUSAL_CUT_PRECAPTURED, -1, -1,
          c->begin.root, c->begin.collective_ordinal, c->begin.op_uid, 0, &d);
    end_call(c, rc, &d, comm->size > 1 || comm->inter, 0);
  }
  if (comm->uid) {
    pthread_mutex_lock(&metadata);
    if (rc == MPI_SUCCESS && comm->recovery_operation &&
        comm->uid <= RECOVERY_COMM_CAP) {
      uint64_t loss = peak_mpi_causal_runtime_loss_generation();
      if (loss > UINT64_MAX >> 8)
        fail(PEAK_MPI_OBSERVER_REASON_SEQUENCE_LIMIT);
      else {
        RecoveryWitness *w = &recovery_witnesses[comm->uid - 1];
        w->ordinal = comm->ordinal;
        w->ticket = (loss << 8) | comm->recovery_operation;
      }
    }
    if (comms[slot].active_collectives)
      --comms[slot].active_collectives;
    pthread_mutex_unlock(&metadata);
    put_comm(slot);
  }
}
static int positive_counts(const int counts[], int n) {
  if (!peak_mpi_causal_runtime_metadata_enabled() || !counts)
    return 0;
  int positive = 1, inspected = 0;
  for (int i = 0; i < n; ++i) {
    ++inspected;
    if (counts[i] <= 0) {
      positive = 0;
      break;
    }
  }
  peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP,
                                        (size_t)inspected);
  return positive;
}
static int comm_size_cached(MPI_Comm handle) {
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return 0;
  pthread_mutex_lock(&metadata);
  int i = comm_index(handle);
  int n = i >= 0 ? comms[i].size : 0;
  pthread_mutex_unlock(&metadata);
  return n;
}

static int root_is_local(MPI_Comm comm, int root) {
  if (!peak_mpi_causal_runtime_metadata_enabled())
    return 0;
  pthread_mutex_lock(&metadata);
  int i = comm_index(comm);
  int local = i >= 0 && comms[i].rank == root;
  pthread_mutex_unlock(&metadata);
  return local;
}
DECLARE_REAL(Barrier, (MPI_Comm comm))
static int wrap_Barrier(Fn_Barrier real, MPI_Comm comm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_BARRIER, comm, 0, (1), -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Barrier, (MPI_Comm comm), comm)
DECLARE_REAL(Ibarrier, (MPI_Comm comm, MPI_Request *request))
static int wrap_Ibarrier(Fn_Ibarrier real, MPI_Comm comm,
                         MPI_Request *request) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_BARRIER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ibarrier, (MPI_Comm comm, MPI_Request *request), comm, request)
DECLARE_REAL(Bcast, (void *buffer, int count, MPI_Datatype type, int root,
                     MPI_Comm comm))
static int wrap_Bcast(Fn_Bcast real, void *buffer, int count, MPI_Datatype type,
                      int root, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_BCAST, comm, 0, (0), root, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(buffer, count, type, root, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Bcast,
             (void *buffer, int count, MPI_Datatype type, int root,
              MPI_Comm comm),
             buffer, count, type, root, comm)
DECLARE_REAL(Ibcast, (void *buffer, int count, MPI_Datatype type, int root,
                      MPI_Comm comm, MPI_Request *request))
static int wrap_Ibcast(Fn_Ibcast real, void *buffer, int count,
                       MPI_Datatype type, int root, MPI_Comm comm,
                       MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_BCAST, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, root, &m, &slot);
  int rc;
  NATIVE(rc, real(buffer, count, type, root, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ibcast,
             (void *buffer, int count, MPI_Datatype type, int root,
              MPI_Comm comm, MPI_Request *request),
             buffer, count, type, root, comm, request)
DECLARE_REAL(Allreduce, (const void *sendbuf, void *recvbuf, int count,
                         MPI_Datatype type, MPI_Op op, MPI_Comm comm))
static int wrap_Allreduce(Fn_Allreduce real, const void *sendbuf, void *recvbuf,
                          int count, MPI_Datatype type, MPI_Op op,
                          MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLREDUCE, comm, 0,
                         (count > 0 && type_nonzero(type)), -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Allreduce,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm),
             sendbuf, recvbuf, count, type, op, comm)
DECLARE_REAL(Iallreduce,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm, MPI_Request *request))
static int wrap_Iallreduce(Fn_Iallreduce real, const void *sendbuf,
                           void *recvbuf, int count, MPI_Datatype type,
                           MPI_Op op, MPI_Comm comm, MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLREDUCE, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iallreduce,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm, MPI_Request *request),
             sendbuf, recvbuf, count, type, op, comm, request)
DECLARE_REAL(Reduce, (const void *sendbuf, void *recvbuf, int count,
                      MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm))
static int wrap_Reduce(Fn_Reduce real, const void *sendbuf, void *recvbuf,
                       int count, MPI_Datatype type, MPI_Op op, int root,
                       MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_REDUCE, comm, 0, (0), root, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, root, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Reduce,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, int root, MPI_Comm comm),
             sendbuf, recvbuf, count, type, op, root, comm)
DECLARE_REAL(Ireduce,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, int root, MPI_Comm comm, MPI_Request *request))
static int wrap_Ireduce(Fn_Ireduce real, const void *sendbuf, void *recvbuf,
                        int count, MPI_Datatype type, MPI_Op op, int root,
                        MPI_Comm comm, MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_REDUCE, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, root, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, root, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ireduce,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, int root, MPI_Comm comm, MPI_Request *request),
             sendbuf, recvbuf, count, type, op, root, comm, request)
DECLARE_REAL(Allgather, (const void *sendbuf, int sendcount,
                         MPI_Datatype sendtype, void *recvbuf, int recvcount,
                         MPI_Datatype recvtype, MPI_Comm comm))
static int wrap_Allgather(Fn_Allgather real, const void *sendbuf, int sendcount,
                          MPI_Datatype sendtype, void *recvbuf, int recvcount,
                          MPI_Datatype recvtype, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLGATHER, comm, 0,
                         (recvcount > 0 && type_nonzero(recvtype) &&
                          (sendbuf == MPI_IN_PLACE ||
                           (sendcount > 0 && type_nonzero(sendtype)))),
                         -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Allgather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm)
DECLARE_REAL(Iallgather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm, MPI_Request *request))
static int wrap_Iallgather(Fn_Iallgather real, const void *sendbuf,
                           int sendcount, MPI_Datatype sendtype, void *recvbuf,
                           int recvcount, MPI_Datatype recvtype, MPI_Comm comm,
                           MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLGATHER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iallgather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm, MPI_Request *request),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm,
             request)
DECLARE_REAL(Allgatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, MPI_Comm comm))
static int wrap_Allgatherv(Fn_Allgatherv real, const void *sendbuf,
                           int sendcount, MPI_Datatype sendtype, void *recvbuf,
                           const int recvcounts[], const int displs[],
                           MPI_Datatype recvtype, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLGATHER, comm,
                         0 | PEAK_MPI_CAUSAL_VARIABLE_COUNT,
                         (positive_counts(recvcounts, comm_size_cached(comm)) &&
                          type_nonzero(recvtype) &&
                          (sendbuf == MPI_IN_PLACE ||
                           (sendcount > 0 && type_nonzero(sendtype)))),
                         -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                  recvtype, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Allgatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
             recvtype, comm)
DECLARE_REAL(Iallgatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request))
static int wrap_Iallgatherv(Fn_Iallgatherv real, const void *sendbuf,
                            int sendcount, MPI_Datatype sendtype, void *recvbuf,
                            const int recvcounts[], const int displs[],
                            MPI_Datatype recvtype, MPI_Comm comm,
                            MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLGATHER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING |
                             PEAK_MPI_CAUSAL_VARIABLE_COUNT,
                         0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                  recvtype, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iallgatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),
             sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
             recvtype, comm, request)
DECLARE_REAL(Alltoall, (const void *sendbuf, int sendcount,
                        MPI_Datatype sendtype, void *recvbuf, int recvcount,
                        MPI_Datatype recvtype, MPI_Comm comm))
static int wrap_Alltoall(Fn_Alltoall real, const void *sendbuf, int sendcount,
                         MPI_Datatype sendtype, void *recvbuf, int recvcount,
                         MPI_Datatype recvtype, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLTOALL, comm, 0,
                         (recvcount > 0 && type_nonzero(recvtype) &&
                          (sendbuf == MPI_IN_PLACE ||
                           (sendcount > 0 && type_nonzero(sendtype)))),
                         -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Alltoall,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm)
DECLARE_REAL(Ialltoall,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm, MPI_Request *request))
static int wrap_Ialltoall(Fn_Ialltoall real, const void *sendbuf, int sendcount,
                          MPI_Datatype sendtype, void *recvbuf, int recvcount,
                          MPI_Datatype recvtype, MPI_Comm comm,
                          MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLTOALL, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ialltoall,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm, MPI_Request *request),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm,
             request)
DECLARE_REAL(Alltoallv,
             (const void *sendbuf, const int sendcounts[], const int sdispls[],
              MPI_Datatype sendtype, void *recvbuf, const int recvcounts[],
              const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm))
static int wrap_Alltoallv(Fn_Alltoallv real, const void *sendbuf,
                          const int sendcounts[], const int sdispls[],
                          MPI_Datatype sendtype, void *recvbuf,
                          const int recvcounts[], const int rdispls[],
                          MPI_Datatype recvtype, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(
        PEAK_MPI_CAUSAL_ALLTOALL, comm, 0,
        (positive_counts(recvcounts, comm_size_cached(comm)) &&
         type_nonzero(recvtype) &&
         (sendbuf == MPI_IN_PLACE ||
          (positive_counts(sendcounts, comm_size_cached(comm)) &&
           type_nonzero(sendtype)))),
        -1, &m, &slot);
  /* This variable-count family is not an epoch recovery witness. */
  m.recovery_operation = 0;
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
                  rdispls, recvtype, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Alltoallv,
             (const void *sendbuf, const int sendcounts[], const int sdispls[],
              MPI_Datatype sendtype, void *recvbuf, const int recvcounts[],
              const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm),
             sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
             rdispls, recvtype, comm)
DECLARE_REAL(Ialltoallv,
             (const void *sendbuf, const int sendcounts[], const int sdispls[],
              MPI_Datatype sendtype, void *recvbuf, const int recvcounts[],
              const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm,
              MPI_Request *request))
static int wrap_Ialltoallv(Fn_Ialltoallv real, const void *sendbuf,
                           const int sendcounts[], const int sdispls[],
                           MPI_Datatype sendtype, void *recvbuf,
                           const int recvcounts[], const int rdispls[],
                           MPI_Datatype recvtype, MPI_Comm comm,
                           MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(recvtype);
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_ALLTOALL, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING |
                             PEAK_MPI_CAUSAL_VARIABLE_COUNT,
                         0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
                  rdispls, recvtype, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ialltoallv,
             (const void *sendbuf, const int sendcounts[], const int sdispls[],
              MPI_Datatype sendtype, void *recvbuf, const int recvcounts[],
              const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm,
              MPI_Request *request),
             sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
             rdispls, recvtype, comm, request)
DECLARE_REAL(Reduce_scatter,
             (const void *sendbuf, void *recvbuf, const int recvcounts[],
              MPI_Datatype type, MPI_Op op, MPI_Comm comm))
static int wrap_Reduce_scatter(Fn_Reduce_scatter real, const void *sendbuf,
                               void *recvbuf, const int recvcounts[],
                               MPI_Datatype type, MPI_Op op, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_REDUCE_SCATTER, comm, 0,
                         (positive_counts(recvcounts, comm_size_cached(comm)) &&
                          type_nonzero(type)),
                         -1, &m, &slot);
  /* This variable-count family is not an epoch recovery witness. */
  m.recovery_operation = 0;
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, recvcounts, type, op, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Reduce_scatter,
             (const void *sendbuf, void *recvbuf, const int recvcounts[],
              MPI_Datatype type, MPI_Op op, MPI_Comm comm),
             sendbuf, recvbuf, recvcounts, type, op, comm)
DECLARE_REAL(Ireduce_scatter, (const void *sendbuf, void *recvbuf,
                               const int recvcounts[], MPI_Datatype type,
                               MPI_Op op, MPI_Comm comm, MPI_Request *request))
static int wrap_Ireduce_scatter(Fn_Ireduce_scatter real, const void *sendbuf,
                                void *recvbuf, const int recvcounts[],
                                MPI_Datatype type, MPI_Op op, MPI_Comm comm,
                                MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_REDUCE_SCATTER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, recvcounts, type, op, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ireduce_scatter,
             (const void *sendbuf, void *recvbuf, const int recvcounts[],
              MPI_Datatype type, MPI_Op op, MPI_Comm comm,
              MPI_Request *request),
             sendbuf, recvbuf, recvcounts, type, op, comm, request)
DECLARE_REAL(Reduce_scatter_block,
             (const void *sendbuf, void *recvbuf, int recvcount,
              MPI_Datatype type, MPI_Op op, MPI_Comm comm))
static int wrap_Reduce_scatter_block(Fn_Reduce_scatter_block real,
                                     const void *sendbuf, void *recvbuf,
                                     int recvcount, MPI_Datatype type,
                                     MPI_Op op, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_REDUCE_SCATTER, comm, 0,
                         (recvcount > 0 && type_nonzero(type)), -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, recvcount, type, op, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Reduce_scatter_block,
             (const void *sendbuf, void *recvbuf, int recvcount,
              MPI_Datatype type, MPI_Op op, MPI_Comm comm),
             sendbuf, recvbuf, recvcount, type, op, comm)
DECLARE_REAL(Ireduce_scatter_block,
             (const void *sendbuf, void *recvbuf, int recvcount,
              MPI_Datatype type, MPI_Op op, MPI_Comm comm,
              MPI_Request *request))
static int wrap_Ireduce_scatter_block(Fn_Ireduce_scatter_block real,
                                      const void *sendbuf, void *recvbuf,
                                      int recvcount, MPI_Datatype type,
                                      MPI_Op op, MPI_Comm comm,
                                      MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_REDUCE_SCATTER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, recvcount, type, op, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Ireduce_scatter_block,
             (const void *sendbuf, void *recvbuf, int recvcount,
              MPI_Datatype type, MPI_Op op, MPI_Comm comm,
              MPI_Request *request),
             sendbuf, recvbuf, recvcount, type, op, comm, request)
DECLARE_REAL(Gather, (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                      void *recvbuf, int recvcount, MPI_Datatype recvtype,
                      int root, MPI_Comm comm))
static int wrap_Gather(Fn_Gather real, const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf, int recvcount,
                       MPI_Datatype recvtype, int root, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
    if (root_is_local(comm, root))
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_GATHER, comm, 0, (0), root, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  root, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Gather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
              MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root,
             comm)
DECLARE_REAL(Igather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
              MPI_Comm comm, MPI_Request *request))
static int wrap_Igather(Fn_Igather real, const void *sendbuf, int sendcount,
                        MPI_Datatype sendtype, void *recvbuf, int recvcount,
                        MPI_Datatype recvtype, int root, MPI_Comm comm,
                        MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
    if (root_is_local(comm, root))
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_GATHER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, root, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  root, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Igather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
              MPI_Comm comm, MPI_Request *request),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root,
             comm, request)
DECLARE_REAL(Gatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, int root, MPI_Comm comm))
static int wrap_Gatherv(Fn_Gatherv real, const void *sendbuf, int sendcount,
                        MPI_Datatype sendtype, void *recvbuf,
                        const int recvcounts[], const int displs[],
                        MPI_Datatype recvtype, int root, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
    if (root_is_local(comm, root))
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_GATHER, comm,
                         0 | PEAK_MPI_CAUSAL_VARIABLE_COUNT, (0), root, &m,
                         &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                  recvtype, root, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Gatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, int root, MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
             recvtype, root, comm)
DECLARE_REAL(Igatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, int root, MPI_Comm comm,
              MPI_Request *request))
static int wrap_Igatherv(Fn_Igatherv real, const void *sendbuf, int sendcount,
                         MPI_Datatype sendtype, void *recvbuf,
                         const int recvcounts[], const int displs[],
                         MPI_Datatype recvtype, int root, MPI_Comm comm,
                         MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    if (sendbuf != MPI_IN_PLACE)
      validate_type(sendtype);
    if (root_is_local(comm, root))
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_GATHER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING |
                             PEAK_MPI_CAUSAL_VARIABLE_COUNT,
                         0, root, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                  recvtype, root, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Igatherv,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, const int recvcounts[], const int displs[],
              MPI_Datatype recvtype, int root, MPI_Comm comm,
              MPI_Request *request),
             sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
             recvtype, root, comm, request)
DECLARE_REAL(Scatter, (const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf, int recvcount,
                       MPI_Datatype recvtype, int root, MPI_Comm comm))
static int wrap_Scatter(Fn_Scatter real, const void *sendbuf, int sendcount,
                        MPI_Datatype sendtype, void *recvbuf, int recvcount,
                        MPI_Datatype recvtype, int root, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    if (root_is_local(comm, root))
      validate_type(sendtype);
    if (recvbuf != MPI_IN_PLACE)
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_SCATTER, comm, 0, (0), root, &m,
                         &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  root, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Scatter,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
              MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root,
             comm)
DECLARE_REAL(Iscatter,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
              MPI_Comm comm, MPI_Request *request))
static int wrap_Iscatter(Fn_Iscatter real, const void *sendbuf, int sendcount,
                         MPI_Datatype sendtype, void *recvbuf, int recvcount,
                         MPI_Datatype recvtype, int root, MPI_Comm comm,
                         MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    if (root_is_local(comm, root))
      validate_type(sendtype);
    if (recvbuf != MPI_IN_PLACE)
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_SCATTER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, root, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  root, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iscatter,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
              MPI_Comm comm, MPI_Request *request),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root,
             comm, request)
DECLARE_REAL(Scatterv,
             (const void *sendbuf, const int sendcounts[], const int displs[],
              MPI_Datatype sendtype, void *recvbuf, int recvcount,
              MPI_Datatype recvtype, int root, MPI_Comm comm))
static int wrap_Scatterv(Fn_Scatterv real, const void *sendbuf,
                         const int sendcounts[], const int displs[],
                         MPI_Datatype sendtype, void *recvbuf, int recvcount,
                         MPI_Datatype recvtype, int root, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    if (root_is_local(comm, root))
      validate_type(sendtype);
    if (recvbuf != MPI_IN_PLACE)
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_SCATTER, comm,
                         0 | PEAK_MPI_CAUSAL_VARIABLE_COUNT, (0), root, &m,
                         &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
                  recvtype, root, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Scatterv,
             (const void *sendbuf, const int sendcounts[], const int displs[],
              MPI_Datatype sendtype, void *recvbuf, int recvcount,
              MPI_Datatype recvtype, int root, MPI_Comm comm),
             sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
             recvtype, root, comm)
DECLARE_REAL(Iscatterv, (const void *sendbuf, const int sendcounts[],
                         const int displs[], MPI_Datatype sendtype,
                         void *recvbuf, int recvcount, MPI_Datatype recvtype,
                         int root, MPI_Comm comm, MPI_Request *request))
static int wrap_Iscatterv(Fn_Iscatterv real, const void *sendbuf,
                          const int sendcounts[], const int displs[],
                          MPI_Datatype sendtype, void *recvbuf, int recvcount,
                          MPI_Datatype recvtype, int root, MPI_Comm comm,
                          MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    if (root_is_local(comm, root))
      validate_type(sendtype);
    if (recvbuf != MPI_IN_PLACE)
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_SCATTER, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING |
                             PEAK_MPI_CAUSAL_VARIABLE_COUNT,
                         0, root, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
                  recvtype, root, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iscatterv,
             (const void *sendbuf, const int sendcounts[], const int displs[],
              MPI_Datatype sendtype, void *recvbuf, int recvcount,
              MPI_Datatype recvtype, int root, MPI_Comm comm,
              MPI_Request *request),
             sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
             recvtype, root, comm, request)
DECLARE_REAL(Scan, (const void *sendbuf, void *recvbuf, int count,
                    MPI_Datatype type, MPI_Op op, MPI_Comm comm))
static int wrap_Scan(Fn_Scan real, const void *sendbuf, void *recvbuf,
                     int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_SCAN, comm, 0, (0), -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Scan,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm),
             sendbuf, recvbuf, count, type, op, comm)
DECLARE_REAL(Iscan,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm, MPI_Request *request))
static int wrap_Iscan(Fn_Iscan real, const void *sendbuf, void *recvbuf,
                      int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm,
                      MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_SCAN, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iscan,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm, MPI_Request *request),
             sendbuf, recvbuf, count, type, op, comm, request)
DECLARE_REAL(Exscan, (const void *sendbuf, void *recvbuf, int count,
                      MPI_Datatype type, MPI_Op op, MPI_Comm comm))
static int wrap_Exscan(Fn_Exscan real, const void *sendbuf, void *recvbuf,
                       int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_EXSCAN, comm, 0, (0), -1, &m, &slot);
  uint64_t started = collective_timer(c);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, comm));
  if (observe)
    collective_end(c, &m, slot, rc, started, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Exscan,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm),
             sendbuf, recvbuf, count, type, op, comm)
DECLARE_REAL(Iexscan,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm, MPI_Request *request))
static int wrap_Iexscan(Fn_Iexscan real, const void *sendbuf, void *recvbuf,
                        int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm,
                        MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe)
    c = collective_begin(PEAK_MPI_CAUSAL_EXSCAN, comm,
                         PEAK_MPI_CAUSAL_NONBLOCKING, 0, -1, &m, &slot);
  int rc;
  NATIVE(rc, real(sendbuf, recvbuf, count, type, op, comm, request));
  if (observe)
    collective_end(c, &m, slot, rc, 0, request);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Iexscan,
             (const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
              MPI_Op op, MPI_Comm comm, MPI_Request *request),
             sendbuf, recvbuf, count, type, op, comm, request)

static Context *creation_begin(PeakMpiCausalOperation op, MPI_Comm comm,
                               unsigned flags, Comm *m, unsigned *slot,
                               uint64_t *lineage) {
  Context *c = collective_begin(op, comm, flags, 0, -1, m, slot);
  if (m->uid) {
    pthread_mutex_lock(&metadata);
    *lineage = ++comms[*slot].creation_ordinal;
    pthread_mutex_unlock(&metadata);
  }
  return c;
}
static void creation_end(Context *c, Comm *m, unsigned slot, int rc,
                         PeakMpiObserverRecord *d) {
  if (c)
    end_call(c, rc, d, m->size > 1 || m->inter, 0);
  if (m->uid) {
    pthread_mutex_lock(&metadata);
    if (comms[slot].active_collectives)
      --comms[slot].active_collectives;
    pthread_mutex_unlock(&metadata);
    put_comm(slot);
  }
}
DECLARE_REAL(Comm_dup, (MPI_Comm comm, MPI_Comm *newcomm))
static int wrap_Comm_dup(Fn_Comm_dup real, MPI_Comm comm, MPI_Comm *newcomm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  uint64_t ordinal = 0;
  Context *c = NULL;
  if (observe)
    c = creation_begin(PEAK_MPI_CAUSAL_COMM_DUP, comm, 0, &m, &slot, &ordinal);
  int rc;
  NATIVE(rc, real(comm, newcomm));
  if (observe && m.uid && rc == MPI_SUCCESS)
    define_comm(*newcomm, m.uid, ordinal, PEAK_MPI_CAUSAL_COMM_DUPLICATED, 0);
  if (observe)
    creation_end(c, &m, slot, rc, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_dup, (MPI_Comm comm, MPI_Comm *newcomm), comm, newcomm)
DECLARE_REAL(Comm_split, (MPI_Comm comm, int color, int key, MPI_Comm *newcomm))
static int wrap_Comm_split(Fn_Comm_split real, MPI_Comm comm, int color,
                           int key, MPI_Comm *newcomm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  uint64_t ordinal = 0;
  Context *c = NULL;
  if (observe)
    c = creation_begin(PEAK_MPI_CAUSAL_COMM_SPLIT, comm, 0, &m, &slot,
                       &ordinal);
  int rc;
  NATIVE(rc, real(comm, color, key, newcomm));
  if (observe && m.uid && rc == MPI_SUCCESS)
    define_comm(*newcomm, m.uid, ordinal, PEAK_MPI_CAUSAL_COMM_SPLIT_CREATED,
                color);
  if (observe)
    creation_end(c, &m, slot, rc, NULL);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_split, (MPI_Comm comm, int color, int key, MPI_Comm *newcomm),
             comm, color, key, newcomm)
DECLARE_REAL(Comm_idup,
             (MPI_Comm comm, MPI_Comm *newcomm, MPI_Request *request))
static int wrap_Comm_idup(Fn_Comm_idup real, MPI_Comm comm, MPI_Comm *newcomm,
                          MPI_Request *request) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  uint64_t ordinal = 0;
  Context *c = NULL;
  PeakMpiObserverRecord d = {0};
  if (observe)
    c = creation_begin(PEAK_MPI_CAUSAL_COMM_IDUP, comm,
                       PEAK_MPI_CAUSAL_NONBLOCKING, &m, &slot, &ordinal);
  int rc;
  NATIVE(rc, real(comm, newcomm, request));
  if (c && rc == MPI_SUCCESS) {
    if (*request == MPI_REQUEST_NULL)
      define_comm(*newcomm, m.uid, ordinal, PEAK_MPI_CAUSAL_COMM_IDUP_CREATED,
                  0);
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_COMM_IDUP,
                    PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_COLLECTIVE, 0,
                    0, 0, c->begin.collective_ordinal, c->begin.op_uid, 0, &d);
    pthread_mutex_lock(&metadata);
    int i = request_index(*request);
    if (i >= 0) {
      requests[i].idup_comm = *newcomm;
      requests[i].lineage_ordinal = ordinal;
    }
    d.collective_ordinal = c->begin.collective_ordinal;
    pthread_mutex_unlock(&metadata);
  }
  if (observe)
    creation_end(c, &m, slot, rc, &d);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_idup,
             (MPI_Comm comm, MPI_Comm *newcomm, MPI_Request *request), comm,
             newcomm, request)
DECLARE_REAL(Comm_free, (MPI_Comm * comm))
static int wrap_Comm_free(Fn_Comm_free real, MPI_Comm *comm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe && get_comm(*comm, &m, &slot, 0))
    c = begin_call(PEAK_MPI_CAUSAL_COMM_FREE, &m, 0, 0, 0, 0, 0);
  int rc;
  NATIVE(rc, real(comm));
  if (c)
    end_call(c, rc, NULL, 0, 0);
  if (observe && m.uid) {
    if (rc == MPI_SUCCESS) {
      pthread_mutex_lock(&metadata);
      comms[slot].retired = 1;
      pthread_mutex_unlock(&metadata);
    }
    put_comm(slot);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_free, (MPI_Comm * comm), comm)
DECLARE_REAL(Type_contiguous,
             (int count, MPI_Datatype oldtype, MPI_Datatype *newtype))
static int wrap_Type_contiguous(Fn_Type_contiguous real, int count,
                                MPI_Datatype oldtype, MPI_Datatype *newtype) {
  int observe = wrapper_enter();
  int rc;
  NATIVE(rc, real(count, oldtype, newtype));
  if (observe && rc == MPI_SUCCESS)
    remember_type(*newtype);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Type_contiguous,
             (int count, MPI_Datatype oldtype, MPI_Datatype *newtype), count,
             oldtype, newtype)
#if MPI_VERSION >= 4
DECLARE_REAL(Type_contiguous_c,
             (MPI_Count count, MPI_Datatype oldtype, MPI_Datatype *newtype))
static int wrap_Type_contiguous_c(Fn_Type_contiguous_c real, MPI_Count count,
                                  MPI_Datatype oldtype, MPI_Datatype *newtype) {
  int observe = wrapper_enter();
  int rc;
  NATIVE(rc, real(count, oldtype, newtype));
  if (observe && rc == MPI_SUCCESS)
    remember_type(*newtype);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Type_contiguous_c,
             (MPI_Count count, MPI_Datatype oldtype, MPI_Datatype *newtype),
             count, oldtype, newtype)
#endif
DECLARE_REAL(Type_commit, (MPI_Datatype * type))
static int wrap_Type_commit(Fn_Type_commit real, MPI_Datatype *type) {
  int observe = wrapper_enter();
  int rc;
  NATIVE(rc, real(type));
  if (observe && rc == MPI_SUCCESS)
    remember_type(*type);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Type_commit, (MPI_Datatype * type), type)
DECLARE_REAL(Type_free, (MPI_Datatype * type))
static int wrap_Type_free(Fn_Type_free real, MPI_Datatype *type) {
  int observe = wrapper_enter();
  MPI_Datatype old = *type;
  int rc;
  NATIVE(rc, real(type));
  if (observe && rc == MPI_SUCCESS) {
    pthread_mutex_lock(&metadata);
    for (unsigned i = 0; i < TYPE_CAP; ++i)
      if (types[i].occupied && types[i].handle == old)
        types[i].occupied = 0;
    pthread_mutex_unlock(&metadata);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Type_free, (MPI_Datatype * type), type)

/* Unsupported public communication families remain native calls. Their use
 * explicitly invalidates fine analysis instead of inventing dependencies. */

DECLARE_REAL(Win_create, (void *base, MPI_Aint size, int disp_unit,
                          MPI_Info info, MPI_Comm comm, MPI_Win *win))
static int wrap_Win_create(Fn_Win_create real, void *base, MPI_Aint size,
                           int disp_unit, MPI_Info info, MPI_Comm comm,
                           MPI_Win *win) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(base, size, disp_unit, info, comm, win));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Win_create,
             (void *base, MPI_Aint size, int disp_unit, MPI_Info info,
              MPI_Comm comm, MPI_Win *win),
             base, size, disp_unit, info, comm, win)

DECLARE_REAL(Win_allocate, (MPI_Aint size, int disp_unit, MPI_Info info,
                            MPI_Comm comm, void *baseptr, MPI_Win *win))
static int wrap_Win_allocate(Fn_Win_allocate real, MPI_Aint size, int disp_unit,
                             MPI_Info info, MPI_Comm comm, void *baseptr,
                             MPI_Win *win) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(size, disp_unit, info, comm, baseptr, win));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Win_allocate,
             (MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm,
              void *baseptr, MPI_Win *win),
             size, disp_unit, info, comm, baseptr, win)

DECLARE_REAL(Win_allocate_shared, (MPI_Aint size, int disp_unit, MPI_Info info,
                                   MPI_Comm comm, void *baseptr, MPI_Win *win))
static int wrap_Win_allocate_shared(Fn_Win_allocate_shared real, MPI_Aint size,
                                    int disp_unit, MPI_Info info, MPI_Comm comm,
                                    void *baseptr, MPI_Win *win) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(size, disp_unit, info, comm, baseptr, win));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Win_allocate_shared,
             (MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm,
              void *baseptr, MPI_Win *win),
             size, disp_unit, info, comm, baseptr, win)

DECLARE_REAL(Win_create_dynamic, (MPI_Info info, MPI_Comm comm, MPI_Win *win))
static int wrap_Win_create_dynamic(Fn_Win_create_dynamic real, MPI_Info info,
                                   MPI_Comm comm, MPI_Win *win) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(info, comm, win));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Win_create_dynamic, (MPI_Info info, MPI_Comm comm, MPI_Win *win),
             info, comm, win)

DECLARE_REAL(Intercomm_create,
             (MPI_Comm local_comm, int local_leader, MPI_Comm peer_comm,
              int remote_leader, int tag, MPI_Comm *newintercomm))
static int wrap_Intercomm_create(Fn_Intercomm_create real, MPI_Comm local_comm,
                                 int local_leader, MPI_Comm peer_comm,
                                 int remote_leader, int tag,
                                 MPI_Comm *newintercomm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(local_comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(local_comm, local_leader, peer_comm, remote_leader, tag,
                  newintercomm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Intercomm_create,
             (MPI_Comm local_comm, int local_leader, MPI_Comm peer_comm,
              int remote_leader, int tag, MPI_Comm *newintercomm),
             local_comm, local_leader, peer_comm, remote_leader, tag,
             newintercomm)

DECLARE_REAL(Comm_spawn, (const char *command, char *argv[], int maxprocs,
                          MPI_Info info, int root, MPI_Comm comm,
                          MPI_Comm *intercomm, int array_of_errcodes[]))
static int wrap_Comm_spawn(Fn_Comm_spawn real, const char *command,
                           char *argv[], int maxprocs, MPI_Info info, int root,
                           MPI_Comm comm, MPI_Comm *intercomm,
                           int array_of_errcodes[]) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(command, argv, maxprocs, info, root, comm, intercomm,
                  array_of_errcodes));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_spawn,
             (const char *command, char *argv[], int maxprocs, MPI_Info info,
              int root, MPI_Comm comm, MPI_Comm *intercomm,
              int array_of_errcodes[]),
             command, argv, maxprocs, info, root, comm, intercomm,
             array_of_errcodes)

DECLARE_REAL(Comm_connect, (const char *port_name, MPI_Info info, int root,
                            MPI_Comm comm, MPI_Comm *newcomm))
static int wrap_Comm_connect(Fn_Comm_connect real, const char *port_name,
                             MPI_Info info, int root, MPI_Comm comm,
                             MPI_Comm *newcomm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(port_name, info, root, comm, newcomm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_connect,
             (const char *port_name, MPI_Info info, int root, MPI_Comm comm,
              MPI_Comm *newcomm),
             port_name, info, root, comm, newcomm)

DECLARE_REAL(Comm_accept, (const char *port_name, MPI_Info info, int root,
                           MPI_Comm comm, MPI_Comm *newcomm))
static int wrap_Comm_accept(Fn_Comm_accept real, const char *port_name,
                            MPI_Info info, int root, MPI_Comm comm,
                            MPI_Comm *newcomm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(port_name, info, root, comm, newcomm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_accept,
             (const char *port_name, MPI_Info info, int root, MPI_Comm comm,
              MPI_Comm *newcomm),
             port_name, info, root, comm, newcomm)

DECLARE_REAL(Comm_split_type, (MPI_Comm comm, int split_type, int key,
                               MPI_Info info, MPI_Comm *newcomm))
static int wrap_Comm_split_type(Fn_Comm_split_type real, MPI_Comm comm,
                                int split_type, int key, MPI_Info info,
                                MPI_Comm *newcomm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  }
  int rc;
  NATIVE(rc, real(comm, split_type, key, info, newcomm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Comm_split_type,
             (MPI_Comm comm, int split_type, int key, MPI_Info info,
              MPI_Comm *newcomm),
             comm, split_type, key, info, newcomm)

DECLARE_REAL(Alltoallw,
             (const void *sendbuf, const int sendcounts[], const int sdispls[],
              const MPI_Datatype sendtypes[], void *recvbuf,
              const int recvcounts[], const int rdispls[],
              const MPI_Datatype recvtypes[], MPI_Comm comm))
static int wrap_Alltoallw(Fn_Alltoallw real, const void *sendbuf,
                          const int sendcounts[], const int sdispls[],
                          const MPI_Datatype sendtypes[], void *recvbuf,
                          const int recvcounts[], const int rdispls[],
                          const MPI_Datatype recvtypes[], MPI_Comm comm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 1))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  }
  int rc;
  NATIVE(rc, real(sendbuf, sendcounts, sdispls, sendtypes, recvbuf, recvcounts,
                  rdispls, recvtypes, comm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Alltoallw,
             (const void *sendbuf, const int sendcounts[], const int sdispls[],
              const MPI_Datatype sendtypes[], void *recvbuf,
              const int recvcounts[], const int rdispls[],
              const MPI_Datatype recvtypes[], MPI_Comm comm),
             sendbuf, sendcounts, sdispls, sendtypes, recvbuf, recvcounts,
             rdispls, recvtypes, comm)

DECLARE_REAL(Neighbor_allgather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm))
static int wrap_Neighbor_allgather(Fn_Neighbor_allgather real,
                                   const void *sendbuf, int sendcount,
                                   MPI_Datatype sendtype, void *recvbuf,
                                   int recvcount, MPI_Datatype recvtype,
                                   MPI_Comm comm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 0))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  comm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Neighbor_allgather,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm)

DECLARE_REAL(Neighbor_alltoall,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm))
static int wrap_Neighbor_alltoall(Fn_Neighbor_alltoall real,
                                  const void *sendbuf, int sendcount,
                                  MPI_Datatype sendtype, void *recvbuf,
                                  int recvcount, MPI_Datatype recvtype,
                                  MPI_Comm comm) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  if (observe) {
    if (get_comm(comm, &m, &slot, 0))
      put_comm(slot);
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
    peak_mpi_causal_runtime_note_peer(-1);
  }
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                  comm));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Neighbor_alltoall,
             (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
              void *recvbuf, int recvcount, MPI_Datatype recvtype,
              MPI_Comm comm),
             sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm)

#if MPI_VERSION >= 4
/* MPICH 4.0 published a mutable send buffer; 4.0.1 corrected it. */
#if defined(MPICH_NUMVERSION) && MPICH_NUMVERSION >= 40000000 && \
    MPICH_NUMVERSION < 40001300
#define PEAK_MPI_PSEND_CONST
#else
#define PEAK_MPI_PSEND_CONST const
#endif
DECLARE_REAL(Psend_init, (PEAK_MPI_PSEND_CONST void *buf, int partitions, MPI_Count count,
                          MPI_Datatype datatype, int dest, int tag,
                          MPI_Comm comm, MPI_Info info, MPI_Request *request))
static int wrap_Psend_init(Fn_Psend_init real, PEAK_MPI_PSEND_CONST void *buf, int partitions,
                           MPI_Count count, MPI_Datatype datatype, int dest,
                           int tag, MPI_Comm comm, MPI_Info info,
                           MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe)
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  int rc;
  NATIVE(rc, real(buf, partitions, count, datatype, dest, tag, comm, info,
                  request));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Psend_init,
             (PEAK_MPI_PSEND_CONST void *buf, int partitions, MPI_Count count,
              MPI_Datatype datatype, int dest, int tag, MPI_Comm comm,
              MPI_Info info, MPI_Request *request),
             buf, partitions, count, datatype, dest, tag, comm, info, request)
#undef PEAK_MPI_PSEND_CONST
DECLARE_REAL(Precv_init, (void *buf, int partitions, MPI_Count count,
                          MPI_Datatype datatype, int source, int tag,
                          MPI_Comm comm, MPI_Info info, MPI_Request *request))
static int wrap_Precv_init(Fn_Precv_init real, void *buf, int partitions,
                           MPI_Count count, MPI_Datatype datatype, int source,
                           int tag, MPI_Comm comm, MPI_Info info,
                           MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe)
    fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);
  int rc;
  NATIVE(rc, real(buf, partitions, count, datatype, source, tag, comm, info,
                  request));
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Precv_init,
             (void *buf, int partitions, MPI_Count count, MPI_Datatype datatype,
              int source, int tag, MPI_Comm comm, MPI_Info info,
              MPI_Request *request),
             buf, partitions, count, datatype, source, tag, comm, info, request)
#endif

DECLARE_REAL(Sendrecv_replace,
             (void *buf, int count, MPI_Datatype type, int dest, int sendtag,
              int source, int recvtag, MPI_Comm comm, MPI_Status *status))
static int wrap_Sendrecv_replace(Fn_Sendrecv_replace real, void *buf, int count,
                                 MPI_Datatype type, int dest, int sendtag,
                                 int source, int recvtag, MPI_Comm comm,
                                 MPI_Status *status) {
  int observe = wrapper_enter();
  if (observe) {
    if (dest != MPI_PROC_NULL || source != MPI_PROC_NULL)
      validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  int actual = 0;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    int peer = world_peer(&m, dest);
    note_peer(peer);
    actual = actual_peer(peer);
    record_capability(PEAK_MPI_CAUSAL_SENDRECV, 0, actual);
    c = begin_call(PEAK_MPI_CAUSAL_SENDRECV, &m, PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(sendtag), world_peer(&m, source), 1);
    if (c) {
      c->begin.actual_tag = normalized_tag(recvtag);
      publish_begin(c);
    }
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc,
         real(buf, count, type, dest, sendtag, source, recvtag, comm, native));
  PeakMpiObserverRecord d = {0};
  if (observe && m.uid && rc == MPI_SUCCESS)
    actual |= receive_status(&m, native, &d);
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Sendrecv_replace,
             (void *buf, int count, MPI_Datatype type, int dest, int sendtag,
              int source, int recvtag, MPI_Comm comm, MPI_Status *status),
             buf, count, type, dest, sendtag, source, recvtag, comm, status)

#if MPI_VERSION >= 4
BLOCK_SEND_TYPED(Send_c, PEAK_MPI_CAUSAL_SEND, MPI_Count)
NB_SEND_TYPED(Isend_c, PEAK_MPI_CAUSAL_SEND, MPI_Count)
BLOCK_SEND_TYPED(Ssend_c, PEAK_MPI_CAUSAL_SSEND, MPI_Count)
NB_SEND_TYPED(Issend_c, PEAK_MPI_CAUSAL_SSEND, MPI_Count)
BLOCK_SEND_TYPED(Bsend_c, PEAK_MPI_CAUSAL_BSEND, MPI_Count)
NB_SEND_TYPED(Ibsend_c, PEAK_MPI_CAUSAL_BSEND, MPI_Count)
BLOCK_SEND_TYPED(Rsend_c, PEAK_MPI_CAUSAL_RSEND, MPI_Count)
NB_SEND_TYPED(Irsend_c, PEAK_MPI_CAUSAL_RSEND, MPI_Count)
DECLARE_REAL(Recv_c, (void *buf, MPI_Count count, MPI_Datatype type, int source,
                      int tag, MPI_Comm comm, MPI_Status *status))
static int wrap_Recv_c(Fn_Recv_c real, void *buf, MPI_Count count,
                       MPI_Datatype type, int source, int tag, MPI_Comm comm,
                       MPI_Status *status) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    if (source != MPI_PROC_NULL)
      validate_type(type);
    int peer = world_peer(&m, source);
    note_peer(peer);
    record_capability(PEAK_MPI_CAUSAL_RECV, 0, actual_peer(peer));
    c = begin_call(PEAK_MPI_CAUSAL_RECV, &m, PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(tag), -1, 0);
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(buf, count, type, source, tag, comm, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS) {
    actual = receive_status(&m, native, &d);
    record_capability(PEAK_MPI_CAUSAL_RECV, 0, actual);
  }
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Recv_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int source,
              int tag, MPI_Comm comm, MPI_Status *status),
             buf, count, type, source, tag, comm, status)

DECLARE_REAL(Irecv_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int source,
              int tag, MPI_Comm comm, MPI_Request *request))
static int wrap_Irecv_c(Fn_Irecv_c real, void *buf, MPI_Count count,
                        MPI_Datatype type, int source, int tag, MPI_Comm comm,
                        MPI_Request *request) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  int peer = PEAK_MPI_CAUSAL_UNKNOWN_PEER;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    if (source != MPI_PROC_NULL)
      validate_type(type);
    peer = world_peer(&m, source);
    note_peer(peer);
    if (peer == PEAK_MPI_CAUSAL_ANY_SOURCE &&
        !peak_mpi_causal_runtime_metadata_enabled())
      peak_mpi_causal_runtime_note_peer(-1);
    record_capability(PEAK_MPI_CAUSAL_RECV, PEAK_MPI_CAUSAL_NONBLOCKING,
                      actual_peer(peer));
    c = begin_call(PEAK_MPI_CAUSAL_RECV, &m,
                   PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(tag), -1, 0);
  }
  int rc;
  NATIVE(rc, real(buf, count, type, source, tag, comm, request));
  PeakMpiObserverRecord d = {0};
  if (c && rc == MPI_SUCCESS && request)
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_RECV,
                    PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_RECEIVE, peer,
                    normalized_tag(tag), -1, 0, c->begin.op_uid, 0, &d);
  if (c)
    end_call(c, rc, &d, actual_peer(peer), 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Irecv_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int source,
              int tag, MPI_Comm comm, MPI_Request *request),
             buf, count, type, source, tag, comm, request)

#endif

void peak_mpi_causal_events_calibrate_work(PeakMpiCausalWorkPrices *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  const unsigned iterations = 20000;
  volatile uint64_t sample[64];
  volatile uint64_t sink = 0;
  for (unsigned i = 0; i < 64; ++i)
    sample[i] = i + 1;
  uint64_t start = monotonic_ns();
  for (unsigned i = 0; i < iterations; ++i) {
    pthread_mutex_lock(&metadata);
    pthread_mutex_unlock(&metadata);
    peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, 0);
  }
  double fixed = (double)(monotonic_ns() - start) / (double)iterations * 1e-9;
  out->lookup_fixed_seconds = fixed;
  start = monotonic_ns();
  for (unsigned i = 0; i < iterations; ++i) {
    pthread_mutex_lock(&metadata);
    for (unsigned j = 0; j < 64; ++j)
      sink += sample[j];
    pthread_mutex_unlock(&metadata);
    peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_LOOKUP, 64);
  }
  double full = (double)(monotonic_ns() - start) / (double)iterations * 1e-9;
  out->seconds[PEAK_MPI_CAUSAL_WORK_LOOKUP] =
      (full > fixed ? full - fixed : 1e-9) / 64.;
  start = monotonic_ns();
  for (unsigned i = 0; i < iterations; ++i) {
    pthread_mutex_lock(&metadata);
    sink += group_active;
    pthread_mutex_unlock(&metadata);
    peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_POLL, 1);
  }
  out->seconds[PEAK_MPI_CAUSAL_WORK_POLL] =
      (double)(monotonic_ns() - start) / (double)iterations * 1e-9;
  Request representative = {0};
  representative.generation = 1;
  volatile Request copied;
  start = monotonic_ns();
  for (unsigned i = 0; i < iterations; ++i) {
    copied = representative;
    sink += copied.generation;
    peak_mpi_causal_runtime_observer_work(PEAK_MPI_CAUSAL_WORK_COMPLETION, 1);
  }
  out->seconds[PEAK_MPI_CAUSAL_WORK_COMPLETION] =
      (double)(monotonic_ns() - start) / (double)iterations * 1e-9;
  (void)sink;
}

#if MPI_VERSION >= 4
PERSISTENT_SEND_TYPED(Send_init_c, PEAK_MPI_CAUSAL_SEND, MPI_Count)
PERSISTENT_SEND_TYPED(Ssend_init_c, PEAK_MPI_CAUSAL_SSEND, MPI_Count)
PERSISTENT_SEND_TYPED(Bsend_init_c, PEAK_MPI_CAUSAL_BSEND, MPI_Count)
PERSISTENT_SEND_TYPED(Rsend_init_c, PEAK_MPI_CAUSAL_RSEND, MPI_Count)
DECLARE_REAL(Recv_init_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int source,
              int tag, MPI_Comm comm, MPI_Request *request))
static int wrap_Recv_init_c(Fn_Recv_init_c real, void *buf, MPI_Count count,
                            MPI_Datatype type, int source, int tag,
                            MPI_Comm comm, MPI_Request *request) {
  int observe = wrapper_enter();
  Comm m = {0};
  unsigned slot = 0;
  int known = observe && get_comm(comm, &m, &slot, 0);
  if (known && source != MPI_PROC_NULL)
    validate_type(type);
  int rc;
  NATIVE(rc, real(buf, count, type, source, tag, comm, request));
  if (known && peak_mpi_causal_runtime_metadata_enabled() &&
      rc == MPI_SUCCESS && request) {
    PeakMpiObserverRecord d = {0};
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_RECV,
                    PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_PERSISTENT |
                        PEAK_MPI_CAUSAL_RECEIVE,
                    world_peer(&m, source), normalized_tag(tag), -1, 0, 0, 1,
                    &d);
  }
  if (known)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Recv_init_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int source,
              int tag, MPI_Comm comm, MPI_Request *request),
             buf, count, type, source, tag, comm, request)

#endif

#if MPI_VERSION >= 4
DECLARE_REAL(Sendrecv_c,
             (const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype,
              int dest, int sendtag, void *recvbuf, MPI_Count recvcount,
              MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm,
              MPI_Status *status))
static int wrap_Sendrecv_c(Fn_Sendrecv_c real, const void *sendbuf,
                           MPI_Count sendcount, MPI_Datatype sendtype, int dest,
                           int sendtag, void *recvbuf, MPI_Count recvcount,
                           MPI_Datatype recvtype, int source, int recvtag,
                           MPI_Comm comm, MPI_Status *status) {
  int observe = wrapper_enter();
  if (observe) {
    if (dest != MPI_PROC_NULL)
      validate_type(sendtype);
    if (source != MPI_PROC_NULL)
      validate_type(recvtype);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  int actual = 0;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    int peer = world_peer(&m, dest);
    note_peer(peer);
    actual = actual_peer(peer);
    record_capability(PEAK_MPI_CAUSAL_SENDRECV, 0, actual);
    c = begin_call(PEAK_MPI_CAUSAL_SENDRECV, &m, PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(sendtag), world_peer(&m, source), 1);
    if (c) {
      c->begin.actual_tag = normalized_tag(recvtag);
      publish_begin(c);
    }
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(sendbuf, sendcount, sendtype, dest, sendtag, recvbuf,
                  recvcount, recvtype, source, recvtag, comm, native));
  PeakMpiObserverRecord d = {0};
  if (observe && m.uid && rc == MPI_SUCCESS)
    actual |= receive_status(&m, native, &d);
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Sendrecv_c,
             (const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype,
              int dest, int sendtag, void *recvbuf, MPI_Count recvcount,
              MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm,
              MPI_Status *status),
             sendbuf, sendcount, sendtype, dest, sendtag, recvbuf, recvcount,
             recvtype, source, recvtag, comm, status)

DECLARE_REAL(Mrecv_c, (void *buf, MPI_Count count, MPI_Datatype type,
                       MPI_Message *message, MPI_Status *status))
static int wrap_Mrecv_c(Fn_Mrecv_c real, void *buf, MPI_Count count,
                        MPI_Datatype type, MPI_Message *message,
                        MPI_Status *status) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Message msg = {0};
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  if (observe && message && take_message(*message, &msg, &m, &slot)) {
    c = begin_call(PEAK_MPI_CAUSAL_MRECV, &m, PEAK_MPI_CAUSAL_RECEIVE, msg.peer,
                   msg.tag, -1, 1);
    if (c) {
      c->begin.request_uid = msg.uid;
      c->begin.progress_begin = msg.probe_uid;
      publish_begin(c);
    }
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc, real(buf, count, type, message, native));
  PeakMpiObserverRecord d = {0};
  int actual = 0;
  if (observe && m.uid && rc == MPI_SUCCESS)
    actual = receive_status(&m, native, &d);
  d.request_uid = msg.uid;
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Mrecv_c,
             (void *buf, MPI_Count count, MPI_Datatype type,
              MPI_Message *message, MPI_Status *status),
             buf, count, type, message, status)
DECLARE_REAL(Imrecv_c, (void *buf, MPI_Count count, MPI_Datatype type,
                        MPI_Message *message, MPI_Request *request))
static int wrap_Imrecv_c(Fn_Imrecv_c real, void *buf, MPI_Count count,
                         MPI_Datatype type, MPI_Message *message,
                         MPI_Request *request) {
  int observe = wrapper_enter();
  if (observe) {
    validate_type(type);
  }
  Message msg = {0};
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  if (observe && message && take_message(*message, &msg, &m, &slot)) {
    c = begin_call(PEAK_MPI_CAUSAL_MRECV, &m,
                   PEAK_MPI_CAUSAL_RECEIVE | PEAK_MPI_CAUSAL_NONBLOCKING,
                   msg.peer, msg.tag, -1, 1);
    if (c) {
      c->begin.request_uid = msg.uid;
      c->begin.progress_begin = msg.probe_uid;
      publish_begin(c);
    }
  }
  int rc;
  NATIVE(rc, real(buf, count, type, message, request));
  PeakMpiObserverRecord d = {0};
  if (c && rc == MPI_SUCCESS && request)
    install_request(*request, &m, slot, PEAK_MPI_CAUSAL_MRECV,
                    PEAK_MPI_CAUSAL_RECEIVE | PEAK_MPI_CAUSAL_NONBLOCKING,
                    msg.peer, msg.tag, -1, 0, c->begin.op_uid, 0, &d);
  if (c)
    end_call(c, rc, &d, actual_peer(msg.peer), 0);
  if (m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Imrecv_c,
             (void *buf, MPI_Count count, MPI_Datatype type,
              MPI_Message *message, MPI_Request *request),
             buf, count, type, message, request)

DECLARE_REAL(Buffer_attach_c, (void *buffer, MPI_Count size))
static int wrap_Buffer_attach_c(Fn_Buffer_attach_c real, void *buffer,
                                MPI_Count size) {
  int observe = wrapper_enter();
  int rc;
  NATIVE(rc, real(buffer, size));
  if (observe && rc == MPI_SUCCESS) {
    pthread_mutex_lock(&metadata);
    if (buffer_generation == UINT64_MAX)
      fail(PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY);
    else
      ++buffer_generation;
    pthread_mutex_unlock(&metadata);
  }
  if (observe) {
    PeakMpiObserverRecord cut = {0};
    peak_mpi_causal_runtime_boundary(&cut);
  }
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Buffer_attach_c, (void *buffer, MPI_Count size), buffer, size)
DECLARE_REAL(Sendrecv_replace_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int dest,
              int sendtag, int source, int recvtag, MPI_Comm comm,
              MPI_Status *status))
static int wrap_Sendrecv_replace_c(Fn_Sendrecv_replace_c real, void *buf,
                                   MPI_Count count, MPI_Datatype type, int dest,
                                   int sendtag, int source, int recvtag,
                                   MPI_Comm comm, MPI_Status *status) {
  int observe = wrapper_enter();
  if (observe) {
    if (dest != MPI_PROC_NULL || source != MPI_PROC_NULL)
      validate_type(type);
  }
  Comm m = {0};
  unsigned slot = 0;
  Context *c = NULL;
  MPI_Status scratch;
  MPI_Status *native = status;
  int actual = 0;
  if (observe && get_comm(comm, &m, &slot, 0)) {
    int peer = world_peer(&m, dest);
    note_peer(peer);
    actual = actual_peer(peer);
    record_capability(PEAK_MPI_CAUSAL_SENDRECV, 0, actual);
    c = begin_call(PEAK_MPI_CAUSAL_SENDRECV, &m, PEAK_MPI_CAUSAL_RECEIVE, peer,
                   normalized_tag(sendtag), world_peer(&m, source), 1);
    if (c) {
      c->begin.actual_tag = normalized_tag(recvtag);
      publish_begin(c);
    }
    if (status == MPI_STATUS_IGNORE)
      native = &scratch;
  }
  int rc;
  NATIVE(rc,
         real(buf, count, type, dest, sendtag, source, recvtag, comm, native));
  PeakMpiObserverRecord d = {0};
  if (observe && m.uid && rc == MPI_SUCCESS)
    actual |= receive_status(&m, native, &d);
  if (c)
    end_call(c, rc, &d, actual, 0);
  if (observe && m.uid)
    put_comm(slot);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Sendrecv_replace_c,
             (void *buf, MPI_Count count, MPI_Datatype type, int dest,
              int sendtag, int source, int recvtag, MPI_Comm comm,
              MPI_Status *status),
             buf, count, type, dest, sendtag, source, recvtag, comm, status)

DECLARE_REAL(Buffer_detach_c, (void *buffer, MPI_Count *size))
static int wrap_Buffer_detach_c(Fn_Buffer_detach_c real, void *buffer,
                                MPI_Count *size) {
  int observe = wrapper_enter();
  Context *c = observe ? begin_call(PEAK_MPI_CAUSAL_BUFFER_DETACH, NULL, 0, -1,
                                    -1, -1, 0)
                       : NULL;
  int rc;
  NATIVE(rc, real(buffer, size));
  if (c)
    end_call(c, rc, NULL, 0, 0);
  wrapper_leave();
  return rc;
}
FORWARD_BOTH(Buffer_detach_c, (void *buffer, MPI_Count *size), buffer, size)

#endif

/* Families outside the reviewed model keep their original public call and
 * explicitly invalidate fine capture. These are capability boundaries. */
#define PEAK_UNPAREN(...) __VA_ARGS__
#define PUBLIC_COARSE(name, params, args, discover)                            \
  DECLARE_REAL(name, params)                                                   \
  static int wrap_##name(Fn_##name real, PEAK_UNPAREN params) {                \
    int observe = wrapper_enter();                                             \
    if (observe) {                                                             \
      discover;                                                                \
      fail(PEAK_MPI_OBSERVER_REASON_CAPABILITY);                               \
    }                                                                          \
    int rc;                                                                    \
    NATIVE(rc, real args);                                                     \
    wrapper_leave();                                                           \
    return rc;                                                                 \
  }                                                                            \
  FORWARD_BOTH(name, params, PEAK_UNPAREN args)
#define PUBLIC_COARSE_COMM(name, params, args)                                 \
  PUBLIC_COARSE(name, params, args, Comm m = {0}; unsigned slot = 0;           \
                if (get_comm(comm, &m, &slot, 1)) put_comm(slot))
#define PUBLIC_COARSE_UNKNOWN(name, params, args)                              \
  PUBLIC_COARSE(name, params, args, peak_mpi_causal_runtime_note_peer(-1))
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allgather_init,
                   (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int recvcount, MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allgatherv_init,
                   (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, const int recvcounts[], const int displs[],
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allreduce_init,
                   (const void *sendbuf, void *recvbuf, int count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoall_init,
                   (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int recvcount, MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoallv_init,
                   (const void *sendbuf, const int sendcounts[],
                    const int sdispls[], MPI_Datatype sendtype, void *recvbuf,
                    const int recvcounts[], const int rdispls[],
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
                    rdispls, recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoallw_init,
                   (const void *sendbuf, const int sendcounts[],
                    const int sdispls[], const MPI_Datatype sendtypes[],
                    void *recvbuf, const int recvcounts[], const int rdispls[],
                    const MPI_Datatype recvtypes[], MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                    recvcounts, rdispls, recvtypes, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Barrier_init,
                   (MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Bcast_init,
                   (void *buffer, int count, MPI_Datatype datatype, int root,
                    MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (buffer, count, datatype, root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Exscan_init,
                   (const void *sendbuf, void *recvbuf, int count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Gather_init,
                   (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int recvcount, MPI_Datatype recvtype,
                    int root, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Gatherv_init,
                   (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, const int recvcounts[], const int displs[],
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, root, comm, info, request))
#endif
PUBLIC_COARSE_UNKNOWN(
    Ineighbor_allgather,
    (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf,
     int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),
    (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, request))
PUBLIC_COARSE_UNKNOWN(Ineighbor_allgatherv,
                      (const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       const int recvcounts[], const int displs[],
                       MPI_Datatype recvtype, MPI_Comm comm,
                       MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                       displs, recvtype, comm, request))
PUBLIC_COARSE_UNKNOWN(
    Ineighbor_alltoall,
    (const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf,
     int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),
    (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, request))
PUBLIC_COARSE_UNKNOWN(Ineighbor_alltoallv,
                      (const void *sendbuf, const int sendcounts[],
                       const int sdispls[], MPI_Datatype sendtype,
                       void *recvbuf, const int recvcounts[],
                       const int rdispls[], MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                       recvcounts, rdispls, recvtype, comm, request))
PUBLIC_COARSE_UNKNOWN(Ineighbor_alltoallw,
                      (const void *sendbuf, const int sendcounts[],
                       const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                       void *recvbuf, const int recvcounts[],
                       const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                       MPI_Comm comm, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                       recvcounts, rdispls, recvtypes, comm, request))
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_allgather_init,
                      (const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf, int recvcount,
                       MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                       MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, comm, info, request))
#endif
PUBLIC_COARSE_UNKNOWN(Neighbor_allgatherv,
                      (const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       const int recvcounts[], const int displs[],
                       MPI_Datatype recvtype, MPI_Comm comm),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                       displs, recvtype, comm))
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_allgatherv_init,
                      (const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       const int recvcounts[], const int displs[],
                       MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                       MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                       displs, recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoall_init,
                      (const void *sendbuf, int sendcount,
                       MPI_Datatype sendtype, void *recvbuf, int recvcount,
                       MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                       MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, comm, info, request))
#endif
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallv,
                      (const void *sendbuf, const int sendcounts[],
                       const int sdispls[], MPI_Datatype sendtype,
                       void *recvbuf, const int recvcounts[],
                       const int rdispls[], MPI_Datatype recvtype,
                       MPI_Comm comm),
                      (sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                       recvcounts, rdispls, recvtype, comm))
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallv_init,
                      (const void *sendbuf, const int sendcounts[],
                       const int sdispls[], MPI_Datatype sendtype,
                       void *recvbuf, const int recvcounts[],
                       const int rdispls[], MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Info info, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                       recvcounts, rdispls, recvtype, comm, info, request))
#endif
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallw,
                      (const void *sendbuf, const int sendcounts[],
                       const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                       void *recvbuf, const int recvcounts[],
                       const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                       MPI_Comm comm),
                      (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                       recvcounts, rdispls, recvtypes, comm))
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallw_init,
                      (const void *sendbuf, const int sendcounts[],
                       const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                       void *recvbuf, const int recvcounts[],
                       const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                       MPI_Comm comm, MPI_Info info, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                       recvcounts, rdispls, recvtypes, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_init,
                   (const void *sendbuf, void *recvbuf, int count,
                    MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, root, comm, info,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_scatter_block_init,
                   (const void *sendbuf, void *recvbuf, int recvcount,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, recvcount, datatype, op, comm, info,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_scatter_init,
                   (const void *sendbuf, void *recvbuf, const int recvcounts[],
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, recvcounts, datatype, op, comm, info,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scan_init,
                   (const void *sendbuf, void *recvbuf, int count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scatter_init,
                   (const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int recvcount, MPI_Datatype recvtype,
                    int root, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scatterv_init,
                   (const void *sendbuf, const int sendcounts[],
                    const int displs[], MPI_Datatype sendtype, void *recvbuf,
                    int recvcount, MPI_Datatype recvtype, int root,
                    MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
                    recvtype, root, comm, info, request))
#endif
PUBLIC_COARSE_COMM(Comm_create,
                   (MPI_Comm comm, MPI_Group group, MPI_Comm *newcomm),
                   (comm, group, newcomm))
PUBLIC_COARSE_UNKNOWN(Comm_create_group,
                      (MPI_Comm comm, MPI_Group group, int tag,
                       MPI_Comm *newcomm),
                      (comm, group, tag, newcomm))
PUBLIC_COARSE_COMM(Comm_dup_with_info,
                   (MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm),
                   (comm, info, newcomm))
PUBLIC_COARSE_COMM(Cart_sub,
                   (MPI_Comm comm, const int remain_dims[], MPI_Comm *newcomm),
                   (comm, remain_dims, newcomm))
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allgather_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, MPI_Comm comm),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allgather_init_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allgatherv_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf,
                    const MPI_Count recvcounts[], const MPI_Aint displs[],
                    MPI_Datatype recvtype, MPI_Comm comm),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allgatherv_init_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf,
                    const MPI_Count recvcounts[], const MPI_Aint displs[],
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allreduce_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),
                   (sendbuf, recvbuf, count, datatype, op, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Allreduce_init_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoall_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, MPI_Comm comm),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoall_init_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoallv_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint sdispls[], MPI_Datatype sendtype,
                    void *recvbuf, const MPI_Count recvcounts[],
                    const MPI_Aint rdispls[], MPI_Datatype recvtype,
                    MPI_Comm comm),
                   (sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
                    rdispls, recvtype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoallv_init_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint sdispls[], MPI_Datatype sendtype,
                    void *recvbuf, const MPI_Count recvcounts[],
                    const MPI_Aint rdispls[], MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
                    rdispls, recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoallw_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                    void *recvbuf, const MPI_Count recvcounts[],
                    const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                    MPI_Comm comm),
                   (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                    recvcounts, rdispls, recvtypes, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Alltoallw_init_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                    void *recvbuf, const MPI_Count recvcounts[],
                    const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                    MPI_Comm comm, MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                    recvcounts, rdispls, recvtypes, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Bcast_c,
                   (void *buffer, MPI_Count count, MPI_Datatype datatype,
                    int root, MPI_Comm comm),
                   (buffer, count, datatype, root, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Bcast_init_c,
                   (void *buffer, MPI_Count count, MPI_Datatype datatype,
                    int root, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (buffer, count, datatype, root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Exscan_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),
                   (sendbuf, recvbuf, count, datatype, op, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Exscan_init_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Gather_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, int root, MPI_Comm comm),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Gather_init_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Gatherv_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf,
                    const MPI_Count recvcounts[], const MPI_Aint displs[],
                    MPI_Datatype recvtype, int root, MPI_Comm comm),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, root, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Gatherv_init_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf,
                    const MPI_Count recvcounts[], const MPI_Aint displs[],
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iallgather_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iallgatherv_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf,
                    const MPI_Count recvcounts[], const MPI_Aint displs[],
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iallreduce_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ialltoall_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ialltoallv_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint sdispls[], MPI_Datatype sendtype,
                    void *recvbuf, const MPI_Count recvcounts[],
                    const MPI_Aint rdispls[], MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts,
                    rdispls, recvtype, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ialltoallw_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                    void *recvbuf, const MPI_Count recvcounts[],
                    const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                    MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                    recvcounts, rdispls, recvtypes, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ibcast_c,
                   (void *buffer, MPI_Count count, MPI_Datatype datatype,
                    int root, MPI_Comm comm, MPI_Request *request),
                   (buffer, count, datatype, root, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iexscan_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Igather_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Igatherv_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf,
                    const MPI_Count recvcounts[], const MPI_Aint displs[],
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs,
                    recvtype, root, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Ineighbor_allgather_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       MPI_Count recvcount, MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Ineighbor_allgatherv_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       const MPI_Count recvcounts[], const MPI_Aint displs[],
                       MPI_Datatype recvtype, MPI_Comm comm,
                       MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                       displs, recvtype, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Ineighbor_alltoall_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       MPI_Count recvcount, MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Ineighbor_alltoallv_c,
                      (const void *sendbuf, const MPI_Count sendcounts[],
                       const MPI_Aint sdispls[], MPI_Datatype sendtype,
                       void *recvbuf, const MPI_Count recvcounts[],
                       const MPI_Aint rdispls[], MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                       recvcounts, rdispls, recvtype, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Ineighbor_alltoallw_c,
                      (const void *sendbuf, const MPI_Count sendcounts[],
                       const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                       void *recvbuf, const MPI_Count recvcounts[],
                       const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                       MPI_Comm comm, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                       recvcounts, rdispls, recvtypes, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ireduce_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, root, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ireduce_scatter_c,
                   (const void *sendbuf, void *recvbuf,
                    const MPI_Count recvcounts[], MPI_Datatype datatype,
                    MPI_Op op, MPI_Comm comm, MPI_Request *request),
                   (sendbuf, recvbuf, recvcounts, datatype, op, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Ireduce_scatter_block_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, recvbuf, recvcount, datatype, op, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iscan_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iscatter_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Iscatterv_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint displs[], MPI_Datatype sendtype,
                    void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype,
                    int root, MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
                    recvtype, root, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(
    Neighbor_allgather_c,
    (const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype,
     void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm),
    (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_allgather_init_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       MPI_Count recvcount, MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Info info, MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_allgatherv_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       const MPI_Count recvcounts[], const MPI_Aint displs[],
                       MPI_Datatype recvtype, MPI_Comm comm),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                       displs, recvtype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_allgatherv_init_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       const MPI_Count recvcounts[], const MPI_Aint displs[],
                       MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info,
                       MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcounts,
                       displs, recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(
    Neighbor_alltoall_c,
    (const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype,
     void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm),
    (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoall_init_c,
                      (const void *sendbuf, MPI_Count sendcount,
                       MPI_Datatype sendtype, void *recvbuf,
                       MPI_Count recvcount, MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Info info, MPI_Request *request),
                      (sendbuf, sendcount, sendtype, recvbuf, recvcount,
                       recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallv_c,
                      (const void *sendbuf, const MPI_Count sendcounts[],
                       const MPI_Aint sdispls[], MPI_Datatype sendtype,
                       void *recvbuf, const MPI_Count recvcounts[],
                       const MPI_Aint rdispls[], MPI_Datatype recvtype,
                       MPI_Comm comm),
                      (sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                       recvcounts, rdispls, recvtype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallv_init_c,
                      (const void *sendbuf, const MPI_Count sendcounts[],
                       const MPI_Aint sdispls[], MPI_Datatype sendtype,
                       void *recvbuf, const MPI_Count recvcounts[],
                       const MPI_Aint rdispls[], MPI_Datatype recvtype,
                       MPI_Comm comm, MPI_Info info, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtype, recvbuf,
                       recvcounts, rdispls, recvtype, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallw_c,
                      (const void *sendbuf, const MPI_Count sendcounts[],
                       const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                       void *recvbuf, const MPI_Count recvcounts[],
                       const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                       MPI_Comm comm),
                      (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                       recvcounts, rdispls, recvtypes, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_UNKNOWN(Neighbor_alltoallw_init_c,
                      (const void *sendbuf, const MPI_Count sendcounts[],
                       const MPI_Aint sdispls[], const MPI_Datatype sendtypes[],
                       void *recvbuf, const MPI_Count recvcounts[],
                       const MPI_Aint rdispls[], const MPI_Datatype recvtypes[],
                       MPI_Comm comm, MPI_Info info, MPI_Request *request),
                      (sendbuf, sendcounts, sdispls, sendtypes, recvbuf,
                       recvcounts, rdispls, recvtypes, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm),
                   (sendbuf, recvbuf, count, datatype, op, root, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_init_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, root, comm, info,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_scatter_c,
                   (const void *sendbuf, void *recvbuf,
                    const MPI_Count recvcounts[], MPI_Datatype datatype,
                    MPI_Op op, MPI_Comm comm),
                   (sendbuf, recvbuf, recvcounts, datatype, op, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_scatter_block_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),
                   (sendbuf, recvbuf, recvcount, datatype, op, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_scatter_block_init_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, recvcount, datatype, op, comm, info,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Reduce_scatter_init_c,
                   (const void *sendbuf, void *recvbuf,
                    const MPI_Count recvcounts[], MPI_Datatype datatype,
                    MPI_Op op, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, recvbuf, recvcounts, datatype, op, comm, info,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scan_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),
                   (sendbuf, recvbuf, count, datatype, op, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scan_init_c,
                   (const void *sendbuf, void *recvbuf, MPI_Count count,
                    MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, recvbuf, count, datatype, op, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scatter_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, int root, MPI_Comm comm),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scatter_init_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount,
                    MPI_Datatype recvtype, int root, MPI_Comm comm,
                    MPI_Info info, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                    root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scatterv_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint displs[], MPI_Datatype sendtype,
                    void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype,
                    int root, MPI_Comm comm),
                   (sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
                    recvtype, root, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Scatterv_init_c,
                   (const void *sendbuf, const MPI_Count sendcounts[],
                    const MPI_Aint displs[], MPI_Datatype sendtype,
                    void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype,
                    int root, MPI_Comm comm, MPI_Info info,
                    MPI_Request *request),
                   (sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount,
                    recvtype, root, comm, info, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Pack_c,
                   (const void *inbuf, MPI_Count incount, MPI_Datatype datatype,
                    void *outbuf, MPI_Count outsize, MPI_Count *position,
                    MPI_Comm comm),
                   (inbuf, incount, datatype, outbuf, outsize, position, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Pack_size_c,
                   (MPI_Count incount, MPI_Datatype datatype, MPI_Comm comm,
                    MPI_Count *size),
                   (incount, datatype, comm, size))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Unpack_c,
                   (const void *inbuf, MPI_Count insize, MPI_Count *position,
                    void *outbuf, MPI_Count outcount, MPI_Datatype datatype,
                    MPI_Comm comm),
                   (inbuf, insize, position, outbuf, outcount, datatype, comm))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Isendrecv_c,
                   (const void *sendbuf, MPI_Count sendcount,
                    MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf,
                    MPI_Count recvcount, MPI_Datatype recvtype, int source,
                    int recvtag, MPI_Comm comm, MPI_Request *request),
                   (sendbuf, sendcount, sendtype, dest, sendtag, recvbuf,
                    recvcount, recvtype, source, recvtag, comm, request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Isendrecv_replace_c,
                   (void *buf, MPI_Count count, MPI_Datatype datatype, int dest,
                    int sendtag, int source, int recvtag, MPI_Comm comm,
                    MPI_Request *request),
                   (buf, count, datatype, dest, sendtag, source, recvtag, comm,
                    request))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Win_allocate_c,
                   (MPI_Aint size, MPI_Aint disp_unit, MPI_Info info,
                    MPI_Comm comm, void *baseptr, MPI_Win *win),
                   (size, disp_unit, info, comm, baseptr, win))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Win_allocate_shared_c,
                   (MPI_Aint size, MPI_Aint disp_unit, MPI_Info info,
                    MPI_Comm comm, void *baseptr, MPI_Win *win),
                   (size, disp_unit, info, comm, baseptr, win))
#endif
#if MPI_VERSION >= 4
PUBLIC_COARSE_COMM(Win_create_c,
                   (void *base, MPI_Aint size, MPI_Aint disp_unit,
                    MPI_Info info, MPI_Comm comm, MPI_Win *win),
                   (base, size, disp_unit, info, comm, win))
#endif

/* Unsupported collective I/O and external membership use the existing
 * bounded conservative entry path; their native calls remain unchanged. */
PUBLIC_COARSE_COMM(File_open,
                   (MPI_Comm comm, const char *filename, int amode,
                    MPI_Info info, MPI_File *fh),
                   (comm, filename, amode, info, fh))
PUBLIC_COARSE(Comm_spawn_multiple,
              (int count, char *commands[], char **arguments[],
               const int maxprocs[], const MPI_Info infos[], int root,
               MPI_Comm comm, MPI_Comm *intercomm, int errors[]),
              (count, commands, arguments, maxprocs, infos, root, comm,
               intercomm, errors),
              Comm m = {0};
              unsigned slot = 0;
              if (get_comm(comm, &m, &slot, 1)) put_comm(slot);
              peak_mpi_causal_runtime_note_peer(-1))
PUBLIC_COARSE_UNKNOWN(Comm_join, (int fd, MPI_Comm *intercomm), (fd, intercomm))

int peak_mpi_causal_events_recovery(PeakMpiCausalRecovery *r) {
  if (pthread_mutex_trylock(&metadata))
    return 0;
  int ok = 0;
  if (r->state == PEAK_MPI_RECOVERY_QUERY) {
    uint64_t loss = peak_mpi_causal_runtime_loss_generation();
    if (r->comm && r->comm <= RECOVERY_COMM_CAP &&
        recovery_witnesses[r->comm - 1].ordinal >= r->ordinal &&
        (recovery_witnesses[r->comm - 1].ticket >> 8) == loss) {
      r->loss = loss;
      r->state = PEAK_MPI_RECOVERY_READY;
      r->uid_floor = peak_mpi_causal_runtime_operation_floor();
      ok = 1;
    }
  } else {
    uint64_t first = UINT64_MAX;
    for (unsigned i = 0; i < RECOVERY_COMM_CAP; i++) {
      RecoveryWitness *w = &recovery_witnesses[i];
      if (w->ordinal && (w->ticket >> 8) == r->loss && w->ordinal < first) {
        first = w->ordinal;
        r->comm = i + 1;
        r->ordinal = w->ordinal;
        r->operation = w->ticket & 255;
        ok = 1;
      }
    }
  }
  pthread_mutex_unlock(&metadata);
  return ok;
}
void peak_mpi_causal_events_recovery_adopt(uint64_t epoch) {
  pthread_mutex_lock(&metadata);
  group_capture_epoch = epoch;
  group_active = group_emitted = group_published = 0;
  group_uid = previous_group = 0;
  pthread_mutex_unlock(&metadata);
}
