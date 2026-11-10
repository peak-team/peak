/* Frontend regression TU; native local MPI initializes objects. Only alias
 * return/completion are shims for Open MPI's shared completed-send handle. */
#ifndef SUBJECT_EVENTS
#define SUBJECT_EVENTS "../../src/mpi_causal_events.c"
#endif
#include SUBJECT_EVENTS
#include "internal/mpi_observer.h"
#include <assert.h>
#include <stdio.h>
static PeakMpiObserver *observer;
static PeakMpiObserverProducer writer;
static uint64_t uid, cost;
static _Atomic uint64_t lookup_work;
static _Thread_local size_t test_slot;
static pthread_mutex_t collected = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t in_flight;
static int concurrent_test, concurrent_free_test, require_buffer_activation;
static uint64_t snapshot_ids[2][2];
static _Atomic unsigned remote_active;
static unsigned failures, completed;
static int self_loop, require_mixed_start;
static uint64_t completed_uid[8], completed_start[8];
static unsigned native_buffer_results;
static uint64_t native_buffer_ids[16];
static int check_native_buffer;
int peak_mpi_causal_runtime_enabled(void) { return 1; }
int peak_mpi_causal_runtime_needs_service(void) { return remote_active != 0; }
int peak_mpi_causal_runtime_capture_enabled(void) {
  return peak_mpi_observer_coarse_reason(observer) ==
         PEAK_MPI_OBSERVER_REASON_NONE;
}
int peak_mpi_causal_runtime_metadata_enabled(void) { return 1; }
uint64_t peak_mpi_causal_runtime_capture_epoch(void) { return 1; }
uint64_t peak_mpi_causal_runtime_loss_generation(void) { return 0; }
uint64_t peak_mpi_causal_runtime_operation_floor(void) { return 1; }
int peak_mpi_causal_runtime_world_size(void) { return 2; }
void peak_mpi_causal_runtime_observer_cost(double d) { (void)d; }
void peak_mpi_causal_runtime_observer_work(PeakMpiCausalWorkKind k, size_t n) {
  if (k == PEAK_MPI_CAUSAL_WORK_LOOKUP)
    atomic_fetch_add(&lookup_work, n);
}
void peak_mpi_causal_runtime_boundary(PeakMpiObserverRecord *r) {
  r->P_cut = r->A_cut = r->L_cut = cost;
  r->flags |= PEAK_MPI_CAUSAL_APP_COST_KNOWN;
}
void peak_mpi_causal_runtime_prepare_submit(PeakMpiObserverRecord *r) {
  peak_mpi_causal_runtime_boundary(r);
  r->flags |= PEAK_MPI_CAUSAL_CUT_PRECAPTURED | PEAK_MPI_CAUSAL_EMIT_PRECHARGED;
}
void peak_mpi_causal_runtime_submit(PeakMpiObserverRecord *r, int actual) {
  pthread_mutex_lock(&collected);
  if (require_buffer_activation &&
      r->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE &&
      r->operation == PEAK_MPI_CAUSAL_BSEND) {
    assert((r->flags & PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED) &&
           !(r->flags & PEAK_MPI_OBSERVER_PROGRESS_SEALED));
  }
  if (check_native_buffer && r->operation == PEAK_MPI_CAUSAL_BSEND &&
      (r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN ||
       r->phase == PEAK_MPI_CAUSAL_CALL_END)) {
    assert((r->flags & PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED) &&
           !(r->flags & PEAK_MPI_OBSERVER_PROGRESS_SEALED));
    if (r->phase == PEAK_MPI_CAUSAL_CALL_END) {
      assert(native_buffer_results < 16 && observed_open_buffered);
      for (unsigned i = 0; i < native_buffer_results; ++i)
        assert(native_buffer_ids[i] != r->op_uid);
      native_buffer_ids[native_buffer_results++] = r->op_uid;
    }
  }
  if (require_mixed_start &&
      ((r->operation == PEAK_MPI_CAUSAL_START && r->phase == PEAK_MPI_CAUSAL_CALL_BEGIN) ||
       r->phase == PEAK_MPI_CAUSAL_REQUEST_ACTIVATE))
    assert(remote_active && r->op_uid && r->batch_uid);
  if (actual)
    remote_active = 1;
  peak_mpi_causal_runtime_boundary(r);
  peak_mpi_observer_activate(observer);
  (void)peak_mpi_observer_emit(observer, &writer, r);
  if (r->phase == PEAK_MPI_CAUSAL_REQUEST_COMPLETE && !self_loop) {
    assert(completed < 8);
    completed_uid[completed] = r->request_uid;
    completed_start[completed++] = r->op_uid;
  }
  pthread_mutex_unlock(&collected);
}
void peak_mpi_causal_runtime_define(const PeakMpiObserverObject *o) { (void)o; }
void peak_mpi_causal_runtime_fail(PeakMpiObserverReason r) {
  (void)r;
  failures++;
}
uint64_t peak_mpi_causal_runtime_operation_id(void) { return ++uid; }
size_t peak_mpi_causal_runtime_thread_capacity(void) { return 3; }
int peak_mpi_causal_runtime_thread_token(size_t *s, uint64_t *g) {
  *s = test_slot;
  *g = 1;
  return 1;
}
void peak_mpi_causal_runtime_note_peer(int r) {
  if (r >= 0 && r != world_rank)
    remote_active = 1;
}
void peak_mpi_causal_runtime_note_members(const int32_t *r, size_t n) {
  (void)r;
  if (n > 1)
    remote_active = 1;
}
int peak_mpi_causal_runtime_internal_enter(void) { return 0; }
void peak_mpi_causal_runtime_internal_leave(int i) { (void)i; }
static int fake_send(const void *b, int n, MPI_Datatype t, int p, int tag,
                     MPI_Comm c, MPI_Request *r) {
  (void)b;
  (void)n;
  (void)t;
  (void)p;
  (void)tag;
  (void)c;
  *r = (MPI_Request)(uintptr_t)12345;
  return MPI_SUCCESS;
}
static int fake_waitall(int n, MPI_Request *r, MPI_Status *s) {
  assert(n == 2 && r[0] == r[1]);
  if (concurrent_test) {
    Context *c = &contexts[test_slot];
    snapshot_ids[test_slot - 1][0] = c->requests[0].uid;
    snapshot_ids[test_slot - 1][1] = c->requests[1].uid;
    (void)pthread_barrier_wait(&in_flight);
  }
  for (int i = 0; i < n; i++) {
    r[i] = MPI_REQUEST_NULL;
    memset(&s[i], 0, sizeof s[i]);
    s[i].MPI_ERROR = MPI_SUCCESS;
  }
  return MPI_SUCCESS;
}
static int fake_wait(MPI_Request *r, MPI_Status *s) {
  assert(*r != MPI_REQUEST_NULL);
  if (concurrent_free_test) {
    snapshot_ids[test_slot - 1][0] = contexts[test_slot].requests[0].uid;
    (void)pthread_barrier_wait(&in_flight);
  }
  *r = MPI_REQUEST_NULL;
  memset(s, 0, sizeof *s);
  return MPI_SUCCESS;
}
static int fake_any(int n, MPI_Request *r, int *i, MPI_Status *s) {
  assert(n == 2);
  *i = 1;
  r[1] = MPI_REQUEST_NULL;
  memset(s, 0, sizeof *s);
  return MPI_SUCCESS;
}
static int fake_some(int n, MPI_Request *r, int *out, int *indices,
                     MPI_Status *s) {
  assert(n == 2);
  *out = 1;
  indices[0] = 1;
  r[1] = MPI_REQUEST_NULL;
  memset(s, 0, sizeof *s);
  return MPI_SUCCESS;
}
static int fake_start(MPI_Request *r) {
  assert(*r != MPI_REQUEST_NULL);
  return MPI_SUCCESS;
}
static int fake_persistent_wait(MPI_Request *r, MPI_Status *s) {
  assert(*r != MPI_REQUEST_NULL);
  memset(s, 0, sizeof *s);
  return MPI_SUCCESS;
}
static int fake_null_send(const void *b, int n, MPI_Datatype dt, int peer,
                          int tag, MPI_Comm c, MPI_Request *r) {
  (void)b;
  (void)n;
  (void)dt;
  (void)peer;
  (void)tag;
  (void)c;
  *r = MPI_REQUEST_NULL;
  return MPI_SUCCESS;
}
static void send_alias(MPI_Request *r) {
  int v = 1;
  assert(wrap_Isend(fake_send, &v, 1, MPI_INT, 1 - world_rank, 7,
                    MPI_COMM_WORLD, r) == MPI_SUCCESS);
}
static void no_live_requests(void) {
  for (size_t i = 0; i < request_capacity; i++)
    assert(!requests[i].occupied);
  assert(!observed_open_requests && !failures);
}
static void *copied_batch(void *arg) {
  test_slot = (size_t)(uintptr_t)arg;
  MPI_Request r[2] = {(MPI_Request)(uintptr_t)12345,
                      (MPI_Request)(uintptr_t)12345};
  MPI_Status st[2];
  assert(wrap_Waitall(fake_waitall, 2, r, st) == MPI_SUCCESS);
  return NULL;
}
static int false_test(MPI_Request *r, int *flag, MPI_Status *s) {
  (void)r;
  (void)s;
  assert(!wrapper_enter());
  wrapper_leave();
  assert(pending_requests &&
         requests[pending_requests->requests[0].registry_slot].selected);
  *flag = 0;
  return MPI_SUCCESS;
}
static int false_all(int n, MPI_Request *r, int *flag, MPI_Status *s) {
  (void)n;
  (void)r;
  (void)s;
  *flag = 0;
  return MPI_SUCCESS;
}
static int false_any(int n, MPI_Request *r, int *i, int *flag, MPI_Status *s) {
  (void)n;
  (void)r;
  (void)s;
  *i = MPI_UNDEFINED;
  *flag = 0;
  return MPI_SUCCESS;
}
static int false_some(int n, MPI_Request *r, int *out, int *i, MPI_Status *s) {
  (void)n;
  (void)r;
  (void)i;
  (void)s;
  *out = 0;
  return MPI_SUCCESS;
}
static int status_ready(MPI_Request r, int *flag, MPI_Status *s) {
  (void)r;
  *flag = 1;
  memset(s, 0, sizeof *s);
  return MPI_SUCCESS;
}
static int partial_all(int n, MPI_Request *r, MPI_Status *s) {
  assert(n == 2);
  memset(s, 0, 2 * sizeof *s);
  r[0] = MPI_REQUEST_NULL;
  s[0].MPI_ERROR = MPI_SUCCESS;
  s[1].MPI_ERROR = MPI_ERR_PENDING;
  return MPI_ERR_IN_STATUS;
}
static void no_reservations(void) {
  for (size_t i = 0; i < request_used; i++)
    assert(!requests[i].selected);
  assert(!pending_requests);
}
static int fake_free(MPI_Request *r) {
  snapshot_ids[test_slot - 1][0] = contexts[test_slot].requests[0].uid;
  (void)pthread_barrier_wait(&in_flight);
  *r = MPI_REQUEST_NULL;
  return MPI_SUCCESS;
}
static void *free_or_wait(void *arg) {
  test_slot = (size_t)(uintptr_t)arg;
  MPI_Request r = (MPI_Request)(uintptr_t)12345;
  MPI_Status st;
  if (test_slot == 1)
    assert(wrap_Request_free(fake_free, &r) == MPI_SUCCESS);
  else
    assert(wrap_Wait(fake_wait, &r, &st) == MPI_SUCCESS);
  assert(!pending_requests);
  return NULL;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  setenv("PEAK_MPI_COMPLETION_CAPACITY", "1024", 1);
  int provided;
  int requested = (!strcmp(argv[1], "startup-compact") || !strcmp(argv[1], "compact-request")) ? MPI_THREAD_FUNNELED : MPI_THREAD_MULTIPLE;
  assert(PMPI_Init_thread(&argc, &argv, requested, &provided) ==
             MPI_SUCCESS &&
         provided == requested);
  PeakMpiObserverConfig cfg = {1, 256, 4, 100};
  observer = peak_mpi_observer_create(&cfg);
  assert(observer);
  assert(peak_mpi_observer_claim(observer, 0, 1, 1, &writer) ==
         PEAK_MPI_OBSERVER_OK);
  peak_mpi_causal_events_init();
  contract = (PeakMpiProgressContract){
      .profile = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX,
      .progress_estimate = 1,
      .observed_p2p_completion = 1,
      .observed_collective_completion = 1,
      .name = "unit",
      .reason = "unit"};
  if (!strcmp(argv[1], "startup-compact")) {
    for (int i = 0; i < 100; ++i) {
      int value = world_rank == 0 ? i : -1;
      assert(MPI_Bcast(&value, 1, MPI_INT, 0, MPI_COMM_WORLD) == MPI_SUCCESS && value == i);
    }
    PeakMpiObserverSlotState state;
    assert(peak_mpi_observer_slot_state(observer, 0, &state));
    fprintf(stderr, "startup rank=%d records=%llu lost=%llu failures=%u\n", world_rank,
      (unsigned long long)state.published, (unsigned long long)state.lost, failures);
    assert(state.published == 200 && !state.lost && !failures);
  } else if (!strcmp(argv[1], "alias") || !strcmp(argv[1], "compact-request")) {
    MPI_Request r[2];
    MPI_Status statuses[2];
    int value = 1;
    assert(wrap_Isend(fake_send, &value, 1, MPI_INT, 1 - world_rank, 7,
                      MPI_COMM_WORLD, &r[0]) == MPI_SUCCESS);
    assert(wrap_Isend(fake_send, &value, 1, MPI_INT, 1 - world_rank, 7,
                      MPI_COMM_WORLD, &r[1]) == MPI_SUCCESS);
    assert(r[0] == r[1] && r[0] != MPI_REQUEST_NULL);
    assert(wrap_Waitall(fake_waitall, 2, r, statuses) == MPI_SUCCESS);
    unsigned occupied = 0;
    for (size_t i = 0; i < request_capacity; i++)
      occupied += requests[i].occupied;
    fprintf(stderr,
            "alias completed=%u uid=%llu/%llu start=%llu/%llu occupied=%u "
            "open=%llu failures=%u\n",
            completed, (unsigned long long)completed_uid[0],
            (unsigned long long)completed_uid[1],
            (unsigned long long)completed_start[0],
            (unsigned long long)completed_start[1], occupied,
            (unsigned long long)observed_open_requests, failures);
    assert(completed == 2 && completed_uid[0] != completed_uid[1] &&
           completed_start[0] != completed_start[1] && !occupied &&
           !observed_open_requests && !failures);
  } else if (!strcmp(argv[1], "native")) {
    int sent[4], received[4];
    MPI_Request r[8], moved[8];
    MPI_Status st[8];
    unsigned aliases = 0;
    for (unsigned i = 0; i < 4; i++) {
      sent[i] = world_rank * 100 + (int)i;
      received[i] = -1;
      assert(MPI_Isend(&sent[i], 1, MPI_INT, 1 - world_rank, (int)i,
                       MPI_COMM_WORLD, &r[i]) == MPI_SUCCESS);
      for (unsigned j = 0; j < i; j++)
        aliases += r[i] != MPI_REQUEST_NULL && r[i] == r[j];
    }
    for (unsigned i = 0; i < 4; i++)
      assert(MPI_Irecv(&received[i], 1, MPI_INT, 1 - world_rank, (int)i,
                       MPI_COMM_WORLD, &r[4 + i]) == MPI_SUCCESS);
    memcpy(moved, r, sizeof moved);
    assert(MPI_Waitall(8, moved, st) == MPI_SUCCESS && completed == 8);
    for (unsigned i = 0; i < 4; i++)
      assert(received[i] == (1 - world_rank) * 100 + (int)i &&
             st[4 + i].MPI_SOURCE == 1 - world_rank &&
             st[4 + i].MPI_TAG == (int)i);
    for (unsigned i = 0; i < 8; i++)
      for (unsigned j = 0; j < i; j++)
        assert(completed_uid[i] != completed_uid[j]);
    no_live_requests();
    PeakMpiObserverSlotState state;
    assert(peak_mpi_observer_slot_state(observer, 0, &state) && !state.lost &&
           peak_mpi_observer_coarse_reason(observer) ==
               PEAK_MPI_OBSERVER_REASON_NONE);
    printf("native wrapper metadata rank=%d shared_handles=%u "
           "unique_completions=%u registry_empty=1 lost=0\n",
           world_rank, aliases, completed);
  } else if (!strcmp(argv[1], "native-buffer")) {
    contract = (PeakMpiProgressContract){
        .profile = PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED,
        .observed_p2p_completion = 1, .observed_buffer_detach = 1,
        .name = "unit", .reason = "unit"};
    int packed;
    assert(PMPI_Pack_size(1, MPI_INT, MPI_COMM_WORLD, &packed) == MPI_SUCCESS);
    int capacity = 16 * (packed + MPI_BSEND_OVERHEAD);
    void *buffer = malloc((size_t)capacity);
    assert(buffer && MPI_Buffer_attach(buffer, capacity) == MPI_SUCCESS);
    check_native_buffer = 1;
    for (int i = 0; i < 16; ++i) {
      int sent = world_rank * 100 + i, received = -1;
      MPI_Status st;
      assert(MPI_Bsend(&sent, 1, MPI_INT, 1 - world_rank, 73,
                       MPI_COMM_WORLD) == MPI_SUCCESS);
      assert(MPI_Recv(&received, 1, MPI_INT, 1 - world_rank, 73,
                      MPI_COMM_WORLD, &st) == MPI_SUCCESS);
      assert(received == (1 - world_rank) * 100 + i &&
             st.MPI_SOURCE == 1 - world_rank && st.MPI_TAG == 73 &&
             native_buffer_results == (unsigned)i + 1 &&
             observed_open_buffered == (uint64_t)i + 1);
    }
    /* The observed delivery cuts precede the only terminal resource cleanup. */
    assert(native_buffer_results == 16 && !failures && !completed);
    void *detached = NULL;
    int detached_size = 0;
    assert(MPI_Buffer_detach(&detached, &detached_size) == MPI_SUCCESS &&
           detached == buffer && detached_size == capacity &&
           !observed_open_buffered);
    free(buffer);
    no_live_requests();
    printf("native buffered rank=%d estimated_results=16 unique=16 before_detach=1\n",
           world_rank);
  } else if (!strcmp(argv[1], "native-latewait")) {
    for (unsigned i = 0; i < 8; ++i) {
      int value = (int)i + 100, ack = -1;
      MPI_Status st;
      if (world_rank == 0) {
        MPI_Request r;
        assert(MPI_Isend(&value, 1, MPI_INT, 1, 74, MPI_COMM_WORLD, &r) ==
               MPI_SUCCESS);
        Request start;
        assert(snapshot_request_at(r, &r, &start));
        assert(MPI_Recv(&ack, 1, MPI_INT, 1, 75, MPI_COMM_WORLD, &st) ==
               MPI_SUCCESS && ack == value && st.MPI_SOURCE == 1 &&
               st.MPI_TAG == 75);
        Request before_wait;
        assert(completed == i && observed_open_requests == 1 &&
               snapshot_request_at(r, &r, &before_wait) &&
               before_wait.uid == start.uid && before_wait.start_uid == start.start_uid);
        assert(MPI_Wait(&r, &st) == MPI_SUCCESS && completed == i + 1 &&
               completed_uid[i] == start.uid && completed_start[i] == start.start_uid);
        for (unsigned j = 0; j < i; ++j)
          assert(completed_uid[i] != completed_uid[j]);
        no_live_requests();
      } else {
        assert(MPI_Recv(&ack, 1, MPI_INT, 0, 74, MPI_COMM_WORLD, &st) ==
               MPI_SUCCESS && ack == value && st.MPI_SOURCE == 0 &&
               st.MPI_TAG == 74);
        assert(MPI_Send(&ack, 1, MPI_INT, 0, 75, MPI_COMM_WORLD) == MPI_SUCCESS);
      }
    }
    printf("native latewait rank=%d completions=%u request_retained=1\n",
           world_rank, completed);
  } else if (!strcmp(argv[1], "concurrent")) {
    MPI_Request r[4];
    for (unsigned i = 0; i < 4; i++)
      send_alias(&r[i]);
    concurrent_test = 1;
    assert(!pthread_barrier_init(&in_flight, NULL, 2));
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, copied_batch, (void *)(uintptr_t)1));
    assert(!pthread_create(&b, NULL, copied_batch, (void *)(uintptr_t)2));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    for (unsigned i = 0; i < 2; i++)
      for (unsigned j = 0; j < 2; j++)
        assert(snapshot_ids[0][i] != snapshot_ids[1][j]);
    assert(completed == 4);
    no_live_requests();
    assert(!pthread_barrier_destroy(&in_flight));
  } else if (!strcmp(argv[1], "free-wait")) {
    MPI_Request r[2];
    send_alias(&r[0]);
    send_alias(&r[1]);
    requests[0].observed = requests[1].observed = 1;
    observed_open_requests = 0;
    concurrent_free_test = 1;
    assert(!pthread_barrier_init(&in_flight, NULL, 2));
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, free_or_wait, (void *)(uintptr_t)1));
    assert(!pthread_create(&b, NULL, free_or_wait, (void *)(uintptr_t)2));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(snapshot_ids[0][0] != snapshot_ids[1][0] && !completed);
    no_live_requests();
    assert(!pthread_barrier_destroy(&in_flight));
  } else if (!strcmp(argv[1], "batch128") || !strcmp(argv[1], "batch256")) {
    unsigned n = !strcmp(argv[1], "batch128") ? 128 : 256;
    MPI_Request r[256], copied[256];
    PeakMpiObserverRecord rec;
    for (unsigned i = 0; i < n; i++) {
      send_alias(&r[i]);
      copied[i] = r[i];
      while (peak_mpi_observer_consume(observer, 0, &rec)) {
      }
    }
    assert(request_used == n);
    atomic_store(&lookup_work, 0);
    assert(wrapper_enter());
    Context *c = prepare_many((int)n, copied, PEAK_MPI_CAUSAL_TEST, 1);
    assert(c);
    for (unsigned i = 0; i < n; i++)
      for (unsigned j = 0; j < i; j++)
        assert(c->requests[i].uid != c->requests[j].uid);
    poll_empty(c);
    wrapper_leave();
    uint64_t work = atomic_load(&lookup_work);
    assert(work <= n * n);
    no_reservations();
    for (unsigned i = 0; i < n; i++) {
      Request old;
      assert(snapshot_request_at(r[i], &r[i], &old));
      assert(complete_request(NULL, &old, NULL, MPI_SUCCESS, 1, 0));
    }
    assert(!request_used);
    MPI_Request single;
    send_alias(&single);
    atomic_store(&lookup_work, 0);
    MPI_Status st;
    assert(wrap_Wait(fake_wait, &single, &st) == MPI_SUCCESS);
    assert(atomic_load(&lookup_work) <= 2 && !request_used);
    printf("batch%u snapshot_lookup=%llu prefix_after_cleanup=%zu "
           "single_lookup=%llu\n",
           n, (unsigned long long)work, request_used,
           (unsigned long long)atomic_load(&lookup_work));
    no_live_requests();
  } else if (!strcmp(argv[1], "reserved-lookup")) {
    MPI_Request r;
    send_alias(&r);
    Request claimed, lookup;
    assert(snapshot_request_claimed(r, &r, &claimed) && requests[0].selected);
    assert(snapshot_request_at(r, &r, &lookup) && lookup.uid == claimed.uid &&
           lookup.generation == claimed.generation && requests[0].selected &&
           !failures);
    release_requests(&claimed, 1);
    MPI_Status st;
    assert(wrap_Wait(fake_wait, &r, &st) == MPI_SUCCESS);
    no_live_requests();
  } else if (!strcmp(argv[1], "copy")) {
    MPI_Request r[2], moved[2];
    MPI_Status st[2];
    send_alias(&r[0]);
    send_alias(&r[1]);
    moved[0] = r[1];
    moved[1] = r[0];
    assert(wrap_Waitall(fake_waitall, 2, moved, st) == MPI_SUCCESS);
    assert(completed == 2 && completed_uid[0] != completed_uid[1]);
    no_live_requests();
  } else if (!strcmp(argv[1], "overwrite")) {
    MPI_Request r, saved;
    send_alias(&r);
    saved = r;
    uint64_t old = requests[0].uid;
    send_alias(&r);
    uint64_t newest = requests[1].uid;
    MPI_Status st;
    assert(wrap_Wait(fake_wait, &r, &st) == MPI_SUCCESS &&
           completed_uid[0] == newest);
    assert(wrap_Wait(fake_wait, &saved, &st) == MPI_SUCCESS &&
           completed_uid[1] == old);
    no_live_requests();
  } else if (!strcmp(argv[1], "any") || !strcmp(argv[1], "some")) {
    MPI_Request r[2];
    MPI_Status st;
    send_alias(&r[0]);
    send_alias(&r[1]);
    uint64_t second = requests[1].uid;
    int i;
    if (!strcmp(argv[1], "any"))
      assert(wrap_Waitany(fake_any, 2, r, &i, &st) == MPI_SUCCESS && i == 1);
    else {
      int out;
      assert(wrap_Waitsome(fake_some, 2, r, &out, &i, &st) == MPI_SUCCESS &&
             out == 1 && i == 1);
    }
    assert(completed == 1 && completed_uid[0] == second &&
           requests[0].occupied && !requests[1].occupied);
    assert(wrap_Wait(fake_wait, &r[0], &st) == MPI_SUCCESS && completed == 2);
    no_live_requests();
  } else if (!strcmp(argv[1], "false")) {
    MPI_Request r[2];
    MPI_Status st[2];
    send_alias(&r[0]);
    send_alias(&r[1]);
    int flag, index, out;
    assert(wrap_Test(false_test, &r[0], &flag, &st[0]) == MPI_SUCCESS && !flag);
    no_reservations();
    assert(wrap_Testall(false_all, 2, r, &flag, st) == MPI_SUCCESS && !flag);
    no_reservations();
    assert(wrap_Testany(false_any, 2, r, &index, &flag, &st[0]) ==
               MPI_SUCCESS &&
           !flag);
    no_reservations();
    assert(wrap_Testsome(false_some, 2, r, &out, &index, st) == MPI_SUCCESS &&
           out == 0);
    no_reservations();
    assert(wrap_Waitall(fake_waitall, 2, r, st) == MPI_SUCCESS &&
           completed == 2);
    no_live_requests();
  } else if (!strcmp(argv[1], "status")) {
    MPI_Request r;
    MPI_Status st;
    send_alias(&r);
    int flag;
    assert(
        wrap_Request_get_status(status_ready, r, &flag, &st) == MPI_SUCCESS &&
        flag && completed == 1 && requests[0].occupied && requests[0].observed);
    no_reservations();
    assert(wrap_Wait(fake_wait, &r, &st) == MPI_SUCCESS && completed == 1);
    no_live_requests();
  } else if (!strcmp(argv[1], "partial")) {
    MPI_Request r[2];
    MPI_Status st[2];
    send_alias(&r[0]);
    send_alias(&r[1]);
    assert(wrap_Waitall(partial_all, 2, r, st) == MPI_ERR_IN_STATUS &&
           completed == 1);
    no_reservations();
    assert(!requests[0].occupied && requests[1].occupied);
    assert(failures > 0);
    failures = 0;
    assert(wrap_Wait(fake_wait, &r[1], &st[1]) == MPI_SUCCESS &&
           completed == 2);
    no_live_requests();
  } else if (!strcmp(argv[1], "reuse")) {
    MPI_Request r;
    MPI_Status st;
    send_alias(&r);
    Request old;
    assert(snapshot_request_at(r, &r, &old));
    assert(wrap_Wait(fake_wait, &r, &st) == MPI_SUCCESS);
    send_alias(&r);
    assert(requests[0].generation != old.generation);
    unsigned before = completed;
    assert(!complete_request(NULL, &old, &st, MPI_SUCCESS, 1, 0) &&
           completed == before && requests[0].occupied);
    assert(wrap_Wait(fake_wait, &r, &st) == MPI_SUCCESS);
    no_live_requests();
  } else if (!strcmp(argv[1], "persistent-buffer")) {
    contract = (PeakMpiProgressContract){
        .profile =
            PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED,
        .observed_p2p_completion = 1,
        .observed_collective_completion = 1,
        .observed_buffer_detach = 1,
        .name = "unit",
        .reason = "unit"};
    require_buffer_activation = 1;
    void *buffer = malloc(4096);
    assert(buffer && MPI_Buffer_attach(buffer, 4096) == MPI_SUCCESS);
    MPI_Request r;
    MPI_Status st;
    int v = 1;
    assert(wrap_Bsend_init(fake_send, &v, 1, MPI_INT, 1 - world_rank, 7,
                           MPI_COMM_WORLD, &r) == MPI_SUCCESS);
    assert(wrap_Start(fake_start, &r) == MPI_SUCCESS &&
           observed_open_buffered == 1);
    assert(wrap_Wait(fake_persistent_wait, &r, &st) == MPI_SUCCESS &&
           observed_open_buffered == 1);
    Request old;
    assert(snapshot_request_at(r, &r, &old));
    forget_request(&old);
    void *detached = NULL;
    int size;
    assert(MPI_Buffer_detach(&detached, &size) == MPI_SUCCESS &&
           detached == buffer && !observed_open_buffered);
    free(buffer);
    no_live_requests();
  } else if (!strcmp(argv[1], "persistent")) {
    MPI_Request r;
    MPI_Status st;
    int v = 1;
    assert(wrap_Send_init(fake_send, &v, 1, MPI_INT, 1 - world_rank, 7,
                          MPI_COMM_WORLD, &r) == MPI_SUCCESS);
    uint64_t id = requests[0].uid;
    for (unsigned a = 1; a <= 2; a++) {
      assert(wrap_Start(fake_start, &r) == MPI_SUCCESS);
      assert(requests[0].uid == id && requests[0].activation == a &&
             requests[0].active);
      assert(wrap_Wait(fake_persistent_wait, &r, &st) == MPI_SUCCESS &&
             !requests[0].active && requests[0].occupied);
    }
    Request old;
    assert(snapshot_request_at(r, &r, &old));
    forget_request(&old);
    no_live_requests();
  } else if (!strcmp(argv[1], "null")) {
    MPI_Request r;
    int v = 1;
    assert(wrap_Isend(fake_null_send, &v, 1, MPI_INT, 1 - world_rank, 7,
                      MPI_COMM_WORLD, &r) == MPI_SUCCESS &&
           r == MPI_REQUEST_NULL);
    assert(!completed);
    no_live_requests();
  } else {
    assert(!strcmp(argv[1], "self") || !strcmp(argv[1], "singleton"));
    MPI_Comm local = MPI_COMM_SELF;
    uint64_t expected_previous = 0;
    if (!strcmp(argv[1], "singleton")) {
      assert(MPI_Comm_dup(MPI_COMM_SELF, &local) == MPI_SUCCESS);
      expected_previous = contexts[0].previous;
    }
    for (unsigned i = 0; i < 600; i++) {
      cost++;
      assert(MPI_Barrier(local) == MPI_SUCCESS);
    }
    self_loop = 1;
    for (int i = 0; i < 600; ++i) {
      int sent = i + world_rank, received = -1;
      MPI_Request request;
      assert(MPI_Isend(&sent, 1, MPI_INT, 0, 71, local, &request) == MPI_SUCCESS);
      assert(MPI_Recv(&received, 1, MPI_INT, MPI_ANY_SOURCE, 71, local,
                      MPI_STATUS_IGNORE) == MPI_SUCCESS && received == sent);
      assert(MPI_Wait(&request, MPI_STATUS_IGNORE) == MPI_SUCCESS);
      int reduced = -1;
      assert(MPI_Iallreduce(&sent, &reduced, 1, MPI_INT, MPI_SUM, local,
                            &request) == MPI_SUCCESS);
      assert(MPI_Wait(&request, MPI_STATUS_IGNORE) == MPI_SUCCESS && reduced == sent);
      assert(!observed_open_requests && !request_used && !remote_active);
      PeakMpiObserverSlotState local_state;
      assert(peak_mpi_observer_slot_state(observer, 0, &local_state) &&
             !local_state.published && !local_state.lost);
    }
    int probe_sent = 73, probe_received = -1;
    MPI_Request probe_request;
    assert(MPI_Isend(&probe_sent, 1, MPI_INT, 0, 72, local, &probe_request) == MPI_SUCCESS);
    for (int i = 0; i < 600; ++i) {
      int flag = 0;
      assert(MPI_Iprobe(MPI_ANY_SOURCE, 72, local, &flag, MPI_STATUS_IGNORE) == MPI_SUCCESS && flag);
    }
    assert(MPI_Recv(&probe_received, 1, MPI_INT, 0, 72, local,
                    MPI_STATUS_IGNORE) == MPI_SUCCESS && probe_received == probe_sent);
    assert(MPI_Wait(&probe_request, MPI_STATUS_IGNORE) == MPI_SUCCESS);
    for (int variant = 0; variant < 2; ++variant) {
      MPI_Message message;
      MPI_Request sender, receiver;
      int sent = 75 + variant, received = -1;
      assert(MPI_Isend(&sent, 1, MPI_INT, 0, 73 + variant, local, &sender) == MPI_SUCCESS);
      assert(MPI_Mprobe(0, 73 + variant, local, &message, MPI_STATUS_IGNORE) == MPI_SUCCESS);
      assert(observed_open_requests == 2);
      if (!variant)
        assert(MPI_Mrecv(&received, 1, MPI_INT, &message, MPI_STATUS_IGNORE) == MPI_SUCCESS);
      else {
        assert(MPI_Imrecv(&received, 1, MPI_INT, &message, &receiver) == MPI_SUCCESS);
        assert(MPI_Wait(&receiver, MPI_STATUS_IGNORE) == MPI_SUCCESS);
      }
      assert(MPI_Wait(&sender, MPI_STATUS_IGNORE) == MPI_SUCCESS && received == sent);
      assert(!observed_open_requests && !request_used);
    }
    self_loop = 0;
    /* Keep unrecorded SELF branches alive across the first remote join. */
    contract = (PeakMpiProgressContract){.profile=PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC,
      .autonomous_p2p=1,.autonomous_blocking_collective=1,.autonomous_selected_nbc=1,
      .name="unit",.reason="unit"};
    single_flow = 1;
    int late_value = 81, late_received = -1, late_reduced = -1, late_message_value = 83;
    MPI_Request late[3];
    MPI_Message late_message;
    assert(MPI_Isend(&late_value, 1, MPI_INT, 0, 81, local, &late[0]) == MPI_SUCCESS);
    assert(MPI_Recv(&late_received, 1, MPI_INT, 0, 81, local, MPI_STATUS_IGNORE) == MPI_SUCCESS);
    assert(MPI_Iallreduce(&late_value, &late_reduced, 1, MPI_INT, MPI_SUM, local, &late[1]) == MPI_SUCCESS);
    assert(MPI_Isend(&late_message_value, 1, MPI_INT, 0, 83, local, &late[2]) == MPI_SUCCESS);
    assert(MPI_Mprobe(0, 83, local, &late_message, MPI_STATUS_IGNORE) == MPI_SUCCESS);
    MPI_Comm late_comm;
    MPI_Request late_idup;
    assert(MPI_Comm_idup(local, &late_comm, &late_idup) == MPI_SUCCESS);
    assert(observed_open_requests == 4 + (late_idup != MPI_REQUEST_NULL) && !remote_active);
    PeakMpiObserverSlotState state;
    assert(peak_mpi_observer_slot_state(observer, 0, &state));
    fprintf(stderr,
            "self published=%llu lost=%llu coarse=%u remote=%u failures=%u\n",
            (unsigned long long)state.published, (unsigned long long)state.lost,
            (unsigned)peak_mpi_observer_coarse_reason(observer), remote_active,
            failures);
    assert(!state.lost &&
           peak_mpi_observer_coarse_reason(observer) ==
               PEAK_MPI_OBSERVER_REASON_NONE &&
           !remote_active && !failures);
    if (!strcmp(argv[1], "singleton")) {
      MPI_Request persistent[4];
      int send = world_rank + 91, self_received = -1, remote_received = -1;
      assert(MPI_Send_init(&send,1,MPI_INT,0,91,local,&persistent[0])==MPI_SUCCESS);
      assert(MPI_Recv_init(&self_received,1,MPI_INT,0,91,local,&persistent[1])==MPI_SUCCESS);
      assert(MPI_Send_init(&send,1,MPI_INT,1-world_rank,92,MPI_COMM_WORLD,&persistent[2])==MPI_SUCCESS);
      assert(MPI_Recv_init(&remote_received,1,MPI_INT,1-world_rank,92,MPI_COMM_WORLD,&persistent[3])==MPI_SUCCESS);
      require_mixed_start = 1;
      assert(MPI_Startall(4,persistent)==MPI_SUCCESS);
      require_mixed_start = 0;
      assert(MPI_Waitall(4,persistent,MPI_STATUSES_IGNORE)==MPI_SUCCESS);
      assert(self_received==send&&remote_received==1-world_rank+91&&completed==4);
      for(unsigned i=0;i<4;i++) assert(MPI_Request_free(&persistent[i])==MPI_SUCCESS);
      expected_previous=contexts[0].previous;
    }
    int value = world_rank + 1, got = 0;
    assert(MPI_Sendrecv(&value, 1, MPI_INT, 1 - world_rank, 8, &got, 1, MPI_INT,
                        1 - world_rank, 8, MPI_COMM_WORLD,
                        MPI_STATUS_IGNORE) == MPI_SUCCESS &&
           got == 2 - world_rank);
    unsigned before_local_completion = completed;
    assert(MPI_Barrier(MPI_COMM_WORLD)==MPI_SUCCESS);
    assert(MPI_Wait(&late[0],MPI_STATUS_IGNORE)==MPI_SUCCESS);
    assert(MPI_Wait(&late[1],MPI_STATUS_IGNORE)==MPI_SUCCESS&&late_reduced==late_value);
    int late_message_received=-1;
    MPI_Request late_receiver;
    assert(MPI_Imrecv(&late_message_received,1,MPI_INT,&late_message,&late_receiver)==MPI_SUCCESS);
    assert(MPI_Wait(&late_receiver,MPI_STATUS_IGNORE)==MPI_SUCCESS);
    assert(MPI_Wait(&late[2],MPI_STATUS_IGNORE)==MPI_SUCCESS&&late_message_received==late_message_value);
    assert(MPI_Wait(&late_idup,MPI_STATUS_IGNORE)==MPI_SUCCESS);
    Comm late_context; unsigned late_slot;
    assert(get_comm(late_comm,&late_context,&late_slot,0) && local_comm(&late_context));
    put_comm(late_slot);
    assert(MPI_Comm_free(&late_comm)==MPI_SUCCESS);
    assert(completed==before_local_completion&&!observed_open_requests);
    assert(MPI_Barrier(MPI_COMM_WORLD)==MPI_SUCCESS);
    MPI_Request recorded;
    int recorded_reduced = -1;
    assert(MPI_Iallreduce(&late_value,&recorded_reduced,1,MPI_INT,MPI_SUM,local,&recorded)==MPI_SUCCESS);
    Request recorded_identity;
    assert(snapshot_request_at(recorded,&recorded,&recorded_identity) &&
           recorded_identity.start_uid && recorded_identity.graph_epoch);
    assert(MPI_Wait(&recorded,MPI_STATUS_IGNORE)==MPI_SUCCESS && recorded_reduced==late_value &&
           completed==before_local_completion+1 &&
           completed_start[before_local_completion]==recorded_identity.start_uid);
    PeakMpiObserverRecord rec;
    int found = 0, barrier_count=0;
    while (peak_mpi_observer_consume(observer, 0, &rec)) {
      if (rec.operation==PEAK_MPI_CAUSAL_BARRIER&&rec.phase==PEAK_MPI_CAUSAL_CALL_BEGIN) {
        assert(((rec.flags&PEAK_MPI_OBSERVER_SLACK_NO_OPEN_PROGRESS)!=0)==(barrier_count!=0));
        barrier_count++;
      }
      assert(rec.phase!=PEAK_MPI_CAUSAL_REQUEST_COMPLETE||rec.op_uid);
      if (rec.operation == PEAK_MPI_CAUSAL_SENDRECV &&
          rec.phase == PEAK_MPI_CAUSAL_CALL_BEGIN) {
        assert(rec.L_cut == 600 && rec.input_ref == expected_previous);
        found = 1;
      }
    }
    assert(found && remote_active && barrier_count==2 && !failures);
    no_live_requests();
  }
  if (!strcmp(argv[1], "native-buffer") || !strcmp(argv[1], "native-latewait")) {
    PeakMpiObserverSlotState state;
    assert(!failures && peak_mpi_observer_slot_state(observer, 0, &state) &&
           !state.lost && peak_mpi_observer_coarse_reason(observer) ==
                              PEAK_MPI_OBSERVER_REASON_NONE);
  }
  if (!strcmp(argv[1], "compact-request")) {
    PeakMpiObserverSlotState state;
    assert(peak_mpi_observer_slot_state(observer, 0, &state) &&
           state.published == 8 && !state.lost);
  }
  no_reservations();
  assert(PMPI_Finalize() == MPI_SUCCESS);
  puts("frontend regression PASS");
  return 0;
}
