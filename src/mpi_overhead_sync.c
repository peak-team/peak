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
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Telemetry only: one cumulative record and reply, no credit/STOP protocol. */
enum { RECORD_SIZE = 72, ENDPOINT_SIZE = 256, WIRE_VERSION = 1 };
_Static_assert(sizeof(double) == 8, "telemetry wire requires an eight-byte double");
#define WIRE_MAGIC UINT32_C(0x504f5331)

typedef struct {
    int fd;
    int rank;
    bool connecting;
    unsigned char input[RECORD_SIZE], output[RECORD_SIZE];
    size_t received, sent;
    bool replying;
} Peer;

static pthread_mutex_t sample_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
static PeakMpiOverheadSample latest;
/* The target and the cumulative sample it priced form one coherent reply. */
static PeakMpiOverheadPolicySample acknowledged;
static double acknowledged_ratio;
static bool acknowledged_valid;
static atomic_bool started, running, helper_exited, helper_created, startup_complete;
static atomic_bool accounting_valid = true;
static atomic_bool normal_end;
static _Atomic double target_ratio, last_effective_input;
static atomic_uint_fast64_t last_ack_sequence;
static atomic_uint_fast64_t helper_cpu_ns, startup_cpu_ns, startup_wall_ns;
static atomic_uint_fast64_t policy_updates, records_sent, records_received;
static bool trace_diagnostics;
static pthread_t helper;
static int wake_pipe[2] = {-1, -1};
static int listener = -1, world_rank, world_size;
static double configured_q;
static char root_host[ENDPOINT_SIZE];
static unsigned short root_port;
static struct sockaddr_in root_address;
static pid_t owner;
extern bool enable_global_heartbeat;
extern float global_target_ratio;

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static double module_seconds(void)
{
    return ((double)atomic_load(&startup_cpu_ns) +
            (double)atomic_load(&helper_cpu_ns)) / 1e9;
}

static void put32(unsigned char* p, uint32_t value)
{
    for (int i = 3; i >= 0; --i) { p[i] = (unsigned char)value; value >>= 8; }
}
static uint32_t get32(const unsigned char* p)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | p[i];
    return value;
}
static void put64(unsigned char* p, uint64_t value)
{
    for (int i = 7; i >= 0; --i) { p[i] = (unsigned char)value; value >>= 8; }
}
static uint64_t get64(const unsigned char* p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}
static void put_double(unsigned char* p, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    put64(p, bits);
}
static double get_double(const unsigned char* p)
{
    uint64_t bits = get64(p);
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void encode(unsigned char* p, int rank, unsigned kind,
                   const PeakMpiOverheadPolicySample* s, double ratio)
{
    put32(p, WIRE_MAGIC); put32(p + 4, WIRE_VERSION);
    put32(p + 8, (uint32_t)rank); put32(p + 12, kind);
    put64(p + 16, s->sequence);
    put_double(p + 24, s->elapsed_seconds);
    put_double(p + 32, s->profile_seconds);
    put_double(p + 40, s->control_seconds);
    put_double(p + 48, s->management_seconds);
    put_double(p + 56, s->module_cpu_seconds);
    put_double(p + 64, ratio);
}

static bool decode(const unsigned char* p, unsigned kind,
                   PeakMpiOverheadPolicySample* s)
{
    if (get32(p) != WIRE_MAGIC || get32(p + 4) != WIRE_VERSION ||
        get32(p + 12) != kind) return false;
    s->sequence = get64(p + 16);
    s->elapsed_seconds = get_double(p + 24);
    s->profile_seconds = get_double(p + 32);
    s->control_seconds = get_double(p + 40);
    s->management_seconds = get_double(p + 48);
    s->module_cpu_seconds = get_double(p + 56);
    return s->sequence && isfinite(s->elapsed_seconds) && s->elapsed_seconds > 0 &&
        isfinite(s->profile_seconds) && s->profile_seconds >= 0 &&
        isfinite(s->control_seconds) && s->control_seconds >= 0 &&
        isfinite(s->management_seconds) && s->management_seconds >= 0 &&
        isfinite(s->module_cpu_seconds) && s->module_cpu_seconds >= 0;
}

static PeakMpiOverheadPolicySample local_snapshot(void)
{
    PeakMpiOverheadSample s;
    pthread_mutex_lock(&sample_lock);
    s = latest;
    pthread_mutex_unlock(&sample_lock);
    PeakMpiOverheadPolicySample result = {
        .sequence = s.sequence, .elapsed_seconds = s.elapsed_seconds,
        .profile_seconds = s.profile_seconds, .control_seconds = s.control_seconds,
        .management_seconds = s.management_seconds,
        .module_cpu_seconds = module_seconds()
    };
    return result;
}

static void publish_ack(const PeakMpiOverheadPolicySample* sample, double ratio)
{
    pthread_mutex_lock(&sample_lock);
    acknowledged = *sample;
    acknowledged_ratio = ratio;
    acknowledged_valid = true;
    atomic_store(&target_ratio, ratio);
    pthread_mutex_unlock(&sample_lock);
    if (trace_diagnostics) atomic_store(&last_ack_sequence, sample->sequence);
}

static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
    int no_delay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof no_delay);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return true;
}

static void close_peer(Peer* p)
{
    if (p->fd >= 0) close(p->fd);
    *p = (Peer){.fd = -1, .rank = -1};
}

static bool read_record(Peer* p)
{
    ssize_t n = recv(p->fd, p->input + p->received, RECORD_SIZE - p->received, 0);
    if (n > 0) p->received += (size_t)n;
    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        close_peer(p);
    return p->fd >= 0 && p->received == RECORD_SIZE;
}

static void write_record(Peer* p)
{
    ssize_t n = send(p->fd, p->output + p->sent, RECORD_SIZE - p->sent, MSG_NOSIGNAL);
    if (n > 0) p->sent += (size_t)n;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        close_peer(p);
}

static int poll_timeout(uint64_t now, uint64_t ready)
{
    if (now >= ready) return 0;
    uint64_t ms = (ready - now) / UINT64_C(1000000) + 1;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

static int connect_root(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && nonblocking(fd) &&
        (connect(fd, (struct sockaddr*)&root_address, sizeof root_address) == 0 ||
         errno == EINPROGRESS)) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

static void* service(void* unused)
{
    (void)unused;
    uint64_t cpu_origin = clock_ns(CLOCK_THREAD_CPUTIME_ID), next = 0;
    pthread_listener_exclude_current_thread();
    bool failed = false;
    Peer client = {.fd = -1, .rank = world_rank};
    Peer* peers = NULL;
    struct pollfd* fds = calloc((size_t)world_size + 2, sizeof *fds);
    PeakMpiOverheadPolicySample* samples = NULL;
    double* ratios = NULL;
    PeakMpiOverheadPolicy* policy = NULL;
    if (world_rank == 0) {
        peers = calloc((size_t)world_size, sizeof *peers);
        samples = calloc((size_t)world_size, sizeof *samples);
        ratios = calloc((size_t)world_size, sizeof *ratios);
        policy = peak_mpi_overhead_policy_create((size_t)world_size);
        if (peers) for (int i = 0; i < world_size; ++i) peers[i].fd = peers[i].rank = -1;
    } else {
        client.fd = connect_root();
        client.connecting = client.fd >= 0;
    }
    if (!cpu_origin) atomic_store(&accounting_valid, false);
    if (!cpu_origin || !fds || (world_rank == 0 && (!peers || !samples || !ratios || !policy)) ||
        (world_rank != 0 && client.fd < 0)) {
        failed = true;
        atomic_store(&running, false);
    }
    /* No record can escape before the startup application's CPU fee is final. */
    while (atomic_load(&running) && !atomic_load_explicit(&startup_complete, memory_order_acquire)) {
        struct pollfd startup_wait = {.fd = wake_pipe[0], .events = POLLIN};
        (void)poll(&startup_wait, 1, 100);
    }
    uint64_t last_sent = 0, last_completed = 0, previous_cpu = cpu_origin;
    while (atomic_load(&running) && getpid() == owner) {
        uint64_t began = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        uint64_t now = clock_ns(CLOCK_MONOTONIC);
        if (!began || !now || began < cpu_origin) {
            atomic_store(&accounting_valid, false); failed = true; break;
        }
        atomic_store(&helper_cpu_ns, began - cpu_origin);
        bool due = now >= next;
        size_t count = 1;
        fds[0] = (struct pollfd){.fd = wake_pipe[0], .events = POLLIN};
        if (due && world_rank == 0) {
            fds[count++] = (struct pollfd){.fd = listener, .events = POLLIN};
            for (int i = 0; i < world_size; ++i)
                fds[count++] = (struct pollfd){.fd = peers[i].fd,
                    .events = peers[i].replying ? POLLOUT : POLLIN};
        } else if (due && client.fd >= 0) {
            fds[count++] = (struct pollfd){.fd = client.fd,
                .events = (client.connecting || client.replying) ? POLLOUT : POLLIN};
        }
        /* When throttled, socket readability must not turn into a busy loop. */
        int timeout = due ? 100 : poll_timeout(now, next);
        int rc = poll(fds, (nfds_t)count, timeout);
        if (rc < 0 && errno != EINTR) { failed = true; break; }
        if (!atomic_load(&running)) break;
        if (!due) continue;
        if (world_rank == 0) {
            if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { failed = true; break; }
            if (fds[1].revents & POLLIN) {
                int fd = accept(listener, NULL, NULL);
                if (fd >= 0) {
                    int slot = -1;
                    for (int i = 0; i < world_size; ++i) if (peers[i].fd < 0) { slot = i; break; }
                    if (slot < 0 || !nonblocking(fd)) close(fd);
                    else peers[slot] = (Peer){.fd = fd, .rank = -1};
                }
            }
            bool changed = false, complete = true;
            PeakMpiOverheadPolicySample local = local_snapshot();
            if (local.sequence && local.sequence >= samples[0].sequence) {
                changed |= local.sequence > samples[0].sequence;
                samples[0] = local;
            }
            for (int i = 0; i < world_size; ++i) {
                Peer* p = &peers[i];
                short events = fds[i + 2].revents;
                if (p->fd < 0) continue;
                if (events & (POLLERR | POLLHUP | POLLNVAL)) { failed = true; break; }
                if (p->replying && (events & POLLOUT)) {
                    write_record(p);
                    if (p->fd >= 0 && p->sent == RECORD_SIZE) {
                        if (trace_diagnostics) atomic_fetch_add(&records_sent, 1);
                        p->replying = false; p->sent = 0; p->received = 0;
                    }
                } else if (!p->replying && (events & (POLLIN | POLLHUP)) && read_record(p)) {
                    int rank = (int)get32(p->input + 8);
                    PeakMpiOverheadPolicySample s;
                    if (rank <= 0 || rank >= world_size || (p->rank >= 0 && p->rank != rank) ||
                        !decode(p->input, 1, &s) || s.sequence < samples[rank].sequence) {
                        failed = true; break;
                    }
                    for (int j = 0; j < world_size; ++j)
                        if (&peers[j] != p && peers[j].fd >= 0 && peers[j].rank == rank)
                            failed = true;
                    if (failed) break;
                    p->rank = rank;
                    if (trace_diagnostics) atomic_fetch_add(&records_received, 1);
                    samples[rank] = s; changed = true;
                    /* Exactly one outstanding reply, never a snapshot queue. */
                    p->replying = true;
                }
                if (p->fd < 0) { failed = true; break; }
            }
            if (failed) break;
            for (int i = 0; i < world_size; ++i) complete &= samples[i].sequence != 0;
            if (changed && complete) {
                PeakMpiOverheadDecision decision;
                peak_mpi_overhead_policy_update(policy, samples, configured_q, ratios, &decision);
                publish_ack(&samples[0], ratios[0]);
                if (trace_diagnostics) atomic_fetch_add(&policy_updates, 1);
            }
            for (int i = 0; i < world_size; ++i) {
                Peer* p = &peers[i];
                if (p->fd >= 0 && p->replying && p->sent == 0 && p->rank > 0)
                    encode(p->output, p->rank, 2, &samples[p->rank],
                           complete ? ratios[p->rank] : .9 * configured_q / world_size);
            }
        } else {
            short events = count > 1 ? fds[1].revents : 0;
            if (events & (POLLERR | POLLHUP | POLLNVAL)) { failed = true; break; }
            if (client.connecting && (events & POLLOUT)) {
                int error = 0; socklen_t size = sizeof error;
                if (getsockopt(client.fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) { failed = true; break; }
                client.connecting = false;
            }
            if (!client.connecting && client.replying && (events & POLLOUT)) {
                write_record(&client);
                if (client.fd < 0) { failed = true; break; }
                if (client.sent == RECORD_SIZE) {
                    if (trace_diagnostics) atomic_fetch_add(&records_sent, 1);
                    client.replying = false;
                }
            } else if (!client.connecting && !client.replying &&
                       (events & (POLLIN | POLLHUP)) && read_record(&client)) {
                PeakMpiOverheadPolicySample s;
                double ratio = get_double(client.input + 64);
                if (!decode(client.input, 2, &s) || get32(client.input + 8) != (uint32_t)world_rank ||
                    s.sequence != last_sent || !isfinite(ratio) || ratio < 0) { failed = true; break; }
                publish_ack(&s, ratio);
                if (trace_diagnostics) {
                    atomic_fetch_add(&policy_updates, 1);
                    atomic_fetch_add(&records_received, 1);
                }
                client.received = client.sent = 0;
                last_completed = s.sequence;
                last_sent = 0;
            }
            if (client.fd < 0) { failed = true; break; }
            if (!client.connecting && !last_sent) {
                PeakMpiOverheadPolicySample s = local_snapshot();
                if (s.sequence && s.sequence > last_completed) {
                    encode(client.output, world_rank, 1, &s, 0);
                    last_sent = s.sequence; client.replying = true;
                }
            }
        }
        uint64_t ended = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        uint64_t wall = clock_ns(CLOCK_MONOTONIC);
        if (!ended || ended < began || !wall) {
            atomic_store(&accounting_valid, false); failed = true; break;
        }
        atomic_store(&helper_cpu_ns, ended - cpu_origin);
        double share = world_rank == 0 ? .05 * configured_q : .05 * configured_q / world_size;
        double pause = (double)(ended - previous_cpu) / share;
        previous_cpu = ended;
        next = pause >= (double)(UINT64_MAX - wall) ? UINT64_MAX : wall + (uint64_t)pause;
    }
    /* A lost member invalidates this whole telemetry cohort. EOF propagates
     * fallback to every client; stale samples never authorize global claims. */
    bool heartbeat_stopped = !atomic_load(&heartbeat_running);
    atomic_store(&normal_end, !failed || heartbeat_stopped);
    if (failed && !heartbeat_stopped && world_rank == 0)
        fprintf(stderr, "[peak] MPI overhead sync unavailable: rank=%d; using baseline policy\n", world_rank);
    close_peer(&client);
    if (peers) for (int i = 0; i < world_size; ++i) close_peer(&peers[i]);
    if (listener >= 0) { close(listener); listener = -1; }
    peak_mpi_overhead_policy_free(policy);
    free(peers); free(samples); free(ratios); free(fds);
    uint64_t ended = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (ended >= cpu_origin) atomic_store(&helper_cpu_ns, ended - cpu_origin);
    atomic_store(&running, false);
    pthread_mutex_lock(&exit_lock);
    atomic_store(&helper_exited, true);
    pthread_cond_signal(&exit_cond);
    pthread_mutex_unlock(&exit_lock);
    return NULL;
}

static void stop_sync(void)
{
    if (getpid() != owner) return;
    bool created = atomic_exchange(&helper_created, false);
    atomic_store(&running, false);
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
            close(wake_pipe[0]); close(wake_pipe[1]);
            wake_pipe[0] = wake_pipe[1] = -1;
        }
    }
    /* This is an atexit hook, not a new MPI-finalization protocol. */
    if (trace_diagnostics)
        fprintf(stderr, "[peak] MPI overhead sync: rank=%d module_cpu_seconds=%.9g startup_wall_seconds=%.9g accounting_valid=%d helper_exited=%d normal_end=%d policy_or_reply_updates=%llu records_received=%llu records_sent=%llu final_proposed_ratio=%.9g configured_q=%.9g acknowledged_sequence=%llu last_effective_input=%.9g\n",
                world_rank, module_seconds(), (double)atomic_load(&startup_wall_ns) / 1e9,
                (int)atomic_load(&accounting_valid), (int)atomic_load(&helper_exited),
                (int)atomic_load(&normal_end), (unsigned long long)atomic_load(&policy_updates),
                (unsigned long long)atomic_load(&records_received),
                (unsigned long long)atomic_load(&records_sent),
                atomic_load(&target_ratio), configured_q,
                (unsigned long long)atomic_load(&last_ack_sequence),
                atomic_load(&last_effective_input));
}

void peak_mpi_overhead_sync_start(void)
{
    /* Job-wide configuration contract: the escape hatch is exactly baseline,
     * with no extra MPI calls. It cannot detect ranks without an interposer. */
    const char* disabled = getenv("PEAK_MPI_OVERHEAD_SYNC");
    if (disabled && strcmp(disabled, "0") == 0) return;
    if (atomic_exchange(&started, true)) return;
    uint64_t cpu_start = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t wall_start = clock_ns(CLOCK_MONOTONIC);
    const char* trace = getenv("PEAK_MPI_OVERHEAD_SYNC_TRACE");
    trace_diagnostics = trace && strcmp(trace, "1") == 0;
    owner = getpid();
    (void)atexit(stop_sync);
    int ready = cpu_start && wall_start && atomic_load(&heartbeat_running) &&
        enable_global_heartbeat && isfinite(global_target_ratio) && global_target_ratio > 0;
    int all_ready = 0;
    if (PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
        PMPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS || world_size < 1) ready = 0;
    if (world_size == 1) goto finished; /* No rank amplification to synchronize. */
    if (PMPI_Allreduce(&ready, &all_ready, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD) != MPI_SUCCESS ||
        !all_ready) goto finished;
    double local_q = global_target_ratio;
    if (PMPI_Allreduce(&local_q, &configured_q, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD) != MPI_SUCCESS)
        goto finished;
    ready = 1;
    if (world_rank == 0) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
        socklen_t size = sizeof address;
        if (listener < 0 || !nonblocking(listener) ||
            bind(listener, (struct sockaddr*)&address, sizeof address) || listen(listener, SOMAXCONN) ||
            getsockname(listener, (struct sockaddr*)&address, &size) ||
            gethostname(root_host, sizeof root_host - 1)) ready = 0;
        else root_port = ntohs(address.sin_port);
    }
    if (PMPI_Bcast(&ready, 1, MPI_INT, 0, MPI_COMM_WORLD) != MPI_SUCCESS || !ready) goto finished;
    if (PMPI_Bcast(root_host, sizeof root_host, MPI_CHAR, 0, MPI_COMM_WORLD) != MPI_SUCCESS ||
        PMPI_Bcast(&root_port, 1, MPI_UNSIGNED_SHORT, 0, MPI_COMM_WORLD) != MPI_SUCCESS) goto finished;
    /* Potentially blocking DNS is startup work, never on the helper. */
    if (world_rank != 0) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo* addresses = NULL;
        char port[8];
        snprintf(port, sizeof port, "%u", root_port);
        if (getaddrinfo(root_host, port, &hints, &addresses) != 0) ready = 0;
        else {
            memcpy(&root_address, addresses->ai_addr, sizeof root_address);
            freeaddrinfo(addresses);
        }
    }
    if (pipe(wake_pipe) || !nonblocking(wake_pipe[0]) || !nonblocking(wake_pipe[1])) ready = 0;
    if (PMPI_Allreduce(&ready, &all_ready, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD) != MPI_SUCCESS ||
        !all_ready) goto finished;
    atomic_store(&target_ratio, .9 * configured_q / world_size);
    atomic_store(&running, true);
    pthread_listener_mark_next_created_thread_helper();
    ready = pthread_create(&helper, NULL, service, NULL) == 0;
    atomic_store(&helper_created, ready);
    if (!ready) atomic_store(&running, false);
    if (PMPI_Allreduce(&ready, &all_ready, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD) != MPI_SUCCESS ||
        !all_ready) {
        atomic_store(&running, false);
        if (wake_pipe[1] >= 0) { unsigned char byte = 1; (void)write(wake_pipe[1], &byte, 1); }
    }
finished:
    if (!atomic_load(&helper_created)) {
        if (listener >= 0) { close(listener); listener = -1; }
        if (wake_pipe[0] >= 0) close(wake_pipe[0]);
        if (wake_pipe[1] >= 0) close(wake_pipe[1]);
        wake_pipe[0] = wake_pipe[1] = -1;
    }
    uint64_t wall_end = clock_ns(CLOCK_MONOTONIC);
    uint64_t cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    if (wall_start && wall_end >= wall_start) atomic_store(&startup_wall_ns, wall_end - wall_start);
    if (cpu_start && cpu_end >= cpu_start) atomic_store(&startup_cpu_ns, cpu_end - cpu_start);
    else {
        atomic_store(&accounting_valid, false);
        atomic_store(&running, false);
        if (wake_pipe[1] >= 0) { unsigned char byte = 1; (void)write(wake_pipe[1], &byte, 1); }
    }
    atomic_store_explicit(&startup_complete, true, memory_order_release);
}

double peak_mpi_overhead_sync_update(const PeakMpiOverheadSample* sample, double q,
                                     double detach_factor, unsigned local_control_ranks)
{
    if (!sample || !atomic_load(&running) || getpid() != owner) return q;
    PeakMpiOverheadPolicySample current = {
        .sequence = sample->sequence, .elapsed_seconds = sample->elapsed_seconds,
        .profile_seconds = sample->profile_seconds, .control_seconds = sample->control_seconds,
        .management_seconds = sample->management_seconds
    };
    PeakMpiOverheadPolicySample ack;
    double raw_ratio;
    bool valid;
    pthread_mutex_lock(&sample_lock);
    if (sample->sequence > latest.sequence) latest = *sample;
    ack = acknowledged;
    raw_ratio = acknowledged_ratio;
    valid = acknowledged_valid;
    pthread_mutex_unlock(&sample_lock);
    /* Read after the ACK tuple so a newly received ACK cannot be newer than
     * the current cumulative helper-CPU observation. */
    current.module_cpu_seconds = module_seconds();
    /* No local elapsed-time credit: spend only the headroom priced at ACK.
     * The initial share has no fabricated ACK and keeps its original mapping. */
    double effective = valid ? peak_mpi_overhead_policy_input(
        &current, &ack, raw_ratio, detach_factor, local_control_ranks) :
        .9 * configured_q / world_size / fmax(1.0, detach_factor);
    if (trace_diagnostics) atomic_store(&last_effective_input, effective);
    return effective;
}
