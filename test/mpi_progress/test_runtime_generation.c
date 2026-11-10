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
  puts("coherent event/RAW control generation survives stale heartbeat store "
       "PASS");
}
