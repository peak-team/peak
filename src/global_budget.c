#define _GNU_SOURCE
#include "internal/global_budget.h"
#if !defined(__linux__) || UINTPTR_MAX != UINT64_MAX
int peak_global_budget_listen(unsigned short *p) { (void)p; return -1; }
uint64_t peak_global_budget_nonce(void) { return 0; }
void peak_global_budget_set_nonce(uint64_t n) { (void)n; }
int peak_global_budget_start(const char *h,unsigned short p,unsigned r,unsigned s,double t,void(*w)(void)) { (void)h;(void)p;(void)r;(void)s;(void)t;(void)w;return -1; }
void peak_global_budget_publish(const PeakGlobalBudgetSample *s) { (void)s; }
PeakGlobalBudgetView peak_global_budget_view(void) { return (PeakGlobalBudgetView){0}; }
bool peak_global_budget_try_spend(uint64_t n) { (void)n;return false; }
void peak_global_budget_pressure(void) {}
void peak_global_budget_complete(uint64_t n) { (void)n; }
void peak_global_budget_startup(double s) { (void)s; }
void peak_global_budget_request_credit(uint64_t n) { (void)n; }
bool peak_global_budget_flush(unsigned n) { (void)n;return false; }
void peak_global_budget_stop(void) {}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/* Version one is a homogeneous-host protocol, confined to an MPI job. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "budget credits require lock-free 64-bit atomics");
_Static_assert(__atomic_always_lock_free(sizeof(double), 0), "budget views require lock-free doubles");
#define LIMIT 64
#define MAGIC UINT32_C(0x50474231)
#define FRESH_NS UINT64_C(1000000000)
#ifndef PEAK_GLOBAL_BUDGET_IO_CHUNK
#define PEAK_GLOBAL_BUDGET_IO_CHUNK 65536
#endif
#define IO_CHUNK PEAK_GLOBAL_BUDGET_IO_CHUNK
typedef struct {
    uint32_t magic, version, rank, size;
    uint64_t reply_sequence, consumed_ns, nonce, requested_ns, demand_generation, message_sequence;
    bool request_credit;
    PeakGlobalBudgetSample sample;
    PeakGlobalBudgetView view;
} Frame;
typedef struct {
    int fd, rank;
    Frame incoming, outgoing;
    size_t received, sent;
    bool pending;
    uint64_t last_activity, last_send;
} Peer;
typedef struct {
    PeakGlobalBudgetSample latest;
    bool seen, connected, wants_credit;
    double reservation, previous_cost, granted_total;
    uint64_t consumed_ns, requested_ns, demand_generation, actioned_generation, message_sequence;
    uint64_t last_update;
} Rank;
static int listener = -1, client = -1, wake_fd = -1;
static unsigned rank_id, world;
static uint64_t job_nonce;
uint64_t peak_global_budget_nonce(void) { return job_nonce; }
void peak_global_budget_set_nonce(uint64_t n) { job_nonce = n; }
static double ratio;
static _Atomic pid_t owner;
static pthread_t worker;
static atomic_bool running, pressure_notice, flush_requested;
static pthread_mutex_t sample_mutex = PTHREAD_MUTEX_INITIALIZER;
static PeakGlobalBudgetSample local_sample;
static void (*notify)(void);
static atomic_uint view_sequence;
/* Atomic fields avoid the data race of a seqlock around non-atomic payload. */
static atomic_bool public_active;
static atomic_uint_fast64_t local_completed_wave, public_wave, public_decision, public_completed, public_ack, public_timestamp, granted_ns, consumed_ns, requested_ns, demand_generation;
static _Atomic double public_spent, public_remaining, public_allowance, public_span, public_granted, public_budget, public_reserved, public_control_reserve, startup_charge, local_transport_cpu;
extern void pthread_listener_exclude_current_thread(void) __attribute__((weak));
extern void pthread_listener_mark_next_created_thread_helper(void) __attribute__((weak));
static uint64_t ns_now(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t)) return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static double seconds_now(void) { return (double)ns_now(CLOCK_MONOTONIC) / 1e9; }
static void poke(void)
{
    if (!atomic_load_explicit(&running, memory_order_acquire) || wake_fd < 0) return;
    uint64_t value = 1;
    (void)write(wake_fd, &value, sizeof(value)); /* EFD_NONBLOCK: EAGAIN is harmless. */
}
static void close_fd(int *fd) { if (*fd >= 0) close(*fd); *fd = -1; }
static bool number(double n) { return isfinite(n) && n >= 0; }
static double cost(const PeakGlobalBudgetSample *s)
{
    return s->profile_seconds + s->control_seconds + s->management_seconds + s->transport_seconds;
}
static bool valid_sample(const PeakGlobalBudgetSample *s, const Rank *r)
{
    if (!s->sequence || !number(s->profile_seconds) || !number(s->control_seconds) ||
        !number(s->management_seconds) || !number(s->transport_seconds) || !number(s->elapsed_seconds) ||
        !number(s->pressure) || !number(s->unfunded_seconds) || s->unfunded_seconds > .000500001 || !number(cost(s))) return false;
    if (!r->seen) return true;
    return s->profile_seconds >= r->latest.profile_seconds &&
        s->control_seconds >= r->latest.control_seconds &&
        s->management_seconds >= r->latest.management_seconds &&
        s->transport_seconds >= r->latest.transport_seconds &&
        s->elapsed_seconds >= r->latest.elapsed_seconds &&
        s->completed_wave >= r->latest.completed_wave &&
        s->unfunded_seconds >= r->latest.unfunded_seconds;
}
static void publish_view(PeakGlobalBudgetView v)
{
    atomic_fetch_add_explicit(&view_sequence, 1, memory_order_acq_rel);
    atomic_store_explicit(&public_active, v.active, memory_order_relaxed);
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
    if (v.granted_total_seconds >= 0 && v.granted_total_seconds < 1e9) {
        uint64_t value = (uint64_t)(v.granted_total_seconds * 1e9);
        if (value > atomic_load(&granted_ns)) atomic_store(&granted_ns, value);
    }
    atomic_store_explicit(&public_timestamp, ns_now(CLOCK_MONOTONIC), memory_order_relaxed);
    atomic_fetch_add_explicit(&view_sequence, 1, memory_order_release);
}
PeakGlobalBudgetView peak_global_budget_view(void)
{
    PeakGlobalBudgetView v = {0};
    if (getpid() != atomic_load_explicit(&owner, memory_order_relaxed)) return v;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        unsigned before = atomic_load_explicit(&view_sequence, memory_order_acquire);
        if (before & 1) continue;
        v.active = atomic_load_explicit(&public_active, memory_order_relaxed);
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
        uint64_t stamp = atomic_load_explicit(&public_timestamp, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (before == atomic_load_explicit(&view_sequence, memory_order_acquire)) {
            uint64_t now = ns_now(CLOCK_MONOTONIC);
            if (!stamp || now < stamp || now - stamp > FRESH_NS) v.active = false;
            return v;
        }
    }
    return (PeakGlobalBudgetView){0};
}
int peak_global_budget_listen(unsigned short *port)
{
    if (!port || listener >= 0 || atomic_load(&running)) return -1;
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t length = sizeof(addr);
    if (bind(s, (void *)&addr, sizeof(addr)) || listen(s, LIMIT) ||
        getsockname(s, (void *)&addr, &length)) { close(s); return -1; }
    if (getrandom(&job_nonce, sizeof(job_nonce), 0) != sizeof(job_nonce) || !job_nonce) { close(s); return -1; }
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
        fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
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
/* Return -1 on peer loss; partial I/O is retained across poll iterations. */
static int read_frame(Peer *p)
{
    size_t count = sizeof(Frame) - p->received;
    if (count > IO_CHUNK) count = IO_CHUNK;
    ssize_t n = recv(p->fd, (char *)&p->incoming + p->received, count, 0);
    if (n > 0) { p->received += (size_t)n; p->last_activity = ns_now(CLOCK_MONOTONIC); }
    else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return -1;
    return p->received == sizeof(Frame) ? 1 : 0;
}
static int write_frame(Peer *p)
{
    if (!p->pending) return 0;
    size_t count = sizeof(Frame) - p->sent;
    if (count > IO_CHUNK) count = IO_CHUNK;
    ssize_t n = send(p->fd, (char *)&p->outgoing + p->sent, count, MSG_NOSIGNAL);
    if (n > 0) p->sent += (size_t)n;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    if (p->sent == sizeof(Frame)) { p->sent = 0; p->pending = false; }
    return 0;
}
static bool header(const Frame *f)
{
    return f->magic == MAGIC && f->version == 1 && f->nonce == job_nonce && f->rank < world && f->size == world;
}
static void disconnect(Peer *p, Rank *ranks)
{
    if (p->rank >= 0) ranks[p->rank].connected = false;
    close_fd(&p->fd);
}
static void *service(void *unused)
{
    (void)unused;
    uint64_t cpu_origin = ns_now(CLOCK_THREAD_CPUTIME_ID);
    if (pthread_listener_exclude_current_thread) pthread_listener_exclude_current_thread();
    Peer peers[LIMIT], local = {.fd = client, .rank = (int)rank_id};
    Rank ranks[LIMIT] = {0};
    for (unsigned i = 0; i < LIMIT; ++i) peers[i] = (Peer){.fd = -1, .rank = -1};
    double start = seconds_now(), transport_cpu = 0, span = 0, wave_start = 0;
    uint64_t wave = 0, reply_sequence = 0, received_reply = 0, last_wave = 0, completed_wave = 0, message_sequence = 0;
    bool wave_pending = false;
    unsigned allocation_cursor = 0;
    bool broken = false; double broken_deadline = 0;
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        uint64_t now_ns = ns_now(CLOCK_MONOTONIC);
        struct pollfd ready[LIMIT + 3];
        ready[0] = (struct pollfd){rank_id == 0 ? listener : -1, POLLIN, 0};
        ready[1] = (struct pollfd){local.fd, POLLIN | (local.pending ? POLLOUT : 0), 0};
        ready[2] = (struct pollfd){wake_fd, POLLIN, 0};
        for (unsigned i = 0; i < world; ++i)
            ready[i + 3] = (struct pollfd){rank_id == 0 ? peers[i].fd : -1, POLLIN | (peers[i].pending ? POLLOUT : 0), 0};
        (void)poll(ready, world + 3, broken ? 10 : 100);
        now_ns = ns_now(CLOCK_MONOTONIC);
        if (ready[2].revents & POLLIN) { uint64_t events; (void)read(wake_fd, &events, sizeof(events)); }
        if (rank_id == 0) {
            int s = ready[0].revents & POLLIN
                ? accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC) : -1;
            if (s >= 0) {
                int one = 1; (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                unsigned slot;
                for (slot = 0; slot < world; ++slot) if (peers[slot].fd < 0) {
                    peers[slot] = (Peer){.fd = s, .rank = -1, .last_activity = now_ns}; break;
                }
                if (slot == world) close(s);
            }
            for (unsigned i = 0; i < world; ++i)
                if (peers[i].fd >= 0 && now_ns > peers[i].last_activity && now_ns - peers[i].last_activity > UINT64_C(2000000000)) {
                    disconnect(&peers[i], ranks); broken = true;
                }
            for (unsigned i = 0; i < world; ++i) if (peers[i].fd >= 0 && (ready[i + 3].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                Peer *p = &peers[i];
                int result = read_frame(p);
                if (result < 0) { disconnect(p, ranks); broken = true; continue; }
                if (result != 1) continue;
                Frame f = p->incoming; p->received = 0;
                if (!header(&f) || f.sample.completed_wave > wave || (p->rank >= 0 && (unsigned)p->rank != f.rank) ||
                    (p->rank < 0 && ranks[f.rank].seen)) {
                    disconnect(p, ranks); broken = true; continue;
                }
                Rank *r = &ranks[f.rank];
                if (r->seen && f.message_sequence <= r->message_sequence) continue;
                if (!f.message_sequence || (r->seen && f.sample.sequence < r->latest.sequence)) {
                    disconnect(p, ranks); broken = true; continue;
                }
                r->message_sequence = f.message_sequence;
                if (f.demand_generation < r->demand_generation) { disconnect(p, ranks); broken = true; continue; }
                r->wants_credit = f.request_credit || (f.sample.completed_wave > 0 && !f.sample.attached_targets); r->requested_ns = f.requested_ns;
                r->demand_generation = f.demand_generation;
                if (!valid_sample(&f.sample, r)) { disconnect(p, ranks); broken = true; continue; }
                p->rank = (int)f.rank;
                if (f.consumed_ns < r->consumed_ns || (double)f.consumed_ns / 1e9 > r->granted_total + 1e-8) {
                    disconnect(p, ranks); broken = true; continue;
                }
                double charged = (double)(f.consumed_ns - r->consumed_ns) / 1e9;
                r->reservation = fmax(0, r->reservation - charged);
                r->consumed_ns = f.consumed_ns;
                double current = fmax(cost(&f.sample), (double)f.consumed_ns / 1e9);
                if (current > r->previous_cost) r->previous_cost = current;
                r->latest = f.sample; r->seen = r->connected = true; r->last_update = now_ns;
            }
            bool all = !broken, unfunded = false, done = wave != 0;
            double spent = atomic_load_explicit(&startup_charge, memory_order_relaxed), reserved = 0;
            for (unsigned i = 0; i < world; ++i) {
                Rank *r = &ranks[i];
                if (r->seen && now_ns - r->last_update > FRESH_NS) broken = true;
                all = all && r->seen && r->connected && now_ns - r->last_update <= FRESH_NS;
                unfunded = unfunded || (r->wants_credit && r->reservation < fmax(.0001, (double)r->requested_ns / 1e9));
                done = done && r->latest.completed_wave >= wave;
                spent += r->previous_cost; reserved += r->reservation;
            }
            /* Reserve one estimated 3ms control window per rank; never lend it. */
            double budget = ratio * (seconds_now() - start);
            double remaining = fmax(0, budget - spent - reserved - .0035 * world);
            if (wave_pending && done) { span = seconds_now() - wave_start; completed_wave = wave; wave_pending = false; }
            if (all && !wave_pending) {
                /* Spend only already earned global headroom, prioritizing
                 * demand; no timeout ever reclaims an unacknowledged grant. */
                for (unsigned k = 0; k < world && remaining > 0; ++k) {
                    unsigned i = (allocation_cursor + k) % world;
                    Rank *r = &ranks[i];
                    double desired = r->wants_credit ? fmax(r->latest.pressure * .02, fmax(.0001, (double)r->requested_ns / 1e9)) : 0;
                    double grant = fmin(remaining, fmax(0, desired - r->reservation));
                    r->reservation += grant; r->granted_total += grant; remaining -= grant;
                }
                allocation_cursor = (allocation_cursor + 1) % world;
                /* Exhausting the free pool by reservation is not an exhaustion
                 * event: stop only when a request remains unserved. */
                unfunded = false;
                for (unsigned i = 0; i < world; ++i)
                    unfunded |= ranks[i].wants_credit && ranks[i].latest.attached_targets > 0 &&
                        ranks[i].demand_generation > ranks[i].actioned_generation &&
                        ranks[i].reservation < fmax(.0001, (double)ranks[i].requested_ns / 1e9);
                bool attached = false;
                for (unsigned i = 0; i < world; ++i) attached |= ranks[i].latest.attached_targets > 0;
                if (unfunded && attached && remaining <= 1e-9 && wave != UINT64_MAX) {
                    ++wave; wave_start = seconds_now(); span = 0; wave_pending = true;
                    for (unsigned i = 0; i < world; ++i)
                        ranks[i].actioned_generation = ranks[i].demand_generation;
                }
            }

            reserved = 0;
            for (unsigned i = 0; i < world; ++i) reserved += ranks[i].reservation;
            for (unsigned i = 0; i < world; ++i) {
                Peer *p = &peers[i];
                if (p->fd < 0 || p->rank < 0 || p->pending) continue;
                unsigned id = (unsigned)p->rank;
                PeakGlobalBudgetView previous = p->outgoing.view;
                bool urgent = previous.active != all || previous.wave != wave ||
                    previous.root_completed_wave != completed_wave ||
                    previous.granted_total_seconds != ranks[id].granted_total ||
                    previous.acknowledged_sequence != ranks[id].latest.sequence;
                if (!urgent && now_ns - p->last_send < UINT64_C(100000000)) continue;
                p->last_send = now_ns;
                uint64_t decision = ++reply_sequence;
                p->outgoing = (Frame){.magic = MAGIC, .version = 1, .nonce = job_nonce, .rank = id,
                    .size = world, .reply_sequence = decision,
                    .view = {.active = all, .wave = wave, .decision_sequence = decision, .view_epoch = decision, .root_completed_wave = completed_wave,
                        .acknowledged_sequence = ranks[id].latest.sequence,
                        .budget_seconds = budget, .reserved_seconds = reserved, .control_reserve_seconds = .0035 * world, .global_spent_seconds = spent,
                        .remaining_seconds = remaining,
                        .allowance_seconds = ranks[id].reservation,
                        .wave_span_seconds = span, .granted_total_seconds = ranks[id].granted_total}};
                p->pending = true;
            }
            for (unsigned i = 0; i < world; ++i)
                if (peers[i].fd >= 0 && now_ns > peers[i].last_activity && now_ns - peers[i].last_activity > UINT64_C(2000000000)) {
                    disconnect(&peers[i], ranks); broken = true;
                }
            for (unsigned i = 0; i < world; ++i) if (peers[i].fd >= 0 && write_frame(&peers[i])) {
                disconnect(&peers[i], ranks); broken = true;
            }
        }
        if (!local.pending) {
            pthread_mutex_lock(&sample_mutex);
            Frame next = {.magic = MAGIC, .version = 1, .nonce = job_nonce, .rank = rank_id,
                .size = world, .sample = local_sample, .consumed_ns = atomic_load(&consumed_ns),
                .requested_ns = atomic_load(&requested_ns), .demand_generation = atomic_load(&demand_generation)};
            pthread_mutex_unlock(&sample_mutex);
            uint64_t ack = atomic_load_explicit(&local_completed_wave, memory_order_acquire);
            if (ack > next.sample.completed_wave) next.sample.completed_wave = ack;
            next.sample.transport_seconds = transport_cpu;
            next.request_credit = atomic_load(&pressure_notice);
            Frame *prior = &local.outgoing;
            bool urgent = atomic_exchange(&flush_requested, false) || next.requested_ns != prior->requested_ns ||
                next.demand_generation != prior->demand_generation || next.request_credit != prior->request_credit ||
                next.sample.completed_wave != prior->sample.completed_wave;
            if (next.sample.sequence && (urgent || now_ns - local.last_send >= UINT64_C(100000000))) {
                next.message_sequence = ++message_sequence;
                local.outgoing = next; local.pending = true; local.last_send = now_ns;
            }
        }
        if (local.fd >= 0 && write_frame(&local)) { close_fd(&local.fd); broken = true; }
        if (local.fd >= 0 && (ready[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            int result = read_frame(&local);
            if (result < 0) { close_fd(&local.fd); broken = true; }
            if (result == 1) {
                Frame f = local.incoming; local.received = 0;
                if (!header(&f) || f.rank != rank_id || !f.reply_sequence ||
                    !number(f.view.global_spent_seconds) || !number(f.view.remaining_seconds) ||
                    !number(f.view.allowance_seconds) || !number(f.view.wave_span_seconds) || !number(f.view.granted_total_seconds) || !number(f.view.budget_seconds) || !number(f.view.reserved_seconds) || !number(f.view.control_reserve_seconds)) {
                    close_fd(&local.fd); broken = true;
                } else if (f.reply_sequence > received_reply) {
                    received_reply = f.reply_sequence;
                    publish_view(f.view);
                    if (f.view.wave > last_wave) { last_wave = f.view.wave; if (notify) notify(); }
                }
            }
        }
        if (broken) {
            PeakGlobalBudgetView old = peak_global_budget_view(); old.active = false; publish_view(old);
            if (!broken_deadline) broken_deadline = seconds_now() + .05;
            if (seconds_now() >= broken_deadline) break;
        }
        uint64_t cpu_end = ns_now(CLOCK_THREAD_CPUTIME_ID);
        if (cpu_end >= cpu_origin) transport_cpu = (double)(cpu_end - cpu_origin) / 1e9;
        atomic_store_explicit(&local_transport_cpu, transport_cpu, memory_order_relaxed);

    }
    for (unsigned i = 0; i < world; ++i) close_fd(&peers[i].fd);
    close_fd(&local.fd); client = -1;
    PeakGlobalBudgetView old = peak_global_budget_view(); old.active = false; publish_view(old);
    return NULL;
}
int peak_global_budget_start(const char *host, unsigned short port, unsigned rank,
                            unsigned size, double target, void (*wake)(void))
{
    if (!job_nonce || !host || !port || !size || size > LIMIT || rank >= size || !number(target) ||
        atomic_load(&running) || (rank == 0 && listener < 0)) return -1;
    int fd = connect_host(host, port);
    if (fd < 0) return -1;
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0) { close(fd); return -1; }
    client = fd; rank_id = rank; world = size; ratio = target; notify = wake;
    atomic_store(&owner, getpid());
    atomic_store(&flush_requested, false); atomic_store(&local_transport_cpu, 0); atomic_store(&startup_charge, 0); atomic_store(&local_completed_wave, 0); atomic_store(&granted_ns, 0); atomic_store(&consumed_ns, 0); atomic_store(&pressure_notice, false); atomic_store(&requested_ns, 0); atomic_store(&demand_generation, 0);
    memset(&local_sample, 0, sizeof(local_sample)); publish_view((PeakGlobalBudgetView){0});
    atomic_store(&running, true);
    if (pthread_listener_mark_next_created_thread_helper) pthread_listener_mark_next_created_thread_helper();
    if (pthread_create(&worker, NULL, service, NULL)) {
        atomic_store(&running, false); close_fd(&client); close_fd(&wake_fd); return -1;
    }
    return 0;
}
void peak_global_budget_publish(const PeakGlobalBudgetSample *s)
{
    if (!s || getpid() != atomic_load(&owner) || !atomic_load(&running)) return;
    pthread_mutex_lock(&sample_mutex);
    bool first = !local_sample.sequence;
    if (s->sequence > local_sample.sequence) local_sample = *s;
    pthread_mutex_unlock(&sample_mutex);
    if (first) poke();
}
void peak_global_budget_startup(double seconds)
{
    if (getpid() != atomic_load(&owner) || rank_id != 0 || !number(seconds)) return;
    double old = atomic_load_explicit(&startup_charge, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 2 && old < seconds; ++attempt)
        if (atomic_compare_exchange_weak_explicit(&startup_charge, &old, seconds,
                memory_order_release, memory_order_relaxed)) break;
    poke();
}
void peak_global_budget_complete(uint64_t wave)
{
    if (getpid() != atomic_load(&owner)) return;
    uint64_t old = atomic_load_explicit(&local_completed_wave, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 2 && old < wave; ++attempt)
        if (atomic_compare_exchange_weak_explicit(&local_completed_wave, &old, wave,
                memory_order_release, memory_order_relaxed)) { poke(); break; }
}
void peak_global_budget_pressure(void)
{
    if (getpid() == atomic_load(&owner) &&
        !atomic_exchange_explicit(&pressure_notice, true, memory_order_acq_rel))
        { atomic_fetch_add_explicit(&demand_generation, 1, memory_order_release); poke(); }
}
void peak_global_budget_request_credit(uint64_t amount)
{
    if (getpid() != atomic_load(&owner)) return;
    uint64_t request = atomic_load(&requested_ns);
    bool changed = false;
    for (unsigned attempt = 0; attempt < 2 && request < amount; ++attempt)
        if (atomic_compare_exchange_weak(&requested_ns, &request, amount)) { changed = true; break; }
    bool prior = atomic_load_explicit(&pressure_notice, memory_order_acquire);
    peak_global_budget_pressure();
    if (prior && changed) poke();
}
bool peak_global_budget_try_spend(uint64_t amount)
{
    if (!peak_global_budget_view().active) return false;
    uint64_t spent = atomic_load_explicit(&consumed_ns, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        uint64_t granted = atomic_load_explicit(&granted_ns, memory_order_acquire);
        if (spent > granted || amount > granted - spent) {
            uint64_t request = atomic_load(&requested_ns);
            for (unsigned i = 0; i < 2 && request < amount; ++i)
                if (atomic_compare_exchange_weak(&requested_ns, &request, amount)) break;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&consumed_ns, &spent, spent + amount,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&pressure_notice, false, memory_order_release);
            return true;
        }
    }
    return false;
}
bool peak_global_budget_flush(unsigned timeout_ms)
{
    if (getpid() != atomic_load(&owner) || !atomic_load(&running)) return false;
    if (timeout_ms > 100) timeout_ms = 100;
    pthread_mutex_lock(&sample_mutex);
    uint64_t sequence = local_sample.sequence, completed = local_sample.completed_wave;
    pthread_mutex_unlock(&sample_mutex);
    uint64_t ack = atomic_load(&local_completed_wave); if (ack > completed) completed = ack;
    atomic_store(&flush_requested, true); poke();
    double deadline = seconds_now() + (double)timeout_ms / 1000;
    do {
        PeakGlobalBudgetView v = peak_global_budget_view();
        if (v.wave > completed) completed = v.wave;
        if (v.acknowledged_sequence >= sequence && v.root_completed_wave >= completed) return true;
        /* A peer can have finished normally while this rank's final frame
         * is still in flight. Inactive forbids spending, not authenticated
         * settlement: wait for our exact revision and the all-rank ACK. */
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    } while (seconds_now() < deadline);
    return false;
}
void peak_global_budget_stop(void)
{
    if (getpid() != atomic_load(&owner)) { close_fd(&client); close_fd(&listener); close_fd(&wake_fd); return; }
    if (atomic_load(&running)) {
        poke();
        if (atomic_exchange(&running, false)) pthread_join(worker, NULL);
    }
    close_fd(&client); close_fd(&listener); close_fd(&wake_fd);
}

#endif
