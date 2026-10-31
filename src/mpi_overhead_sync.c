#define _GNU_SOURCE
#include "internal/mpi_overhead_sync.h"
#include "internal/mpi_overhead_policy.h"
#include "pthread_listener.h"
#include "general_listener.h"

#include <mpi.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Telemetry only. HELLO negotiates identity/q; SAMPLE/READY carry no credits. */
enum { RECORD_SIZE = 96, WIRE_VERSION = 2 };
enum { HELLO = 1, HELLO_ACK, SAMPLE, READY };
enum { R_HELLO, R_COHORT, R_HELLO_REPLY, R_FIRST_SAMPLE, R_FIRST_WAIT,
       R_FIRST_REPLY, R_SAMPLE, R_REPLY };
enum { C_RETRY, C_CONNECT, C_HELLO, C_HELLO_WAIT, C_LOCAL_SAMPLE,
       C_FIRST_SEND, C_FIRST_WAIT, C_IDLE, C_SEND, C_REPLY };
enum { S_OFF, S_NO_ROUTE, S_HB_OFF, S_STARTING, S_WAITING, S_ACTIVE,
       S_RESOLVE_FAILED, S_UNAVAILABLE, S_NORMAL_END };
#define WIRE_MAGIC UINT32_C(0x504f5331)
#define CONNECT_NS UINT64_C(1000000000)
_Static_assert(sizeof(double) == 8, "telemetry wire requires an eight-byte double");

typedef struct {
    int fd, rank, phase;
    unsigned char input[RECORD_SIZE], output[RECORD_SIZE];
    size_t received, token_received, sent;
    bool output_token, prepared, bootstrap;
} Peer;

static pthread_mutex_t sample_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
static PeakMpiOverheadSample latest;
static PeakMpiOverheadPolicySample acknowledged;
static double acknowledged_ratio;
static bool acknowledged_valid;
/* Creation and stop claims are serialized by sample_lock. */
static bool stopping, creation_attempted;
static atomic_bool parameters_ready, creator_excluded;
static atomic_bool started, running, helper_exited, helper_created, startup_complete;
static atomic_bool accounting_valid = true;
static atomic_bool normal_end, cohort_ready, ever_ready, fallback_reported;
static atomic_int status;
static _Atomic double target_ratio, last_effective_input, negotiated_q;
static atomic_uint_fast64_t helper_cpu_ns, startup_cpu_ns, startup_wall_ns, bootstrap_cpu_ns;
static atomic_uint_fast64_t policy_updates, records_sent, records_received, last_ack_sequence;
static atomic_bool trace_diagnostics;
static bool startup_queries;
static pthread_t helper;
static int wake_pipe[2] = {-1, -1};
static int world_rank, world_size;
static double configured_q;
static char *root_host, *cohort_token;
static size_t token_length;
static unsigned short root_port;
static pid_t owner;
extern bool enable_global_heartbeat;
extern float global_target_ratio;
extern GumInterceptor* interceptor;

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static double module_seconds(void)
{
    return ((double)atomic_load(&startup_cpu_ns) + (double)atomic_load(&helper_cpu_ns)) / 1e9;
}
static void put32(unsigned char* p, uint32_t v)
{
    for (int i = 3; i >= 0; --i) { p[i] = (unsigned char)v; v >>= 8; }
}
static uint32_t get32(const unsigned char* p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
    return v;
}
static void put64(unsigned char* p, uint64_t v)
{
    for (int i = 7; i >= 0; --i) { p[i] = (unsigned char)v; v >>= 8; }
}
static uint64_t get64(const unsigned char* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static void put_double(unsigned char* p, double v)
{
    uint64_t bits; memcpy(&bits, &v, sizeof bits); put64(p, bits);
}
static double get_double(const unsigned char* p)
{
    uint64_t bits = get64(p); double v; memcpy(&v, &bits, sizeof v); return v;
}
static void encode(Peer* p, unsigned kind, int rank,
                   const PeakMpiOverheadPolicySample* s, double q, double ratio, bool token)
{
    memset(p->output, 0, sizeof p->output);
    put32(p->output, WIRE_MAGIC); put32(p->output + 4, WIRE_VERSION);
    put32(p->output + 8, kind); put32(p->output + 12, (uint32_t)rank);
    put32(p->output + 16, (uint32_t)world_size);
    put32(p->output + 20, token ? (uint32_t)token_length : 0);
    if (s) {
        put64(p->output + 24, s->sequence);
        put_double(p->output + 32, s->elapsed_seconds);
        put_double(p->output + 40, s->profile_seconds);
        put_double(p->output + 48, s->control_seconds);
        put_double(p->output + 56, s->management_seconds);
        put_double(p->output + 64, s->module_cpu_seconds);
    }
    put_double(p->output + 72, q); put_double(p->output + 80, ratio);
    p->sent = 0; p->output_token = token; p->prepared = true;
}
static bool header_valid(const Peer* p, unsigned kind, int rank)
{
    return get32(p->input) == WIRE_MAGIC && get32(p->input + 4) == WIRE_VERSION &&
        get32(p->input + 8) == kind && get32(p->input + 12) == (uint32_t)rank &&
        get32(p->input + 16) == (uint32_t)world_size;
}
static bool decode_sample(const Peer* p, PeakMpiOverheadPolicySample* s)
{
    s->sequence = get64(p->input + 24);
    s->elapsed_seconds = get_double(p->input + 32);
    s->profile_seconds = get_double(p->input + 40);
    s->control_seconds = get_double(p->input + 48);
    s->management_seconds = get_double(p->input + 56);
    s->module_cpu_seconds = get_double(p->input + 64);
    return s->sequence && isfinite(s->elapsed_seconds) && s->elapsed_seconds > 0 &&
        isfinite(s->profile_seconds) && s->profile_seconds >= 0 &&
        isfinite(s->control_seconds) && s->control_seconds >= 0 &&
        isfinite(s->management_seconds) && s->management_seconds >= 0 &&
        isfinite(s->module_cpu_seconds) && s->module_cpu_seconds >= 0;
}
static PeakMpiOverheadPolicySample local_snapshot(void)
{
    pthread_mutex_lock(&sample_lock);
    PeakMpiOverheadSample s = latest;
    pthread_mutex_unlock(&sample_lock);
    return (PeakMpiOverheadPolicySample){.sequence = s.sequence,
        .elapsed_seconds = s.elapsed_seconds, .profile_seconds = s.profile_seconds,
        .control_seconds = s.control_seconds, .management_seconds = s.management_seconds,
        .module_cpu_seconds = module_seconds()};
}
static void publish_ack(const PeakMpiOverheadPolicySample* s, double ratio)
{
    pthread_mutex_lock(&sample_lock);
    acknowledged = *s; acknowledged_ratio = ratio; acknowledged_valid = true;
    atomic_store(&target_ratio, ratio); atomic_store(&cohort_ready, true);
    atomic_store(&ever_ready, true);
    pthread_mutex_unlock(&sample_lock);
    if (trace_diagnostics) atomic_store(&last_ack_sequence, s->sequence);
}
static void invalidate_ack(void)
{
    pthread_mutex_lock(&sample_lock);
    acknowledged_valid = false; atomic_store(&cohort_ready, false);
    pthread_mutex_unlock(&sample_lock);
}
static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0), one = 1;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return true;
}
static void close_peer(Peer* p)
{
    if (p->fd >= 0) close(p->fd);
    *p = (Peer){.fd = -1, .rank = -1};
}
static void reset_input(Peer* p)
{
    p->received = p->token_received = 0;
}
/* 1 complete, 0 would block, -1 closed/invalid. Token comparison is complete,
 * streamed against the configured bytes, with no peer-sized allocation. */
static int read_frame(Peer* p)
{
    while (p->received < RECORD_SIZE) {
        ssize_t n = recv(p->fd, p->input + p->received, RECORD_SIZE - p->received, 0);
        if (n > 0) p->received += (size_t)n;
        else return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
    uint32_t length = get32(p->input + 20);
    unsigned kind = get32(p->input + 8);
    if (get32(p->input) != WIRE_MAGIC || get32(p->input + 4) != WIRE_VERSION ||
        ((kind == HELLO || kind == HELLO_ACK) ? length != token_length : length != 0)) return -1;
    while (p->token_received < length) {
        unsigned char part[256];
        size_t count = length - p->token_received;
        if (count > sizeof part) count = sizeof part;
        ssize_t n = recv(p->fd, part, count, 0);
        if (n > 0) {
            if (memcmp(part, cohort_token + p->token_received, (size_t)n)) return -1;
            p->token_received += (size_t)n;
        } else return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
    return 1;
}
static int write_frame(Peer* p)
{
    struct iovec iov[2]; size_t count = 0;
    size_t total = RECORD_SIZE + (p->output_token ? token_length : 0);
    if (p->sent < RECORD_SIZE)
        iov[count++] = (struct iovec){p->output + p->sent, RECORD_SIZE - p->sent};
    if (p->output_token) {
        size_t offset = p->sent > RECORD_SIZE ? p->sent - RECORD_SIZE : 0;
        if (offset < token_length)
            iov[count++] = (struct iovec){cohort_token + offset, token_length - offset};
    }
    struct msghdr message = {.msg_iov = iov, .msg_iovlen = count};
    ssize_t n = sendmsg(p->fd, &message, MSG_NOSIGNAL);
    if (n > 0) p->sent += (size_t)n;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    return p->sent == total ? 1 : 0;
}
static int poll_timeout(uint64_t now, uint64_t ready)
{
    if (now >= ready) return 0;
    uint64_t ms = (ready - now) / UINT64_C(1000000) + 1;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}
static bool parse_route(const char* endpoint, const char* token)
{
    const char *host = endpoint, *port;
    bool ipv6 = *host == '[';
    size_t length;
    if (*host == '[') {
        const char* end = strchr(++host, ']');
        if (!end || end[1] != ':') return false;
        length = (size_t)(end - host); port = end + 2;
    } else {
        port = strrchr(host, ':');
        if (!port || memchr(host, ':', (size_t)(port - host))) return false;
        length = (size_t)(port++ - host);
    }
    char* end; errno = 0;
    unsigned long value = strtoul(port, &end, 10);
    if (!length || !*port || *end || errno || value == 0 || value > 65535) return false;
    const char* report_port = getenv("PEAK_OUTPUT_AGGREGATION_PORT");
    if (report_port && strtoul(report_port, &end, 10) == value && !*end) return false;
    token_length = strlen(token);
    if (!token_length || token_length > UINT32_MAX) return false;
    root_host = strndup(host, length); cohort_token = strdup(token); root_port = (unsigned short)value;
    if (!root_host || !cohort_token) return false;
    struct in6_addr address;
    return inet_pton(ipv6 ? AF_INET6 : AF_INET, root_host, &address) == 1;
}
/* Numeric-only conversion cannot initiate hostname/service-name resolution. */
static struct addrinfo* resolve_route(void)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                            .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
    struct addrinfo* addresses = NULL;
    char port[8]; snprintf(port, sizeof port, "%u", root_port);
    return getaddrinfo(root_host, port, &hints, &addresses) == 0 ? addresses : NULL;
}
static int open_connection(const struct addrinfo* address, bool server)
{
    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0 || !nonblocking(fd)) { if (fd >= 0) close(fd); return -1; }
    if (server) {
        if (bind(fd, address->ai_addr, address->ai_addrlen) || listen(fd, SOMAXCONN)) { close(fd); return -1; }
    } else if (connect(fd, address->ai_addr, address->ai_addrlen) && errno != EINPROGRESS) {
        close(fd); return -1;
    }
    return fd;
}
static const char* status_name(int value)
{
    switch (value) {
    case S_NO_ROUTE: return "no_route";
    case S_HB_OFF: return "heartbeat_off";
    case S_STARTING: return "starting";
    case S_WAITING: return "waiting_for_cohort";
    case S_ACTIVE: return "active";
    case S_RESOLVE_FAILED: return "route_unavailable";
    case S_UNAVAILABLE: return "unavailable";
    case S_NORMAL_END: return "normal_end";
    default: return "off";
    }
}

static void* service(void* unused)
{
    (void)unused;
    uint64_t cpu_origin = clock_ns(CLOCK_THREAD_CPUTIME_ID), previous_cpu = cpu_origin, next = 0;
    pthread_listener_exclude_current_thread();
    bool failed = false, negotiated = false, active = false;
    int listener = -1;
    Peer client = {.fd = -1, .rank = world_rank, .phase = C_RETRY};
    if (!cpu_origin) atomic_store(&accounting_valid, false);
    struct addrinfo* addresses = cpu_origin ? resolve_route() : NULL;
    struct addrinfo* attempt = addresses;
    Peer* peers = NULL; int *rank_slot = NULL, *free_slots = NULL;
    double *requests = NULL, *ratios = NULL;
    bool* bootstrap_used = NULL;
    PeakMpiOverheadPolicySample* samples = NULL;
    PeakMpiOverheadPolicy* policy = NULL;
    struct pollfd* fds = calloc((size_t)world_size + 2, sizeof *fds);
    int free_count = world_size, hello_count = 1, sample_count = 0;
    bool local_first = false;
    uint64_t deadline = 0, last_sent = 0, last_completed = 0;
    if (world_rank == 0 && addresses) {
        for (struct addrinfo* a = addresses; a && listener < 0; a = a->ai_next)
            listener = open_connection(a, true);
        peers = calloc((size_t)world_size, sizeof *peers);
        rank_slot = malloc((size_t)world_size * sizeof *rank_slot);
        free_slots = malloc((size_t)world_size * sizeof *free_slots);
        requests = calloc((size_t)world_size, sizeof *requests);
        bootstrap_used = calloc((size_t)world_size, sizeof *bootstrap_used);
        ratios = calloc((size_t)world_size, sizeof *ratios);
        samples = calloc((size_t)world_size, sizeof *samples);
        policy = peak_mpi_overhead_policy_create((size_t)world_size);
        if (peers) for (int i = 0; i < world_size; ++i)
            peers[i].fd = peers[i].rank = -1;
        if (rank_slot && free_slots) for (int i = 0; i < world_size; ++i) {
            rank_slot[i] = -1; free_slots[i] = i;
        }
        if (requests) requests[0] = configured_q;
    }
    if (!cpu_origin || !addresses || !fds || (world_rank == 0 &&
        (listener < 0 || !peers || !rank_slot || !free_slots || !requests || !bootstrap_used || !ratios || !samples || !policy))) {
        failed = true; atomic_store(&status, addresses ? S_UNAVAILABLE : S_RESOLVE_FAILED);
    }
    while (!failed && atomic_load(&running) && !atomic_load(&startup_complete)) {
        struct pollfd wait = {.fd = wake_pipe[0], .events = POLLIN}; (void)poll(&wait, 1, 100);
    }
    uint64_t setup_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (cpu_origin && setup_end >= cpu_origin) {
        atomic_store(&bootstrap_cpu_ns, setup_end - cpu_origin);
        previous_cpu = setup_end;
    }
    bool attempted = false, connection_used = false;
    atomic_store(&status, failed ? atomic_load(&status) : S_WAITING);
    while (!failed && atomic_load(&running) && getpid() == owner) {
        uint64_t began = clock_ns(CLOCK_THREAD_CPUTIME_ID), now = clock_ns(CLOCK_MONOTONIC);
        if (!began || !now || began < cpu_origin) { atomic_store(&accounting_valid, false); failed = true; break; }
        atomic_store(&helper_cpu_ns, began - cpu_origin);
        bool due = now >= next, fixed_progress = false;
        if (world_rank != 0 && client.phase == C_RETRY && due) {
            fixed_progress = !attempted;
            attempted = true;
            client.fd = open_connection(attempt, false);
            attempt = attempt->ai_next ? attempt->ai_next : addresses;
            if (client.fd >= 0) { client.phase = C_CONNECT; deadline = now + CONNECT_NS; }
        }
        size_t count = 1;
        fds[0] = (struct pollfd){.fd = wake_pipe[0], .events = POLLIN};
        if (world_rank == 0) {
            fds[count++] = (struct pollfd){.fd = listener, .events = !negotiated && due ? POLLIN : 0};
            for (int i = 0; i < world_size; ++i) {
                Peer* p = &peers[i]; bool bootstrap = p->bootstrap && p->phase < R_SAMPLE;
                short events = 0;
                if (bootstrap || due) {
                    if (p->phase == R_HELLO || p->phase == R_FIRST_SAMPLE || p->phase == R_SAMPLE) events = POLLIN;
                    if (p->phase == R_HELLO_REPLY || p->phase == R_FIRST_REPLY || p->phase == R_REPLY) events = POLLOUT;
                }
                fds[count++] = (struct pollfd){.fd = p->fd, .events = events};
            }
        } else if (client.fd >= 0) {
            bool bootstrap = client.phase == C_CONNECT ? !connection_used :
                client.bootstrap && client.phase < C_IDLE;
            short events = 0;
            if (bootstrap || due) {
                if (client.phase == C_CONNECT || client.phase == C_HELLO ||
                    client.phase == C_FIRST_SEND || client.phase == C_SEND) events = POLLOUT;
                if (client.phase == C_HELLO_WAIT || client.phase == C_FIRST_WAIT || client.phase == C_REPLY) events = POLLIN;
            }
            fds[count++] = (struct pollfd){.fd = client.fd, .events = events};
        }
        /* Idle/retry cycles always honor measured pacing. Only actual first
         * protocol readiness can interrupt it; there is no bootstrap spin. */
        int timeout = due ? 100 : poll_timeout(now, next);
        if (world_rank && client.phase == C_CONNECT) {
            int connect_wait = poll_timeout(now, deadline);
            if (connect_wait < timeout) timeout = connect_wait;
        }
        int rc = poll(fds, (nfds_t)count, timeout);
        if (rc < 0 && errno != EINTR) { failed = true; break; }
        if (!atomic_load(&running)) break;
        now = clock_ns(CLOCK_MONOTONIC);
        if (!now) { atomic_store(&accounting_valid, false); failed = true; break; }
        if (world_rank == 0) {
            if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { failed = true; break; }
            if (fds[1].revents & POLLIN) {
                int fd;
                while ((fd = accept(listener, NULL, NULL)) >= 0) {
                    if (!free_count || !nonblocking(fd)) close(fd);
                    else { int slot = free_slots[--free_count]; peers[slot] = (Peer){.fd=fd,.rank=-1,.phase=R_HELLO}; }
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { failed = true; break; }
            }
            bool changed = false;
            for (int i = 0; i < world_size; ++i) {
                Peer* p = &peers[i]; short events = fds[i + 2].revents;
                if (p->fd < 0) continue;
                bool drop = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
                if (!drop && (events & POLLOUT) && p->prepared) {
                    size_t before = p->sent;
                    int result = write_frame(p);
                    if (p->bootstrap && p->phase < R_SAMPLE && p->sent > before) fixed_progress = true;
                    if (result < 0) drop = true;
                    else if (result > 0) {
                        if (trace_diagnostics) atomic_fetch_add(&records_sent, 1);
                        p->prepared = false; reset_input(p);
                        p->phase = p->phase == R_HELLO_REPLY ? R_FIRST_SAMPLE : R_SAMPLE;
                    }
                }
                if (!drop && (events & POLLIN)) {
                    int result = read_frame(p);
                    if (result < 0) drop = true;
                    else if (result > 0) {
                        if (p->phase == R_HELLO) {
                            int rank = (int)get32(p->input + 12); double q = get_double(p->input + 72);
                            if (rank <= 0 || rank >= world_size || !header_valid(p, HELLO, rank) ||
                                rank_slot[rank] >= 0 || !isfinite(q) || q <= 0) drop = true;
                            else {
                                p->rank = rank; rank_slot[rank] = i; requests[rank] = q;
                                p->bootstrap = !bootstrap_used[rank]; bootstrap_used[rank] = true;
                                ++hello_count; p->phase = R_COHORT;
                                fixed_progress |= p->bootstrap;
                            }
                        } else {
                            PeakMpiOverheadPolicySample s;
                            if (!header_valid(p, SAMPLE, p->rank) || !decode_sample(p, &s) ||
                                s.sequence <= samples[p->rank].sequence || get_double(p->input + 72) != atomic_load(&negotiated_q)) drop = true;
                            else {
                                samples[p->rank] = s; changed = true;
                                if (p->phase == R_FIRST_SAMPLE) { ++sample_count; p->phase = R_FIRST_WAIT; fixed_progress |= p->bootstrap; }
                                else p->phase = R_REPLY;
                            }
                        }
                        if (!drop && trace_diagnostics) atomic_fetch_add(&records_received, 1);
                    }
                }
                if (drop) {
                    if (p->rank >= 0) {
                        if (negotiated || active) { failed = true; break; }
                        rank_slot[p->rank] = -1; requests[p->rank] = 0; --hello_count;
                    }
                    close_peer(p); free_slots[free_count++] = i;
                }
            }
            if (failed) break;
            if (!negotiated && hello_count == world_size) {
                double q = requests[0];
                for (int i = 1; i < world_size; ++i) q = fmin(q, requests[i]);
                atomic_store(&negotiated_q, q); negotiated = true; fixed_progress = true;
                for (int i = 0; i < world_size; ++i) if (peers[i].fd >= 0 && peers[i].rank > 0 && peers[i].phase == R_COHORT) {
                    encode(&peers[i], HELLO_ACK, peers[i].rank, NULL, q, 0, true); peers[i].phase = R_HELLO_REPLY;
                }
            }
            if (negotiated && (due || !local_first)) {
                PeakMpiOverheadPolicySample s = local_snapshot();
                if (s.sequence && s.elapsed_seconds > 0 && s.sequence > samples[0].sequence) {
                    samples[0] = s; changed = true;
                    if (!local_first) { local_first = true; ++sample_count; fixed_progress = true; }
                }
            }
            if (changed && sample_count == world_size) {
                PeakMpiOverheadDecision decision;
                peak_mpi_overhead_policy_update(policy, samples, atomic_load(&negotiated_q), ratios, &decision);
                publish_ack(&samples[0], ratios[0]);
                fixed_progress |= !active;
                active = true; atomic_store(&status, S_ACTIVE);
                if (trace_diagnostics) atomic_fetch_add(&policy_updates, 1);
                for (int i = 0; i < world_size; ++i) if (peers[i].fd >= 0 &&
                    (peers[i].phase == R_FIRST_WAIT || peers[i].phase == R_REPLY) && !peers[i].prepared) {
                    encode(&peers[i], READY, peers[i].rank, &samples[peers[i].rank], atomic_load(&negotiated_q), ratios[peers[i].rank], false);
                    peers[i].phase = peers[i].phase == R_FIRST_WAIT ? R_FIRST_REPLY : R_REPLY;
                }
            }
        } else {
            short events = count > 1 ? fds[1].revents : 0;
            bool drop = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if (client.phase == C_CONNECT && now >= deadline) drop = true;
            if (!drop && client.phase == C_CONNECT && (events & POLLOUT)) {
                int error = 0; socklen_t size = sizeof error;
                if (getsockopt(client.fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) drop = true;
                else {
                    client.bootstrap = !connection_used; connection_used = true;
                    encode(&client, HELLO, world_rank, NULL, configured_q, 0, true);
                    client.phase = C_HELLO; fixed_progress |= client.bootstrap;
                }
            }
            if (!drop && (events & POLLOUT) && client.prepared && client.phase != C_CONNECT) {
                size_t before = client.sent;
                int result = write_frame(&client);
                if (client.bootstrap && client.phase < C_IDLE && client.sent > before) fixed_progress = true;
                if (result < 0) drop = true;
                else if (result > 0) {
                    if (trace_diagnostics) atomic_fetch_add(&records_sent, 1);
                    client.prepared = false; reset_input(&client);
                    client.phase = client.phase == C_HELLO ? C_HELLO_WAIT :
                        client.phase == C_FIRST_SEND ? C_FIRST_WAIT : C_REPLY;
                }
            }
            if (!drop && (events & POLLIN)) {
                int result = read_frame(&client);
                if (result < 0) drop = true;
                else if (result > 0) {
                    if (client.phase == C_HELLO_WAIT) {
                        double q = get_double(client.input + 72);
                        if (!header_valid(&client, HELLO_ACK, world_rank) || !isfinite(q) || q <= 0 || q > configured_q) drop = true;
                        else { atomic_store(&negotiated_q, q); client.phase = C_LOCAL_SAMPLE; fixed_progress |= client.bootstrap; }
                    } else {
                        PeakMpiOverheadPolicySample s; double ratio = get_double(client.input + 80);
                        if (!header_valid(&client, READY, world_rank) || !decode_sample(&client, &s) ||
                            s.sequence != last_sent || get_double(client.input + 72) != atomic_load(&negotiated_q) || !isfinite(ratio) || ratio < 0) drop = true;
                        else {
                            publish_ack(&s, ratio); fixed_progress |= !active && client.bootstrap;
                            active = true; atomic_store(&status, S_ACTIVE);
                            last_completed = s.sequence; client.phase = C_IDLE;
                            if (trace_diagnostics) atomic_fetch_add(&policy_updates, 1);
                        }
                    }
                    if (!drop && trace_diagnostics) atomic_fetch_add(&records_received, 1);
                }
            }
            if (drop) {
                if (active) { failed = true; break; }
                close_peer(&client); client.phase = C_RETRY; last_sent = 0;
            }
            if ((client.phase == C_LOCAL_SAMPLE && (due || client.bootstrap)) || (client.phase == C_IDLE && due)) {
                PeakMpiOverheadPolicySample s = local_snapshot();
                if (s.sequence && s.elapsed_seconds > 0 && s.sequence > last_completed) {
                    bool first = client.phase == C_LOCAL_SAMPLE;
                    fixed_progress |= first && client.bootstrap;
                    encode(&client, SAMPLE, world_rank, &s, atomic_load(&negotiated_q), 0, false);
                    last_sent = s.sequence; client.phase = first ? C_FIRST_SEND : C_SEND;
                }
            }
        }
        uint64_t ended = clock_ns(CLOCK_THREAD_CPUTIME_ID), wall = clock_ns(CLOCK_MONOTONIC);
        if (!ended || ended < previous_cpu || !wall) { atomic_store(&accounting_valid, false); failed = true; break; }
        atomic_store(&helper_cpu_ns, ended - cpu_origin);
        /* An idle wait before admission does not move the admission clock.
         * Its CPU remains in ended - previous_cpu for the next due/progress
         * round, and in the full helper CPU bill immediately. */
        if (!due && !fixed_progress) continue;
        double q = atomic_load(&negotiated_q); if (q <= 0) q = configured_q;
        double share = world_rank == 0 ? .05 * q : .05 * q / world_size;
        uint64_t cycle_cpu = ended - previous_cpu;
        if (fixed_progress) atomic_fetch_add(&bootstrap_cpu_ns, cycle_cpu);
        double pause = fixed_progress ? 0 : (double)cycle_cpu / share;
        previous_cpu = ended;
        next = pause >= (double)(UINT64_MAX - wall) ? UINT64_MAX : wall + (uint64_t)pause;
    }
    invalidate_ack();
    bool stopped = !atomic_load(&heartbeat_running);
    atomic_store(&normal_end, !failed || stopped);
    if (stopped) atomic_store(&status, S_NORMAL_END);
    else if (failed && atomic_load(&status) != S_RESOLVE_FAILED) atomic_store(&status, S_UNAVAILABLE);
    close_peer(&client);
    if (peers) for (int i = 0; i < world_size; ++i) close_peer(&peers[i]);
    if (listener >= 0) close(listener);
    if (addresses) freeaddrinfo(addresses);
    peak_mpi_overhead_policy_free(policy);
    free(peers); free(rank_slot); free(free_slots); free(requests); free(bootstrap_used); free(ratios); free(samples); free(fds);
    uint64_t ended = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (cpu_origin && ended >= cpu_origin) atomic_store(&helper_cpu_ns, ended - cpu_origin);
    atomic_store(&running, false);
    pthread_mutex_lock(&exit_lock); atomic_store(&helper_exited, true);
    pthread_cond_signal(&exit_cond); pthread_mutex_unlock(&exit_lock);
    return NULL;
}

static void stop_sync(void)
{
    if (getpid() != owner) return;
    pthread_mutex_lock(&sample_lock);
    stopping = true;
    atomic_store(&parameters_ready, false);
    bool created = atomic_exchange(&helper_created, false);
    atomic_store(&running, false);
    pthread_mutex_unlock(&sample_lock);
    if (!created) {
        if (wake_pipe[0] >= 0) close(wake_pipe[0]);
        if (wake_pipe[1] >= 0) close(wake_pipe[1]);
        wake_pipe[0] = wake_pipe[1] = -1;
        free(root_host); free(cohort_token); root_host = cohort_token = NULL;
    }
    if (created && wake_pipe[1] >= 0) { unsigned char byte = 1; (void)write(wake_pipe[1], &byte, 1); }
    struct timespec until;
    if (created && clock_gettime(CLOCK_REALTIME, &until) == 0) {
        until.tv_sec += 1;
        pthread_mutex_lock(&exit_lock);
        while (!atomic_load(&helper_exited))
            if (pthread_cond_timedwait(&exit_cond, &exit_lock, &until) == ETIMEDOUT) break;
        bool done = atomic_load(&helper_exited);
        pthread_mutex_unlock(&exit_lock);
        if (done) {
            (void)pthread_join(helper, NULL);
            close(wake_pipe[0]); close(wake_pipe[1]); wake_pipe[0] = wake_pipe[1] = -1;
            free(root_host); free(cohort_token); root_host = cohort_token = NULL;
        }
    }
    /* Live helper state is retained if cooperative stop reaches its bound.
     * No hostname lookup or MPI can run here. Normal performance is quiet. */
    if (trace_diagnostics)
        fprintf(stderr, "[peak] MPI overhead sync: rank=%d module_cpu_seconds=%.9g startup_wall_seconds=%.9g accounting_valid=%d helper_created=%d creator_excluded=%d startup_queries=%d helper_exited=%d normal_end=%d cohort_ready=%d ever_ready=%d bootstrap_cpu_seconds=%.9g fallback_reason=%s policy_or_reply_updates=%llu records_received=%llu records_sent=%llu final_proposed_ratio=%.9g configured_q=%.9g negotiated_q=%.9g acknowledged_sequence=%llu last_effective_input=%.9g\n",
            world_rank, module_seconds(), (double)atomic_load(&startup_wall_ns)/1e9,
            (int)atomic_load(&accounting_valid), created, (int)atomic_load(&creator_excluded), startup_queries,
            (int)atomic_load(&helper_exited), (int)atomic_load(&normal_end),
            (int)atomic_load(&cohort_ready), (int)atomic_load(&ever_ready),
            (double)atomic_load(&bootstrap_cpu_ns)/1e9, status_name(atomic_load(&status)),
            (unsigned long long)atomic_load(&policy_updates),
            (unsigned long long)atomic_load(&records_received), (unsigned long long)atomic_load(&records_sent),
            atomic_load(&target_ratio), configured_q, atomic_load(&negotiated_q),
            (unsigned long long)atomic_load(&last_ack_sequence), atomic_load(&last_effective_input));
}

void peak_mpi_overhead_sync_start(void)
{
    const char* trace = getenv("PEAK_MPI_OVERHEAD_SYNC_TRACE");
    trace_diagnostics = trace && strcmp(trace, "1") == 0;
    const char* disabled = getenv("PEAK_MPI_OVERHEAD_SYNC");
    const char* endpoint = getenv("PEAK_MPI_OVERHEAD_SYNC_ENDPOINT");
    const char* token = getenv("PEAK_MPI_OVERHEAD_SYNC_TOKEN");
    if ((disabled && strcmp(disabled, "0") == 0)) return;
    if (!enable_global_heartbeat || !atomic_load(&heartbeat_running) ||
        !isfinite(global_target_ratio) || global_target_ratio <= 0) { atomic_store(&status, S_HB_OFF); return; }
    if (!endpoint || !*endpoint || !token || !*token) { atomic_store(&status, S_NO_ROUTE); return; }
    if (atomic_exchange(&started, true)) return;
    atomic_store(&status, S_STARTING);
    peak_general_listener_fast_ignore_current_thread();
    GumInterceptor* startup_interceptor = interceptor;
    bool gum_ignored = startup_interceptor != NULL;
    if (gum_ignored) gum_interceptor_ignore_current_thread(startup_interceptor);
    uint64_t cpu_start = clock_ns(CLOCK_THREAD_CPUTIME_ID), wall_start = clock_ns(CLOCK_MONOTONIC);
    bool prepared = false;
    owner = getpid(); configured_q = global_target_ratio;
    if (!cpu_start || !wall_start) { atomic_store(&accounting_valid, false); goto finished; }
    if (PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
        PMPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS) goto finished;
    startup_queries = true;
    if (world_size <= 1) goto finished;
    if (!parse_route(endpoint, token)) { atomic_store(&status, S_NO_ROUTE); goto finished; }
    if (pipe(wake_pipe) || !nonblocking(wake_pipe[0]) || !nonblocking(wake_pipe[1])) goto finished;
    prepared = true;
finished:
    if (!prepared) {
        if (wake_pipe[0] >= 0) close(wake_pipe[0]);
        if (wake_pipe[1] >= 0) close(wake_pipe[1]);
        wake_pipe[0] = wake_pipe[1] = -1;
        free(root_host); free(cohort_token); root_host = cohort_token = NULL;
    }
    /* Publish only complete setup. An exit callback cannot cancel/free it
     * between registration and publication, or race a later HB creator. */
    pthread_mutex_lock(&sample_lock);
    bool exit_registered = atexit(stop_sync) == 0;
    uint64_t wall_end = clock_ns(CLOCK_MONOTONIC), cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (wall_start && wall_end >= wall_start) atomic_store(&startup_wall_ns, wall_end - wall_start);
    if (cpu_start && cpu_end >= cpu_start) atomic_store(&startup_cpu_ns, cpu_end - cpu_start);
    else { atomic_store(&accounting_valid, false); atomic_store(&running, false); }
    atomic_store_explicit(&startup_complete, true, memory_order_release);
    atomic_store(&parameters_ready, prepared && exit_registered && atomic_load(&accounting_valid));
    pthread_mutex_unlock(&sample_lock);
    if (prepared && !exit_registered) {
        close(wake_pipe[0]); close(wake_pipe[1]); wake_pipe[0] = wake_pipe[1] = -1;
        free(root_host); free(cohort_token); root_host = cohort_token = NULL;
    }
    if (gum_ignored) gum_interceptor_unignore_current_thread(startup_interceptor);
    peak_general_listener_fast_unignore_current_thread();
}

double peak_mpi_overhead_sync_update(const PeakMpiOverheadSample* sample, double q,
                                     double detach_factor, unsigned local_control_ranks)
{
    if (!sample || !atomic_load(&parameters_ready) || getpid() != owner) {
        if (trace_diagnostics && atomic_load(&status) == S_NO_ROUTE &&
            !atomic_exchange(&fallback_reported, true))
            fprintf(stderr, "[peak] MPI overhead sync fallback: reason=no_route policy=main\n");
        return q;
    }
    PeakMpiOverheadPolicySample current = {.sequence=sample->sequence,.elapsed_seconds=sample->elapsed_seconds,
        .profile_seconds=sample->profile_seconds,.control_seconds=sample->control_seconds,
        .management_seconds=sample->management_seconds};
    PeakMpiOverheadPolicySample ack; double ratio; bool valid;
    pthread_mutex_lock(&sample_lock);
    if (stopping || !atomic_load(&parameters_ready)) {
        pthread_mutex_unlock(&sample_lock);
        return q;
    }
    if (sample->sequence > latest.sequence) latest = *sample;
    if (!creation_attempted && atomic_load(&heartbeat_running)) {
        /* The existing HB is already excluded from user profiling. Leave the
         * original pthread listener/gate enabled to consume its helper mark.
         * Creator CPU is normal HB management, never added to module CPU. */
        creation_attempted = true;
        atomic_store(&running, true);
        atomic_store(&creator_excluded, pthread_listener_current_thread_excluded());
        pthread_listener_mark_next_created_thread_helper();
        bool created = pthread_create(&helper, NULL, service, NULL) == 0;
        atomic_store(&helper_created, created);
        if (!created) atomic_store(&running, false);
    }
    ack = acknowledged; ratio = acknowledged_ratio;
    valid = acknowledged_valid && atomic_load(&running);
    pthread_mutex_unlock(&sample_lock);
    if (!valid) return q; /* Partial/absent cohort is ordinary MAIN, never q/N. */
    current.module_cpu_seconds = module_seconds();
    double effective = peak_mpi_overhead_policy_input(&current,&ack,ratio,detach_factor,local_control_ranks);
    if (trace_diagnostics) atomic_store(&last_effective_input,effective);
    return effective;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_API int peak_mpi_overhead_sync_test_ever_ready(void)
{
    return atomic_load(&ever_ready);
}
#endif
