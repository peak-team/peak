#define _GNU_SOURCE
#include <stdatomic.h>
static void after_store(const volatile void *);
#undef atomic_store_explicit
#ifdef __clang__
#define atomic_store_explicit(PTR, VAL, MO)                                    \
  __extension__({                                                              \
    __auto_type __test_ptr = (PTR);                                            \
    __c11_atomic_store(__test_ptr, (VAL), (MO));                               \
    after_store(__test_ptr);                                                   \
  })
#else
#define atomic_store_explicit(PTR, VAL, MO)                                    \
  __extension__({                                                              \
    __auto_type __test_ptr = (PTR);                                            \
    __typeof__((void)0, *__test_ptr) __test_value = (VAL);                     \
    __atomic_store(__test_ptr, &__test_value, (MO));                           \
    after_store(__test_ptr);                                                   \
  })
#endif
#include "../../src/mpi_causal_runtime.c"
#include "internal/mpi_causal_transport.h"
#include <assert.h>
static pthread_barrier_t enter, finished;
static _Thread_local int pause_reload;
static unsigned sample_no;
static uint64_t before_tick, after_tick;
/* Pause after the actual fresh-generation store. The other thread executes
 * the actual tick prefix with an older argument; backend_ready=0 isolates that
 * unconditional prefix without networking. No production operation is replaced.
 */
static void barrier_wait(void) {
  int result = pthread_barrier_wait(&enter);
  assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void finish_wait(void) {
  int result = pthread_barrier_wait(&finished);
  assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void after_store(const volatile void *p) {
  if (p == &control_generation && pause_reload) {
    pause_reload = 0;
    barrier_wait();
    finish_wait();
  }
}
static void *heartbeat(void *unused) {
  (void)unused;
  barrier_wait();
  before_tick = atomic_load(&control_generation);
  assert(!peak_mpi_causal_runtime_tick(2, 2, 0, 25e-9, 1));
  after_tick = atomic_load(&control_generation);
  finish_wait();
  return NULL;
}
size_t peak_mpi_causal_cost_used_slots(void) { return 1; }
int peak_mpi_causal_listener_thread_token(size_t *s, uint64_t *g) {
  *s = 0;
  *g = 1;
  return 1;
}
int peak_mpi_causal_registry_active(void) { return 0; }
void peak_mpi_causal_runtime_note_slot(size_t s) { (void)s; }
void peak_mpi_causal_cost_observer_add(size_t s, double d) {
  (void)s;
  (void)d;
}
int peak_mpi_causal_cost_read(double *p, double *o) {
  *p = (50 + sample_no) * 1e-9;
  *o = 25e-9;
  return 1;
}
void peak_mpi_causal_listener_actual_costs(double *c, double *m, uint64_t *g) {
  *c = 0;
  *m = 25e-9;
  *g = 2;
}
int peak_mpi_causal_events_idle_cut(PeakMpiCausalIdleCapture f, void *p) {
  (void)f;
  (void)p;
  return 0;
}
const PeakMpiProgressContract *peak_mpi_causal_events_contract(void) {
  static PeakMpiProgressContract c;
  return &c;
}
int peak_mpi_causal_events_recovery(PeakMpiCausalRecovery *r) {
  (void)r;
  return 0;
}
void peak_mpi_causal_events_recovery_adopt(uint64_t e) { (void)e; }
int peak_mpi_causal_listener_internal_enter(void) { return 0; }
void peak_mpi_causal_listener_internal_leave(int i) { (void)i; }
static PeakMpiPolicySample sample(PeakMpiObserverRecord *r, unsigned gen) {
  PeakMpiPolicySample s = {0};
  s.member = (PeakMpiPolicyMemberId){0, 99};
  s.generation = s.event_cut = gen;
  s.control_generation = r->control_generation;
  s.P = r->P_cut / 1e9;
  s.A = r->A_cut / 1e9;
  s.L = r->L_cut / 1e9;
  s.cut = (PeakMpiCostCut){r->P_cut, r->A_cut, r->L_cut};
  s.app_cost_known = 1;
  s.elapsed = gen;
  return s;
}
static void test_transport_continuation(int scheduled) {
  /* Real TCP input after capture loss does not make the normal parser
   * runnable. A full IO turn must nevertheless continue before the timer. */
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 2;
  c.job_context = 99;
  c.incarnation = 100;
  c.address = "127.0.0.1";
  c.port = 39879;
  c.slots = 1;
  c.queue_frames = 4096;
  c.max_operations = 64;
  c.max_context_parts = 32;
  c.max_pending_records = 64;
  c.model = (PeakMpiCostConfig){1024, 4096, 1024, 128, 2, 64};
  c.policy = (PeakMpiPolicyConfig){2, .1, 1, .5, 0, 0, 1};
  service = peak_mpi_causal_service_create(&c);
  assert(service);
  for (unsigned r = 0; r < 2; ++r) {
    PeakMpiObserverObject object = {0};
    object.kind = PEAK_MPI_CAUSAL_OBJECT_COMM;
    object.part_count = 1;
    PeakMpiCausalCommPart part = {0};
    part.local_uid = 1;
    part.creation_kind = PEAK_MPI_CAUSAL_COMM_WORLD;
    part.world_size = part.local_size = part.member_count = 2;
    part.members[1] = 1;
    memcpy(object.payload, &part, sizeof part);
    assert(peak_mpi_causal_service_ingest_object(service, r, 100 + r, &object));
  }
  PeakMpiCausalTransportSink sink = {0};
  c.rank = 1;
  c.incarnation = 101;
  PeakMpiCausalTransport *client = peak_mpi_causal_transport_create(&c, &sink);
  assert(client);
  unsigned char members[2] = {1, 1};
  uint64_t prefix = 0;
  PeakMpiCausalRaw raw[2];
  memset(raw, 0, sizeof raw);
  for (unsigned r = 0; r < 2; ++r) {
    raw[r].incarnation = 100 + r;
    raw[r].generation = raw[r].active = 1;
    raw[r].elapsed = 1;
    raw[r].known_peers = members;
    raw[r].peer_count = 2;
    raw[r].slot_prefixes = &prefix;
    raw[r].slot_count = 1;
  }
  PeakMpiCausalServiceStats sent = {0};
  for (unsigned k = 0; k < 2000; ++k) {
    peak_mpi_causal_service_tick(service, NULL, &raw[0], 1, 65536, 16384);
    peak_mpi_causal_transport_tick(client, NULL, &raw[1], 1, 65536);
    peak_mpi_causal_transport_stats(client, &sent);
    if (sent.connected)
      break;
    const struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
  }
  assert(sent.connected);
  raw[1].generation++;
  raw[1].lost_events = raw[1].recovery.loss = 1;
  raw[1].coarse_reason = PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW;
  raw[1].recovery.fault_mask =
      UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW;
  raw[1].recovery.comm = raw[1].recovery.ordinal = 1;
  raw[1].recovery.operation = PEAK_MPI_CAUSAL_BARRIER;
  assert(peak_mpi_causal_service_ingest_raw(service, 1, &raw[1], 2));
  peak_mpi_causal_service_tick(service, NULL, NULL, 2, 65536, 16384);
  PeakMpiObserverConfig cfg = {1, 256, 1, 101};
  PeakMpiObserver *source = peak_mpi_observer_create(&cfg);
  PeakMpiObserverProducer writer;
  assert(source);
  peak_mpi_observer_activate(source);
  assert(peak_mpi_observer_claim(source, 0, 1, 1, &writer) == PEAK_MPI_OBSERVER_OK);
  for (unsigned i = 0; i < 4000; ++i) {
    PeakMpiObserverRecord r = {0};
    r.kind = PEAK_MPI_OBSERVER_EVENT_START;
    r.op_uid = 10 + i / 2;
    r.operation = PEAK_MPI_CAUSAL_SEND;
    r.phase = i % 2 ? PEAK_MPI_CAUSAL_CALL_END : PEAK_MPI_CAUSAL_CALL_BEGIN;
    r.flags = PEAK_MPI_CAUSAL_NONBLOCKING;
    r.communicator_uid = 1;
    assert(peak_mpi_observer_emit(source, &writer, &r) == PEAK_MPI_OBSERVER_OK);
    if (i % 64 == 63)
      peak_mpi_causal_transport_tick(client, source, NULL, 2, 65536);
  }
  peak_mpi_causal_transport_tick(client, source, NULL, 2, 65536);
  assert(!peak_mpi_observer_has_pending(source));
  peak_mpi_causal_transport_stats(client, &sent);
  assert(sent.connected && sent.sent_bytes > 65536);
  policy = c.policy;
  assert(!peak_mpi_policy_receiver_init(&receiver, &policy,
      (PeakMpiPolicyCohortKey){99, 0, 1}, (PeakMpiPolicyMemberId){0, 99}, 2));
  atomic_store(&active, 1);
  atomic_store(&backend_ready, 1);
  heartbeat_enabled = 1;
  _Atomic unsigned char membership[2];
  atomic_init(&membership[0], 0);
  atomic_init(&membership[1], 1);
  rank_id = owner_rank = 0;
  world_size = 2;
  incarnation = 100;
  raw_generation = 1;
  peers = membership;
  peer_snapshot = members;
  slot_prefixes = &prefix;
  assert(!peak_mpi_causal_runtime_queue_pending());
  assert(scheduled ? peak_mpi_causal_runtime_tick(2, 2, 0, 0, 2)
                   : peak_mpi_causal_runtime_drain(2, 2, 0, 0, 2));
  PeakMpiCausalServiceStats stats;
  peak_mpi_causal_service_stats(service, &stats);
  assert(stats.records < 4000 && peak_mpi_causal_runtime_queue_pending());
  unsigned turns = 1;
  while (peak_mpi_causal_runtime_queue_pending()) {
    assert(peak_mpi_causal_runtime_drain(2, 2, 0, 0, 2));
    assert(++turns < 8);
  }
  peak_mpi_causal_service_stats(service, &stats);
  assert(stats.records == 4000);
  /* Backend housekeeping may consume work; no IO must not request a retry. */
  peak_mpi_causal_runtime_drain(2, 2, 0, 0, 2);
  assert(!peak_mpi_causal_runtime_queue_pending());
  printf("real TCP IO continuation: scheduled=%d records=%llu bounded_turns=%u "
         "idle_pending=0\n", scheduled, (unsigned long long)stats.records, turns);
  peak_mpi_observer_release(source, &writer);
  peak_mpi_observer_destroy(source);
  peak_mpi_causal_transport_destroy(client);
  peak_mpi_causal_service_destroy(service);
  service = NULL;
  peers = NULL;
  peer_snapshot = NULL;
  slot_prefixes = NULL;
}
static unsigned retry_notices, retry_accepted;
static void retry_notify(void) { ++retry_notices; }
static int retry_record(void *context, unsigned rank,
                        const PeakMpiObserverRecord *record) {
  (void)context;
  (void)record;
  assert(rank == 1);
  ++retry_accepted;
  return 1;
}
static void test_nonempty_retry_notification(int ack_available) {
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 2;
  c.owner_rank = 0;
  c.incarnation = 100;
  c.job_context = 99;
  c.address = "127.0.0.1";
  c.port = 39887;
  c.slots = 1;
  c.queue_frames = 256;
  c.max_operations = 64;
  c.max_context_parts = 32;
  c.max_pending_records = 64;
  c.model = (PeakMpiCostConfig){1024, 4096, 1024, 128, 2, 64};
  c.policy = (PeakMpiPolicyConfig){2, .1, 1, .5, 0, 0, 1};
  PeakMpiCausalTransportSink sink = {.record = retry_record};
  PeakMpiCausalTransport *owner = peak_mpi_causal_transport_create(&c, &sink);
  assert(owner);
  c.rank = 1;
  c.incarnation = 101;
  service = peak_mpi_causal_service_create(&c);
  assert(service);
  unsigned char members[2] = {1, 1};
  uint64_t prefix = 0;
  PeakMpiCausalRaw raw = {.incarnation = 101, .generation = 1, .active = 1,
      .elapsed = 1, .known_peers = members, .peer_count = 2,
      .slot_prefixes = &prefix, .slot_count = 1};
  PeakMpiCausalRaw owner_raw = raw;
  owner_raw.incarnation = 100;
  peak_mpi_causal_transport_tick(owner, NULL, &owner_raw, 1, 65536);
  for (unsigned i = 0; i < 32; ++i) {
    peak_mpi_causal_service_tick(service, NULL, &raw, 1, 65536, 16384);
    peak_mpi_causal_transport_tick(owner, NULL, NULL, 1, 65536);
  }
  PeakMpiObserverConfig config = {1, 256, 1, 101};
  capture = peak_mpi_observer_create(&config);
  assert(capture);
  peak_mpi_observer_activate(capture);
  peak_mpi_observer_set_notify(capture, retry_notify);
  PeakMpiObserverProducer writer;
  assert(!peak_mpi_observer_claim(capture, 0, 1, 1, &writer));
  PeakMpiObserverRecord event = {.kind = PEAK_MPI_OBSERVER_EVENT_START, .op_uid = 1};
  for (unsigned i = 0; i < 256; ++i)
    assert(!peak_mpi_observer_emit(capture, &writer, &event));
  peak_mpi_causal_service_tick(service, capture, NULL, 1, 65536, 16384);
  atomic_store(&active, 1);
  atomic_store(&backend_ready, 1);
  heartbeat_enabled = 1;
  rank_id = 1;
  owner_rank = 0;
  world_size = 2;
  incarnation = 101;
  effective_byte_budget = 65536;
  observer_work_budget = 16384;
  assert(!peak_mpi_policy_receiver_init(&receiver, &c.policy,
      (PeakMpiPolicyCohortKey){99, 0, 1}, (PeakMpiPolicyMemberId){1, 101}, 2));
  int progress = 1;
  for (unsigned i = 0; progress && i < 16; ++i)
    progress = peak_mpi_causal_runtime_drain(1, 1, 0, 0, 0);
  assert(!progress && peak_mpi_causal_runtime_queue_pending());
  PeakMpiObserverSlotState before, after;
  assert(peak_mpi_observer_slot_state(capture, 0, &before));
  assert(before.published == 256 && before.consumed == 254);
  retry_accepted = 0;
  PeakMpiCausalServiceStats sent_before, sent_after;
  peak_mpi_causal_transport_stats(owner, &sent_before);
  if (ack_available)
    for (unsigned i = 0; i < 32; ++i)
      peak_mpi_causal_transport_tick(owner, NULL, NULL, 1, 65536);
  peak_mpi_causal_transport_stats(owner, &sent_after);
  assert(ack_available ? retry_accepted == 254 &&
      sent_after.sent_bytes > sent_before.sent_bytes : !retry_accepted);
  /* The ring never empties while replay waits for ACK: empty-only notification
   * cannot restart the heartbeat's suppressed zero-progress retry. */
  unsigned notices = retry_notices;
  for (unsigned i = 0; i < 64; ++i)
    assert(!peak_mpi_observer_emit(capture, &writer, &event));
  assert(retry_notices == notices + 1);
  progress = peak_mpi_causal_runtime_drain(1, 1, 0, 0, 0);
  assert(ack_available ? progress : !progress);
  assert(peak_mpi_observer_slot_state(capture, 0, &after));
  assert(ack_available ? after.consumed > before.consumed :
      after.consumed == before.consumed);
  assert(!after.lost);
  peak_mpi_causal_service_destroy(service);
  service = NULL;
  peak_mpi_causal_transport_destroy(owner);
  peak_mpi_observer_destroy(capture);
  capture = NULL;
  atomic_store(&active, 0);
  atomic_store(&backend_ready, 0);
  printf("nonempty retry notification ACK_available=%d progress=%d consumed=%llu PASS\n",
      ack_available, progress, (unsigned long long)after.consumed);
}

int main(void) {
  PeakMpiObserverConfig cfg = {1, 8, 1, 1};
  capture = peak_mpi_observer_create(&cfg);
  assert(capture);
  slots = 1;
  atomic_store(&ready, 1);
  atomic_store(&backend_ready, 0);
  PeakMpiObserverRecord a = {0}, b = {0};
  peak_mpi_causal_runtime_boundary(&a);
  assert(a.control_generation == 2);
  assert(!pthread_barrier_init(&enter, NULL, 2));
  assert(!pthread_barrier_init(&finished, NULL, 2));
  pthread_t t;
  assert(!pthread_create(&t, NULL, heartbeat, NULL));
  sample_no = 1;
  pause_reload = 1;
  peak_mpi_causal_runtime_boundary(&b);
  assert(!pthread_join(t, NULL));
  printf("snapshot_listener_generation=2 shared_before_tick=%llu "
         "stale_tick_argument=1 shared_after_tick=%llu boundary_a=%llu "
         "boundary_b=%llu costsP=%llu/%llu A=%llu/%llu L=%llu/%llu\n",
         (unsigned long long)before_tick, (unsigned long long)after_tick,
         (unsigned long long)a.control_generation,
         (unsigned long long)b.control_generation, (unsigned long long)a.P_cut,
         (unsigned long long)b.P_cut, (unsigned long long)a.A_cut,
         (unsigned long long)b.A_cut, (unsigned long long)a.L_cut,
         (unsigned long long)b.L_cut);
  assert(before_tick == 2 && after_tick == 1);
  assert(b.control_generation == 2);
  PeakMpiPolicyConfig c = {1, .1, 1, .5, 0, 0, 1};
  PeakMpiPolicyCohortKey key = {99, 0, 1};
  PeakMpiPolicyMemberId id = {0, 99};
  PeakMpiPolicy *p = peak_mpi_policy_create(&c, key, &id, 1, 1, 1, 0);
  assert(p);
  PeakMpiPolicySample sa = sample(&a, 1), sb = sample(&b, 2);
  assert(peak_mpi_policy_record_raw(p, key, &sa, 1) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicyResult status = peak_mpi_policy_record_raw(p, key, &sb, 2);
  PeakMpiPolicyFeedback f;
  assert(peak_mpi_policy_feedback(p, id, 2, &f) == PEAK_MPI_POLICY_OK);
  printf("actual_policy_second_raw_status=%d mode=%d reason=%d "
         "coherent_snapshot_required=1\n",
         status, f.mode, f.reason);
  assert(status == PEAK_MPI_POLICY_OK && f.reason != PEAK_MPI_POLICY_BAD_CUT);
  puts("actual runtime boundary/tick interleaving policy outcome checked");
  peak_mpi_policy_free(p);
  uint64_t prefix = 0;
  slot_prefixes = &prefix;
  origin = clock_seconds() - 1;
  PeakMpiCausalRaw raw_a = {0}, raw_b = {0};
  sample_no = 2;
  freeze_raw(&raw_a);
  assert(!pthread_create(&t, NULL, heartbeat, NULL));
  sample_no = 3;
  pause_reload = 1;
  freeze_raw(&raw_b);
  assert(!pthread_join(t, NULL));
  printf("actual freeze_raw control=%llu/%llu elapsed=%.17g/%.17g P=%llu/%llu "
         "A=%llu/%llu L=%llu/%llu\n",
         (unsigned long long)raw_a.control_generation,
         (unsigned long long)raw_b.control_generation, raw_a.elapsed,
         raw_b.elapsed, (unsigned long long)raw_a.cut.P_ns,
         (unsigned long long)raw_b.cut.P_ns, (unsigned long long)raw_a.cut.A_ns,
         (unsigned long long)raw_b.cut.A_ns, (unsigned long long)raw_a.cut.L_ns,
         (unsigned long long)raw_b.cut.L_ns);
  p = peak_mpi_policy_create(&c, key, &id, 1, 1, 1, 0);
  assert(p);
  sa = (PeakMpiPolicySample){.member = id,
                             .generation = 1,
                             .event_cut = 1,
                             .control_generation = raw_a.control_generation,
                             .L = raw_a.L,
                             .P = raw_a.P,
                             .A = raw_a.A,
                             .app_cost_known = 1,
                             .cut = raw_a.cut,
                             .elapsed = raw_a.elapsed};
  sb = (PeakMpiPolicySample){.member = id,
                             .generation = 2,
                             .event_cut = 2,
                             .control_generation = raw_b.control_generation,
                             .L = raw_b.L,
                             .P = raw_b.P,
                             .A = raw_b.A,
                             .app_cost_known = 1,
                             .cut = raw_b.cut,
                             .elapsed = raw_b.elapsed};
  assert(peak_mpi_policy_record_raw(p, key, &sa, 1) == PEAK_MPI_POLICY_OK);
  status = peak_mpi_policy_record_raw(p, key, &sb, 2);
  assert(peak_mpi_policy_feedback(p, id, 2, &f) == PEAK_MPI_POLICY_OK);
  printf("actual freeze_raw policy status=%d mode=%d reason=%d\n", status,
         f.mode, f.reason);
  assert(raw_b.control_generation == 2 && status == PEAK_MPI_POLICY_OK &&
         f.reason != PEAK_MPI_POLICY_BAD_CUT);
  peak_mpi_policy_free(p);
  assert(!pthread_barrier_destroy(&enter));
  assert(!pthread_barrier_destroy(&finished));
  peak_mpi_observer_release(capture, &producer);
  peak_mpi_observer_destroy(capture);
  capture = NULL;
  test_transport_continuation(0);
  test_transport_continuation(1);
  test_nonempty_retry_notification(1);
  test_nonempty_retry_notification(0);
  puts("coherent event/RAW control generation survives stale heartbeat store "
       "PASS");
}
