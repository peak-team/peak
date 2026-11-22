#define _GNU_SOURCE
#include "internal/mpi_active_clock.h"

#include <mpi.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define ACTIVE_CLOCK_API __attribute__((visibility("default")))
#else
#define ACTIVE_CLOCK_API
#endif

enum { BOUNDARIES_PER_HEARTBEAT = 16 };

typedef struct {
    uint64_t edge;
    uint64_t timestamp_ns;
} Boundary;

/* The application thread publishes its latest eligible MPI boundary. Odd
 * boundaries are entries and even boundaries are exits. The grant occupies
 * a separate cache line from the state written on every boundary.
 */
static _Alignas(64) _Atomic uint64_t edge_limit;
static _Alignas(64) _Atomic uint64_t published_edge;
static Boundary boundaries[BOUNDARIES_PER_HEARTBEAT];
static _Atomic pid_t clock_owner;
enum { WRITER_INACTIVE, WRITER_IDLE, WRITER_IN_MPI };
static _Thread_local unsigned writer_state;
static _Thread_local int world_rank;
static double clock_epoch;

/* Owned by the heartbeat after acquiring clock_owner. */
static uint64_t last_edge, last_cut_ns, last_limit;
static uint64_t blocked_ns;

static uint64_t
monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000) - 1)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

void
peak_mpi_active_clock_start(double runtime_start)
{
    const char* enabled = getenv("PEAK_MPI_ACTIVE_CLOCK");
    if ((enabled != NULL && strcmp(enabled, "1")) ||
        !isfinite(runtime_start) || runtime_start <= 0.0)
        return;

    int level, size;
    if (PMPI_Query_thread(&level) != MPI_SUCCESS ||
        (level != MPI_THREAD_SINGLE && level != MPI_THREAD_FUNNELED) ||
        PMPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS || size <= 1 ||
        PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS)
        return;

    /* PEAK publishes the runtime origin before enabling MPI wrappers. */
    clock_epoch = runtime_start;
    last_cut_ns = (uint64_t)(runtime_start * 1e9);
    last_limit = BOUNDARIES_PER_HEARTBEAT;
    atomic_store_explicit(&edge_limit, last_limit, memory_order_relaxed);
    writer_state = WRITER_IDLE;
    atomic_store_explicit(&clock_owner, getpid(), memory_order_release);
}

static inline void
publish_boundary(void)
{
    uint64_t edge = atomic_load_explicit(&published_edge, memory_order_relaxed) + 1;
    uint64_t limit = atomic_load_explicit(&edge_limit, memory_order_acquire);
    if (edge <= limit) {
        Boundary* boundary = &boundaries[edge % BOUNDARIES_PER_HEARTBEAT];
        boundary->edge = edge;
        boundary->timestamp_ns = monotonic_ns();
    }
    atomic_store_explicit(&published_edge, edge, memory_order_release);
}

double
peak_mpi_active_clock_blocked_seconds(double runtime_start, double* now,
                                     unsigned expected_us, bool* saturated)
{
    *saturated = false;
    pid_t owner = atomic_load_explicit(&clock_owner, memory_order_acquire);
    if (!owner || owner != getpid() || runtime_start != clock_epoch)
        return -1.0;

    /* Bound the time represented by a state observation even if the
     * heartbeat is descheduled while taking it.
     */
    uint64_t before_read_ns = (uint64_t)(*now * 1e9);
    uint64_t edge = atomic_load_explicit(&published_edge, memory_order_acquire);
    uint64_t cut_ns = monotonic_ns();
    bool state_unchanged = edge ==
        atomic_load_explicit(&published_edge, memory_order_relaxed);
    if (cut_ns < before_read_ns || cut_ns <= last_cut_ns)
        return -1.0;

    /* The edge load publishes completed timestamp slots. The old grant
     * permits at most one turn through the array, so the writer can only
     * reuse any slot we read after the next grant is published below.
     */
    uint64_t prefix_end = edge < last_limit ? edge : last_limit;
    uint64_t cursor_ns = last_cut_ns;
    uint64_t cursor_edge = last_edge;
    uint64_t window_blocked = 0;
    for (uint64_t i = last_edge + 1; i <= prefix_end; ++i) {
        const Boundary* boundary = &boundaries[i % BOUNDARIES_PER_HEARTBEAT];
        uint64_t timestamp_ns = boundary->timestamp_ns;
        /* A writer may pass a boundary under the old grant before it
         * sees the new limit. Its slot then still contains an older tag.
         */
        if (boundary->edge != i || timestamp_ns == 0 || timestamp_ns > cut_ns)
            continue;
        if (timestamp_ns < cursor_ns)
            timestamp_ns = cursor_ns;
        if ((cursor_edge & 1) || i != cursor_edge + 1)
            window_blocked += timestamp_ns - cursor_ns;
        cursor_ns = timestamp_ns;
        cursor_edge = i;
    }

    uint64_t tail_ns = cut_ns - cursor_ns;
    uint64_t observed_tail_ns = before_read_ns > cursor_ns
        ? before_read_ns - cursor_ns : 0;
    *saturated = edge > last_limit;
    if (cursor_edge == edge) {
        if (edge & 1)
            window_blocked += tail_ns;
        else if (!state_unchanged)
            window_blocked += tail_ns - observed_tail_ns;
    } else if (*saturated && !(edge & 1)) {
        /* An outside sample estimates the unmeasured tail. A delayed
         * heartbeat cannot extrapolate that sample over missed time.
         */
        uint64_t expected_ns = (uint64_t)expected_us * 1000;
        window_blocked += tail_ns - (observed_tail_ns < expected_ns
            ? observed_tail_ns : expected_ns);
    } else {
        window_blocked += tail_ns;
    }

    blocked_ns += window_blocked;
    last_cut_ns = cut_ns;
    last_edge = edge;
    last_limit = edge + BOUNDARIES_PER_HEARTBEAT;
    atomic_store_explicit(&edge_limit, last_limit, memory_order_release);

    *now = (double)(cut_ns / UINT64_C(1000000000)) +
           (double)(cut_ns % UINT64_C(1000000000)) * 1e-9;
    return fmin((double)blocked_ns * 1e-9, *now - runtime_start);
}

static bool
network_comm(MPI_Comm comm)
{
    /* WORLD has multiple ranks by admission. Other non-SELF communicators
     * are possible communication, without proof of size or rank mapping. */
    return comm != MPI_COMM_NULL && comm != MPI_COMM_SELF;
}

static bool
remote_peer(MPI_Comm comm, int peer)
{
    return network_comm(comm) && (peer == MPI_ANY_SOURCE ||
        (peer >= 0 && (comm != MPI_COMM_WORLD || peer != world_rank)));
}

/* Resolve MPI and PMPI separately so tool chaining keeps the original next
 * symbol. A nested vendor/tool call must never bill the same interval twice.
 * Hidden, static, native Fortran and unlisted operations produce no clock.
 */
#define ACTIVE_SYMBOLS(Name, Parameters)                                      \
    typedef int (*Name##Fn) Parameters;                                       \
    static _Atomic(Name##Fn) next_mpi_##Name, next_pmpi_##Name;               \
    static pthread_once_t mpi_once_##Name = PTHREAD_ONCE_INIT;                \
    static pthread_once_t pmpi_once_##Name = PTHREAD_ONCE_INIT;               \
    static void resolve_mpi_##Name(void)                                      \
    {                                                                         \
        atomic_store_explicit(&next_mpi_##Name,                               \
            (Name##Fn)dlsym(RTLD_NEXT, "MPI_" #Name), memory_order_release);  \
    }                                                                         \
    static void resolve_pmpi_##Name(void)                                     \
    {                                                                         \
        atomic_store_explicit(&next_pmpi_##Name,                              \
            (Name##Fn)dlsym(RTLD_NEXT, "PMPI_" #Name), memory_order_release); \
    }

#define ACTIVE_WRAPPER(Prefix, prefix, Name, Parameters, Arguments, Eligible) \
    ACTIVE_CLOCK_API int Prefix##_##Name Parameters                           \
    {                                                                         \
        Name##Fn next = atomic_load_explicit(&next_##prefix##_##Name,         \
                                              memory_order_acquire);          \
        if (!next) {                                                          \
            pthread_once(&prefix##_once_##Name, resolve_##prefix##_##Name);   \
            next = atomic_load_explicit(&next_##prefix##_##Name,              \
                                         memory_order_relaxed);               \
            if (!next)                                                        \
                return MPI_ERR_OTHER;                                         \
        }                                                                     \
        if (writer_state != WRITER_IDLE)                                      \
            return next Arguments;                                            \
        writer_state = WRITER_IN_MPI;                                         \
        bool eligible = (Eligible);                                           \
        if (eligible)                                                         \
            publish_boundary();                                               \
        int result = next Arguments;                                          \
        if (eligible)                                                         \
            publish_boundary();                                               \
        writer_state = WRITER_IDLE;                                           \
        return result;                                                        \
    }

#define ACTIVE_CALL(Name, Parameters, Arguments, Eligible)                   \
    ACTIVE_SYMBOLS(Name, Parameters)                                         \
    ACTIVE_WRAPPER(MPI, mpi, Name, Parameters, Arguments, Eligible)          \
    ACTIVE_WRAPPER(PMPI, pmpi, Name, Parameters, Arguments, Eligible)

ACTIVE_CALL(Send, (const void* buffer, int count, MPI_Datatype type, int dest,
                   int tag, MPI_Comm comm),
            (buffer, count, type, dest, tag, comm), remote_peer(comm, dest))
ACTIVE_CALL(Ssend, (const void* buffer, int count, MPI_Datatype type, int dest,
                    int tag, MPI_Comm comm),
            (buffer, count, type, dest, tag, comm), remote_peer(comm, dest))
ACTIVE_CALL(Recv, (void* buffer, int count, MPI_Datatype type, int source,
                   int tag, MPI_Comm comm, MPI_Status* status),
            (buffer, count, type, source, tag, comm, status),
            remote_peer(comm, source))
ACTIVE_CALL(Sendrecv, (const void* send_buffer, int send_count, MPI_Datatype send_type,
                       int dest, int send_tag, void* recv_buffer, int recv_count,
                       MPI_Datatype recv_type, int source, int recv_tag,
                       MPI_Comm comm, MPI_Status* status),
            (send_buffer, send_count, send_type, dest, send_tag, recv_buffer,
             recv_count, recv_type, source, recv_tag, comm, status),
            remote_peer(comm, dest) || remote_peer(comm, source))

/* Requests can originate from MPI APIs outside this wrapper set. Avoid
 * requiring any earlier call or walking request arrays on the hot path. */
ACTIVE_CALL(Wait, (MPI_Request* request, MPI_Status* status), (request, status),
            request && *request != MPI_REQUEST_NULL)
ACTIVE_CALL(Waitall, (int count, MPI_Request requests[], MPI_Status statuses[]),
            (count, requests, statuses), count > 0 && requests)
ACTIVE_CALL(Waitany, (int count, MPI_Request requests[], int* index, MPI_Status* status),
            (count, requests, index, status), count > 0 && requests)
ACTIVE_CALL(Waitsome, (int count, MPI_Request requests[], int* outcount,
                       int indices[], MPI_Status statuses[]),
            (count, requests, outcount, indices, statuses),
            count > 0 && requests)

ACTIVE_CALL(Barrier, (MPI_Comm comm), (comm), network_comm(comm))
ACTIVE_CALL(Bcast, (void* buffer, int count, MPI_Datatype type, int root, MPI_Comm comm),
            (buffer, count, type, root, comm), network_comm(comm))
ACTIVE_CALL(Reduce, (const void* send_buffer, void* recv_buffer, int count,
                     MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm),
            (send_buffer, recv_buffer, count, type, op, root, comm), network_comm(comm))
ACTIVE_CALL(Allreduce, (const void* send_buffer, void* recv_buffer, int count,
                        MPI_Datatype type, MPI_Op op, MPI_Comm comm),
            (send_buffer, recv_buffer, count, type, op, comm), network_comm(comm))
ACTIVE_CALL(Allgather, (const void* send_buffer, int send_count, MPI_Datatype send_type,
                        void* recv_buffer, int recv_count, MPI_Datatype recv_type,
                        MPI_Comm comm),
            (send_buffer, send_count, send_type, recv_buffer, recv_count, recv_type, comm),
            network_comm(comm))
ACTIVE_CALL(Alltoall, (const void* send_buffer, int send_count, MPI_Datatype send_type,
                       void* recv_buffer, int recv_count, MPI_Datatype recv_type,
                       MPI_Comm comm),
            (send_buffer, send_count, send_type, recv_buffer, recv_count, recv_type, comm),
            network_comm(comm))

