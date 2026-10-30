#define _GNU_SOURCE
#include "internal/global_budget.h"
#include "internal/global_budget_protocol.h"
#include "internal/global_budget_causal.h"
#include "internal/mpi_budget_events.h"
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
#include "internal/global_budget_spawn.h"
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include "internal/exec_raw_syscall.h"
#endif
#include <time.h>
#include <unistd.h>

/* Canonical fixed-width frames are independent of rank host ABI. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "budget credits require lock-free 64-bit atomics");
_Static_assert(__atomic_always_lock_free(sizeof(double), 0), "budget views require lock-free doubles");
/* Bound each debit so even INT32_MAX simultaneous failed reservations cannot
 * wrap the signed underflow marker. */
#define MAX_SPEND_NS UINT64_C(4000000000)
#define MAX_TOTAL_NS UINT64_C(999999999000000000)
_Static_assert(MAX_SPEND_NS <= INT64_MAX / INT32_MAX, "temporary debit marker must not overflow");
#define MAGIC PEAK_GLOBAL_BUDGET_PROTOCOL_MAGIC
#define PROTOCOL_VERSION PEAK_GLOBAL_BUDGET_PROTOCOL_VERSION
#define GRACE_NS UINT64_C(500000)
#define FRESH_NS UINT64_C(1000000000)
#define BACKGROUND_NS UINT64_C(100000000)
/* Leave room for freshness/disconnect multipliers and deadline arithmetic. */
#define MAX_BACKGROUND_NS (UINT64_MAX / 16)
typedef struct { double cpu_ns, latest_cpu_ns; } BackgroundCost;
/* Each ordinary background component targets q/8, leaving earned headroom for
 * instrumentation. This is a cadence target, never future spending escrow.
 * Fast backoff uses the current cost; EMA decay lets transient costs recover. */
static uint64_t background_period(BackgroundCost *cost, uint64_t cpu_ns,
    unsigned multiplicity, double q)
{
    if (cpu_ns) {
        cost->cpu_ns = cost->cpu_ns ? .875 * cost->cpu_ns + .125 * cpu_ns : cpu_ns;
        cost->latest_cpu_ns = cpu_ns;
    }
    double estimate = fmax(cost->cpu_ns, cost->latest_cpu_ns);
    double period = q > 0 ? 8.0 * multiplicity * estimate / q : MAX_BACKGROUND_NS;
    return period >= (double)MAX_BACKGROUND_NS ? MAX_BACKGROUND_NS :
        (uint64_t)fmax((double)BACKGROUND_NS, ceil(period));
}
static uint64_t freshness_ns(uint64_t period) { return period > FRESH_NS / 4 ? period * 4 : FRESH_NS; }
static int deadline_timeout(uint64_t now, uint64_t deadline)
{
    if (now >= deadline) return 0;
    uint64_t ms = (deadline - now + 999999) / 1000000;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}
#ifndef PEAK_GLOBAL_BUDGET_IO_CHUNK
#define PEAK_GLOBAL_BUDGET_IO_CHUNK 65536
#endif
#define IO_CHUNK PEAK_GLOBAL_BUDGET_IO_CHUNK
typedef PeakGlobalBudgetFrame Frame;
typedef struct {
    int fd, rank;
    Frame incoming, outgoing;
    unsigned char incoming_wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE];
    unsigned char outgoing_wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE];
    unsigned char *incoming_payload, *outgoing_payload;
    size_t received, sent, payload_received;
    bool pending, final_sent, proof_sent;
    uint64_t last_activity, last_send;
} Peer;
typedef struct {
    PeakGlobalBudgetSample latest;
    bool seen, connected, wants_credit, registered, participant, grace_exposed, reply_requested;
    double reservation, previous_cost, granted_total, closing_estimate;
    uint64_t represented_outside, represented_inside, raw_profile, sum_profile_checkpoint;
    uint64_t registration_sample_sequence, registration_returned_ns;
    bool registration_exposed;
    double serialized_cost;
    uint64_t consumed_ns, requested_ns, minimum_ns, demand_generation, actioned_generation, message_sequence, returned_ns;
    uint64_t last_update, registration_generation, member_generation, member_sample_sequence, fulfilled_generation, credit_total_ns, stop_credit_ns, profile_ns, control_ns, frontend_ns;
} Rank;
typedef struct {
    unsigned peers_count;
    Peer *peers;
    Rank *ranks;
    struct pollfd *ready;
    bool *reply_dirty;
} ServiceStorage;

static void service_storage_free(ServiceStorage *storage)
{
    if (!storage) return;
    free(storage->peers);
    free(storage->ranks);
    free(storage->ready);
    free(storage->reply_dirty);
    free(storage);
}

static ServiceStorage *service_storage_new(unsigned count)
{
    if (count > INT_MAX || count > SIZE_MAX / sizeof(Peer) ||
        count > SIZE_MAX / sizeof(Rank) ||
        (size_t)count > SIZE_MAX / sizeof(struct pollfd) - 3)
        return NULL;
    ServiceStorage *storage = calloc(1, sizeof(*storage));
    if (!storage) return NULL;
    storage->peers_count = count;
    if (count) {
        storage->peers = calloc(count, sizeof(*storage->peers));
        storage->ranks = calloc(count, sizeof(*storage->ranks));
        storage->reply_dirty = calloc(count, sizeof(*storage->reply_dirty));
    }
    storage->ready = calloc((size_t)count + 3, sizeof(*storage->ready));
    if (!storage->ready || (count && (!storage->peers || !storage->ranks || !storage->reply_dirty))) {
        service_storage_free(storage);
        return NULL;
    }
    for (unsigned i = 0; i < count; ++i)
        storage->peers[i] = (Peer){.fd = -1, .rank = -1};
    return storage;
}

static int listener = -1, client = -1, wake_fd = -1, wake_write_fd = -1;
static unsigned rank_id, world;
static uint64_t job_nonce;
uint64_t peak_global_budget_nonce(void) { return job_nonce; }
void peak_global_budget_set_nonce(uint64_t n) { job_nonce = n; }
static double ratio;
static bool broker_only, native_client;
static pid_t broker_owner_pid;
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
static pid_t broker_pid = -1;
#endif
typedef struct { uint64_t time, generation, wave; unsigned rank; char kind; double amount; } BudgetEvent;
static BudgetEvent events[256];
static unsigned event_count;
static bool trace_enabled;
static void record_event(char kind, unsigned rank, uint64_t generation, uint64_t wave, double amount)
{
    if (trace_enabled) {
        struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) return;
        events[event_count++ % (sizeof(events) / sizeof(events[0]))] = (BudgetEvent){(uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec, generation, wave, rank, kind, amount};
    }
}
static _Atomic pid_t owner;
static pthread_t worker;
static atomic_uint_fast64_t public_broker_pid;
static atomic_uint_fast64_t public_background_period, public_freshness, heartbeat_period, transport_period;
static BackgroundCost heartbeat_cost;
static atomic_bool public_broker_final, public_startup_sealed, public_metadata_closed;
static atomic_bool startup_sealed;
static _Atomic double public_broker_cpu;
static atomic_bool running, pressure_notice, flush_requested, local_retiring, request_recovery, request_metadata;
static atomic_uint local_attached;
static atomic_uint_fast64_t control_proof_sequence, reply_goal_sequence, minimum_target_ns;
/* High 32 bits are a nonzero physical activity revision; low bits are count. */
static atomic_uint_fast64_t physical_targets, physical_revision;
static uint32_t effective_targets(uint32_t fallback)
{
    if (atomic_load(&local_retiring)) return 0;
    uint64_t word = atomic_load_explicit(&physical_targets, memory_order_acquire);
    return word >> 32 ? (uint32_t)word : fallback;
}
typedef struct {
    uint64_t word;
    bool attached;
} PhysicalIdleSnapshot;
static PhysicalIdleSnapshot physical_idle_snapshot(uint64_t word, uint32_t fallback, uint64_t now,
    uint64_t *seen, uint64_t *last_activity, bool *returned)
{
    if (!now || now < *last_activity)
        return (PhysicalIdleSnapshot){word, false};
    uint32_t count = word >> 32 ? (uint32_t)word : fallback;
    if (atomic_load(&local_retiring))
        count = 0;
    if (word != *seen) {
        if (count) {
            *last_activity = now;
            *returned = false;
        }
        *seen = word;
    }
    return (PhysicalIdleSnapshot){word, count != 0};
}
static atomic_uint_fast64_t recovery_target_ns, consumed_control_ns, consumed_frontend_ns;
static atomic_uint_fast64_t admission_target_ns, admission_cash_ns, admission_closing_ns;
static uint64_t current_minimum(uint64_t debit)
{
    uint64_t profile=atomic_load_explicit(&minimum_target_ns,memory_order_acquire);
    uint64_t admission=atomic_load_explicit(&admission_target_ns,memory_order_acquire);
    uint64_t target=profile>admission?profile:admission;
    return target>debit?target-debit:0;
}

static atomic_uint_fast64_t frontend_cpu_ns, settled_frontend_ns;
static atomic_bool causal_fallback, metadata_enabled;
static atomic_uint metadata_seed, metadata_tokens, metadata_inflight;
static atomic_uint_fast64_t metadata_block_start, metadata_block_credit, metadata_estimate;
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned metadata_local_admitted;
typedef struct MetadataOperation {
    atomic_size_t references;
    atomic_bool settled;
    uint64_t cost_ns;
    bool has_record, prefix_committed;
} MetadataOperation;
typedef struct MetadataRecord { struct MetadataRecord *next; MetadataOperation *operation; size_t count; uint64_t words[]; } MetadataRecord;
static _Thread_local MetadataOperation *metadata_operation;
static uint64_t metadata_ordered_frontend;
static atomic_uint_fast64_t metadata_unassigned_frontend;
static pthread_mutex_t metadata_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static MetadataRecord *metadata_head, *metadata_tail;
static atomic_bool metadata_pending, metadata_ready;
/* Captured once on the ON -> OFF edge; diagnostics never perform callback I/O. */
static atomic_uint metadata_close_reason;
static uint64_t metadata_started_ns, metadata_closed_ns, metadata_close_quote;
static unsigned metadata_close_seed;
static uint64_t metadata_footer_quote;
static uint64_t metadata_footer_samples[64];
static uint64_t metadata_footer_median,metadata_footer_p95,metadata_footer_max;
static uint64_t metadata_prefetch_goal; /* Protected by metadata_mutex. */
static int64_t metadata_close_wallet;
static double metadata_close_spent, metadata_close_reserved, metadata_close_closing;
static void metadata_close(unsigned reason, uint64_t quote);


static atomic_int_fast64_t available_ns;
static atomic_uint_fast64_t rejected_spend_count;
static pthread_mutex_t sample_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Background writer and diagnostic getter share the existing sample lock. */
static PeakGlobalBudgetLocalBill last_written_bill;
static PeakGlobalBudgetSample local_sample;
static void (*notify)(void);
static atomic_uint view_sequence;
static _Atomic double public_local_closing;
static _Atomic double public_causal_confirmed, public_causal_pending, public_causal_raw, public_causal_tail;
/* Atomic fields avoid the data race of a seqlock around non-atomic payload. */
static atomic_bool public_active, public_participant, public_finalized, public_accounting_invalid, root_final_sent;
static atomic_uint finalizer_state;
static _Atomic clockid_t finalizer_clock;
static atomic_uint_fast64_t finalizer_start;
static _Atomic double finalizer_baseline;
static atomic_uint_fast64_t local_completed_wave, public_wave, public_decision, public_completed, public_ack, public_timestamp, granted_ns, consumed_ns, requested_ns, demand_generation, debit_ns, returned_ns, public_demand_ack, request_started_ns, public_deadline, low_water_ns;
static _Atomic double public_spent, public_remaining, public_allowance, public_span, public_granted, public_budget, public_reserved, public_control_reserve, startup_charge, local_transport_cpu, earned_origin, refill_rtt, consumption_rate;
#if defined(__APPLE__)
#define PEAK_BUDGET_WEAK __attribute__((weak_import))
#else
#define PEAK_BUDGET_WEAK __attribute__((weak))
#endif
extern void pthread_listener_exclude_current_thread(void) PEAK_BUDGET_WEAK;
extern void pthread_listener_mark_next_created_thread_helper(void) PEAK_BUDGET_WEAK;
extern bool peak_global_budget_listener_idle_try_guard(void) PEAK_BUDGET_WEAK;
extern void peak_global_budget_listener_idle_unguard(void) PEAK_BUDGET_WEAK;
extern void peak_global_budget_listener_credit_ready(void) PEAK_BUDGET_WEAK;
extern bool peak_global_budget_listener_admission_waiting(void) PEAK_BUDGET_WEAK;
extern void peak_mpi_budget_events_frontier(void) PEAK_BUDGET_WEAK;
static void frame_credit_demand(Frame *frame, uint64_t debit)
{
    uint64_t profile=atomic_load_explicit(&minimum_target_ns,memory_order_acquire);
    uint64_t admission=atomic_load_explicit(&admission_target_ns,memory_order_acquire);
    uint64_t desired=atomic_load_explicit(&requested_ns,memory_order_acquire);
#ifdef PEAK_GLOBAL_BUDGET_TEST_ADMISSION_SNAPSHOT_HOOK
    PEAK_GLOBAL_BUDGET_TEST_ADMISSION_SNAPSHOT_HOOK();
#endif
    uint64_t minimum=profile>admission?profile:admission;
    if (minimum>desired) desired=minimum;
    frame->minimum_ns=minimum>debit?minimum-debit:0;
    frame->requested_ns=desired>debit?desired-debit:0;
}
static uint64_t ns_now(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t)) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static double seconds_now(void) { return (double)ns_now(CLOCK_MONOTONIC) / 1e9; }
static void poke(void)
{
    if (!atomic_load_explicit(&running, memory_order_acquire) || wake_write_fd < 0) return;
    uint64_t value = 1;
    (void)write(wake_write_fd, &value, sizeof(value)); /* Nonblocking: EAGAIN is harmless. */
}
uint64_t peak_global_budget_heartbeat_period_ns(uint64_t cpu_ns, uint64_t configured_ns)
{
    if (!atomic_load_explicit(&running, memory_order_acquire) ||
        getpid() != atomic_load(&owner)) return configured_ns;
    uint64_t period = background_period(&heartbeat_cost, cpu_ns, world, ratio);
    if (configured_ns > period) period = configured_ns;
    if (period > MAX_BACKGROUND_NS) period = MAX_BACKGROUND_NS;
    uint64_t previous = atomic_exchange(&heartbeat_period, period);
    if (period > previous) poke();
    uint64_t negotiated = atomic_load(&public_background_period);
    return negotiated > period ? negotiated : period;
}
static uint64_t local_background_period(void)
{
    uint64_t period = atomic_load(&heartbeat_period), service = atomic_load(&transport_period);
    return period > service ? period : service;
}
static uint64_t negotiated_period(void)
{
    uint64_t local = local_background_period(), remote = atomic_load(&public_background_period);
    return local > remote ? local : remote;
}
static void close_fd(int *fd) { if (*fd >= 0) close(*fd); *fd = -1; }
static bool configure_fd(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
static bool configure_socket(int fd)
{
    if (!configure_fd(fd)) return false;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one)) return false;
#endif
    return true;
}
static int accept_peer(int fd)
{
    int peer = accept(fd, NULL, NULL);
    if (peer >= 0 && !configure_socket(peer)) { close(peer); return -1; }
    return peer;
}
static int create_wake_pipe(void)
{
    int pipefd[2];
    if (pipe(pipefd)) return -1;
    if (!configure_fd(pipefd[0]) || !configure_fd(pipefd[1])) {
        close(pipefd[1]); close(pipefd[0]); return -1;
    }
    wake_fd = pipefd[0]; wake_write_fd = pipefd[1];
    return 0;
}

static bool idle_return_due(PhysicalIdleSnapshot physical, uint64_t now,
    uint64_t last_activity, bool returned, int64_t balance, uint64_t keep)
{
    return physical.attached && !returned && now >= last_activity &&
        now - last_activity >= UINT64_C(100000000) &&
        !atomic_load(&pressure_notice) && !atomic_load(&request_started_ns) &&
        balance > 0 && (uint64_t)balance > keep;
}
/* The production listener guard shares the controller's existing lock and
 * refuses queued/in-progress mutations. Never wait for a stopped service or
 * introduce a new lock that the STW controller would need to acquire. */
static uint64_t idle_return_commit(PhysicalIdleSnapshot physical, int64_t balance, uint64_t keep)
{
    if (balance <= 0 || (uint64_t)balance <= keep)
        return 0;
    bool has_guard = peak_global_budget_listener_idle_try_guard &&
        peak_global_budget_listener_idle_unguard;
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
    if (!has_guard)
        return 0;
#endif
    if (has_guard && !peak_global_budget_listener_idle_try_guard())
        return 0;
    uint64_t returned = 0;
    if (atomic_load_explicit(&physical_targets, memory_order_acquire) == physical.word &&
        !atomic_load(&local_retiring) && !atomic_load(&pressure_notice) &&
        !atomic_load(&request_started_ns)) {
#ifdef PEAK_GLOBAL_BUDGET_TEST_IDLE_COMMIT_HOOK
        PEAK_GLOBAL_BUDGET_TEST_IDLE_COMMIT_HOOK();
#endif
        int64_t target = (int64_t)keep;
        if (atomic_compare_exchange_strong(&available_ns, &balance, target)) {
            returned = (uint64_t)balance - keep;
            atomic_fetch_add(&debit_ns, returned);
            atomic_fetch_add(&returned_ns, returned);
        }
    }
    if (has_guard)
        peak_global_budget_listener_idle_unguard();
    return returned;
}
static bool number(double n) { return isfinite(n) && n >= 0; }
static uint64_t seconds_to_ns(double seconds)
{
    if (!number(seconds) || seconds >= (double)UINT64_MAX / 1e9) return UINT64_MAX;
    return (uint64_t)ceil(seconds * 1e9);
}
static uint64_t ns_add(uint64_t a, uint64_t b) { return b > UINT64_MAX-a ? UINT64_MAX : a+b; }

static double cost(const PeakGlobalBudgetSample *s)
{
    return s->profile_seconds + s->ignored_seconds + s->frontend_seconds + s->control_seconds + s->management_seconds + s->transport_seconds;
}
static double ledger_cost(const PeakGlobalBudgetSample *s, uint64_t profile)
{
    return fmax(s->profile_seconds, (double)profile / 1e9) + s->ignored_seconds + s->frontend_seconds +
        s->control_seconds +
        s->management_seconds + s->transport_seconds;
}
static PeakGlobalBudgetLocalBill local_bill_from_frame(const Frame *frame,
    double management_input, double finalizer_cpu, double finalizer_baseline,
    bool finalizer_registered)
{
    double profile = fmax(frame->sample.profile_seconds, (double)frame->consumed_profile_ns / 1e9);
    return (PeakGlobalBudgetLocalBill){
        .written = true, .retiring = frame->sample.retiring, .final = frame->sample.final,
        .finalizer_registered = finalizer_registered,
        .sample_sequence = frame->sample.sequence, .message_sequence = frame->message_sequence,
        .profile_debit_ns = frame->consumed_profile_ns,
        .frontend_debit_ns = frame->consumed_frontend_ns,
        .ignored_seconds = frame->sample.ignored_seconds, .profile_inside_seconds = frame->sample.profile_inside_seconds, .frontend_seconds = frame->sample.frontend_seconds,
        .profile_sample_seconds = frame->sample.profile_seconds, .profile_billed_seconds = profile,
        .control_seconds = frame->sample.control_seconds,
        .management_input_seconds = management_input,
        .management_sent_seconds = frame->sample.management_seconds,
        .transport_seconds = frame->sample.transport_seconds,
        .finalizer_baseline_seconds = finalizer_baseline, .finalizer_cpu_seconds = finalizer_cpu,
        .finalizer_override_seconds = fmax(0, frame->sample.management_seconds - management_input),
        .management_increment_seconds = finalizer_registered
            ? fmax(0, frame->sample.management_seconds - finalizer_baseline) : 0,
        .total_seconds = ledger_cost(&frame->sample, frame->consumed_profile_ns)};
}
PeakGlobalBudgetLocalBill peak_global_budget_local_bill(void)
{
    if (getpid() != atomic_load(&owner)) return (PeakGlobalBudgetLocalBill){0};
    pthread_mutex_lock(&sample_mutex);
    PeakGlobalBudgetLocalBill bill = last_written_bill;
    pthread_mutex_unlock(&sample_mutex);
    return bill;
}
/* Control prepayments are releasable escrow, never actual-spent high water.
 * Keep local credit reservation separate for exact grant/admission arithmetic. */
static double control_escrow(const Rank *r)
{
    return ((double)(r->control_ns - r->latest.settled_control_credit_ns) +
        (double)(r->frontend_ns - r->latest.settled_frontend_credit_ns)) / 1e9;
}
static bool broker_cpu_snapshot(uint64_t *last, double *seconds)
{
    uint64_t cpu = ns_now(CLOCK_PROCESS_CPUTIME_ID);
    if (!cpu || cpu < *last)
        return false;
    *last = cpu;
    *seconds = (double)cpu / 1e9;
    return true;
}
static bool root_metadata_valid(const Frame *frame)
{
    if (!number(frame->root_origin_seconds) || !number(frame->root_startup_seconds) ||
        frame->root_origin_seconds > 3600)
        return false;
    if (frame->rank)
        return !frame->root_origin_seconds && !frame->root_startup_seconds &&
            !frame->root_startup_sealed;
    /* Embedded atomics can legitimately be newer than an in-flight frame. */
    if (!broker_only)
        return true;
    return frame->root_origin_seconds >= atomic_load(&earned_origin) &&
        frame->root_startup_seconds >= atomic_load(&startup_charge) &&
        (!atomic_load(&startup_sealed) || frame->root_startup_sealed);
}
static bool valid_sample(const PeakGlobalBudgetSample *s, const Rank *r)
{
    if (s->background_period_ns > MAX_BACKGROUND_NS || !s->sequence || s->finalizer_invalid || (s->final && !s->retiring) || !number(s->profile_seconds) || !number(s->control_seconds) ||
        !number(s->ignored_seconds) || !number(s->profile_inside_seconds) || !number(s->frontend_seconds) || !number(s->management_seconds) || !number(s->transport_seconds) || !number(s->elapsed_seconds) ||
        !number(s->closing_seconds) || !number(s->pressure) || !number(s->unfunded_seconds) || s->unfunded_seconds > .000500001 || !number(cost(s))) return false;
    if (!r->seen) return true;
    return s->ignored_seconds >= r->latest.ignored_seconds && s->profile_inside_seconds >= r->latest.profile_inside_seconds && s->frontend_seconds >= r->latest.frontend_seconds &&
        s->settled_frontend_credit_ns >= r->latest.settled_frontend_credit_ns &&
        s->profile_seconds >= r->latest.profile_seconds &&
        s->control_seconds >= r->latest.control_seconds &&
        s->management_seconds >= r->latest.management_seconds &&
        s->transport_seconds >= r->latest.transport_seconds &&
        s->elapsed_seconds >= r->latest.elapsed_seconds &&
        s->completed_wave >= r->latest.completed_wave &&
        s->unfunded_seconds >= r->latest.unfunded_seconds &&
        s->settled_control_credit_ns >= r->latest.settled_control_credit_ns &&
        (!r->latest.retiring || s->retiring) &&
        (!r->latest.final || (s->final && s->sequence == r->latest.sequence)) &&
        (!s->initial_unexposed || r->latest.initial_unexposed);
}
static void publish_view(PeakGlobalBudgetView v)
{
    if (v.granted_total_ns > MAX_TOTAL_NS || v.granted_total_ns < atomic_load(&granted_ns) || !number(v.granted_total_seconds) ||
        v.granted_total_seconds != (double)v.granted_total_ns / 1e9) {
        v.active = false; v.accounting_invalid = true;
    }
    atomic_fetch_add_explicit(&view_sequence, 1, memory_order_acq_rel);
    atomic_store_explicit(&public_active, v.active, memory_order_relaxed);
    atomic_store(&public_background_period, v.background_period_ns);
    atomic_store(&public_freshness, v.freshness_ns ? v.freshness_ns : FRESH_NS);
    atomic_store(&public_participant, v.wave_participant);
    atomic_store(&public_finalized, v.cohort_finalized);
    atomic_store(&public_broker_pid, v.broker_process_id);
    atomic_store(&public_broker_final, v.broker_final_written);
    atomic_store(&public_startup_sealed, v.startup_sealed);
    atomic_store(&public_metadata_closed,v.metadata_closed);
    atomic_store(&public_local_closing,v.local_closing_reserved_seconds);
    if(v.metadata_closed) { metadata_close(6, 0); atomic_store(&causal_fallback,true); }
    atomic_store(&public_causal_confirmed,v.causal_confirmed_seconds);atomic_store(&public_causal_pending,v.causal_pending_seconds);atomic_store(&public_causal_raw,v.causal_raw_seconds);atomic_store(&public_causal_tail,v.causal_unrepresented_seconds);
    atomic_store(&public_broker_cpu, v.broker_cpu_seconds);
    if (v.accounting_invalid) atomic_store(&public_accounting_invalid, true);
    atomic_store(&public_demand_ack, v.acknowledged_demand_generation);
    atomic_store_explicit(&public_wave, v.wave, memory_order_relaxed);
    atomic_store_explicit(&public_decision, v.decision_sequence, memory_order_relaxed);
    atomic_store_explicit(&public_completed, v.root_completed_wave, memory_order_relaxed);
    atomic_store_explicit(&public_ack, v.acknowledged_sequence, memory_order_relaxed);
    atomic_store_explicit(&public_budget, v.budget_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_reserved, v.reserved_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_control_reserve, v.control_reserve_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_spent, v.global_spent_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_remaining, v.remaining_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_allowance, v.allowance_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_span, v.wave_span_seconds, memory_order_relaxed);
    atomic_store_explicit(&public_granted, v.granted_total_seconds, memory_order_relaxed);
    if (!v.accounting_invalid && v.granted_total_ns <= MAX_TOTAL_NS) {
        uint64_t previous = atomic_load(&granted_ns);
        if (v.granted_total_ns > previous) {
            atomic_store(&granted_ns, v.granted_total_ns);
            atomic_fetch_add_explicit(&available_ns, (int64_t)(v.granted_total_ns - previous), memory_order_release);
        }
    }
    atomic_store_explicit(&public_timestamp, ns_now(CLOCK_MONOTONIC), memory_order_relaxed);
    atomic_fetch_add_explicit(&view_sequence, 1, memory_order_release);
}
bool peak_global_budget_active(void)
{
    if (getpid() != atomic_load_explicit(&owner, memory_order_relaxed) ||
        !atomic_load_explicit(&public_active, memory_order_acquire) ||
        atomic_load_explicit(&public_accounting_invalid, memory_order_relaxed)) return false;
    uint64_t stamp = atomic_load_explicit(&public_timestamp, memory_order_acquire), now = ns_now(CLOCK_MONOTONIC);
    return stamp && now >= stamp && now - stamp <= atomic_load(&public_freshness);
}
PeakGlobalBudgetView peak_global_budget_view(void)
{
    PeakGlobalBudgetView v = {0};
    if (getpid() != atomic_load_explicit(&owner, memory_order_relaxed)) return v;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        unsigned before = atomic_load_explicit(&view_sequence, memory_order_acquire);
        if (before & 1) continue;
        v.active = atomic_load_explicit(&public_active, memory_order_relaxed);
        v.wave_participant = atomic_load(&public_participant);
        v.broker_process_id = atomic_load(&public_broker_pid);
        v.background_period_ns = atomic_load(&public_background_period);
        v.freshness_ns = atomic_load(&public_freshness);
        v.broker_final_written = atomic_load(&public_broker_final);
        v.startup_sealed = atomic_load(&public_startup_sealed);
        v.metadata_closed = atomic_load(&public_metadata_closed);
        v.local_closing_reserved_seconds=atomic_load(&public_local_closing);
        v.causal_confirmed_seconds=atomic_load(&public_causal_confirmed);v.causal_pending_seconds=atomic_load(&public_causal_pending);v.causal_raw_seconds=atomic_load(&public_causal_raw);v.causal_unrepresented_seconds=atomic_load(&public_causal_tail);
        v.broker_cpu_seconds = atomic_load(&public_broker_cpu);
        v.cohort_finalized = atomic_load(&public_finalized);
        v.accounting_invalid = atomic_load(&public_accounting_invalid);
        v.acknowledged_demand_generation = atomic_load(&public_demand_ack);
        v.request_deadline_ns = atomic_load(&public_deadline);
        int64_t balance = atomic_load_explicit(&available_ns, memory_order_acquire);
        v.local_credit_ns = balance > 0 ? (uint64_t)balance : 0;
        v.returned_total_ns = atomic_load(&returned_ns);
        v.rejected_spend_count = atomic_load(&rejected_spend_count);
        v.recovery_target_ns = atomic_load(&recovery_target_ns);
        v.recovery_pending = v.recovery_target_ns && v.local_credit_ns < v.recovery_target_ns;
        v.refill_rtt_seconds = atomic_load(&refill_rtt);
        v.consumption_rate = atomic_load(&consumption_rate);
        v.wave = atomic_load_explicit(&public_wave, memory_order_relaxed);
        v.decision_sequence = atomic_load_explicit(&public_decision, memory_order_relaxed);
        v.view_epoch = v.decision_sequence;
        v.root_completed_wave = atomic_load_explicit(&public_completed, memory_order_relaxed);
        v.acknowledged_sequence = atomic_load_explicit(&public_ack, memory_order_relaxed);
        v.budget_seconds = atomic_load_explicit(&public_budget, memory_order_relaxed);
        v.reserved_seconds = atomic_load_explicit(&public_reserved, memory_order_relaxed);
        v.control_reserve_seconds = atomic_load_explicit(&public_control_reserve, memory_order_relaxed);
        v.startup_seconds = atomic_load_explicit(&startup_charge, memory_order_relaxed);
        v.transport_seconds = atomic_load_explicit(&local_transport_cpu, memory_order_relaxed);
        v.global_spent_seconds = atomic_load_explicit(&public_spent, memory_order_relaxed);
        v.remaining_seconds = atomic_load_explicit(&public_remaining, memory_order_relaxed);
        v.allowance_seconds = atomic_load_explicit(&public_allowance, memory_order_relaxed);
        v.wave_span_seconds = atomic_load_explicit(&public_span, memory_order_relaxed);
        v.granted_total_seconds = atomic_load_explicit(&public_granted, memory_order_relaxed);
        v.granted_total_ns = atomic_load(&granted_ns);
        uint64_t stamp = atomic_load_explicit(&public_timestamp, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (before == atomic_load_explicit(&view_sequence, memory_order_acquire)) {
            uint64_t now = ns_now(CLOCK_MONOTONIC);
            if (!stamp || now < stamp || now - stamp > v.freshness_ns) v.active = false;
            return v;
        }
    }
    return (PeakGlobalBudgetView){0};
}
int peak_global_budget_listen(unsigned short *port)
{
    if (!port || listener >= 0 || atomic_load(&running)) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    if (!configure_socket(s)) { close(s); return -1; }
    struct sockaddr_in addr = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t length = sizeof(addr);
    if (bind(s, (void *)&addr, sizeof(addr)) || listen(s, SOMAXCONN) ||
        getsockname(s, (void *)&addr, &length)) { close(s); return -1; }
    ssize_t entropy_bytes = -1;
#if defined(__linux__) && defined(SYS_getrandom)
    entropy_bytes = peak_exec_raw_syscall6(SYS_getrandom, (long)(uintptr_t)&job_nonce,
        sizeof(job_nonce), 0x0001u /* GRND_NONBLOCK */, 0, 0, 0);
#endif
    if (entropy_bytes != sizeof(job_nonce)) {
        int entropy_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (entropy_fd >= 0) {
            entropy_bytes = read(entropy_fd, &job_nonce, sizeof(job_nonce));
            close(entropy_fd);
        }
    }
    if (entropy_bytes != sizeof(job_nonce) || !job_nonce) { close(s); return -1; }
    listener = s;
    atomic_store(&owner, getpid());
    *port = ntohs(addr.sin_port);
    return 0;
}
static int connect_host(const char *host, unsigned short port)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    char service[16];
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (!configure_socket(fd)) { close(fd); fd = -1; continue; }
        int result = connect(fd, a->ai_addr, a->ai_addrlen);
        if (result && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            int error = 0; socklen_t len = sizeof(error);
            result = poll(&p, 1, 1000);
            result = result > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) &&
                !error ? 0 : -1;
        }
        if (!result) { int one = 1; (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); break; }
        close_fd(&fd);
    }
    freeaddrinfo(addresses);
    return fd;
}
/* All arrays are copied on the application thread. Socket work is exclusively
 * owned by the service; the queue mutex never covers I/O. */
static void metadata_close(unsigned reason, uint64_t quote)
{
    if (!atomic_exchange_explicit(&metadata_enabled, false, memory_order_acq_rel)) return;
    metadata_closed_ns = ns_now(CLOCK_MONOTONIC);
    metadata_close_quote = quote;
    metadata_close_seed = atomic_load(&metadata_seed);
    metadata_close_wallet = atomic_load(&available_ns);
    metadata_close_spent = atomic_load(&public_spent);
    metadata_close_reserved = atomic_load(&public_reserved);
    metadata_close_closing = atomic_load(&public_control_reserve);
    atomic_store_explicit(&metadata_close_reason, reason, memory_order_release);
}
static void metadata_fail(void)
{
    metadata_close(1, 0);
    atomic_store(&causal_fallback, true);
    poke();
}
/* Service-owned cancellation: no new admission may race the gate recheck
 * under metadata_mutex, and inflight==0 proves every measured charge is public. */
static void metadata_settle_closed(void)
{
    if (atomic_load(&metadata_enabled)) return;
    pthread_mutex_lock(&metadata_mutex);
    if (!atomic_load(&metadata_enabled) && !atomic_load(&metadata_inflight)) {
        atomic_store(&metadata_tokens, 0);
        atomic_store(&metadata_block_credit, 0);
        atomic_store_explicit(&settled_frontend_ns,
            atomic_load(&consumed_frontend_ns), memory_order_release);
    }
    pthread_mutex_unlock(&metadata_mutex);
}
static void metadata_operation_release(MetadataOperation *operation)
{
    if (operation && atomic_fetch_sub(&operation->references,1)==1) free(operation);
}
static void metadata_record_free(MetadataRecord *record)
{
    metadata_operation_release(record->operation); free(record);
}
static MetadataRecord *metadata_alloc(size_t count)
{
    if (count > (SIZE_MAX - sizeof(MetadataRecord)) / sizeof(uint64_t)) {
        metadata_fail(); return NULL;
    }
    MetadataRecord *record = malloc(sizeof(*record) + count * sizeof(uint64_t));
    if (!record) { metadata_fail(); return NULL; }
    record->next = NULL; record->operation=NULL; record->count = count;
    return record;
}
static void metadata_append(MetadataRecord *record)
{
    pthread_mutex_lock(&metadata_queue_mutex);
    bool empty = metadata_head == NULL;
    if (metadata_tail) metadata_tail->next = record; else metadata_head = record;
    metadata_tail = record;
    atomic_store(&metadata_pending, true);
    bool ready = metadata_head && (!metadata_head->operation ||
        atomic_load_explicit(&metadata_head->operation->settled,memory_order_acquire));
    atomic_store(&metadata_ready,ready);
    pthread_mutex_unlock(&metadata_queue_mutex);
    if (empty && ready) poke();
}
static void metadata_clear(void)
{
    pthread_mutex_lock(&metadata_queue_mutex);
    while (metadata_head) {
        MetadataRecord *next = metadata_head->next;
        metadata_record_free(metadata_head); metadata_head = next;
    }
    metadata_tail = NULL; atomic_store(&metadata_pending, false); atomic_store(&metadata_ready,false);
    pthread_mutex_unlock(&metadata_queue_mutex);
}
/* A batching threshold is not a record/request/rank limit: an individual
 * larger record is transmitted intact with dynamically sized framing. */
static unsigned char *metadata_batch(uint64_t *words)
{
    *words = 0;
    pthread_mutex_lock(&metadata_queue_mutex);
    MetadataRecord *first = metadata_head, *last = NULL, *cursor = first;
    size_t count = 0;
    while (cursor && (!last || count < 8192)) {
        if (cursor->operation && !atomic_load_explicit(&cursor->operation->settled,memory_order_acquire)) break;
        if (cursor->count > SIZE_MAX - count) break;
        count += cursor->count; last = cursor; cursor = cursor->next;
    }
    if (last) {
        metadata_head = cursor; last->next = NULL;
        if (!cursor) metadata_tail = NULL;
    }
    atomic_store(&metadata_pending, metadata_head != NULL);
    atomic_store(&metadata_ready,metadata_head && (!metadata_head->operation ||
        atomic_load_explicit(&metadata_head->operation->settled,memory_order_acquire)));
    pthread_mutex_unlock(&metadata_queue_mutex);
    if (!last) return NULL;
    unsigned char *payload = count <= SIZE_MAX / 8 ? malloc(count * 8) : NULL;
    unsigned char *output = payload;
    while (first) {
        MetadataRecord *next = first->next;
        if (payload) {
            if (first->words[1]==2) {
                if (first->operation && !first->operation->prefix_committed) {
                    metadata_ordered_frontend=ns_add(metadata_ordered_frontend,first->operation->cost_ns);
                    first->operation->prefix_committed=true;
                }
                metadata_ordered_frontend=ns_add(metadata_ordered_frontend,
                    atomic_exchange(&metadata_unassigned_frontend,0));
                first->words[13]=ns_add(first->words[13],metadata_ordered_frontend);
                first->words[15]=ns_add(first->words[13],first->words[14]);
            }
            for (size_t i = 0; i < first->count; ++i)
                peak_budget_wire_put(&output, first->words[i], 8);
        }
        metadata_record_free(first); first = next;
    }
    if (!payload) { metadata_fail(); return NULL; }
    *words = count;
    return payload;
}
int peak_global_budget_mpi_events_enabled(void)
{
    return atomic_load_explicit(&metadata_enabled, memory_order_acquire) &&
        atomic_load_explicit(&running, memory_order_acquire) &&
        getpid() == atomic_load(&owner) && !atomic_load(&local_retiring) &&
        atomic_load_explicit(&metadata_enabled, memory_order_acquire);
}
/* Request the next paid block while real observations still provide runway.
 * This is asynchronous earned cash, never another seed or a native-MPI wait.
 * A pending request is raised only for a larger forecast, not on every op. */
static void request_credit_internal(uint64_t amount, bool metadata, bool actual);
static void metadata_prefetch(uint64_t forecast)
{
    if (!forecast || forecast>MAX_SPEND_NS/64 || !atomic_load(&metadata_enabled)) return;
    uint64_t quote=forecast*64;
    uint64_t low=atomic_load(&low_water_ns);
    uint64_t target=low>MAX_SPEND_NS-quote ? MAX_SPEND_NS : quote+low;
    int64_t balance=atomic_load_explicit(&available_ns,memory_order_acquire);
    if (balance>=0 && (uint64_t)balance>=target) return;
    pthread_mutex_lock(&metadata_mutex);
    if (atomic_load(&metadata_enabled) && !atomic_load(&local_retiring)) {
        bool pending=atomic_load_explicit(&pressure_notice,memory_order_acquire);
        if (!pending || target>metadata_prefetch_goal) {
            metadata_prefetch_goal=target;
            request_credit_internal(target,true,false);
        }
    }
    pthread_mutex_unlock(&metadata_mutex);
}
int peak_global_budget_mpi_trace_begin(void)
{
    if (!peak_global_budget_mpi_events_enabled()) return 0;
    metadata_prefetch(atomic_load(&metadata_estimate));
    pthread_mutex_lock(&metadata_mutex);
    if (!peak_global_budget_mpi_events_enabled()) {
        pthread_mutex_unlock(&metadata_mutex);
        return 0;
    }
    bool admitted = false;
    unsigned seed = atomic_load(&metadata_seed), tokens = atomic_load(&metadata_tokens);
    if (seed) {
        atomic_store(&metadata_seed, seed - 1); admitted = true;
    } else {
        if (!tokens && !atomic_load(&metadata_inflight)) {
            uint64_t estimate = atomic_load(&metadata_estimate);
            if (!estimate) { metadata_close(2, 0); metadata_fail(); pthread_mutex_unlock(&metadata_mutex); return 0; }
            uint64_t quote = estimate <= MAX_SPEND_NS / 64 ? estimate * 64 : UINT64_MAX;
            int64_t balance = atomic_load(&available_ns);
            if (quote <= MAX_SPEND_NS && balance >= 0 && (uint64_t)balance >= quote) {
                int64_t before = atomic_fetch_sub(&available_ns, (int64_t)quote);
                if (before >= 0 && (uint64_t)before >= quote) {
                    atomic_fetch_add(&debit_ns, quote);
                    atomic_fetch_add(&consumed_frontend_ns, quote);
                    atomic_store(&metadata_block_start, atomic_load(&frontend_cpu_ns));
                    atomic_store(&metadata_block_credit, quote);
                    tokens = 64;
                } else atomic_fetch_add(&available_ns, (int64_t)quote);
            }
        }
        if (tokens) { atomic_store(&metadata_tokens, tokens - 1); admitted = true; }
    }
    if (admitted) {
        MetadataOperation *operation=calloc(1,sizeof(*operation));
        if (!operation) { metadata_fail(); admitted=false; }
        else {
            atomic_init(&operation->references,1);atomic_init(&operation->settled,false);
            metadata_operation=operation;
            ++metadata_local_admitted; atomic_fetch_add(&metadata_inflight, 1);
        }
    } else {
        metadata_close(3, atomic_load(&metadata_estimate) <= MAX_SPEND_NS/64 ? atomic_load(&metadata_estimate)*64 : UINT64_MAX);
        metadata_fail();
    }
    pthread_mutex_unlock(&metadata_mutex);
    return admitted;
}
/* The immutable footer is shared by real settlement and startup calibration.
 * Caller owns metadata_queue_mutex. No record becomes ready before its full
 * measured body plus calibrated footer estimate is in the raw/prefix bill. */
static void metadata_finish_charge(MetadataOperation *operation, uint64_t cost,
    bool calibration)
{
    uint64_t previous = atomic_fetch_add(&frontend_cpu_ns,cost);
    if (cost == UINT64_MAX || cost > UINT64_MAX-previous) {
        atomic_store(&frontend_cpu_ns,UINT64_MAX);
        atomic_store(&public_accounting_invalid,true);
        metadata_close(4,0); metadata_fail();
    }
    if (!atomic_load(&public_accounting_invalid)) {
        uint64_t old = atomic_load(&metadata_estimate);
        uint64_t estimate = old ? old-old/8+cost/8 : cost;
        atomic_store(&metadata_estimate,estimate>cost?estimate:cost);
        uint64_t credit = atomic_load(&metadata_block_credit);
        uint64_t charged = atomic_load(&frontend_cpu_ns)-atomic_load(&metadata_block_start);
        if (credit && (charged>credit || (charged==credit && atomic_load(&metadata_tokens)))) {
            metadata_close(5,credit); metadata_fail();
        }
    }
    if (operation) {
        operation->cost_ns = cost;
        if (!operation->has_record) atomic_fetch_add(&metadata_unassigned_frontend,cost);
        atomic_store_explicit(&operation->settled,true,memory_order_release);
        metadata_operation_release(operation);
    } else if (cost != UINT64_MAX) atomic_fetch_add(&metadata_unassigned_frontend,cost);
    if (metadata_local_admitted) {
        --metadata_local_admitted;
        unsigned before = atomic_fetch_sub(&metadata_inflight,1);
        if (before==1 && (!atomic_load(&metadata_tokens) || !atomic_load(&metadata_enabled))) {
            atomic_store_explicit(&settled_frontend_ns,
                atomic_load(&consumed_frontend_ns),memory_order_release);
            atomic_store(&metadata_block_credit,0);
            if (!atomic_load(&metadata_enabled)) atomic_store(&metadata_tokens,0);
        }
    }
    atomic_store(&metadata_ready,metadata_head && (!metadata_head->operation ||
        atomic_load_explicit(&metadata_head->operation->settled,memory_order_acquire)));
    pthread_mutex_unlock(&metadata_queue_mutex);
    /* Notification after unlock is required: an earlier wake can be consumed
     * before readiness. Its real nonblocking syscall is in the footer quote. */
    if (calibration && wake_write_fd>=0) {
        uint64_t value=1;
        (void)write(wake_write_fd,&value,sizeof(value));
    } else poke();
}
static int compare_footer_sample(const void *left,const void *right)
{
    uint64_t a=*(const uint64_t *)left,b=*(const uint64_t *)right;
    return (a>b)-(a<b);
}
static uint64_t metadata_calibrate_footer(void)
{
    struct timespec resolution;
    if (clock_getres(CLOCK_THREAD_CPUTIME_ID,&resolution) ||
        resolution.tv_sec<0 || resolution.tv_nsec<0) return 0;
    uint64_t clock_resolution=(uint64_t)resolution.tv_sec*UINT64_C(1000000000)+(uint64_t)resolution.tv_nsec;
    if (!clock_resolution) return 0;
    /* Bounded once/start, before service/admission starts. Actual calibration
     * work belongs to the measured startup bill. Like PEAK's instrumentation
     * calibration, a median estimates recurring cost; one-time setup/signal
     * CPU outliers must not become the price of every later operation. */
    for (unsigned repeat=0;repeat<=64;++repeat) {
        MetadataOperation *operation=calloc(1,sizeof(*operation));
        if (!operation) return 0;
        atomic_init(&operation->references,1); atomic_init(&operation->settled,false);
        metadata_local_admitted=1; atomic_store(&metadata_inflight,1);
        pthread_mutex_lock(&metadata_queue_mutex);
        uint64_t begin=ns_now(CLOCK_THREAD_CPUTIME_ID);
        metadata_finish_charge(operation,0,true);
        uint64_t end=ns_now(CLOCK_THREAD_CPUTIME_ID);
        if (!begin || end<begin) return 0;
        /* First actual pass warms allocator/PLT/cache setup; its CPU is
         * startup work, not a recurring footer price. */
        if (repeat) {
            metadata_footer_samples[repeat-1]=end-begin;
        }
    }
    uint64_t sorted[64];memcpy(sorted,metadata_footer_samples,sizeof sorted);
    qsort(sorted,64,sizeof(*sorted),compare_footer_sample);
    uint64_t difference=sorted[32]-sorted[31];
    metadata_footer_median=sorted[31]+difference/2+difference%2;
    metadata_footer_p95=sorted[60];metadata_footer_max=sorted[63];
    /* Keep observer CPU in the quote (no differential subtraction), plus
     * measured clock resolution. This is an estimate, not a worst-case bound. */
    return metadata_footer_median>UINT64_MAX-clock_resolution ? 0 : metadata_footer_median+clock_resolution;
}
void peak_global_budget_mpi_frontend_charge(uint64_t cpu_ns)
{
    uint64_t begin=ns_now(CLOCK_MONOTONIC);
    if (getpid()!=atomic_load(&owner) || !atomic_load(&running)) return;
    uint64_t forecast=atomic_load(&metadata_estimate);
    uint64_t current=ns_add(cpu_ns,metadata_footer_quote);
    if (current>forecast) forecast=current;
    metadata_prefetch(forecast);
    MetadataOperation *operation=metadata_operation;
    metadata_operation=NULL; /* Service may free the operation once ready. */
    pthread_mutex_lock(&metadata_queue_mutex);
    poke();
    uint64_t end=ns_now(CLOCK_MONOTONIC);
    uint64_t own=begin && end>=begin ? end-begin : UINT64_MAX;
    uint64_t cost=ns_add(ns_add(cpu_ns,own),metadata_footer_quote);
    metadata_finish_charge(operation,cost,false);
}
void peak_global_budget_mpi_context_register(uint32_t owner_id, uint64_t serial,
    const int *members, size_t count)
{
    if (!peak_global_budget_mpi_events_enabled()) return;
    if ((count && !members) || count > SIZE_MAX - 5) { metadata_fail(); return; }
    MetadataRecord *record = metadata_alloc(count + 5);
    if (!record) return;
    record->words[0] = record->count; record->words[1] = 1;
    record->words[2] = owner_id; record->words[3] = serial; record->words[4] = count;
    for (size_t i = 0; i < count; ++i) {
        if (members[i] < 0 || (unsigned)members[i] >= world) { metadata_record_free(record); metadata_fail(); return; }
        record->words[5+i] = (unsigned)members[i];
    }
    metadata_append(record);
}
void peak_global_budget_mpi_event_submit(const PeakMpiBudgetEvent *event,
    const PeakMpiBudgetCostSnapshot *snapshot)
{
    /* In-flight admitted records and the terminal UNKNOWN remain admissible
     * after OFF. No sequence is invented by this adapter. */
    if (!event || !snapshot || getpid() != atomic_load(&owner) || !atomic_load(&running)) return;
    if ((event->count && !event->requests) || event->count > SIZE_MAX - 18 ||
        event->rank < 0 || (unsigned)event->rank != rank_id) { metadata_fail(); return; }
    MetadataRecord *record = metadata_alloc(event->count + 18);
    if (!record) return;
    uint64_t outside = ns_add(snapshot->active_outside_ns, snapshot->ignored_outside_ns);
    uint64_t inside = ns_add(snapshot->active_inside_ns, snapshot->ignored_inside_ns);
    uint64_t fixed[] = {record->count, 2, event->context_owner, event->comm,
        event->kind, event->op, event->seq, event->last_mpi_seq, event->round,
        event->tag, event->request, (uint64_t)event->rank, (uint64_t)event->peer,
        outside, inside, ns_add(outside, inside), event->count, event->closure_flags};
    memcpy(record->words, fixed, sizeof fixed);
    if (event->count) memcpy(record->words+18, event->requests, event->count*sizeof(uint64_t));
    if (metadata_operation) {
        record->operation=metadata_operation;metadata_operation->has_record=true;
        atomic_fetch_add(&metadata_operation->references,1);
    }
    metadata_append(record);
}
/* Return -1 on peer loss; partial I/O is retained across poll iterations. */
static int read_frame(Peer *p)
{
    if (p->received < PEAK_GLOBAL_BUDGET_WIRE_SIZE) {
        size_t count = PEAK_GLOBAL_BUDGET_WIRE_SIZE - p->received;
        if (count > IO_CHUNK) count = IO_CHUNK;
        ssize_t n = recv(p->fd, p->incoming_wire + p->received, count, 0);
        if (n > 0) { p->received += (size_t)n; p->last_activity = ns_now(CLOCK_MONOTONIC); }
        else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return -1;
        if (p->received != PEAK_GLOBAL_BUDGET_WIRE_SIZE) return 0;
        if (!peak_global_budget_frame_decode(&p->incoming, p->incoming_wire) ||
            p->incoming.payload_words > SIZE_MAX / 8) return -1;
        if (p->incoming.payload_words) {
            p->incoming_payload = malloc((size_t)p->incoming.payload_words * 8);
            if (!p->incoming_payload) return -1;
        }
    }
    size_t bytes = (size_t)p->incoming.payload_words * 8;
    if (p->payload_received < bytes) {
        size_t count = bytes - p->payload_received;
        if (count > IO_CHUNK) count = IO_CHUNK;
        ssize_t n = recv(p->fd, p->incoming_payload + p->payload_received, count, 0);
        if (n > 0) { p->payload_received += (size_t)n; p->last_activity = ns_now(CLOCK_MONOTONIC); }
        else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return -1;
    }
    return p->payload_received == bytes ? 1 : 0;
}
static void read_frame_done(Peer *p)
{
    p->received = p->payload_received = 0;
    free(p->incoming_payload); p->incoming_payload = NULL;
}
static int write_frame(Peer *p)
{
    if (!p->pending) return 0;
    if (p->outgoing.payload_words > SIZE_MAX / 8) return -1;
    if (!p->sent) peak_global_budget_frame_encode(p->outgoing_wire, &p->outgoing);
    size_t bytes = PEAK_GLOBAL_BUDGET_WIRE_SIZE + (size_t)p->outgoing.payload_words * 8;
    if (bytes < PEAK_GLOBAL_BUDGET_WIRE_SIZE) return -1;
    const unsigned char *source;
    size_t count;
    if (p->sent < PEAK_GLOBAL_BUDGET_WIRE_SIZE) {
        source = p->outgoing_wire + p->sent;
        count = PEAK_GLOBAL_BUDGET_WIRE_SIZE - p->sent;
    } else {
        source = p->outgoing_payload + p->sent - PEAK_GLOBAL_BUDGET_WIRE_SIZE;
        count = bytes - p->sent;
    }
    if (count > IO_CHUNK) count = IO_CHUNK;
#ifdef MSG_NOSIGNAL
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    ssize_t n = send(p->fd, source, count, send_flags);
    if (n > 0) p->sent += (size_t)n;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    if (p->sent == bytes) {
        if (p->outgoing.view.cohort_finalized) p->final_sent = true;
        if (p->outgoing.view.broker_final_written) p->proof_sent = true;
        p->sent = 0; p->pending = false;
        free(p->outgoing_payload); p->outgoing_payload = NULL;
    }
    return 0;
}
static bool header(const Frame *f)
{
    return f->magic == MAGIC && f->version == PROTOCOL_VERSION && f->nonce == job_nonce && f->rank < world && f->size == world;
}
static bool disconnect(Peer *p, Rank *ranks)
{
    bool clean_final = p->rank >= 0 && p->final_sent && ranks[p->rank].latest.final;
    if (clean_final) record_event('C', (unsigned)p->rank, 0, 0, (double)ranks[p->rank].latest.sequence);
    if (p->rank >= 0) ranks[p->rank].connected = false;
    close_fd(&p->fd);
    read_frame_done(p); free(p->outgoing_payload); p->outgoing_payload = NULL;
    return clean_final;
}
static int service_poll_timeout(const Peer *local, uint64_t last_target, bool recheck, bool broken, uint64_t now)
{
    pthread_mutex_lock(&sample_mutex);
    bool sample_ready = local_sample.sequence != 0;
    pthread_mutex_unlock(&sample_mutex);
    bool urgent = sample_ready && (atomic_load(&flush_requested) ||
        (!rank_id && (atomic_load(&earned_origin)!=local->outgoing.root_origin_seconds || atomic_load(&startup_charge)!=local->outgoing.root_startup_seconds || atomic_load(&startup_sealed)!=local->outgoing.root_startup_sealed)) ||
        local_background_period() > local->outgoing.sample.background_period_ns ||
        atomic_load(&demand_generation) != local->outgoing.demand_generation ||
        atomic_load(&requested_ns) != last_target ||
        atomic_load(&metadata_ready) ||
        atomic_load(&causal_fallback) != local->outgoing.sample.causal_fallback ||
        atomic_load(&returned_ns) != local->outgoing.returned_ns ||
        atomic_load(&pressure_notice) != local->outgoing.request_credit ||
        atomic_load(&local_completed_wave) > local->outgoing.sample.completed_wave ||
        effective_targets(atomic_load(&local_attached)) != local->outgoing.sample.attached_targets ||
        atomic_load(&local_retiring) != local->outgoing.sample.retiring ||
        (atomic_load(&local_retiring) && atomic_load(&public_wave) > local->outgoing.sample.completed_wave));
    uint64_t period = negotiated_period();
    uint64_t deadline = local->pending || !local->outgoing.sample.sequence
        ? now + period : local->last_send + period;
    return recheck || (!local->pending && urgent) ? 0 :
        (broken ? 10 : deadline_timeout(now, deadline));
}
static uint64_t closing_reserve_ns(const Rank *r);
static bool reply_urgent(const Peer *p, const Rank *r, bool all, bool final, uint64_t wave, uint64_t completed)
{
    const PeakGlobalBudgetView *v = &p->outgoing.view;
    return v->local_closing_reserved_seconds != (r->registered ? (double)closing_reserve_ns(r)/1e9 : 0) || v->metadata_closed != atomic_load(&causal_fallback) || r->reply_requested || v->startup_sealed != atomic_load(&startup_sealed) || v->broker_final_written != atomic_load(&root_final_sent) || v->active != all || v->wave != wave || v->cohort_finalized != final ||
        v->acknowledged_demand_generation != r->fulfilled_generation ||
        v->root_completed_wave != completed || v->granted_total_ns != r->credit_total_ns ||
        (r->latest.final && v->acknowledged_sequence != r->latest.sequence);
}
/* Acquiring the generation BEFORE its committed costs prevents a new
 * request from describing an older, falsely unspent reservation snapshot. */
/* Producers hold sample_mutex, so an older request never lowers the goal.
 * The service can already own an older snapshot when a new flush arrives. */
static void request_reply_goal_locked(uint64_t sequence)
{
    if (sequence>atomic_load(&reply_goal_sequence))
        atomic_store_explicit(&reply_goal_sequence,sequence,memory_order_release);
    atomic_store_explicit(&flush_requested,true,memory_order_release);
}
static bool frame_reply_requested(uint64_t sequence)
{
    if (!atomic_load_explicit(&flush_requested,memory_order_acquire) ||
        sequence<atomic_load_explicit(&reply_goal_sequence,memory_order_acquire)) return false;
#ifdef PEAK_GLOBAL_BUDGET_TEST_REPLY_CLAIM_HOOK
    PEAK_GLOBAL_BUDGET_TEST_REPLY_CLAIM_HOOK();
#endif
    bool expected=true;
    if (!atomic_compare_exchange_strong_explicit(&flush_requested,&expected,false,
            memory_order_acq_rel,memory_order_acquire)) return false;
    /* A newer producer may have published between the first goal load and
     * flag claim. Preserve that request instead of attaching it to old data. */
    if (sequence<atomic_load_explicit(&reply_goal_sequence,memory_order_acquire)) {
        atomic_store_explicit(&flush_requested,true,memory_order_release);
        return false;
    }
    return true;
}
static Frame request_snapshot(void)
{
    uint64_t generation = atomic_load_explicit(&demand_generation, memory_order_acquire);
    uint64_t frontend_settled = atomic_load_explicit(&settled_frontend_ns, memory_order_acquire);
    pthread_mutex_lock(&sample_mutex);
    Frame next = {.magic = MAGIC, .version = PROTOCOL_VERSION, .nonce = job_nonce, .rank = rank_id,
        .size = world, .sample = local_sample, .demand_generation = generation,
        .consumed_profile_ns = atomic_load_explicit(&consumed_ns, memory_order_acquire),
        .consumed_control_ns = atomic_load_explicit(&consumed_control_ns, memory_order_acquire),
        .consumed_frontend_ns = atomic_load_explicit(&consumed_frontend_ns, memory_order_acquire),
        .returned_ns = atomic_load_explicit(&returned_ns, memory_order_acquire)};
    pthread_mutex_unlock(&sample_mutex);
    next.sample.settled_frontend_credit_ns = frontend_settled;
    next.sample.frontend_seconds = (double)atomic_load(&frontend_cpu_ns)/1e9;
    next.sample.attached_targets = effective_targets(next.sample.attached_targets);
    if (!rank_id) {
        /* Acquire the commitment BEFORE reading its cost. False + new cost is
         * conservative; true + an older cost must never be constructed. */
        next.root_startup_sealed = atomic_load_explicit(&startup_sealed, memory_order_acquire);
        next.root_origin_seconds = atomic_load(&earned_origin);
        next.root_startup_seconds = atomic_load(&startup_charge);
    }
    return next;
}
static void acknowledge_request(uint64_t acknowledged, uint64_t now)
{
    if (acknowledged < atomic_load_explicit(&demand_generation, memory_order_acquire)) return;
    uint64_t start = atomic_load(&request_started_ns);
    if (start && now >= start && !atomic_load(&request_recovery) && !atomic_load(&request_metadata)) {
        double latency = (double)(now - start) / 1e9, old = atomic_load(&refill_rtt);
        atomic_store(&refill_rtt, fmax(.001, fmin(.1, old * .75 + latency * .25)));
    }
    if (start && atomic_compare_exchange_strong(&request_started_ns, &start, 0))
        atomic_store(&public_deadline, 0);
    /* A full funding ACK can need no new grant: its existing reservation
     * suffices. It still completes this request and permits a fresh refill. */
    atomic_store_explicit(&pressure_notice, false, memory_order_release);
    if (atomic_load_explicit(&demand_generation, memory_order_acquire) > acknowledged)
        atomic_store_explicit(&pressure_notice, true, memory_order_release);
}
static uint64_t closing_reserve_ns(const Rank *r)
{
    if (!number(r->closing_estimate) || r->closing_estimate <= 0) return MAX_TOTAL_NS;
    double ns = ceil(r->closing_estimate * 1e9);
    return ns >= (double)MAX_TOTAL_NS ? MAX_TOTAL_NS : (uint64_t)ns;
}
static uint64_t grace_reserve_ns(const Rank *ranks, unsigned count)
{
    uint64_t ns = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!ranks[i].seen || ranks[i].grace_exposed) ns += GRACE_NS;
    return ns;
}
typedef struct { uint64_t credit, closing, grace; } CreditGrant;
static bool credit_exposure_closed(const Rank *r)
{
    return (r->latest.credit_exposure_known ? r->latest.credit_exposed_targets == 0 :
        r->latest.attached_targets == 0) && r->latest.settled_control_credit_ns == r->control_ns;
}
static bool release_closing_registration(Rank *r, uint64_t completed_wave)
{
    bool local_closed = r->latest.credit_exposure_known && credit_exposure_closed(r) &&
        r->latest.sequence > r->registration_sample_sequence &&
        (r->registration_exposed || (r->returned_ns > r->registration_returned_ns && !r->reservation));
    bool wave_closed = r->participant && completed_wave &&
        r->latest.completed_wave >= completed_wave && credit_exposure_closed(r) &&
        r->latest.sequence > r->member_sample_sequence &&
        r->registration_generation <= r->member_generation && r->registration_exposed;
    if (!r->registered) {
        if (r->latest.credit_exposure_known && credit_exposure_closed(r) &&
            r->latest.sequence > r->registration_sample_sequence && r->latest.closing_seconds > 0)
            r->closing_estimate = r->latest.closing_seconds;
        return false;
    }
    if (!(r->latest.retiring || local_closed || wave_closed)) return false;
    r->registered = false;
    /* Actual C is already in S before this release. A fresh closed epoch may
     * price the next exposure from the latest measured forecast; an exposed
     * epoch retains its high-water reserve regardless of cheaper samples. */
    if ((local_closed || wave_closed) && r->latest.closing_seconds > 0)
        r->closing_estimate = r->latest.closing_seconds;
    return true;
}
static bool rank_minimum_unfunded(const Rank *rank)
{
    return rank->wants_credit && rank->minimum_ns>0 && rank->latest.attached_targets>0 &&
        rank->demand_generation>rank->actioned_generation &&
        (!rank->registered || rank->credit_total_ns-rank->consumed_ns-rank->returned_ns<rank->minimum_ns);
}
static bool stop_needed(bool new_funding,bool attached,bool nominal_debt,
    bool unfunded,bool free_empty,bool registration_blocked)
{
    return new_funding && attached &&
        (nominal_debt || (unfunded && (free_empty || registration_blocked)));
}
static CreditGrant grant_rank_credit(Rank *r, uint64_t free_ns)
{
    CreditGrant grant = {0};
    if (!r->wants_credit) return grant;
    uint64_t desired = r->requested_ns > 100000 ? r->requested_ns : 100000;
    bool recovery = !r->latest.attached_targets;
    if (!recovery) {
        double predicted = fmin((double)MAX_TOTAL_NS, ceil(r->latest.pressure * .02 * 1e9));
        if (predicted > (double)desired) desired = (uint64_t)predicted;
    }
    uint64_t reserved = r->credit_total_ns - r->consumed_ns - r->returned_ns;
    uint64_t gap = desired > reserved ? desired - reserved : 0;
    uint64_t closing = r->registered ? 0 : closing_reserve_ns(r);
    uint64_t grace = r->grace_exposed ? 0 : GRACE_NS;
    if (free_ns < closing || free_ns - closing < grace) return grant;
    uint64_t lend = free_ns - closing - grace;
    if (lend > MAX_TOTAL_NS - r->credit_total_ns) lend = MAX_TOTAL_NS - r->credit_total_ns;
    /* Both detached recovery and the first unexposed membership need one
     * complete paid lease, closing reserve and grace. No fragment is parked. */
    if ((recovery || !r->grace_exposed) && lend < gap) return grant;
    grant.credit = gap < lend ? gap : lend; grant.closing = closing; grant.grace = grace;
    if (!r->registered) {
        r->registered = true; r->registration_generation = r->demand_generation;
        r->registration_sample_sequence = r->latest.sequence;
        r->registration_returned_ns = r->returned_ns;
        r->registration_exposed = r->latest.credit_exposure_known ?
            r->latest.credit_exposed_targets > 0 : r->latest.attached_targets > 0;
    }
    if (grace) r->grace_exposed = true;
    r->credit_total_ns += grant.credit;
    r->granted_total = (double)r->credit_total_ns / 1e9;
    r->reservation = (double)(reserved + grant.credit) / 1e9;
    return grant;
}
typedef struct BrokerRecord {
    struct BrokerRecord *next;
    size_t length;
    uint64_t words[];
} BrokerRecord;
typedef struct BrokerExit {
    struct BrokerExit *next;
    uint32_t owner;
    uint64_t context, round;
    bool certified, entry_fence;
} BrokerExit;
typedef struct {
    BrokerRecord *head, *tail;
    BrokerExit *exits;
    bool at_cut, entry_fence, queued;
    uint64_t ready_pass;
} BrokerLane;
typedef struct {
    PeakGlobalBudgetCausal *model;
    Rank *ranks;
    BrokerLane *lanes;
    unsigned *ready;
    unsigned ready_count, reached, fenced, drain_cursor;
    uint64_t drain_pass;
    unsigned count;
    bool cut_active, draining, invalid, closed;
    uint32_t owner;
    uint64_t context, round, checkpoints, fenced_checkpoints;
} BrokerMetadata;

static bool broker_metadata_init(BrokerMetadata *metadata,
    PeakGlobalBudgetCausal *model, Rank *ranks, unsigned count)
{
    *metadata = (BrokerMetadata){.model=model, .ranks=ranks, .count=count};
    metadata->lanes = calloc(count, sizeof(*metadata->lanes));
    metadata->ready = calloc(count, sizeof(*metadata->ready));
    if (metadata->lanes && metadata->ready) return true;
    free(metadata->lanes); free(metadata->ready);
    metadata->lanes=NULL; metadata->ready=NULL;
    return false;
}
static void broker_metadata_clear(BrokerMetadata *metadata)
{
    for (unsigned i = 0; i < metadata->count; ++i) {
        BrokerLane *lane = &metadata->lanes[i];
        while (lane->head) {
            BrokerRecord *next = lane->head->next;
            free(lane->head); lane->head = next;
        }
        while (lane->exits) {
            BrokerExit *next = lane->exits->next;
            free(lane->exits); lane->exits = next;
        }
    }
    free(metadata->lanes);
    free(metadata->ready);
}
static bool broker_ready_before(const BrokerMetadata *metadata, unsigned a, unsigned b)
{
    return metadata->lanes[a].ready_pass < metadata->lanes[b].ready_pass ||
        (metadata->lanes[a].ready_pass == metadata->lanes[b].ready_pass && a < b);
}
static void broker_ready_push(BrokerMetadata *metadata, unsigned member)
{
    BrokerLane *lane=&metadata->lanes[member];
    if (lane->queued || lane->at_cut || !lane->head) return;
    lane->queued=true;
    lane->ready_pass=metadata->draining ? metadata->drain_pass + (member<metadata->drain_cursor) : 0;
    unsigned position=metadata->ready_count++;
    while (position) {
        unsigned parent=(position-1)/2;
        if (!broker_ready_before(metadata,member,metadata->ready[parent])) break;
        metadata->ready[position]=metadata->ready[parent]; position=parent;
    }
    metadata->ready[position]=member;
}
static unsigned broker_ready_pop(BrokerMetadata *metadata)
{
    unsigned member=metadata->ready[0], last=metadata->ready[--metadata->ready_count];
    unsigned position=0;
    while (position*2+1<metadata->ready_count) {
        unsigned child=position*2+1;
        if (child+1<metadata->ready_count &&
            broker_ready_before(metadata,metadata->ready[child+1],metadata->ready[child])) ++child;
        if (!broker_ready_before(metadata,metadata->ready[child],last)) break;
        metadata->ready[position]=metadata->ready[child]; position=child;
    }
    if (metadata->ready_count) metadata->ready[position]=last;
    metadata->lanes[member].queued=false;
    return member;
}
static BrokerExit *broker_exit(BrokerLane *lane, uint32_t owner_id,
    uint64_t context, bool create)
{
    for (BrokerExit *exit = lane->exits; exit; exit = exit->next)
        if (exit->owner == owner_id && exit->context == context) return exit;
    if (!create) return NULL;
    BrokerExit *exit = calloc(1, sizeof(*exit));
    if (exit) {
        exit->owner = owner_id; exit->context = context;
        exit->next = lane->exits; lane->exits = exit;
    }
    return exit;
}
static void broker_cut_release(BrokerMetadata *metadata)
{
    metadata->cut_active = false;
    metadata->reached=metadata->fenced=0;
    for (unsigned i = 0; i < metadata->count; ++i) {
        metadata->lanes[i].at_cut = false; metadata->lanes[i].entry_fence = false;
        broker_ready_push(metadata,i);
    }
}
/* A valid opaque live operation cannot be recertified. Retain its raw bill,
 * stop causal allocation, and release buffered records without folding them.
 * The ordinary permanent-SUM transition reserves their outer raw tail. */
static void broker_metadata_close(BrokerMetadata *metadata)
{
    if (metadata->closed) return;
    metadata->closed=true;
    peak_global_budget_causal_fallback(metadata->model);
    broker_cut_release(metadata);
    metadata->ready_count=0;
    for(unsigned i=0;i<metadata->count;++i) {
        BrokerLane *lane=&metadata->lanes[i];
        while(lane->head) {
            BrokerRecord *next=lane->head->next;
            free(lane->head);lane->head=next;
        }
        lane->tail=NULL;
        lane->queued=false;
    }
}
static bool broker_process_event(BrokerMetadata *metadata, unsigned member,
    const uint64_t *record)
{
    Rank *rank = &metadata->ranks[member];
    if (record[13] > rank->represented_outside) rank->represented_outside = record[13];
    PeakGlobalBudgetCausalEvent event = {.rank=member, .sequence=record[6],
        .last_mpi_sequence=record[7], .context_owner=(uint32_t)record[2],
        .context=record[3], .round=record[8], .tag=record[9], .request=record[10],
        .kind=(PeakGlobalBudgetCausalKind)record[4],
        .operation=(PeakGlobalBudgetCausalOperation)record[5], .peer=(size_t)record[12],
        .cumulative_cost_ns=record[13], .requests=record+18, .request_count=record[0]-18};
    if (trace_enabled && (event.kind==PEAK_CAUSAL_UNKNOWN ||
        event.operation==PEAK_CAUSAL_AMBIGUOUS_SEND))
        record_event('u',member,event.sequence,event.round,(double)event.cumulative_cost_ns/1e9);
    if (!peak_global_budget_causal_submit(metadata->model, &event)) {
        peak_global_budget_causal_fallback(metadata->model);
        peak_global_budget_causal_reserve_outside(metadata->model, member, record[13]);
        metadata->invalid = true;
        broker_cut_release(metadata);
    }
    if (metadata->invalid) return true;
    if (event.kind != PEAK_CAUSAL_MPI_EXIT || event.operation != PEAK_CAUSAL_COLLECTIVE)
        return true;
    BrokerExit *exit = broker_exit(&metadata->lanes[member], event.context_owner,
        event.context, true);
    if (!exit) return false;
    if (event.round < exit->round) return false;
    exit->round = event.round;
    exit->certified = (record[17] & PEAK_MPI_CLOSED_WORLD_PREFIX) != 0;
    exit->entry_fence = (record[17] & PEAK_MPI_ENTRY_FENCE) != 0;
    if (metadata->cut_active && event.context_owner == metadata->owner &&
        event.context == metadata->context &&
        (event.round > metadata->round ||
         (event.round == metadata->round && !exit->certified)))
        broker_cut_release(metadata);
    if (!metadata->cut_active && exit->certified) {
        /* A late certificate cannot recreate a cut another rank already
         * passed without certification. No prior collective is invented. */
        bool possible = true;
        for (unsigned i = 0; i < metadata->count; ++i) {
            BrokerExit *seen = broker_exit(&metadata->lanes[i], event.context_owner,
                event.context, false);
            if (seen && (seen->round > event.round ||
                (seen->round == event.round && !seen->certified))) possible = false;
        }
        if (possible) {
            metadata->cut_active = true; metadata->owner = event.context_owner;
            metadata->context = event.context; metadata->round = event.round;
            metadata->reached=metadata->fenced=0;
            for (unsigned i = 0; i < metadata->count; ++i) {
                BrokerExit *seen = broker_exit(&metadata->lanes[i], metadata->owner,
                    metadata->context, false);
                metadata->lanes[i].at_cut = seen && seen->round == metadata->round && seen->certified;
                metadata->lanes[i].entry_fence = metadata->lanes[i].at_cut && seen->entry_fence;
                metadata->reached+=metadata->lanes[i].at_cut;
                metadata->fenced+=metadata->lanes[i].entry_fence;
            }
        }
    }
    if (metadata->cut_active && event.context_owner == metadata->owner &&
        event.context == metadata->context) {
        if (event.round == metadata->round) {
            BrokerLane *lane=&metadata->lanes[member];
            if (!lane->at_cut) { lane->at_cut=true; ++metadata->reached; }
            if (lane->entry_fence!=exit->entry_fence) {
                if (exit->entry_fence) ++metadata->fenced; else --metadata->fenced;
                lane->entry_fence=exit->entry_fence;
            }
            bool complete=metadata->reached==metadata->count;
            bool entry_fence=metadata->fenced==metadata->count;
            if (complete) {
                /* Only the certified prefix has entered the core. Held
                 * future outside costs remain the outer reversible U tail. */
                bool checkpoint = entry_fence ?
                    peak_global_budget_causal_checkpoint_after_entry_fence(metadata->model) :
                    peak_global_budget_causal_checkpoint(metadata->model);
                if (checkpoint) {
                    ++metadata->checkpoints;
                    if (entry_fence) ++metadata->fenced_checkpoints;
                    if (trace_enabled) record_event('k',member,metadata->round,metadata->checkpoints,
                        (double)peak_global_budget_causal_view(metadata->model).effective_ns/1e9);
                } else if (trace_enabled) record_event('x',member,metadata->round,metadata->checkpoints,
                    (double)peak_global_budget_causal_view(metadata->model).effective_ns/1e9);
                broker_cut_release(metadata);
            }
        }
    }
    return true;
}
static bool broker_drain_held(BrokerMetadata *metadata)
{
    if (metadata->draining || !metadata->ready_count) return true;
    metadata->draining = true;
    metadata->drain_pass=0; metadata->drain_cursor=0;
    bool valid=true;
    while (metadata->ready_count && valid) {
        unsigned member=broker_ready_pop(metadata);
        BrokerLane *lane=&metadata->lanes[member];
        if (lane->at_cut || !lane->head) continue;
        metadata->drain_pass=lane->ready_pass; metadata->drain_cursor=member+1;
        BrokerRecord *record=lane->head;
        lane->head=record->next;
        if (!lane->head) lane->tail=NULL;
        valid=broker_process_event(metadata,member,record->words);
        free(record);
        broker_ready_push(metadata,member);
    }
    metadata->draining = false;
    return valid;
}
static bool broker_accept_event(BrokerMetadata *metadata, unsigned member,
    const uint64_t *record)
{
    Rank *rank = &metadata->ranks[member];
    /* These highwaters retain the complete raw bill immediately, including
     * future records held behind a prefix. Inside work is always SUM. */
    if (record[14] > rank->represented_inside) rank->represented_inside = record[14];
    if (record[15] > rank->raw_profile) rank->raw_profile = record[15];
    if (record[17]&PEAK_MPI_OPAQUE_LIVE) broker_metadata_close(metadata);
    if (metadata->closed) return true;
    BrokerLane *lane = &metadata->lanes[member];
    if (lane->at_cut || lane->head) {
        size_t length = (size_t)record[0];
        if (length > (SIZE_MAX-sizeof(BrokerRecord))/sizeof(uint64_t)) return false;
        BrokerRecord *copy = malloc(sizeof(*copy)+length*sizeof(uint64_t));
        if (!copy) return false;
        copy->next = NULL; copy->length = length;
        memcpy(copy->words, record, length*sizeof(uint64_t));
        if (lane->tail) lane->tail->next = copy; else lane->head = copy;
        lane->tail = copy;
        broker_ready_push(metadata,member);
    } else if (!broker_process_event(metadata, member, record)) return false;
    return broker_drain_held(metadata);
}
static bool consume_metadata(BrokerMetadata *metadata,
    const Frame *frame, const unsigned char *payload)
{
    size_t count = (size_t)frame->payload_words;
    if (!count) return true;
    uint64_t *words = malloc(count * sizeof(*words));
    if (!words) return false;
    const unsigned char *input = payload;
    for (size_t i = 0; i < count; ++i) words[i] = peak_budget_wire_get(&input, 8);
    bool valid = true;
    for (size_t offset = 0; offset < count && valid;) {
        uint64_t *record = words + offset;
        size_t remaining = count - offset;
        if (remaining < 2 || record[0] < 2 || record[0] > remaining) { valid = false; break; }
        size_t length = (size_t)record[0];
        if (record[1] == 1) {
            if (length < 5 || record[4] != length - 5) { valid = false; break; }
            if (metadata->closed) { offset+=length; continue; }
            size_t *members = malloc((length-5) * sizeof(*members));
            if (!members && length > 5) { valid = false; break; }
            for (size_t i = 5; i < length; ++i) members[i-5] = (size_t)record[i];
            if (!peak_global_budget_causal_context(metadata->model, (uint32_t)record[2], record[3], members, length-5)) {
                peak_global_budget_causal_fallback(metadata->model);
                metadata->invalid = true;
                broker_cut_release(metadata);
            }
            free(members);
        } else if (record[1] == 2) {
            if (length < 18 || record[16] != length-18 || record[11] != frame->rank ||
                frame->rank >= metadata->count || record[4] > PEAK_CAUSAL_MPI_EXIT ||
                record[5] > PEAK_CAUSAL_AMBIGUOUS_SEND || (record[17] & ~(PEAK_MPI_CLOSED_WORLD_PREFIX|PEAK_MPI_ENTRY_FENCE|PEAK_MPI_OPAQUE_LIVE)) ||
                ((record[17]&PEAK_MPI_OPAQUE_LIVE) && record[4]!=PEAK_CAUSAL_UNKNOWN)) {
                valid = false; break;
            }
            valid = broker_accept_event(metadata, frame->rank, record);
        } else valid = false;
        offset += length;
    }
    free(words);
    return valid;
}
static void *service(void *data)
{
    ServiceStorage *storage = data;
    uint64_t cpu_origin = ns_now(CLOCK_THREAD_CPUTIME_ID);
    if (pthread_listener_exclude_current_thread) pthread_listener_exclude_current_thread();
    Peer *peers = storage->peers, local = {.fd = client, .rank = (int)rank_id, .last_send = ns_now(CLOCK_MONOTONIC)};
    Rank *ranks = storage->ranks;
    struct pollfd *ready = storage->ready;
    bool *reply_dirty = storage->reply_dirty;
    unsigned peers_count = storage->peers_count;
    double start = seconds_now(), transport_cpu = 0, span = 0, wave_start = 0;
    uint64_t wave = 0, reply_sequence = 0, received_reply = 0, last_wave = 0, completed_wave = 0, message_sequence = 0;
    bool wave_pending = false, idle_returned = false, final_announced = false, recheck_root = false;
    unsigned allocation_cursor = 0;
    uint64_t rate_time = ns_now(CLOCK_MONOTONIC), rate_consumed = 0, last_consumption = rate_time, last_request_target = 0, last_profile_activity = rate_time, learning_generation = 0;
    bool hot_origin = false;
    bool broken = false; double broken_deadline = 0;
    uint64_t broker_cpu_last=0;bool broker_clock_invalid=false;
    uint64_t last_physical_activity = 0;
    BackgroundCost transport_cost = {0}, broker_cost = {0};
    uint64_t background = BACKGROUND_NS, next_round = rate_time;
    uint64_t transport_round_cpu = 0, broker_round_cpu = 0;
    bool root_all_final = false;
    unsigned seen_ranks = 0;
    PeakGlobalBudgetLocalBill pending_bill = {0};
    /* Authoritative accepted-rank snapshot from the final broker decision,
     * rather than a later client write or an uncharged exit CPU clock. */
    struct {
        double ranks, profile, control, management, transport, startup, broker, spent;
        double confirmed, pending, raw, tail, frontend;
        unsigned final_ranks;
    } receipt = {0};
    bool final_proof_sent = false;
    PeakGlobalBudgetCausal *causal = (!rank_id && !native_client) ? peak_global_budget_causal_new(world) : NULL;
    BrokerMetadata metadata = {0};
    if (causal && !broker_metadata_init(&metadata, causal, ranks, world)) {
        peak_global_budget_causal_free(causal); causal = NULL;
    }
    bool causal_sum = false, causal_forced_sum = false;
    double sum_profile_base = 0;
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        /* Process parent, not creating pthread: MPI_Init may use a worker. */
        if (broker_only && getppid() != broker_owner_pid)
            break;
        uint64_t now_ns = ns_now(CLOCK_MONOTONIC);
        uint64_t loop_cpu = ns_now(CLOCK_THREAD_CPUTIME_ID);
        bool root_urgent = recheck_root, client_urgent = false, completed_background_round = false;
        bool ordinary_due = now_ns >= local.last_send + negotiated_period();
        ready[0] = (struct pollfd){(rank_id == 0 && !native_client) ? listener : -1, POLLIN, 0};
        ready[1] = (struct pollfd){local.fd, POLLIN | (local.pending ? POLLOUT : 0), 0};
        ready[2] = (struct pollfd){wake_fd, POLLIN, 0};
        for (unsigned i = 0; i < peers_count; ++i)
            ready[i + 3] = (struct pollfd){(rank_id == 0 && !native_client) ? peers[i].fd : -1, POLLIN | (peers[i].pending ? POLLOUT : 0), 0};
        int timeout = broker_only ? (recheck_root ? 0 : (broken ? 10 : deadline_timeout(now_ns, next_round))) : service_poll_timeout(&local, last_request_target, recheck_root, broken, now_ns);
        recheck_root = false;
        (void)poll(ready, (nfds_t)peers_count + 3, timeout);
        now_ns = ns_now(CLOCK_MONOTONIC);
        if (ready[2].revents & POLLIN) { unsigned char wakes[256]; (void)read(wake_fd, wakes, sizeof(wakes)); }
        if (rank_id == 0 && !native_client) {
            int s = ready[0].revents & POLLIN
                ? accept_peer(listener) : -1;
            while (s >= 0) {
                int one = 1; (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                unsigned slot;
                for (slot = 0; slot < world; ++slot) if (peers[slot].fd < 0 && !peers[slot].final_sent) {
                    peers[slot] = (Peer){.fd = s, .rank = -1, .last_activity = now_ns}; break;
                }
                if (slot == world) close(s);
                s = accept_peer(listener);
            }
            for (unsigned i = 0; i < world; ++i) if (peers[i].fd >= 0 && (ready[i + 3].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                Peer *p = &peers[i];
                int result = read_frame(p);
                if (result < 0) { if (!disconnect(p, ranks)) broken = true; continue; }
                if (result != 1) continue;
                Frame f = p->incoming;
                if (!header(&f) || f.sample.completed_wave > wave || (p->rank >= 0 && (unsigned)p->rank != f.rank) ||
                    (p->rank < 0 && ranks[f.rank].seen)) {
                    (void)disconnect(p, ranks); broken = true; continue;
                }
                Rank *r = &ranks[f.rank];
                if (r->seen && f.message_sequence <= r->message_sequence) continue;
                if (!f.message_sequence || (r->seen && f.sample.sequence < r->latest.sequence)) {
                    (void)disconnect(p, ranks); broken = true; continue;
                }
                if (f.minimum_ns>f.requested_ns || !root_metadata_valid(&f)) {
                    (void)disconnect(p, ranks);
                    broken = true;
                    continue;
                }
                bool first_sample = !r->seen;
                r->reply_requested |= f.request_reply;
                root_urgent |= f.request_reply || (!first_sample &&
                    (r->wants_credit != (f.request_credit && !f.sample.retiring) ||
                     r->requested_ns != f.requested_ns || r->demand_generation != f.demand_generation ||
                     r->returned_ns != f.returned_ns || r->latest.attached_targets != f.sample.attached_targets ||
                     r->latest.completed_wave != f.sample.completed_wave ||
                     r->latest.retiring != f.sample.retiring || r->latest.final != f.sample.final));
                if (first_sample && ++seen_ranks == world) root_urgent = true;
                if (!f.rank && broker_only) {
                    root_urgent |= f.root_startup_sealed != atomic_load(&startup_sealed) ||
                        f.root_startup_seconds != atomic_load(&startup_charge) ||
                        f.root_origin_seconds != atomic_load(&earned_origin);
                    atomic_store(&earned_origin, f.root_origin_seconds);
                    atomic_store(&startup_charge, f.root_startup_seconds);
                    atomic_store(&startup_sealed, f.root_startup_sealed);
                }
                r->message_sequence = f.message_sequence;
                if (f.demand_generation < r->demand_generation) { (void)disconnect(p, ranks); broken = true; continue; }
                r->wants_credit = f.request_credit && !f.sample.retiring; r->requested_ns = f.requested_ns; r->minimum_ns=f.minimum_ns;
                if (f.demand_generation != r->demand_generation) record_event('R', f.rank, f.demand_generation, wave, (double)f.requested_ns / 1e9);
                r->demand_generation = f.demand_generation;
                if (f.sample.settled_control_credit_ns > f.consumed_control_ns || f.sample.settled_frontend_credit_ns > f.consumed_frontend_ns || !valid_sample(&f.sample, r)) { (void)disconnect(p, ranks); broken = true; continue; }
                p->rank = (int)f.rank;
                if (!f.sample.initial_unexposed) r->grace_exposed = true;
                if (f.consumed_profile_ns < r->profile_ns || f.consumed_control_ns < r->control_ns || f.consumed_frontend_ns < r->frontend_ns || f.returned_ns < r->returned_ns ||
                    f.consumed_profile_ns > r->credit_total_ns || f.consumed_control_ns > r->credit_total_ns - f.consumed_profile_ns ||
                    f.consumed_frontend_ns > r->credit_total_ns - f.consumed_profile_ns - f.consumed_control_ns ||
                    f.returned_ns > r->credit_total_ns - f.consumed_profile_ns - f.consumed_control_ns - f.consumed_frontend_ns) {
                    (void)disconnect(p, ranks); broken = true; continue;
                }
                r->returned_ns = f.returned_ns;
                if (f.sample.closing_seconds > r->closing_estimate) r->closing_estimate = f.sample.closing_seconds;
                r->profile_ns = f.consumed_profile_ns; r->control_ns = f.consumed_control_ns;
                r->frontend_ns = f.consumed_frontend_ns;
                r->consumed_ns = r->profile_ns + r->control_ns + r->frontend_ns;
                r->reservation = (double)(r->credit_total_ns - r->consumed_ns - r->returned_ns) / 1e9;
                double current = ledger_cost(&f.sample, r->profile_ns);
                if (current > r->previous_cost) r->previous_cost = current;
                r->latest = f.sample; r->seen = r->connected = true; r->last_update = now_ns;
                if (r->registered && (f.sample.credit_exposure_known ?
                    f.sample.credit_exposed_targets > 0 : f.sample.attached_targets > 0))
                    r->registration_exposed = true;
                if (!f.sample.causal_supported || f.sample.causal_fallback) causal_forced_sum = true;
                if (causal && f.payload_words && !consume_metadata(&metadata,&f,p->incoming_payload)) {
                    causal_forced_sum = true; broken = true;
                }
                if (metadata.closed) { causal_forced_sum=true; root_urgent=true; }
                read_frame_done(p);
            }
            bool root_round = root_urgent || broken || now_ns >= next_round;
            if (root_round) {
            for (unsigned i = 0; i < world; ++i)
                if (peers[i].fd >= 0 && now_ns > peers[i].last_activity &&
                    now_ns - peers[i].last_activity > freshness_ns(background) * 2) {
                    if (!disconnect(&peers[i], ranks)) broken = true;
                }
            bool all = !broken, all_final = !broken, unfunded = false, done = wave != 0;
            double broker_cpu=(double)broker_cpu_last/1e9;
            if (broker_only && !broker_cpu_snapshot(&broker_cpu_last, &broker_cpu)) {
                broker_clock_invalid = true;
                broken = true;
                all = all_final = false;
            }
            double startup = atomic_load_explicit(&startup_charge, memory_order_relaxed);
            double spent = startup + broker_cpu, reserved = 0;
            double rank_bills = 0, rank_profile = 0, rank_control = 0, rank_management = 0, rank_transport = 0, rank_frontend = 0;
            unsigned final_ranks = 0;
            double profile_raw = 0, inside_sum = 0, profile_tail = 0, serialized_sum = startup + broker_cpu;
            for (unsigned i = 0; i < world; ++i) {
                Rank *r = &ranks[i];
                bool final_closed = r->latest.final && !r->connected;
                if (r->seen && !final_closed && now_ns - r->last_update > freshness_ns(background > r->latest.background_period_ns ? background : r->latest.background_period_ns)) broken = true;
                all = all && r->seen && (final_closed || (r->connected && now_ns - r->last_update <= freshness_ns(background > r->latest.background_period_ns ? background : r->latest.background_period_ns)));
                all_final = all_final && r->seen && r->latest.final;
                unfunded = unfunded || (r->wants_credit && (r->credit_total_ns - r->consumed_ns - r->returned_ns) < (r->requested_ns > 100000 ? r->requested_ns : 100000));
                done = done && (r->latest.retiring || r->latest.completed_wave >= wave);
                spent += r->previous_cost; reserved += r->reservation + control_escrow(r);
                rank_bills += r->previous_cost;
                rank_profile += fmax(r->latest.profile_seconds, (double)r->profile_ns / 1e9) + r->latest.ignored_seconds;
                rank_control += r->latest.control_seconds;
                rank_management += r->latest.management_seconds;
                rank_transport += r->latest.transport_seconds;
                rank_frontend += r->latest.frontend_seconds;
                final_ranks += r->seen && r->latest.final;
                uint64_t raw = seconds_to_ns(fmax(r->latest.profile_seconds, (double)r->profile_ns / 1e9) +
                    r->latest.ignored_seconds + r->latest.frontend_seconds);
                if (raw > r->raw_profile) r->raw_profile = raw;
                uint64_t inside = seconds_to_ns(r->latest.profile_inside_seconds);
                if (inside > r->represented_inside) r->represented_inside = inside;
                double serial = r->latest.control_seconds + r->latest.management_seconds + r->latest.transport_seconds;
                if (serial > r->serialized_cost) r->serialized_cost = serial;
                serialized_sum += r->serialized_cost;
                profile_raw += (double)r->raw_profile / 1e9;
                inside_sum += (double)r->represented_inside / 1e9;
                uint64_t represented = ns_add(r->represented_inside,r->represented_outside);
                if (r->raw_profile > represented) profile_tail += (double)(r->raw_profile-represented)/1e9;
            }
            if (metadata.invalid || metadata.closed) causal_forced_sum = true;
            PeakGlobalBudgetCausalView causal_view = causal ? peak_global_budget_causal_view(causal) : (PeakGlobalBudgetCausalView){.sum_mode=true};
            if ((causal_forced_sum || causal_view.resource_failed || causal_view.saturated || !causal) && !causal_sum) {
                if (causal) peak_global_budget_causal_fallback(causal);
                causal_view = causal ? peak_global_budget_causal_view(causal) : (PeakGlobalBudgetCausalView){0};
                sum_profile_base = (double)ns_add(ns_add(causal_view.confirmed_ns,causal_view.unresolved_ns),causal_view.serialized_ns)/1e9 + inside_sum + profile_tail;
                for (unsigned i=0;i<world;++i) ranks[i].sum_profile_checkpoint=ranks[i].raw_profile;
                causal_sum = true;
            }
            if (causal_forced_sum || causal_view.resource_failed || causal_view.saturated)
                atomic_store(&causal_fallback,true);
            double causal_confirmed = serialized_sum;
            double causal_pending = 0;
            if (causal_sum) {
                causal_confirmed += sum_profile_base;
                for (unsigned i=0;i<world;++i)
                    causal_confirmed += (double)(ranks[i].raw_profile-ranks[i].sum_profile_checkpoint)/1e9;
            } else {
                causal_confirmed += (double)ns_add(causal_view.confirmed_ns,causal_view.serialized_ns)/1e9 + inside_sum;
                causal_pending = (double)causal_view.unresolved_ns/1e9 + profile_tail;
            }
            spent = causal_confirmed + causal_pending;
            all_final = all_final && all && !broken;
            if (all_final && !final_announced) { final_announced = true; record_event('F', rank_id, 0, wave, spent); }
            /* Retain each registered rank's measured closing quote; never lend it. */
            double closing_reserve = 0;
            for (unsigned i = 0; i < world; ++i) {
                Rank *r = &ranks[i];
                (void)release_closing_registration(r, completed_wave);
                if (r->registered) closing_reserve += (double)closing_reserve_ns(r) / 1e9;
            }
            double budget = ratio * (seconds_now() - start + atomic_load(&earned_origin));
            double grace_reserve = (double)grace_reserve_ns(ranks, world) / 1e9;
            bool nominal_debt=budget<spent+reserved+grace_reserve+closing_reserve;
            double remaining = fmax(0, budget - spent - reserved - grace_reserve - closing_reserve);
            if (wave_pending && done) {
                span = seconds_now() - wave_start; completed_wave = wave; wave_pending = false;
                record_event('D', rank_id, 0, wave, span);
                for (unsigned i = 0; i < world; ++i)
                    (void)release_closing_registration(&ranks[i], completed_wave);
            }
            if (all && !wave_pending && atomic_load(&startup_sealed)) {
                /* Spend only already earned global headroom, prioritizing
                 * demand; no timeout ever reclaims an unacknowledged grant. */
                for (unsigned k = 0; k < world && remaining > 0; ++k) {
                    unsigned i = (allocation_cursor + k) % world;
                    Rank *r = &ranks[i];
                    double free_money = fmin((double)MAX_TOTAL_NS, floor(remaining * 1e9));
                    uint64_t free_ns = free_money > 0 ? (uint64_t)free_money : 0;
                    CreditGrant issued = grant_rank_credit(r, free_ns);
                    closing_reserve += (double)issued.closing / 1e9;
                    grace_reserve += (double)issued.grace / 1e9;
                    double grant = (double)issued.credit / 1e9;
                    remaining = fmax(0, remaining - (double)(issued.credit + issued.closing + issued.grace) / 1e9);
                    if (grant > 0) record_event('G', i, r->demand_generation, wave, grant);
                }
                allocation_cursor = (allocation_cursor + 1) % world;
                /* Desired runway can exceed the bank without making a paid
                 * next operation insolvent. Only immediate minimum/closing
                 * requirements or actual nominal debt justify a cohort STOP. */
                unfunded = false;
                for (unsigned i = 0; i < world; ++i)
                    unfunded |= rank_minimum_unfunded(&ranks[i]);
                bool attached = false;
                for (unsigned i = 0; i < world; ++i) attached |= ranks[i].latest.attached_targets > 0;
                bool registration_blocked = false;
                bool new_funding = wave == 0;
                for (unsigned i = 0; i < world; ++i) {
                    registration_blocked |= ranks[i].wants_credit && ranks[i].minimum_ns>0 && !ranks[i].registered && ranks[i].latest.attached_targets > 0;
                    new_funding |= ranks[i].credit_total_ns > ranks[i].stop_credit_ns;
                }
                /* A completed wave may preserve cold hooks and old wallets.
                 * Their cash remains reserved and local guards still apply.
                 * A larger unfunded quote alone does not justify another
                 * cohort STOP; only newly issued positive credit rearms it. */
                if (stop_needed(new_funding,attached,nominal_debt,unfunded,remaining<=1e-9,registration_blocked) && wave!=UINT64_MAX) {
                    ++wave; wave_start = seconds_now(); span = 0; wave_pending = true;
                    record_event('S', rank_id, 0, wave, remaining);
                    for (unsigned i = 0; i < world; ++i) {
                        ranks[i].participant = !ranks[i].latest.retiring && (ranks[i].registered || ranks[i].wants_credit);
                        ranks[i].member_generation = ranks[i].demand_generation;
                        ranks[i].member_sample_sequence = ranks[i].latest.sequence;
                        ranks[i].actioned_generation = ranks[i].demand_generation;
                        ranks[i].stop_credit_ns = ranks[i].credit_total_ns;
                    }
                }
            }

            reserved = 0;
            for (unsigned i = 0; i < world; ++i) {
                reserved += ranks[i].reservation + control_escrow(&ranks[i]);
                if (ranks[i].wants_credit && (ranks[i].credit_total_ns - ranks[i].consumed_ns - ranks[i].returned_ns) >= (ranks[i].requested_ns > 100000 ? ranks[i].requested_ns : 100000))
                    ranks[i].fulfilled_generation = ranks[i].demand_generation;
            }
            uint64_t proposed = background_period(&broker_cost, 0, 1, ratio);
            for (unsigned i = 0; i < world; ++i)
                if (ranks[i].latest.background_period_ns > proposed)
                    proposed = ranks[i].latest.background_period_ns;
            background = proposed;
            memset(reply_dirty, 0, world * sizeof(*reply_dirty));
            for (unsigned i = 0; i < world; ++i) {
                Peer *p = &peers[i];
                if (p->fd < 0 || p->rank < 0) continue;
                unsigned id = (unsigned)p->rank;
                bool urgent = reply_urgent(p, &ranks[id], all, all_final, wave, completed_wave) ||
                    p->outgoing.view.background_period_ns != background;
                if (p->pending) { reply_dirty[i] = urgent; continue; }
                if (!urgent && now_ns - p->last_send < background) continue;
                p->last_send = now_ns;
                ranks[id].reply_requested = false;
                uint64_t decision = ++reply_sequence;
                p->outgoing = (Frame){.magic = MAGIC, .version = PROTOCOL_VERSION, .nonce = job_nonce, .rank = id,
                    .size = world, .reply_sequence = decision,
                    .view = {.metadata_closed=atomic_load(&causal_fallback), .local_closing_reserved_seconds=ranks[id].registered?(double)closing_reserve_ns(&ranks[id])/1e9:0, .causal_confirmed_seconds=causal_confirmed, .causal_pending_seconds=causal_pending, .causal_raw_seconds=profile_raw+serialized_sum, .causal_unrepresented_seconds=causal_sum?0:profile_tail, .background_period_ns = background, .freshness_ns = freshness_ns(background), .startup_sealed = atomic_load(&startup_sealed), .broker_process_id = broker_only ? (uint64_t)getpid() : 0, .accounting_invalid = broker_clock_invalid, .broker_final_written = atomic_load(&root_final_sent), .broker_cpu_seconds = broker_cpu, .granted_total_ns = ranks[id].credit_total_ns, .active = all, .cohort_finalized = all_final, .returned_total_ns = ranks[id].returned_ns, .wave_participant = ranks[id].participant, .acknowledged_demand_generation = ranks[id].fulfilled_generation, .wave = wave, .decision_sequence = decision, .view_epoch = decision, .root_completed_wave = completed_wave,
                        .acknowledged_sequence = ranks[id].latest.sequence,
                        .budget_seconds = budget, .reserved_seconds = reserved, .control_reserve_seconds = grace_reserve + closing_reserve, .global_spent_seconds = spent,
                        .remaining_seconds = remaining,
                        .allowance_seconds = ranks[id].reservation,
                        .wave_span_seconds = span, .granted_total_seconds = ranks[id].granted_total}};
                p->pending = true;
            }
            receipt.ranks = rank_bills; receipt.profile = rank_profile;
            receipt.control = rank_control; receipt.management = rank_management;
            receipt.transport = rank_transport; receipt.startup = startup;
            receipt.broker = broker_cpu; receipt.spent = spent;
            receipt.confirmed=causal_confirmed; receipt.pending=causal_pending;
            receipt.raw=profile_raw+serialized_sum; receipt.tail=causal_sum?0:profile_tail;
            receipt.frontend=rank_frontend;
            receipt.final_ranks = final_ranks;
            root_all_final = all_final;
            next_round = now_ns + background;
            completed_background_round = true;
            }
            for (unsigned i = 0; i < world; ++i) if (peers[i].fd >= 0) {
                bool pending = peers[i].pending;
                if (write_frame(&peers[i]) && !disconnect(&peers[i], ranks)) broken = true;
                if (pending && !peers[i].pending && reply_dirty[i]) recheck_root = true;
            }
            bool sent_all = root_all_final;
            for (unsigned i = 0; i < world; ++i) sent_all = sent_all && peers[i].rank >= 0 && peers[i].final_sent;
            if (sent_all && !atomic_load(&root_final_sent)) record_event('A', rank_id, 0, wave, 0);
            bool was_sent = atomic_exchange_explicit(&root_final_sent, sent_all, memory_order_acq_rel);
            if (sent_all && !was_sent)
                recheck_root = true;
            if (broker_only && sent_all) {
                bool proof = false;
                for (unsigned i = 0; i < world; ++i) {
                    if (peers[i].rank == 0 && peers[i].proof_sent)
                        proof = true;
                }
                if (proof) {
                    final_proof_sent = true;
                    break;
                }
            }
        }
        if (broker_only) {
            uint64_t broker_cpu_end = ns_now(CLOCK_THREAD_CPUTIME_ID);
            if (broker_cpu_end >= loop_cpu) broker_round_cpu += broker_cpu_end - loop_cpu;
            if (root_urgent) broker_round_cpu = 0;
            else if (completed_background_round) {
                (void)background_period(&broker_cost, broker_round_cpu, 1, ratio);
                broker_round_cpu = 0;
            }
            if (broken) {
                if (!broken_deadline)
                    broken_deadline = seconds_now() + .05;
                if (seconds_now() >= broken_deadline)
                    break;
            }
            continue;
        }
        uint64_t current_consumed = atomic_load(&consumed_ns);
        uint64_t learning_request = atomic_load_explicit(&demand_generation, memory_order_acquire);
        if (learning_request != learning_generation && atomic_load(&pressure_notice) && !atomic_load(&request_recovery)) {
            if (!hot_origin || (now_ns >= last_profile_activity && now_ns - last_profile_activity >= UINT64_C(100000000))) {
                uint64_t start = atomic_load(&request_started_ns);
                rate_time = start && start <= now_ns ? start : now_ns;
                rate_consumed = current_consumed; last_profile_activity = rate_time; hot_origin = true;
            }
            learning_generation = learning_request;
        }
        if (current_consumed != rate_consumed) { last_consumption = now_ns; idle_returned = false; last_profile_activity = now_ns; }
        if (current_consumed > rate_consumed && now_ns > rate_time) {
            uint64_t interval = now_ns - rate_time;
            if (interval < 1000) interval = 1000;
            double rate = (double)(current_consumed - rate_consumed) / (double)interval;
            double smoothed = fmax(rate, atomic_load(&consumption_rate) * .75);
            atomic_store(&consumption_rate, smoothed);
            double horizon = fmax(.005, 2 * atomic_load(&refill_rtt) + .002);
            uint64_t desired = (uint64_t)fmin(1e9, fmax(100000, smoothed * horizon * 1e9));
            atomic_store(&low_water_ns, desired);
            rate_time = now_ns; rate_consumed = current_consumed;
        }
        if (local.fd >= 0) metadata_settle_closed();
        if (!local.pending) {
            Frame next = request_snapshot();
            uint64_t ack = atomic_load_explicit(&local_completed_wave, memory_order_acquire);
            if (ack > next.sample.completed_wave) next.sample.completed_wave = ack;
            if (next.sample.retiring) {
                uint64_t retired_wave = atomic_load(&public_wave);
                if (retired_wave > next.sample.completed_wave) next.sample.completed_wave = retired_wave;
            }
            next.sample.transport_seconds = transport_cpu;
            double management_input = next.sample.management_seconds;
            double finalizer_cpu = 0, frozen_management = 0;
            unsigned clock_state = atomic_load_explicit(&finalizer_state, memory_order_acquire);
            if (clock_state == 2) {
                uint64_t cpu_now = ns_now(atomic_load(&finalizer_clock)), cpu_start = atomic_load(&finalizer_start);
                if (!cpu_now || cpu_now < cpu_start) {
                    atomic_store(&finalizer_state, 3); atomic_store(&public_accounting_invalid, true);
                    next.sample.finalizer_invalid = true; broken = true;
                } else {
                    finalizer_cpu = (double)(cpu_now - cpu_start) / 1e9;
                    frozen_management = atomic_load(&finalizer_baseline);
                    double live = frozen_management + finalizer_cpu;
                    next.sample.management_seconds = fmax(next.sample.management_seconds, live);
                }
            } else if (clock_state == 3) next.sample.finalizer_invalid = true;
            /* Describe demand against this frame's settlement, not a later local
             * debit: otherwise concurrent callbacks can make the broker
             * count already-consumed reservations as available credit. */
            uint64_t debit = next.consumed_profile_ns + next.consumed_control_ns + next.consumed_frontend_ns + next.returned_ns;
            frame_credit_demand(&next,debit);
            uint64_t request_target=atomic_load(&requested_ns);
            next.request_credit = atomic_load(&pressure_notice);
            if (next.request_credit && !atomic_load(&request_recovery)) {
                uint64_t predicted = atomic_load(&low_water_ns);
                if (predicted <= UINT64_MAX / 2 && next.requested_ns < predicted * 2) next.requested_ns = predicted * 2;
            }
            if (next.sample.sequence) local.outgoing_payload = metadata_batch(&next.payload_words);
            next.sample.causal_supported = peak_mpi_budget_events_frontier != NULL;
            if (!next.sample.causal_supported) {
                metadata_close(8,0); atomic_store(&causal_fallback, true);
            }
            next.sample.causal_fallback = atomic_load(&causal_fallback);
            if (atomic_load(&public_accounting_invalid)) next.sample.finalizer_invalid = true;
            if (atomic_load(&metadata_pending)) next.sample.final = false;
            Frame *prior = &local.outgoing;
            next.request_reply = frame_reply_requested(next.sample.sequence);
            next.sample.background_period_ns = local_background_period();
            bool urgent = next.payload_words || next.sample.causal_fallback != prior->sample.causal_fallback || !prior->sample.sequence || next.request_reply ||
                next.sample.background_period_ns > prior->sample.background_period_ns || next.returned_ns != prior->returned_ns || request_target != last_request_target ||
                next.demand_generation != prior->demand_generation || next.request_credit != prior->request_credit ||
                next.sample.completed_wave != prior->sample.completed_wave || next.sample.retiring != prior->sample.retiring || next.sample.final != prior->sample.final ||
                next.root_origin_seconds != prior->root_origin_seconds || next.root_startup_seconds != prior->root_startup_seconds ||
                next.root_startup_sealed != prior->root_startup_sealed ||
                next.sample.finalizer_invalid != prior->sample.finalizer_invalid ||
                next.sample.attached_targets != prior->sample.attached_targets;
            client_urgent = urgent;
            ordinary_due = now_ns >= local.last_send + negotiated_period();
            if (next.sample.sequence && (urgent || ordinary_due)) {
                if (next.demand_generation != prior->demand_generation)
                    record_event('r', rank_id, next.demand_generation, next.sample.completed_wave, (double)atomic_load(&request_started_ns) / 1e9);
                if (next.demand_generation != prior->demand_generation) { last_consumption = now_ns; idle_returned = false; }
                last_request_target = request_target;
                next.message_sequence = ++message_sequence;
                local.outgoing = next; local.pending = true; local.last_send = now_ns;
                pending_bill = local_bill_from_frame(&next, management_input,
                    finalizer_cpu, frozen_management, clock_state == 2);
            }
        }
        bool was_pending = local.pending;
        if (local.fd >= 0 && write_frame(&local)) { close_fd(&local.fd); broken = true; }
        if (was_pending && !local.pending) {
            pthread_mutex_lock(&sample_mutex);
            last_written_bill = pending_bill;
            pthread_mutex_unlock(&sample_mutex);
        }
        if (local.fd >= 0 && (ready[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            int result = read_frame(&local);
            if (result < 0) { close_fd(&local.fd); broken = true; }
            if (result == 1) {
                Frame f = local.incoming;
                read_frame_done(&local);
                if (!header(&f) || f.rank != rank_id || !f.reply_sequence ||
                    f.view.background_period_ns < BACKGROUND_NS || f.view.background_period_ns > MAX_BACKGROUND_NS ||
                    f.view.freshness_ns != freshness_ns(f.view.background_period_ns) ||
                    (native_client && !f.view.broker_process_id) || !number(f.view.broker_cpu_seconds) || !number(f.view.global_spent_seconds) || !number(f.view.remaining_seconds) ||
                    !number(f.view.allowance_seconds) || !number(f.view.wave_span_seconds) || !number(f.view.granted_total_seconds) || f.view.granted_total_ns > MAX_TOTAL_NS || f.view.granted_total_ns < atomic_load(&granted_ns) ||
                    f.view.granted_total_seconds != (double)f.view.granted_total_ns / 1e9 || !number(f.view.budget_seconds) || !number(f.view.reserved_seconds) || !number(f.view.control_reserve_seconds)) {
                    close_fd(&local.fd); broken = true;
                } else if (f.reply_sequence > received_reply) {
                    received_reply = f.reply_sequence;
                    acknowledge_request(f.view.acknowledged_demand_generation, ns_now(CLOCK_MONOTONIC));
                    if (f.view.granted_total_ns > atomic_load(&granted_ns)) {
                        last_consumption = now_ns; idle_returned = false;
                    }
                    uint64_t proof = atomic_load(&control_proof_sequence);
                    bool proof_ready = proof && f.view.acknowledged_sequence >= proof &&
                        atomic_compare_exchange_strong(&control_proof_sequence,&proof,0);
                    bool credit_ready = f.view.granted_total_ns > atomic_load(&granted_ns);
                    bool closing_ready = f.view.local_closing_reserved_seconds > atomic_load(&public_local_closing);
                    if (credit_ready)
                        record_event('g', rank_id, f.view.acknowledged_demand_generation, f.view.wave, f.view.granted_total_seconds);
                    if (f.view.wave > last_wave) record_event('s', rank_id, 0, f.view.wave, f.view.wave_participant);
                    publish_view(f.view);
                    if ((credit_ready || closing_ready || proof_ready) && peak_global_budget_listener_credit_ready &&
                        peak_global_budget_listener_admission_waiting &&
                        peak_global_budget_listener_admission_waiting())
                        peak_global_budget_listener_credit_ready();
                    if (f.view.wave > last_wave) { last_wave = f.view.wave; if (notify && !atomic_load(&local_retiring)) notify(); }
                }
            }
        }
        if (broken) {
            PeakGlobalBudgetView old = peak_global_budget_view(); old.active = false; publish_view(old);
            if (!broken_deadline) broken_deadline = seconds_now() + .05;
            if (seconds_now() >= broken_deadline) break;
        }
        if (atomic_load(&local_retiring) && atomic_load(&granted_ns) > atomic_load(&debit_ns))
            peak_global_budget_return_idle();
        current_consumed = atomic_load(&consumed_ns);
        if (current_consumed != rate_consumed) { last_consumption = now_ns; idle_returned = false; }
        /* Only the owner returns credits; the broker never reclaims on time.
         * Keep one predictive block and never cancel an outstanding refill. */
        int64_t balance = atomic_load_explicit(&available_ns, memory_order_acquire);
        uint64_t keep = atomic_load(&low_water_ns);
        pthread_mutex_lock(&sample_mutex);
        uint32_t fallback_attached = local_sample.attached_targets;
        pthread_mutex_unlock(&sample_mutex);
        /* Load the event before its fresh activity clock. A thread stop can
         * have held this service since the loop's earlier timestamp. */
        uint64_t physical_word = atomic_load_explicit(&physical_targets, memory_order_acquire);
        uint64_t idle_now = ns_now(CLOCK_MONOTONIC);
        PhysicalIdleSnapshot physical = physical_idle_snapshot(physical_word, fallback_attached,
            idle_now, &last_physical_activity, &last_consumption, &idle_returned);
        if (idle_now >= now_ns &&
            idle_return_due(physical, idle_now, last_consumption, idle_returned, balance, keep)) {
            uint64_t returned = idle_return_commit(physical, balance, keep);
            if (returned) {
                idle_returned = true;
                record_event('I', rank_id, atomic_load(&demand_generation), last_wave,
                    (double)returned / 1e9);
                poke();
            }
        }
        uint64_t cpu_end = ns_now(CLOCK_THREAD_CPUTIME_ID);
        if (cpu_end >= cpu_origin) transport_cpu = (double)(cpu_end - cpu_origin) / 1e9;
        atomic_store_explicit(&local_transport_cpu, transport_cpu, memory_order_relaxed);
        if (cpu_end >= loop_cpu) transport_round_cpu += cpu_end - loop_cpu;
        if (ordinary_due && !client_urgent) {
            atomic_store(&transport_period, background_period(&transport_cost, transport_round_cpu, world, ratio));
            transport_round_cpu = 0;
        } else if (client_urgent) {
            /* Urgent work is fully charged, but does not set idle cadence. */
            transport_round_cpu = 0;
        }

    }
    if (broker_only)
        fprintf(stderr, "[peak] MPI global broker bill: complete=%d final_ranks=%u world_size=%u rank_bill_seconds=%.17g profile_billed_seconds=%.17g control_seconds=%.17g management_seconds=%.17g transport_seconds=%.17g root_startup_seconds=%.17g broker_cpu_charged_seconds=%.17g global_spent_seconds=%.17g\n",
            final_proof_sent, receipt.final_ranks, world, receipt.ranks,
            receipt.profile, receipt.control, receipt.management, receipt.transport,
            receipt.startup, receipt.broker, receipt.spent);
    if (broker_only)
        fprintf(stderr, "[peak] MPI global causal bill: confirmed_seconds=%.17g pending_seconds=%.17g effective_seconds=%.17g raw_seconds=%.17g unrepresented_seconds=%.17g frontend_raw_seconds=%.17g closed_prefixes=%llu entry_fence_prefixes=%llu opaque_closed=%d permanent_sum=%d\n",
            receipt.confirmed,receipt.pending,receipt.spent,receipt.raw,receipt.tail,receipt.frontend,
            (unsigned long long)metadata.checkpoints,(unsigned long long)metadata.fenced_checkpoints,metadata.closed,causal_sum);
    if (metadata.lanes) broker_metadata_clear(&metadata);
    peak_global_budget_causal_free(causal);
    for (unsigned i = 0; i < peers_count; ++i) {
        read_frame_done(&peers[i]); free(peers[i].outgoing_payload); close_fd(&peers[i].fd);
    }
    read_frame_done(&local); free(local.outgoing_payload);
    close_fd(&local.fd); client = -1;
    service_storage_free(storage);
    PeakGlobalBudgetView old = peak_global_budget_view(); old.active = false; publish_view(old);
    return NULL;
}
int peak_global_budget_start(const char *host, unsigned short port, unsigned rank,
                            unsigned size, double target, void (*wake)(void))
{
    if (!job_nonce || !host || !port || !size || size > INT_MAX || rank >= size || !number(target) ||
        atomic_load(&running) || (rank == 0 && listener < 0)) return -1;
    const char *trace = getenv("PEAK_MPI_GLOBAL_TRACE");
    const char *legacy = getenv("PEAK_GLOBAL_BUDGET_TRACE");
    bool tracing = (trace && !strcmp(trace, "1")) || (legacy && !strcmp(legacy, "1"));
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
    if (!rank) {
        broker_pid = peak_global_budget_spawn(listener, size, target, job_nonce, tracing);
        if (broker_pid < 0)
            return -1;
        close_fd(&listener);
    }
    native_client = true;
#else
    (void)tracing;
#endif
    int fd = connect_host(host, port);
    if (fd < 0) {
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
        peak_global_budget_reap(broker_pid);
        broker_pid = -1;
#endif
        return -1;
    }
    if (create_wake_pipe() != 0) { close(fd);
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
        peak_global_budget_reap(broker_pid);
        broker_pid = -1;
#endif
        return -1; }
    ServiceStorage *storage = service_storage_new(!rank && !native_client ? size : 0);
    if (!storage) {
        close(fd); close_fd(&wake_write_fd); close_fd(&wake_fd);
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
        peak_global_budget_reap(broker_pid);
        broker_pid = -1;
#endif
        return -1;
    }
    event_count = 0; trace = getenv("PEAK_MPI_GLOBAL_TRACE");
    const char *legacy_trace = getenv("PEAK_GLOBAL_BUDGET_TRACE");
    trace_enabled = (trace && !strcmp(trace, "1")) || (legacy_trace && !strcmp(legacy_trace, "1"));
    client = fd; rank_id = rank; world = size; ratio = target; notify = wake;
    atomic_store(&owner, getpid());
    atomic_store(&startup_sealed, !native_client);
    atomic_store(&physical_revision, 0);
    atomic_store(&control_proof_sequence, 0); atomic_store(&reply_goal_sequence,0); atomic_store(&minimum_target_ns,0); atomic_store(&admission_target_ns,0); atomic_store(&admission_cash_ns,0); atomic_store(&admission_closing_ns,0);
    atomic_store(&physical_targets, 0); atomic_store(&earned_origin, 0); atomic_store(&local_retiring, false);
    atomic_store(&rejected_spend_count, 0); atomic_store(&available_ns, 0); atomic_store(&local_attached, 0); atomic_store(&recovery_target_ns, 0); atomic_store(&request_recovery, false); atomic_store(&request_metadata,false);
    atomic_store(&finalizer_state, 0); atomic_store(&public_finalized, false);
    atomic_store(&public_accounting_invalid, false); atomic_store(&root_final_sent, false);
    atomic_store(&debit_ns, 0); atomic_store(&returned_ns, 0); atomic_store(&public_demand_ack, 0); atomic_store(&request_started_ns, 0); atomic_store(&public_deadline, 0); atomic_store(&low_water_ns, 100000); atomic_store(&refill_rtt, .005); atomic_store(&consumption_rate, 0);
    atomic_store(&flush_requested, false); atomic_store(&local_transport_cpu, 0); atomic_store(&startup_charge, 0); atomic_store(&local_completed_wave, 0); atomic_store(&granted_ns, 0); atomic_store(&consumed_ns, 0); atomic_store(&consumed_control_ns, 0); atomic_store(&pressure_notice, false); atomic_store(&requested_ns, 0); atomic_store(&minimum_target_ns,0); atomic_store(&admission_target_ns,0); atomic_store(&admission_cash_ns,0); atomic_store(&admission_closing_ns,0); atomic_store(&demand_generation, 0);
    metadata_clear();
    atomic_store(&frontend_cpu_ns, 0); atomic_store(&consumed_frontend_ns, 0); atomic_store(&settled_frontend_ns, 0);
    atomic_store(&metadata_close_reason,0); metadata_started_ns=ns_now(CLOCK_MONOTONIC);
    atomic_store(&causal_fallback, false); atomic_store(&metadata_enabled, true);
    atomic_store(&metadata_seed, 64); atomic_store(&metadata_tokens, 0); atomic_store(&metadata_inflight, 0);
    atomic_store(&metadata_block_credit, 0); atomic_store(&metadata_block_start, 0); atomic_store(&metadata_estimate, 0);
    metadata_ordered_frontend=0;atomic_store(&metadata_unassigned_frontend,0);
    metadata_prefetch_goal=0;
    metadata_footer_quote=metadata_calibrate_footer();
    if (!metadata_footer_quote) {
        atomic_store(&public_accounting_invalid,true);
        metadata_close(4,0); atomic_store(&causal_fallback,true);
    }
    heartbeat_cost = (BackgroundCost){0};
    atomic_store(&heartbeat_period, BACKGROUND_NS); atomic_store(&transport_period, BACKGROUND_NS);
    memset(&last_written_bill, 0, sizeof(last_written_bill));
    memset(&local_sample, 0, sizeof(local_sample)); publish_view((PeakGlobalBudgetView){0});
    atomic_store(&running, true);
    if (pthread_listener_mark_next_created_thread_helper) pthread_listener_mark_next_created_thread_helper();
    if (pthread_create(&worker, NULL, service, storage)) {
        service_storage_free(storage);
        atomic_store(&running, false); close_fd(&client); close_fd(&wake_write_fd); close_fd(&wake_fd);
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
        peak_global_budget_reap(broker_pid);
        broker_pid = -1;
#endif
        return -1;
    }
    /* HB may have dropped its current sample just before running became
     * true, then selected a multi-second adaptive sleep. A sticky lifecycle
     * wake requests the first real sample; it grants no credit itself. */
    if (peak_global_budget_listener_credit_ready)
        peak_global_budget_listener_credit_ready();
    return 0;
}
void peak_global_budget_publish(const PeakGlobalBudgetSample *s)
{
    if (!s || getpid() != atomic_load(&owner) || !atomic_load(&running)) return;
    pthread_mutex_lock(&sample_mutex);
    bool first = !local_sample.sequence;
    bool proof = s->settled_control_credit_ns > local_sample.settled_control_credit_ns &&
        peak_global_budget_listener_admission_waiting &&
        peak_global_budget_listener_admission_waiting();
    if (s->retiring) atomic_store(&local_retiring, true);
    if (!local_sample.final && s->sequence > local_sample.sequence) {
        local_sample = *s; atomic_store(&local_attached, s->attached_targets);
        if (effective_targets(s->attached_targets)) { atomic_store(&recovery_target_ns, 0); atomic_store(&request_recovery, false); }
    }
    local_sample.retiring = atomic_load(&local_retiring);
    if (proof) {
        if (s->sequence>atomic_load(&control_proof_sequence))
            atomic_store(&control_proof_sequence,s->sequence);
        request_reply_goal_locked(s->sequence);
    }
    if (s->final) request_reply_goal_locked(s->sequence);
    pthread_mutex_unlock(&sample_mutex);
    if (first || proof || s->retiring) poke();
}
void peak_global_budget_physical_targets(uint32_t count)
{
    if (getpid() != atomic_load(&owner) || !atomic_load(&running)) return;
    /* Every backend publication is an activity event, including a complete
     * zero -> same-positive cycle that the service never observed in between.
     * Publish revision and count together before waking the service. */
    uint64_t revision = (uint32_t)(atomic_fetch_add(&physical_revision, 1) + 1);
    if (!revision)
        revision = (uint32_t)(atomic_fetch_add(&physical_revision, 1) + 1);
    uint64_t word = (revision << 32) | count;
    atomic_store_explicit(&physical_targets, word, memory_order_release);
    poke();
    if (count) { atomic_store(&recovery_target_ns, 0); atomic_store(&request_recovery, false); }
}
void peak_global_budget_startup(double seconds)
{
    if (getpid() != atomic_load(&owner) || rank_id != 0 || !number(seconds)) return;
    double old = atomic_load_explicit(&startup_charge, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 2 && old < seconds; ++attempt)
        if (atomic_compare_exchange_strong_explicit(&startup_charge, &old, seconds,
                memory_order_release, memory_order_relaxed)) break;
    /* A failed bounded maximum update must never seal an older fee. */
    if (atomic_load_explicit(&startup_charge, memory_order_acquire) >= seconds)
        atomic_store_explicit(&startup_sealed, true, memory_order_release);
    poke();
}
void peak_global_budget_origin(double seconds)
{
    if (rank_id == 0 && number(seconds) && seconds <= 3600) { atomic_store(&earned_origin, seconds); poke(); }
}
void peak_global_budget_return_idle(void)
{
    if (getpid() != atomic_load(&owner)) return;
    int64_t balance = atomic_load_explicit(&available_ns, memory_order_acquire);
    for (unsigned i = 0; i < 2 && balance > 0; ++i)
        if (atomic_compare_exchange_weak_explicit(&available_ns, &balance, 0,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add(&debit_ns, (uint64_t)balance);
            atomic_fetch_add(&returned_ns, (uint64_t)balance); break;
        }
    atomic_store(&pressure_notice, false); atomic_store(&requested_ns, 0); atomic_store(&minimum_target_ns,0); atomic_store(&admission_target_ns,0); atomic_store(&admission_cash_ns,0); atomic_store(&admission_closing_ns,0);
    atomic_store(&recovery_target_ns, 0); atomic_store(&request_recovery, false); atomic_store(&request_metadata,false);
    atomic_store(&request_started_ns, 0); atomic_store(&public_deadline, 0); poke();
}
void peak_global_budget_retire(void)
{
    if (getpid() != atomic_load(&owner) || !atomic_load(&running)) return;
    atomic_store_explicit(&local_retiring, true, memory_order_release);
    pthread_mutex_lock(&metadata_mutex);
    metadata_close(7,0);
    if (!atomic_load(&metadata_inflight)) {
        atomic_store(&metadata_tokens, 0);
        atomic_store(&metadata_block_credit, 0);
        atomic_store_explicit(&settled_frontend_ns, atomic_load(&consumed_frontend_ns), memory_order_release);
    }
    pthread_mutex_unlock(&metadata_mutex);
    pthread_mutex_lock(&sample_mutex);
    local_sample.retiring = true;
    pthread_mutex_unlock(&sample_mutex);
    peak_global_budget_return_idle(); poke();
}
void peak_global_budget_finalizer(clockid_t clock, uint64_t start, double baseline)
{
    if (getpid() != atomic_load(&owner)) return;
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong(&finalizer_state, &expected, 1)) return;
    if (!number(baseline)) { atomic_store(&public_accounting_invalid, true); atomic_store(&finalizer_state, 3); poke(); return; }
    atomic_store(&finalizer_clock, clock); atomic_store(&finalizer_start, start);
    atomic_store(&finalizer_baseline, baseline);
    atomic_store_explicit(&finalizer_state, 2, memory_order_release); poke();
}
void peak_global_budget_complete(uint64_t wave)
{
    if (getpid() != atomic_load(&owner)) return;
    uint64_t old = atomic_load_explicit(&local_completed_wave, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 2 && old < wave; ++attempt)
        if (atomic_compare_exchange_weak_explicit(&local_completed_wave, &old, wave,
                memory_order_release, memory_order_relaxed)) { poke(); break; }
}
static void pressure_request(bool metadata)
{
    if (getpid() == atomic_load(&owner) && !atomic_load(&local_retiring) &&
        !atomic_exchange_explicit(&pressure_notice, true, memory_order_acq_rel))
        { atomic_store(&request_recovery, !effective_targets(atomic_load(&local_attached)));
          atomic_store(&request_metadata,metadata);
          uint64_t now = ns_now(CLOCK_MONOTONIC); atomic_store(&request_started_ns, now);
          atomic_store(&public_deadline, now + UINT64_C(100000000));
          atomic_fetch_add_explicit(&demand_generation, 1, memory_order_release);
          atomic_store_explicit(&pressure_notice, true, memory_order_release); poke(); }
}
void peak_global_budget_pressure(void) { pressure_request(false); }
static void request_credit_internal(uint64_t amount, bool metadata, bool actual)
{
    if (getpid() != atomic_load(&owner) || atomic_load(&local_retiring)) return;
    uint64_t debit = atomic_load(&debit_ns);
    if (!metadata && !effective_targets(atomic_load(&local_attached))) {
        uint64_t target = atomic_load(&recovery_target_ns);
        if (!target) {
            uint64_t zero = 0;
            (void)atomic_compare_exchange_strong(&recovery_target_ns, &zero, amount);
            target = atomic_load(&recovery_target_ns);
        }
        if (target) {
            int64_t balance = atomic_load_explicit(&available_ns, memory_order_acquire);
            if (balance >= 0 && (uint64_t)balance >= target && atomic_load(&public_local_closing)>0) return;
            if (atomic_load(&request_recovery) && !atomic_load(&request_metadata) && atomic_load(&pressure_notice) &&
                atomic_load(&public_demand_ack) < atomic_load(&demand_generation)) return;
            amount = target;
        }
    }
    uint64_t required=actual?amount:0;
    /* A physical admission can supersede metadata demand, but cannot erase
     * the current measured metadata block plus profile operating headroom. */
    if (!metadata && atomic_load(&metadata_enabled)) {
        uint64_t forecast=atomic_load(&metadata_estimate);
        if (forecast && forecast<=MAX_SPEND_NS/64) {
            uint64_t quote=forecast*64,low=atomic_load(&low_water_ns);
            uint64_t needed=low>MAX_SPEND_NS-quote?MAX_SPEND_NS:quote+low;
            if (needed>amount) amount=needed;
        }
    }
    if (amount > UINT64_MAX - debit) amount = UINT64_MAX; else amount += debit;
    uint64_t request = atomic_load(&requested_ns);
    bool changed = false;
    for (unsigned attempt = 0; attempt < 2 && request < amount; ++attempt)
        if (atomic_compare_exchange_weak(&requested_ns, &request, amount)) { changed = true; break; }
    uint64_t minimum=required>UINT64_MAX-debit?UINT64_MAX:debit+required;
    uint64_t previous_minimum=atomic_load(&minimum_target_ns);
    bool minimum_changed=false;
    for(unsigned attempt=0;required && attempt<2 && previous_minimum<minimum;++attempt)
        if(atomic_compare_exchange_strong(&minimum_target_ns,&previous_minimum,minimum)) {
            minimum_changed=true;break;
        }
    bool prior = atomic_load_explicit(&pressure_notice, memory_order_acquire);
    bool supersede=!metadata && atomic_load(&request_metadata);
    pressure_request(metadata);
    bool missing_closing = atomic_load(&public_local_closing) <= 0 &&
        atomic_load(&public_demand_ack) >= atomic_load(&demand_generation);
    if (prior && (changed || minimum_changed || missing_closing || supersede)) {
        atomic_store(&request_recovery, !effective_targets(atomic_load(&local_attached)));
        atomic_store(&request_metadata,metadata);
        /* A larger target is a new demand, so an ACK for the smaller target
         * cannot certify it. Successful callbacks coalesce while pending. */
        uint64_t now = ns_now(CLOCK_MONOTONIC);
        atomic_store(&request_started_ns, now);
        atomic_store(&public_deadline, now + UINT64_C(100000000));
        atomic_fetch_add_explicit(&demand_generation, 1, memory_order_release);
        atomic_store_explicit(&pressure_notice, true, memory_order_release); poke();
    }
}
void peak_global_budget_request_credit(uint64_t amount) { request_credit_internal(amount,false,true); }
static void request_admission(uint64_t cash, uint64_t closing, bool replace)
{
    if (getpid()!=atomic_load(&owner) || atomic_load(&local_retiring)) return;
    uint64_t debit=atomic_load_explicit(&debit_ns,memory_order_acquire);
    uint64_t target=cash>UINT64_MAX-debit?UINT64_MAX:debit+cash;
    uint64_t old=atomic_load(&admission_target_ns);
    bool changed=false;
    if (replace) {
        changed=atomic_exchange(&admission_target_ns,cash?target:0)!=(cash?target:0);
        changed|=atomic_exchange(&admission_cash_ns,cash)!=cash;
        changed|=atomic_exchange(&admission_closing_ns,closing)!=closing;
        atomic_store(&recovery_target_ns,cash);
    } else {
        for(unsigned attempt=0;attempt<2 && old<target;++attempt)
            if(atomic_compare_exchange_strong(&admission_target_ns,&old,target)) { changed=true; break; }
        old=atomic_load(&admission_closing_ns);
        for(unsigned attempt=0;attempt<2 && old<closing;++attempt)
            if(atomic_compare_exchange_strong(&admission_closing_ns,&old,closing)) { changed=true; break; }
    }
    bool missing_closing=cash && (double)closing/1e9>atomic_load(&public_local_closing);
    if (!changed && !(missing_closing && atomic_load(&public_demand_ack)>=atomic_load(&demand_generation))) return;
    atomic_store(&request_recovery,!effective_targets(atomic_load(&local_attached)));
    atomic_store(&request_metadata,false);
    uint64_t now=ns_now(CLOCK_MONOTONIC);
    atomic_store(&request_started_ns,now);
    atomic_store(&public_deadline,now+UINT64_C(100000000));
    uint64_t generation=atomic_fetch_add_explicit(&demand_generation,1,memory_order_release)+1;
    bool pending=atomic_load_explicit(&requested_ns,memory_order_acquire)>debit || current_minimum(debit)>0;
    atomic_store_explicit(&pressure_notice,pending,memory_order_release);
    /* Cancellation cannot erase a callback/metadata request published while
     * its class was being cleared. A later producer sees false and wakes it. */
    if (!pending && (atomic_load_explicit(&demand_generation,memory_order_acquire)!=generation ||
        atomic_load_explicit(&requested_ns,memory_order_acquire)>debit || current_minimum(debit)>0))
        atomic_store_explicit(&pressure_notice,true,memory_order_release);
    poke();
}
void peak_global_budget_request_admission(uint64_t cash, uint64_t closing)
{
    request_admission(cash,closing,true);
}
void peak_global_budget_ensure_admission(uint64_t cash, uint64_t closing)
{
    request_admission(cash,closing,false);
}
static bool try_spend(uint64_t amount, bool control)
{
    if (atomic_load(&local_retiring) || !peak_global_budget_active()) return false;
    if (amount > MAX_SPEND_NS) { atomic_fetch_add(&rejected_spend_count, 1); return false; }
    int64_t seen = atomic_load_explicit(&available_ns, memory_order_acquire);
    if (seen < 0 || (uint64_t)seen < amount) {
        if(control) request_admission(amount,0,false); else peak_global_budget_request_credit(amount);
        return false;
    }
    int64_t before = atomic_fetch_sub_explicit(&available_ns, (int64_t)amount, memory_order_acq_rel);
    if (before < 0 || (uint64_t)before < amount) {
        /* Only true depletion can enter the temporary negative marker. The
         * failed claim never enters the monotonic committed ledger. */
        atomic_fetch_add_explicit(&available_ns, (int64_t)amount, memory_order_release);
        if(control) request_admission(amount,0,false); else peak_global_budget_request_credit(amount);
        return false;
    }
    atomic_fetch_add_explicit(&debit_ns, amount, memory_order_release);
    atomic_fetch_add_explicit(control ? &consumed_control_ns : &consumed_ns, amount, memory_order_release);
    uint64_t remaining = (uint64_t)before - amount, water = atomic_load(&low_water_ns);
    if (remaining <= water && !atomic_load(&pressure_notice))
        request_credit_internal(water > UINT64_MAX / 2 ? water : water * 2,false,false);
    else if (atomic_load(&public_demand_ack) >= atomic_load(&demand_generation))
        atomic_store_explicit(&pressure_notice, false, memory_order_release);
    return true;
}
bool peak_global_budget_try_spend(uint64_t amount) { return try_spend(amount, false); }
bool peak_global_budget_try_spend_control(uint64_t amount) { return try_spend(amount, true); }
uint64_t peak_global_budget_control_consumed_ns(void)
{
    return getpid() == atomic_load(&owner) ? atomic_load_explicit(&consumed_control_ns, memory_order_acquire) : 0;
}
bool peak_global_budget_flush(unsigned timeout_ms)
{
    if (getpid() != atomic_load(&owner) || !atomic_load(&running)) return false;
    if (timeout_ms > 100) timeout_ms = 100;
    pthread_mutex_lock(&sample_mutex);
    uint64_t sequence = local_sample.sequence, completed = local_sample.completed_wave;
    request_reply_goal_locked(sequence);
    pthread_mutex_unlock(&sample_mutex);
    uint64_t ack = atomic_load(&local_completed_wave); if (ack > completed) completed = ack;
    poke();
    uint64_t started = ns_now(CLOCK_MONOTONIC), previous = started;
    if (!started) return false;
    /* The independent iteration bound also covers a stuck clock. */
    for (unsigned waits = 0; waits <= timeout_ms; ++waits) {
        uint64_t now = ns_now(CLOCK_MONOTONIC);
        if (!now || now < previous) return false;
        previous = now;
        PeakGlobalBudgetView v = peak_global_budget_view();
        if (v.wave > completed) completed = v.wave;
        if (v.acknowledged_sequence >= sequence && v.root_completed_wave >= completed) return true;
        /* A peer can have finished normally while this rank's final frame
         * is still in flight. Inactive forbids spending, not authenticated
         * settlement: wait for our exact revision and the all-rank ACK. */
        if (now - started >= (uint64_t)timeout_ms * 1000000 || waits == timeout_ms) break;
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
    return false;
}
bool peak_global_budget_final_flush(unsigned timeout_ms)
{
    if (getpid() != atomic_load(&owner) || !atomic_load(&running)) return false;
    if (timeout_ms > 2000) timeout_ms = 2000;
    pthread_mutex_lock(&sample_mutex);
    uint64_t sequence = local_sample.sequence;
    bool final = local_sample.final && local_sample.retiring;
    if (final) request_reply_goal_locked(sequence);
    pthread_mutex_unlock(&sample_mutex);
    if (!final) return false;
    poke();
    uint64_t started = ns_now(CLOCK_MONOTONIC), previous = started;
    if (!started) return false;
    /* The independent iteration bound also covers a stuck clock. */
    for (unsigned waits = 0; waits <= timeout_ms; ++waits) {
        uint64_t now = ns_now(CLOCK_MONOTONIC);
        if (!now || now < previous) return false;
        previous = now;
        PeakGlobalBudgetView v = peak_global_budget_view();
        if (v.accounting_invalid) return false;
        bool root_written = rank_id || (native_client ? v.broker_final_written :
            atomic_load_explicit(&root_final_sent, memory_order_acquire));
        if (v.acknowledged_sequence >= sequence && v.cohort_finalized && root_written)
            return true;
        if (now - started >= (uint64_t)timeout_ms * 1000000 || waits == timeout_ms) break;
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
    }
    return false;
}
void peak_global_budget_stop(void)
{
    if (getpid() != atomic_load(&owner)) { close_fd(&client); close_fd(&listener); close_fd(&wake_write_fd); close_fd(&wake_fd); return; }
    if (atomic_exchange_explicit(&running, false, memory_order_acq_rel)) {
        /* Stop must be visible before the wake. Otherwise a same-CPU helper
         * can consume the wake, reenter poll while running is still true,
         * and leave join waiting for the entire 100ms normal poll timeout.
         * poke() cannot be used here because it deliberately ignores stopped
         * sessions. The nonblocking stop wake bypasses that running guard. */
        if (wake_write_fd >= 0) {
            uint64_t value = 1;
            (void)write(wake_write_fd, &value, sizeof(value));
        }
        pthread_join(worker, NULL);
        unsigned capacity = sizeof(events) / sizeof(events[0]);
        for (unsigned i = event_count > capacity ? event_count - capacity : 0; i < event_count; ++i) {
            BudgetEvent *e = &events[i % capacity];
            fprintf(stderr, "[peak] global event: ns=%llu kind=%c rank=%u demand=%llu wave=%llu amount=%.17g\n",
                (unsigned long long)e->time, e->kind, e->rank,
                (unsigned long long)e->generation, (unsigned long long)e->wave, e->amount);
        }
    }
    unsigned close_reason=atomic_load_explicit(&metadata_close_reason,memory_order_acquire);
    static const char *const close_names[]={"open","resource","no_quote","cash","clock","overrun","global_off","retire","unsupported"};
    if (close_reason)
        fprintf(stderr, "[peak] MPI metadata gate: reason=%u reason_name=%s footer_quote_ns=%llu footer_p50_ns=%llu footer_p95_ns=%llu footer_max_ns=%llu elapsed_ns=%llu seed_remaining=%u wallet_ns=%lld quote_ns=%llu last_spent_seconds=%.17g last_reserved_seconds=%.17g last_closing_seconds=%.17g frontend_seconds=%.17g frontend_debit_ns=%llu settled_frontend_credit_ns=%llu\n",
            close_reason,close_reason<sizeof(close_names)/sizeof(close_names[0])?close_names[close_reason]:"invalid",
            (unsigned long long)metadata_footer_quote,
            (unsigned long long)metadata_footer_median,(unsigned long long)metadata_footer_p95,(unsigned long long)metadata_footer_max,
            (unsigned long long)(metadata_closed_ns>=metadata_started_ns ? metadata_closed_ns-metadata_started_ns : 0),
            metadata_close_seed,(long long)metadata_close_wallet,(unsigned long long)metadata_close_quote,
            metadata_close_spent,metadata_close_reserved,metadata_close_closing,
            (double)atomic_load(&frontend_cpu_ns)/1e9,
            (unsigned long long)atomic_load(&consumed_frontend_ns),
            (unsigned long long)atomic_load(&settled_frontend_ns));
    metadata_clear();
    close_fd(&client); close_fd(&listener); close_fd(&wake_write_fd); close_fd(&wake_fd);
#ifdef PEAK_GLOBAL_BUDGET_NATIVE
    if (broker_pid > 0) {
        PeakGlobalBudgetView final = peak_global_budget_view();
        pid_t old = broker_pid;
        bool reaped = peak_global_budget_reap(old);
        broker_pid = -1;
        fprintf(stderr, "[peak] native global broker: pid=%ld cpu_seconds=%.9g "
                        "all_final_replies_written=%d reap_confirmed=%d\n",
                (long)old, final.broker_cpu_seconds, final.broker_final_written, reaped);
    }
#endif
}
int peak_global_budget_broker_main(int argc, char **argv)
{
    /* This trusted, non-preloaded helper closes application descriptors before
     * readiness or protocol processing. Do not run application atfork handlers
     * after MPI has initialized a transport such as libfabric/EFA. */
#if defined(__linux__) && defined(SYS_close_range)
    if (peak_exec_raw_syscall6(SYS_close_range, 5u, ~0u, 0, 0, 0, 0) < 0)
#endif
    {
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 5 || maxfd > 1048576)
            maxfd = 1048576;
        for (int fd = 5; fd < maxfd; fd++)
            close(fd);
    }
    if (argc != 9)
        return 2;
    char *end = NULL;
    unsigned long long values[5];
    const unsigned indexes[5] = {2, 3, 4, 6, 7};
    for (unsigned i = 0; i < 5; i++) {
        errno = 0;
        values[i] = strtoull(argv[indexes[i]], &end, 10);
        if (errno || !end || *end)
            return 2;
    }
    errno = 0;
    uint64_t bits = strtoull(argv[5], &end, 10);
    double target;
    memcpy(&target, &bits, sizeof target);
    if (errno || !end || *end || !number(target) || !values[2] || values[2] > INT_MAX || !values[3] ||
        values[0] != 3 || values[1] != 4 || values[4] != (unsigned long long)getppid())
        return 2;
    broker_owner_pid = (pid_t)values[4];
    listener = 3;
    world = (unsigned)values[2];
    job_nonce = values[3];
    ratio = target;
    rank_id = 0;
    broker_only = true;
    atomic_store(&owner, getpid());
    ServiceStorage *storage = service_storage_new(world);
    if (!storage) return 2;
    atomic_store(&running, true);
    trace_enabled = !strcmp(argv[8], "1");
    unsigned char ready = 0xa7;
    if (write(4, &ready, 1) != 1) {
        service_storage_free(storage);
        return 2;
    }
    close(4);
    service(storage);
    atomic_store(&running, false);
    close_fd(&listener);
    for (unsigned i = event_count > 256 ? event_count - 256 : 0; i < event_count; i++) {
        BudgetEvent *e = &events[i % 256];
        fprintf(stderr,
                "[peak] global event: ns=%llu kind=%c rank=%u demand=%llu wave=%llu amount=%.17g\n",
                (unsigned long long)e->time, e->kind, e->rank, (unsigned long long)e->generation,
                (unsigned long long)e->wave, e->amount);
    }
    return 0;
}
