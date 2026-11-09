#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "internal/mpi_causal_runtime.h"
#include "internal/mpi_causal_backend.h"
#include "internal/mpi_causal_cost.h"
#include "internal/mpi_causal_events.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

/* Only the heartbeat owns service and receiver. Application producers touch
 * fixed slots and atomic monotone communication evidence. */
static PeakMpiObserver *capture;
static PeakMpiCausalService *service;
static PeakMpiPolicyReceiver receiver;
static PeakMpiPolicyConfig policy;
static _Atomic int ready, backend_ready, active, unknown;
static _Atomic uint64_t next_operation = 1;
static _Atomic size_t fallback_next;
static _Atomic unsigned char *peers;
static unsigned char *peer_snapshot;
static uint64_t *slot_prefixes;
static size_t slots;
static size_t observer_work_budget = 16384;
static size_t configured_owner_byte_budget = 65536;
static size_t effective_byte_budget = 65536;
static int rank_id, world_size;
static int owner_rank, observer_cpu_requested = -1;
static _Atomic int observer_cpu_effective = -1;
#ifdef __linux__
static _Thread_local int observer_worker, observer_cpu_attempted;
#endif
static uint64_t incarnation, raw_generation;
static double origin, startup, activation_start;
static _Atomic double control_cost, management_cost;
static _Atomic uint64_t control_generation;
static double boundary_price, boundary_slot_price, emit_price;
static double mode_seconds[5], previous_tick, exceeded_seconds,
    maximum_feedback_age;
static uint64_t age_histogram[64], age_samples;
static PeakMpiPolicyDecision last_decision;
static PeakMpiPolicyFeedback last_feedback;
static PeakMpiCausalWorkPrices work_prices;
static _Atomic int fine_paused;
static _Atomic uint64_t capture_epoch = 1;
static uint64_t handled_loss;
static PeakMpiCausalRecovery local_recovery;
static uint64_t *recovery_prefixes;
static int finalizing, service_configuration_reported;
static int heartbeat_enabled = 1, hook_controller_enabled;
static _Thread_local size_t local_slot = SIZE_MAX;
static _Thread_local uint64_t local_generation;
static _Thread_local PeakMpiObserverProducer producer;
static _Thread_local int producer_valid;

extern void peak_mpi_causal_listener_actual_costs(double *, double *,
                                                  uint64_t *);
extern int peak_mpi_causal_registry_active(void);
extern int peak_mpi_causal_listener_internal_enter(void);
extern void peak_mpi_causal_listener_internal_leave(int);
extern int peak_mpi_causal_listener_thread_token(size_t *, uint64_t *);

static double clock_seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}
static uint64_t ns(double x) {
  double scaled = x * 1e9;
  return x > 0 && scaled < (double)UINT64_MAX ? (uint64_t)scaled : 0;
}
static int parse_unsigned(const char *value, uint64_t maximum,
                          uint64_t *result) {
  if (!value || !*value || *value < '0' || *value > '9')
    return 0;
  char *end;
  errno = 0;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (errno || *end || parsed > maximum)
    return 0;
  *result = (uint64_t)parsed;
  return 1;
}
static size_t owner_byte_budget_from_environment(void) {
  const char *value = getenv("PEAK_MPI_OBSERVER_OWNER_BYTE_BUDGET");
  uint64_t parsed;
  if (!value)
    return 65536;
  if (parse_unsigned(value, 16777216, &parsed) && parsed > 0)
    return (size_t)parsed;
  fprintf(stderr, "[peak mpi causal] invalid PEAK_MPI_OBSERVER_OWNER_BYTE_BUDGET; using 65536 (valid range 1..16777216)\n");
  return 65536;
}
int peak_mpi_causal_runtime_requested(void) {
  const char *v = getenv("PEAK_MPI_CAUSAL_FEEDBACK");
  return v && !strcmp(v, "1");
}
int peak_mpi_causal_runtime_needs_service(void) {
  return atomic_load_explicit(&active, memory_order_relaxed);
}
int peak_mpi_causal_runtime_enabled(void) {
  return atomic_load_explicit(&ready, memory_order_acquire);
}
int peak_mpi_causal_runtime_capture_enabled(void) {
  return !atomic_load_explicit(&fine_paused, memory_order_acquire) &&
         heartbeat_enabled && peak_mpi_causal_runtime_enabled() &&
         peak_mpi_observer_coarse_reason(capture) ==
             PEAK_MPI_OBSERVER_REASON_NONE;
}
int peak_mpi_causal_runtime_metadata_enabled(void) {
  uint64_t mask = peak_mpi_observer_fault_mask(capture);
  return heartbeat_enabled && peak_mpi_causal_runtime_enabled() &&
         !(mask & ~(UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW));
}
uint64_t peak_mpi_causal_runtime_loss_generation(void) {
  return peak_mpi_observer_loss_generation(capture);
}
uint64_t peak_mpi_causal_runtime_capture_epoch(void) {
  return atomic_load_explicit(&capture_epoch, memory_order_acquire);
}
uint64_t peak_mpi_causal_runtime_operation_floor(void) {
  return atomic_load_explicit(&next_operation, memory_order_acquire);
}
int peak_mpi_causal_runtime_world_size(void) { return world_size; }
void peak_mpi_causal_runtime_worker_start(void) {
#ifdef __linux__
  observer_worker = 1;
#endif
}
static void bind_observer_worker(void) {
#ifdef __linux__
  if (!observer_worker || observer_cpu_attempted || observer_cpu_requested < 0 ||
      rank_id != owner_rank)
    return;
  observer_cpu_attempted = 1;
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(observer_cpu_requested, &cpus);
  int error = pthread_setaffinity_np(pthread_self(), sizeof cpus, &cpus);
  if (!error)
    atomic_store_explicit(&observer_cpu_effective, observer_cpu_requested,
                          memory_order_relaxed);
  else
    fprintf(stderr, "[peak mpi causal] cannot bind observer CPU %d: %s; retaining existing affinity\n",
            observer_cpu_requested, strerror(error));
#endif
}
int peak_mpi_causal_runtime_prepare(size_t n, double q, double hb, double ema) {
  if (!peak_mpi_causal_runtime_requested())
    return 0;
  double begin = clock_seconds();
  slots = n;
  const char *cpu = getenv("PEAK_MPI_OBSERVER_CPU");
  uint64_t parsed_cpu;
#ifdef __linux__
  if (cpu && parse_unsigned(cpu, CPU_SETSIZE - 1, &parsed_cpu))
    observer_cpu_requested = (int)parsed_cpu;
  else if (cpu)
    fprintf(stderr, "[peak mpi causal] invalid PEAK_MPI_OBSERVER_CPU; retaining existing affinity\n");
#else
  (void)parsed_cpu;
  if (cpu)
    fprintf(stderr, "[peak mpi causal] PEAK_MPI_OBSERVER_CPU is supported only on Linux; retaining existing affinity\n");
#endif
  const char *work_budget_env = getenv("PEAK_MPI_OBSERVER_WORK_BUDGET");
  uint64_t parsed_work_budget;
  observer_work_budget = 16384;
  configured_owner_byte_budget = owner_byte_budget_from_environment();
  effective_byte_budget = 65536;
  if (work_budget_env) {
    if (parse_unsigned(work_budget_env, 4194304, &parsed_work_budget) &&
        parsed_work_budget > 0)
      observer_work_budget = (size_t)parsed_work_budget;
    else
      fprintf(stderr, "[peak mpi causal] invalid PEAK_MPI_OBSERVER_WORK_BUDGET; using 16384 (valid range 1..4194304)\n");
  }
  origin = begin;
  if (!peak_mpi_causal_cost_prepare(n)) {
    fprintf(
        stderr,
        "[peak mpi causal] unavailable: fixed cost-slot allocation failed\n");
    return 0;
  }
  incarnation = ns(begin) ^ (uint64_t)(uintptr_t)&capture;
  PeakMpiObserverConfig c = {n, 256, 4096, incarnation};
  capture = peak_mpi_observer_create(&c);
  slot_prefixes = calloc(n, sizeof(*slot_prefixes));
  recovery_prefixes = calloc(n, sizeof *recovery_prefixes);
  if (!capture || !slot_prefixes || !recovery_prefixes) {
    fprintf(stderr,
            "[peak mpi causal] unavailable: fixed capture allocation failed\n");
    return 0;
  }
  policy = (PeakMpiPolicyConfig){.member_capacity = 1,
                                 .q = q,
                                 .heartbeat_max_seconds = hb,
                                 .ema_alpha = ema,
                                 .min_observation_seconds = hb,
                                 .policy_generation = 1};
  double p, o, start = clock_seconds();
  for (unsigned i = 0; i < 1024; i++)
    peak_mpi_causal_runtime_cost_snapshot(&p, &o);
  boundary_price = (clock_seconds() - start) / 1024;
  start = clock_seconds();
  for (unsigned i = 0; i < 1024; i++)
    peak_mpi_causal_cost_read_prefix(0, &p, &o);
  double empty = (clock_seconds() - start) / 1024;
  start = clock_seconds();
  for (unsigned i = 0; i < 1024; i++)
    peak_mpi_causal_cost_read_prefix(n, &p, &o);
  boundary_slot_price =
      fmax(0, ((clock_seconds() - start) / 1024 - empty) / (double)n);
  PeakMpiObserverConfig calibration_config = {1, 2048, 1, incarnation};
  PeakMpiObserver *calibration = peak_mpi_observer_create(&calibration_config);
  if (calibration) {
    PeakMpiObserverProducer writer;
    PeakMpiObserverRecord record = {.kind = PEAK_MPI_OBSERVER_EVENT_START,
                                    .op_uid = 1,
                                    .flags = PEAK_MPI_CAUSAL_APP_COST_KNOWN};
    peak_mpi_observer_activate(calibration);
    if (peak_mpi_observer_claim(calibration, 0, 1, 1, &writer) ==
        PEAK_MPI_OBSERVER_OK) {
      start = clock_seconds();
      for (unsigned i = 0; i < 1024; i++)
        peak_mpi_observer_emit(calibration, &writer, &record);
      emit_price = (clock_seconds() - start) / 1024;
      peak_mpi_observer_release(calibration, &writer);
    }
    peak_mpi_observer_destroy(calibration);
  }
  startup = clock_seconds() - begin;
  return 1;
}
void peak_mpi_causal_runtime_configure_control(int heartbeat, int controller) {
  heartbeat_enabled = !!heartbeat;
  hook_controller_enabled = !!controller;
  if (capture && !heartbeat_enabled)
    fprintf(stderr, "[peak mpi causal] global control unavailable: existing "
                    "heartbeat disabled\n");
}
void peak_mpi_causal_runtime_activation_begin(void) {
  if (capture)
    activation_start = clock_seconds();
}
void peak_mpi_causal_runtime_activation_end(void) {
  if (capture && activation_start) {
    double now = clock_seconds();
    startup += now - activation_start;
    origin = now;
    activation_start = 0;
  }
}
int peak_mpi_causal_runtime_thread_token(size_t *s, uint64_t *g) {
  if (!capture)
    return 0;
  size_t index;
  uint64_t generation;
  if (!peak_mpi_causal_listener_thread_token(&index, &generation)) {
    if (peak_mpi_causal_registry_active()) {
      peak_mpi_causal_runtime_fail(PEAK_MPI_OBSERVER_REASON_METADATA);
      return 0;
    }
    if (local_slot == SIZE_MAX) {
      local_slot =
          atomic_fetch_add_explicit(&fallback_next, 1, memory_order_relaxed);
      local_generation = 1;
    }
    index = local_slot;
    generation = local_generation;
  }
  if (index >= slots) {
    peak_mpi_causal_runtime_fail(PEAK_MPI_OBSERVER_REASON_METADATA);
    return 0;
  }
  peak_mpi_causal_runtime_note_slot(index);
  *s = index;
  *g = generation;
  return 1;
}
static int get_producer(void) {
  size_t s;
  uint64_t g;
  if (!peak_mpi_causal_runtime_thread_token(&s, &g))
    return 0;
  if (producer_valid && producer.slot == s && producer.generation == g)
    return 1;
  if (producer_valid)
    peak_mpi_observer_release(capture, &producer);
  if (peak_mpi_observer_claim(capture, s, g, s + 1, &producer) !=
      PEAK_MPI_OBSERVER_OK) {
    peak_mpi_causal_runtime_fail(PEAK_MPI_OBSERVER_REASON_METADATA);
    return 0;
  }
  producer_valid = 1;
  return 1;
}
static int cost_component_ns(double value, uint64_t *out) {
  double scaled = value * 1e9;
  if (!isfinite(value) || value < 0 || !isfinite(scaled) || scaled >= (double)UINT64_MAX)
    return 0;
  *out = (uint64_t)scaled;
  return 1;
}
static int cost_add_component(uint64_t *sum, double value) {
  uint64_t part;
  if (!cost_component_ns(value, &part) || part > UINT64_MAX - *sum)
    return 0;
  *sum += part;
  return 1;
}
void peak_mpi_causal_runtime_cost_now(PeakMpiCausalCostSnapshot *s) {
  memset(s, 0, sizeof *s);
  peak_mpi_causal_cost_read(&s->P, &s->producer_observer);
  s->control = atomic_load_explicit(&control_cost, memory_order_relaxed);
  s->heartbeat = atomic_load_explicit(&management_cost, memory_order_relaxed);
  peak_mpi_causal_listener_actual_costs(&s->control, &s->heartbeat,
                                       &s->control_generation);
  atomic_store_explicit(&control_generation, s->control_generation, memory_order_relaxed);
  s->startup = startup;
  s->A = s->P + s->producer_observer + s->startup + s->control;
  s->L = s->A + s->heartbeat;
  s->app_cost_known = cost_component_ns(s->P, &s->cut.P_ns);
  s->cut.A_ns = s->cut.P_ns;
  s->app_cost_known = s->app_cost_known &&
      cost_add_component(&s->cut.A_ns, s->producer_observer) &&
      cost_add_component(&s->cut.A_ns, s->startup) &&
      cost_add_component(&s->cut.A_ns, s->control);
  s->cut.L_ns = s->cut.A_ns;
  s->app_cost_known = s->app_cost_known &&
      cost_add_component(&s->cut.L_ns, s->heartbeat);
  if (!s->app_cost_known)
    peak_mpi_causal_runtime_fail(PEAK_MPI_OBSERVER_REASON_INVALID_COST);
}
void peak_mpi_causal_runtime_cost_components(double *L, double *P, double *A) {
  PeakMpiCausalCostSnapshot s;
  peak_mpi_causal_runtime_cost_now(&s);
  *L = s.L; *P = s.P; *A = s.A;
}
void peak_mpi_causal_runtime_cost_snapshot(double *L, double *P) {
  double A;
  peak_mpi_causal_runtime_cost_components(L, P, &A);
}
void peak_mpi_causal_runtime_boundary(PeakMpiObserverRecord *r) {
  if (!get_producer())
    return;
  peak_mpi_causal_cost_observer_add(
      producer.slot,
      boundary_price +
          boundary_slot_price * (double)peak_mpi_causal_cost_used_slots());
  PeakMpiCausalCostSnapshot cost;
  peak_mpi_causal_runtime_cost_now(&cost);
  r->P_cut = cost.app_cost_known ? cost.cut.P_ns : ns(cost.P);
  r->A_cut = cost.app_cost_known ? cost.cut.A_ns : ns(cost.L);
  r->L_cut = cost.app_cost_known ? cost.cut.L_ns : ns(cost.L);
  if (cost.app_cost_known) r->flags |= PEAK_MPI_CAUSAL_APP_COST_KNOWN;
  else r->flags &= ~PEAK_MPI_CAUSAL_APP_COST_KNOWN;
  r->control_generation =
      atomic_load_explicit(&control_generation, memory_order_relaxed);
  if (r->phase == PEAK_MPI_CAUSAL_GROUP_BEGIN ||
      r->phase == PEAK_MPI_CAUSAL_GROUP_END)
    r->progress_end = ns(clock_seconds() - origin);
}
void peak_mpi_causal_runtime_submit(PeakMpiObserverRecord *r, int remote) {
  if (!peak_mpi_causal_runtime_enabled())
    return;
  if (remote)
    atomic_store_explicit(&active, 1, memory_order_relaxed);
  if (!peak_mpi_causal_runtime_capture_enabled() || !get_producer())
    return;
  peak_mpi_observer_activate(capture);
  peak_mpi_causal_cost_observer_add(producer.slot, emit_price);
  if (!(r->flags & PEAK_MPI_CAUSAL_CUT_PRECAPTURED))
    peak_mpi_causal_runtime_boundary(r);
  r->flags &= ~PEAK_MPI_CAUSAL_CUT_PRECAPTURED;
  peak_mpi_observer_emit(capture, &producer, r);
}
void peak_mpi_causal_runtime_define(const PeakMpiObserverObject *o) {
  uint64_t uid;
  if (capture)
    peak_mpi_observer_object_add(capture, o, &uid);
}
void peak_mpi_causal_runtime_fail(PeakMpiObserverReason why) {
  if (capture)
    peak_mpi_observer_fail(capture, why);
}
uint64_t peak_mpi_causal_runtime_operation_id(void) {
  return atomic_fetch_add_explicit(&next_operation, 1, memory_order_relaxed);
}
size_t peak_mpi_causal_runtime_thread_capacity(void) { return slots; }
void peak_mpi_causal_runtime_note_peer(int peer) {
  if (peer < 0 || peer >= world_size) {
    atomic_store_explicit(&unknown, 1, memory_order_relaxed);
    atomic_store_explicit(&active, 1, memory_order_relaxed);
  } else if (peer != rank_id && peers) {
    atomic_store_explicit(&peers[peer], 1, memory_order_relaxed);
    atomic_store_explicit(&active, 1, memory_order_relaxed);
  }
}
void peak_mpi_causal_runtime_note_members(const int32_t *m, size_t count) {
  for (size_t i = 0; i < count; i++)
    peak_mpi_causal_runtime_note_peer(m[i]);
}
int peak_mpi_causal_runtime_internal_enter(void) {
  return peak_mpi_causal_listener_internal_enter();
}
void peak_mpi_causal_runtime_internal_leave(int prior) {
  peak_mpi_causal_listener_internal_leave(prior);
}
void peak_mpi_causal_runtime_mpi_initialized(void) {
  if (!capture || peak_mpi_causal_runtime_enabled())
    return;
  double initialization_start = clock_seconds();
  PMPI_Comm_rank(MPI_COMM_WORLD, &rank_id);
  PMPI_Comm_size(MPI_COMM_WORLD, &world_size);
  peers = calloc((size_t)world_size, sizeof(*peers));
  peer_snapshot = calloc((size_t)world_size, 1);
  if (!peers || !peer_snapshot) {
    fprintf(
        stderr,
        "[peak mpi causal] unavailable: fixed membership allocation failed\n");
    return;
  }
  for (int i = 0; i < world_size; i++)
    atomic_init(&peers[i], 0);
  const char *address = getenv("PEAK_MPI_OBSERVER_ADDRESS"),
             *port = getenv("PEAK_MPI_OBSERVER_PORT"),
             *job = getenv("PEAK_MPI_JOB_CONTEXT");
  const char *owner = getenv("PEAK_MPI_OBSERVER_RANK");
  uint64_t context = 0, parsed_port = 0, parsed_owner = 0;
  int configuration_valid =
      parse_unsigned(job, UINT64_MAX, &context) && context &&
      parse_unsigned(port, 65535, &parsed_port) && parsed_port &&
      (!owner ||
       parse_unsigned(owner, (uint64_t)world_size - 1, &parsed_owner));
  effective_byte_budget = configuration_valid && rank_id == (int)parsed_owner
                              ? configured_owner_byte_budget : 65536;
  owner_rank = (int)parsed_owner;
  policy.member_capacity = (size_t)world_size;
  PeakMpiCausalServiceConfig c = {0};
  c.rank = (unsigned)rank_id;
  c.world_size = (unsigned)world_size;
  c.owner_rank = (unsigned)parsed_owner;
  c.job_context = context;
  c.incarnation = incarnation;
  c.address = address;
  c.port = (unsigned)parsed_port;
  c.slots = slots;
  c.queue_frames = 4096;
  c.max_operations = 16384;
  c.max_context_parts = 4096;
  c.max_pending_records = 32768;
  c.model = (PeakMpiCostConfig){
      65536, 262144, 65536, 32768, (uint32_t)world_size, 32};
  c.policy = policy;
  if (heartbeat_enabled && configuration_valid && address &&
      c.owner_rank < (unsigned)world_size)
    service = peak_mpi_causal_service_create(&c);
  PeakMpiPolicyCohortKey key = {context, (uint64_t)rank_id, 1};
  PeakMpiPolicyMemberId member = {(uint64_t)rank_id, context};
  peak_mpi_policy_receiver_init(&receiver, &policy, key, member,
                                clock_seconds());
  atomic_store_explicit(&ready, 1, memory_order_release);
  peak_mpi_causal_events_init();
  PeakMpiCausalWorkPrices measured;
  peak_mpi_causal_events_calibrate_work(&measured);
  work_prices = measured;
  /* An enclosing activation span already prices this startup interval. */
  if (!activation_start)
    startup += clock_seconds() - initialization_start;
  atomic_store_explicit(&backend_ready, 1, memory_order_release);
}
/* A cohort's permanent failure also ends fine production locally. This does
 * not touch counters, object lifetime or temporary feedback-stale state. */
static void latch_capture_coarse(void) {
  if (!receiver.permanent_coarse)
    return;
  if (receiver.coarse_reason == PEAK_MPI_POLICY_EVENT_LOSS &&
      (peak_mpi_observer_fault_mask(capture) ==
           (UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW) ||
       (last_feedback.recovery.loss && !last_feedback.recovery.fault_mask)))
    return;
  PeakMpiObserverReason reason;
  switch (receiver.coarse_reason) {
  case PEAK_MPI_POLICY_MISSING_CONTRACT:
    reason = PEAK_MPI_OBSERVER_REASON_CAPABILITY;
    break;
  case PEAK_MPI_POLICY_RESOURCE_LIMIT:
    reason = PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY;
    break;
  case PEAK_MPI_POLICY_BAD_IDENTITY:
    reason = PEAK_MPI_OBSERVER_REASON_IDENTITY;
    break;
  case PEAK_MPI_POLICY_EVENT_LOSS:
    reason = PEAK_MPI_OBSERVER_REASON_METADATA;
    break;
  default:
    reason = PEAK_MPI_OBSERVER_REASON_METADATA;
    break;
  }
  peak_mpi_causal_runtime_fail(reason);
}
static void freeze_raw(PeakMpiCausalRaw *raw) {
  raw->lost_events = 0;
  for (size_t i = 0; i < slots; i++) {
    PeakMpiObserverSlotState state;
    if (peak_mpi_observer_slot_state(capture, i, &state)) {
      slot_prefixes[i] =
          atomic_load_explicit(&capture_epoch, memory_order_relaxed) > 1
              ? state.last_attempt_sequence
              : state.published;
      raw->lost_events += state.lost;
    }
  }
  PeakMpiCausalCostSnapshot cost;
  peak_mpi_causal_runtime_cost_now(&cost);
  raw->L = cost.L; raw->P = cost.P; raw->A = cost.A;
  raw->cut = cost.cut;
  raw->app_cost_known = cost.app_cost_known;
  raw->control_generation =
      atomic_load_explicit(&control_generation, memory_order_relaxed);
  raw->elapsed = clock_seconds() - origin;
}
static int freeze_idle_raw(uint64_t previous_closed_group, void *data) {
  PeakMpiCausalRaw *raw = data;
  freeze_raw(raw);
  raw->idle_cut = 1;
  raw->idle_group_uid = previous_closed_group;
  return 1;
}
void peak_mpi_causal_runtime_tick(double now, double elapsed, double C,
                                  double M, uint64_t generation) {
  atomic_store_explicit(&control_cost, C, memory_order_relaxed);
  atomic_store_explicit(&management_cost, M, memory_order_relaxed);
  atomic_store_explicit(&control_generation, generation, memory_order_relaxed);
  if (!atomic_load_explicit(&backend_ready, memory_order_acquire))
    return;
  bind_observer_worker();
  uint64_t notified = peak_mpi_observer_loss_generation(capture);
  if (notified != handled_loss) {
    handled_loss = notified;
    uint64_t pending_epoch = local_recovery.epoch;
    memset(&local_recovery, 0, sizeof local_recovery);
    uint64_t current =
        atomic_load_explicit(&capture_epoch, memory_order_relaxed);
    local_recovery.epoch = pending_epoch > current ? pending_epoch : current;
  }
  int enabled = atomic_load_explicit(&active, memory_order_relaxed);
  size_t known_count = 1;
  for (int i = 0; i < world_size; i++)
    if (atomic_load_explicit(&peers[i], memory_order_relaxed))
      known_count++;
  if (enabled && !receiver.active)
    peak_mpi_policy_receiver_rebind(&receiver, receiver.cohort, known_count, 0,
                                    now);
  else if (enabled && known_count > receiver.known_member_count)
    peak_mpi_policy_receiver_note_membership_hint(&receiver, known_count, now);
  PeakMpiObserverReason reason = peak_mpi_observer_coarse_reason(capture);
  if (!service || !heartbeat_enabled) {
    if (enabled) {
      if (heartbeat_enabled && !service_configuration_reported) {
        fprintf(stderr,
                "[peak mpi causal] rank=%d global control unavailable: "
                "invalid/missing observer endpoint/context or fixed service "
                "allocation\n",
                rank_id);
        service_configuration_reported = 1;
      }
      peak_mpi_policy_receiver_mark_coarse(
          &receiver,
          reason == PEAK_MPI_OBSERVER_REASON_CAPABILITY
              ? PEAK_MPI_POLICY_MISSING_CONTRACT
              : PEAK_MPI_POLICY_BAD_IDENTITY,
          now);
    }
    latch_capture_coarse();
    return;
  }
  for (int i = 0; i < world_size; i++)
    peer_snapshot[i] = atomic_load_explicit(&peers[i], memory_order_relaxed);
  uint64_t watermark_nonce = peak_mpi_causal_service_requested_nonce(service);
  PeakMpiCausalRaw raw = {
      .generation = ++raw_generation,
      .incarnation = incarnation,
      .control_generation = generation,
      .L = 0,
      .P = 0,
      .elapsed = elapsed,
      .contract_profile = peak_mpi_causal_events_contract()->profile,
      .active = enabled,
      .unknown_members = atomic_load_explicit(&unknown, memory_order_relaxed),
      .coarse_reason = reason,
      .known_peers = peer_snapshot,
      .peer_count = (size_t)world_size,
      .finalized = finalizing,
      .slot_prefixes = slot_prefixes,
      .slot_count = slots,
      .watermark_nonce = watermark_nonce};
  if (!peak_mpi_causal_events_idle_cut(freeze_idle_raw, &raw))
    freeze_raw(&raw);
  if (local_recovery.state == PEAK_MPI_RECOVERY_READY &&
      local_recovery.loss != peak_mpi_observer_loss_generation(capture)) {
    uint64_t epoch = local_recovery.epoch;
    memset(&local_recovery, 0, sizeof local_recovery);
    local_recovery.epoch = epoch;
  }
  raw.recovery = local_recovery;
  raw.recovery.loss = raw.lost_events;
  raw.recovery.fault_mask = peak_mpi_observer_fault_mask(capture);
  if (local_recovery.state == PEAK_MPI_RECOVERY_READY) {
    memcpy(slot_prefixes, recovery_prefixes, slots * sizeof *slot_prefixes);
  } else if (raw.recovery.loss && raw.recovery.state != PEAK_MPI_RECOVERY_ACK) {
    (void)peak_mpi_causal_events_recovery(&raw.recovery);
  }
  double A = raw.A;

  peak_mpi_causal_service_tick(service, capture, &raw, now, effective_byte_budget, observer_work_budget);
  PeakMpiPolicyReason service_fault =
      peak_mpi_causal_service_fault_reason(service);
  if (service_fault != PEAK_MPI_POLICY_NO_REASON)
    peak_mpi_policy_receiver_mark_coarse(
        &receiver,
        service_fault >= PEAK_MPI_POLICY_MISSING_CONTRACT
            ? service_fault
            : PEAK_MPI_POLICY_BAD_IDENTITY,
        now);
  PeakMpiPolicyFeedback f;
  if (peak_mpi_causal_service_feedback(service, &f)) {
    uint64_t hard = peak_mpi_observer_fault_mask(capture) &
                    ~(UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW);
    if (receiver.permanent_coarse &&
        receiver.coarse_reason != PEAK_MPI_POLICY_EVENT_LOSS)
      hard |= UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_METADATA;
    if (!hard && f.recovery.state == PEAK_MPI_RECOVERY_QUERY &&
        f.recovery.epoch >
            atomic_load_explicit(&capture_epoch, memory_order_relaxed)) {
      if (local_recovery.epoch != f.recovery.epoch ||
          local_recovery.state != PEAK_MPI_RECOVERY_READY) {
        PeakMpiCausalRecovery candidate = f.recovery;
        atomic_store_explicit(&fine_paused, 1, memory_order_release);
        if (peak_mpi_causal_events_recovery(&candidate)) {
          PeakMpiCausalCostSnapshot cost;
          peak_mpi_causal_runtime_cost_now(&cost);
          candidate.L = cost.L; candidate.P = cost.P; candidate.A = cost.A;
          candidate.cut = cost.cut; candidate.app_cost_known = cost.app_cost_known;
          candidate.elapsed = clock_seconds() - origin;
          for (size_t i = 0; i < slots; i++) {
            PeakMpiObserverSlotState st;
            peak_mpi_observer_slot_state(capture, i, &st);
            recovery_prefixes[i] = st.last_attempt_sequence;
          }
          local_recovery = candidate;
        }
      }
    } else if (!hard && f.recovery.state == PEAK_MPI_RECOVERY_ACK &&
               local_recovery.state == PEAK_MPI_RECOVERY_READY &&
               f.recovery.epoch == local_recovery.epoch) {
      uint64_t mask = peak_mpi_observer_fault_mask(capture);
      int resumed = mask ? peak_mpi_observer_recover_generation(
                               capture, local_recovery.loss)
                         : 1;
      if (!mask)
        peak_mpi_observer_discard(capture);
      if (resumed) {
        peak_mpi_causal_events_recovery_adopt(f.recovery.epoch);
        atomic_store_explicit(&capture_epoch, f.recovery.epoch,
                              memory_order_release);
        local_recovery.state = PEAK_MPI_RECOVERY_ACK;
        atomic_store_explicit(&fine_paused, 0, memory_order_release);
        peak_mpi_policy_receiver_recover(&receiver, now);
        reason = peak_mpi_observer_coarse_reason(capture);
      }
    }

    if (memcmp(&f.cohort, &receiver.cohort, sizeof(f.cohort)))
      peak_mpi_policy_receiver_rebind(&receiver, f.cohort, f.member_count,
                                      f.membership_complete, now);
    peak_mpi_policy_receiver_accept(&receiver, &f, now);
    last_feedback = f;
  }
  if (enabled && reason != PEAK_MPI_OBSERVER_REASON_NONE)
    peak_mpi_policy_receiver_mark_coarse(
        &receiver,
        reason == PEAK_MPI_OBSERVER_REASON_CAPABILITY
            ? PEAK_MPI_POLICY_MISSING_CONTRACT
            : PEAK_MPI_POLICY_EVENT_LOSS,
        now);
  latch_capture_coarse();
  if ((raw.app_cost_known
           ? peak_mpi_policy_receiver_snapshot_cut(&receiver, now, raw.elapsed, raw.cut, 0, &last_decision)
           : peak_mpi_policy_receiver_snapshot(&receiver, now, raw.elapsed, raw.L, 0, &last_decision)) == PEAK_MPI_POLICY_OK) {
    double dt = previous_tick ? fmax(0, now - previous_tick) : 0;
    mode_seconds[last_decision.mode] += dt;
    if (A > policy.q * raw.elapsed)
      exceeded_seconds += dt;
    if (receiver.have_feedback &&
        isfinite(last_decision.feedback_age_seconds) &&
        last_decision.feedback_age_seconds >= 0) {
      double age = last_decision.feedback_age_seconds;
      maximum_feedback_age = fmax(maximum_feedback_age, age);
      unsigned bucket = 0;
      while (bucket < 63 && age > 0.000001) {
        age *= 0.5;
        bucket++;
      }
      age_histogram[bucket]++;
      age_samples++;
    }
  }
  previous_tick = now;
}
int peak_mpi_causal_runtime_decision(double now, double elapsed,
                                     double headroom,
                                     PeakMpiPolicyDecision *d) {
  if (!peak_mpi_causal_runtime_enabled() ||
      !atomic_load_explicit(&active, memory_order_relaxed))
    return 0;
  PeakMpiCausalCostSnapshot cost;
  peak_mpi_causal_runtime_cost_now(&cost);
  return (cost.app_cost_known
      ? peak_mpi_policy_receiver_snapshot_cut(&receiver, now, elapsed, cost.cut, headroom, d)
      : peak_mpi_policy_receiver_snapshot(&receiver, now, elapsed, cost.L, headroom, d)) == PEAK_MPI_POLICY_OK;
}
void peak_mpi_causal_runtime_report(void) {
  if (!peak_mpi_causal_runtime_enabled())
    return;
  const PeakMpiProgressContract *contract = peak_mpi_causal_events_contract();
  fprintf(stderr,
          "[peak mpi causal recovery] rank=%d epoch=%llu quality=%s "
          "lost_history=%llu state=%llu\n",
          rank_id, (unsigned long long)local_recovery.epoch,
          last_feedback.approximate ? "approx" : "original",
          (unsigned long long)last_feedback.lost_events,
          (unsigned long long)local_recovery.state);
  fprintf(stderr,
          "[peak mpi causal contract] rank=%d reason=%s profile=%s fabrics=%s "
          "ofi_provider=%s fi_provider=%s async_setting=%s effective_async=%d "
          "async_verified=%d provided_thread_level=%d progress_estimate=%d\n",
          rank_id, contract->reason, contract->requested_profile,
          contract->fabrics, contract->ofi_provider, contract->fi_provider,
          contract->async_setting, contract->effective_async_progress,
          contract->async_verified, contract->provided_thread_level,
          contract->progress_estimate);
  fprintf(stderr,
          "[peak mpi causal mpi_config] rank=%d ucx_tls=%s ucx_proto=%s "
          "ucx_tm=%s pml=%s coll=%s pml_verified=%d coll_verified=%d "
          "fixed_rules_verified=%d\n",
          rank_id, contract->ucx_tls, contract->ucx_proto, contract->ucx_tm,
          contract->pml, contract->coll, contract->pml_verified,
          contract->coll_verified, contract->fixed_rules_verified);
  fprintf(stderr,
          "[peak mpi causal control] rank=%d heartbeat=%s hook_controller=%s work_budget=%zu configured_slots=%zu configured_owner_byte_budget=%zu effective_byte_budget=%zu observer_cpu_requested=%d observer_cpu_effective=%d\n",
          rank_id, heartbeat_enabled ? "enabled" : "disabled",
          hook_controller_enabled ? "enabled" : "none", observer_work_budget, slots,
          configured_owner_byte_budget, effective_byte_budget,
          observer_cpu_requested,
          atomic_load_explicit(&observer_cpu_effective, memory_order_relaxed));
  PeakMpiCausalServiceStats stats = {0};
  if (service)
    peak_mpi_causal_service_stats(service, &stats);
  uint64_t published = 0, lost_local = 0;
  for (size_t i = 0; i < slots; i++) {
    PeakMpiObserverSlotState state;
    if (peak_mpi_observer_slot_state(capture, i, &state)) {
      published += state.published;
      lost_local += state.lost;
    }
  }
  PeakMpiCausalCostSnapshot cost;
  peak_mpi_causal_runtime_cost_now(&cost);
  double L = cost.L, P = cost.P, A = cost.A;
  double priced_profile = cost.P, producer_observer = cost.producer_observer;
  double actual_control = cost.control, actual_management = cost.heartbeat;
  fprintf(stderr,
          "[peak mpi causal cost] rank=%d profile_seconds=%.12g "
          "control_seconds=%.12g management_seconds=%.12g "
          "producer_observer_seconds=%.12g startup_seconds=%.12g L=%.12g app_cost=%.12g excluded_heartbeat=%.12g\n",
          rank_id, priced_profile, actual_control, actual_management,
          producer_observer, startup, L, A, actual_management);
  fprintf(stderr,
          "[peak mpi causal app_cost] rank=%d known=%d A_cut=%.12g latest_A=%.12g received_app_sum=%.12g received_raw_sum=%.12g billable_risk=%.12g\n",
          rank_id, last_feedback.app_cost_known, last_feedback.A_cut, last_feedback.local_latest_A,
          last_feedback.received_app_sum, last_feedback.received_raw_sum,
          last_feedback.risk);
  if (cost.app_cost_known)
    peak_mpi_policy_receiver_snapshot_cut(&receiver, clock_seconds(), clock_seconds() - origin,
                                          cost.cut, 0, &last_decision);
  else peak_mpi_policy_receiver_snapshot(&receiver, clock_seconds(), clock_seconds() - origin,
                                          L, 0, &last_decision);
  double age_p50 = NAN, age_p95 = NAN, age_p99 = NAN;
  uint64_t cumulative = 0;
  for (unsigned bucket = 0; bucket < 64; bucket++) {
    cumulative += age_histogram[bucket];
    if (age_samples && isnan(age_p50) &&
        (double)cumulative >= 0.5 * (double)age_samples)
      age_p50 = ldexp(1e-6, (int)bucket);
    if (age_samples && isnan(age_p95) &&
        (double)cumulative >= 0.95 * (double)age_samples)
      age_p95 = ldexp(1e-6, (int)bucket);
    if (age_samples && isnan(age_p99) &&
        (double)cumulative >= 0.99 * (double)age_samples)
      age_p99 = ldexp(1e-6, (int)bucket);
  }
  fprintf(
      stderr,
      "[peak mpi causal latency] rank=%d age_p50_upper=%.9g age_p95_upper=%.9g "
      "age_p99_upper=%.9g age_max_observed=%.9g samples=%llu "
      "oldest_unresolved=%.9g\n",
      rank_id, age_p50, age_p95, age_p99,
      age_samples ? maximum_feedback_age : NAN, (unsigned long long)age_samples,
      last_feedback.oldest_unresolved_age);
  static const char *mode_names[] = {"local", "complete", "stale", "coarse",
                                     "approx"};
  fprintf(stderr,
          "[peak mpi causal feedback] rank=%d mode=%s global_control=%s "
          "policy_reason=%u "
          "covered=%llu age=%.9g g=%.9g D=%.9g risk=%.9g risk_known=%d "
          "covered_complete=%d covered_local_L_minus_D=%.9g "
          "local_seconds=%.9g fine_seconds=%.9g stale_seconds=%.9g "
          "coarse_seconds=%.9g approximate_seconds=%.9g approximate_D=%.9g "
          "estimated_budget_exceeded_seconds=%.9g\n",
          rank_id, mode_names[last_decision.mode],
          last_decision.global_available ? "available" : "unavailable",
          (unsigned)last_decision.reason,
          (unsigned long long)last_feedback.event_cut,
          last_decision.feedback_age_seconds, last_decision.g,
          last_feedback.coverage_complete &&
                  last_feedback.mode == PEAK_MPI_POLICY_COMPLETE
              ? last_feedback.D_cut
              : NAN,
          last_feedback.risk_known ? last_feedback.risk : NAN,
          last_feedback.risk_known, last_feedback.coverage_complete,
          last_feedback.coverage_complete &&
                  last_feedback.mode == PEAK_MPI_POLICY_COMPLETE
              ? last_feedback.L_cut - last_feedback.D_cut
              : NAN,
          mode_seconds[0], mode_seconds[1], mode_seconds[2], mode_seconds[3],
          mode_seconds[4],
          last_feedback.approximate ? last_feedback.D_cut : NAN,
          exceeded_seconds);
  fprintf(
      stderr,
      "[peak mpi causal] rank=%d active=%d contract=%s reason=%u L=%.9g P=%.9g "
      "events_published=%llu backend_records=%llu lost_local=%llu "
      "lost_backend=%llu "
      "pending=%zu pending_peak=%zu active_operations=%zu "
      "oldest_unresolved_seconds=%.9g observer_cpu=%.9g "
      "sent=%llu received=%llu\n",
      rank_id, atomic_load(&active), contract->name,
      (unsigned)peak_mpi_observer_coarse_reason(capture), L, P,
      (unsigned long long)published, (unsigned long long)stats.records,
      (unsigned long long)lost_local, (unsigned long long)stats.lost_events,
      stats.pending_records, stats.pending_peak, stats.active_operations,
      stats.oldest_unresolved_age, stats.observer_cpu_seconds,
      (unsigned long long)stats.sent_bytes,
      (unsigned long long)stats.received_bytes);
}
void peak_mpi_causal_runtime_finalize(void) {
  if (peak_mpi_causal_runtime_enabled() && !finalizing) {
    finalizing = 1;
    peak_mpi_causal_events_finalize_begin();
    double now = clock_seconds();
    struct timespec cpu_begin, cpu_end;
    int have_cpu_begin = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_begin) == 0;
    peak_mpi_causal_runtime_tick(
        now, now - origin,
        atomic_load_explicit(&control_cost, memory_order_relaxed),
        atomic_load_explicit(&management_cost, memory_order_relaxed),
        atomic_load_explicit(&control_generation, memory_order_relaxed));
    /* This bounded drain executes synchronously on the finalizing caller.
     * The original PMPI_Finalize duration is outside this measured interval. */
    if (have_cpu_begin && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0)
      peak_mpi_causal_runtime_observer_cost(
          (double)(cpu_end.tv_sec - cpu_begin.tv_sec) +
          (double)(cpu_end.tv_nsec - cpu_begin.tv_nsec) * 1e-9);
    peak_mpi_causal_runtime_report();
  }
}

void peak_mpi_causal_runtime_retire_slot(size_t slot, uint64_t generation) {
  if (capture) {
    PeakMpiObserverProducer old = {slot, generation, slot + 1};
    peak_mpi_observer_release(capture, &old);
  }
}

void peak_mpi_causal_runtime_observer_work(PeakMpiCausalWorkKind kind,
                                           size_t count) {
  if (kind >= PEAK_MPI_CAUSAL_WORK_COUNT || !get_producer())
    return;
  double price = work_prices.seconds[kind] * (double)count;
  if (kind == PEAK_MPI_CAUSAL_WORK_LOOKUP)
    price += work_prices.lookup_fixed_seconds;
  peak_mpi_causal_cost_observer_add(producer.slot, price);
}

void peak_mpi_causal_runtime_observer_cost(double actual_seconds) {
  if (!atomic_load_explicit(&backend_ready, memory_order_acquire))
    return;
  if (!isfinite(actual_seconds) || actual_seconds < 0) {
    peak_mpi_causal_runtime_fail(PEAK_MPI_OBSERVER_REASON_INVALID_COST);
    return;
  }
  if (get_producer())
    peak_mpi_causal_cost_observer_add(producer.slot, actual_seconds);
}
