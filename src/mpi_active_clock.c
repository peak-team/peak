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

static _Atomic unsigned sequence;
static _Atomic uint64_t open_ns, completed_ns, last_end_ns;
static _Atomic pid_t clock_owner;
static _Thread_local pid_t writer_pid;
static _Thread_local unsigned wrapper_depth;
static _Thread_local bool saw_remote;
static _Thread_local int world_rank;
static double clock_epoch;

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
    if (enabled == NULL || strcmp(enabled, "1") ||
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
    writer_pid = getpid();
    atomic_store_explicit(&clock_owner, writer_pid, memory_order_release);
}

static void
open_interval(void)
{
    const uint64_t now = monotonic_ns();
    if (!now)
        return;
    atomic_fetch_add_explicit(&sequence, 1, memory_order_acq_rel);
    atomic_store_explicit(&open_ns, now, memory_order_relaxed);
    atomic_fetch_add_explicit(&sequence, 1, memory_order_release);
}

static void
close_interval(void)
{
    const uint64_t now = monotonic_ns();
    uint64_t start = atomic_load_explicit(&open_ns, memory_order_relaxed);
    if (!start)
        return;
    atomic_fetch_add_explicit(&sequence, 1, memory_order_acq_rel);
    if (now >= start) {
        /* Nonoverlapping monotonic intervals fit in the elapsed clock range. */
        atomic_store_explicit(&completed_ns,
            atomic_load_explicit(&completed_ns, memory_order_relaxed) +
                now - start, memory_order_relaxed);
        atomic_store_explicit(&last_end_ns, now, memory_order_relaxed);
    }
    atomic_store_explicit(&open_ns, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&sequence, 1, memory_order_release);
}

double
peak_mpi_active_clock_blocked_seconds(double runtime_start, double now)
{
    pid_t owner = atomic_load_explicit(&clock_owner, memory_order_acquire);
    if (!owner || owner != getpid() || runtime_start != clock_epoch ||
        !isfinite(now) || now <= runtime_start)
        return -1.0;

    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        unsigned start = atomic_load_explicit(&sequence, memory_order_acquire);
        if (start & 1)
            continue;
        uint64_t completed = atomic_load_explicit(&completed_ns, memory_order_relaxed);
        uint64_t open = atomic_load_explicit(&open_ns, memory_order_relaxed);
        uint64_t ended = atomic_load_explicit(&last_end_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (start != atomic_load_explicit(&sequence, memory_order_relaxed))
            continue;
        if ((double)ended * 1e-9 > now)
            return -1.0; /* Completion crossed the heartbeat's wall cut. */
        double open_seconds = (double)open * 1e-9;
        double running = open && open_seconds < now
            ? now - fmax(runtime_start, open_seconds) : 0.0;
        return (double)completed * 1e-9 + running;
    }
    return -1.0;
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
    static Name##Fn next_mpi_##Name, next_pmpi_##Name;                          \
    static pthread_once_t mpi_once_##Name = PTHREAD_ONCE_INIT;                  \
    static pthread_once_t pmpi_once_##Name = PTHREAD_ONCE_INIT;                 \
    static void resolve_mpi_##Name(void)                                      \
    { next_mpi_##Name = (Name##Fn)dlsym(RTLD_NEXT, "MPI_" #Name); }             \
    static void resolve_pmpi_##Name(void)                                     \
    { next_pmpi_##Name = (Name##Fn)dlsym(RTLD_NEXT, "PMPI_" #Name); }

#define ACTIVE_WRAPPER(Prefix, prefix, Name, Parameters, Arguments, Eligible, Measure) \
    ACTIVE_CLOCK_API int Prefix##_##Name Parameters                           \
    {                                                                        \
        pthread_once(&prefix##_once_##Name, resolve_##prefix##_##Name);       \
        Name##Fn next = next_##prefix##_##Name;                               \
        if (!next)                                                           \
            return MPI_ERR_OTHER;                                            \
        if (!writer_pid)                                                     \
            return next Arguments;                                           \
        bool eligible = wrapper_depth++ == 0 && (Eligible);                  \
        if (eligible) {                                                      \
            saw_remote = true;                                               \
            if (Measure)                                                     \
                open_interval();                                             \
        }                                                                    \
        int result = next Arguments;                                         \
        if (eligible && Measure)                                             \
            close_interval();                                                \
        --wrapper_depth;                                                     \
        return result;                                                       \
    }

#define ACTIVE_CALL(Name, Parameters, Arguments, Eligible, Measure)          \
    ACTIVE_SYMBOLS(Name, Parameters)                                         \
    ACTIVE_WRAPPER(MPI, mpi, Name, Parameters, Arguments, Eligible, Measure) \
    ACTIVE_WRAPPER(PMPI, pmpi, Name, Parameters, Arguments, Eligible, Measure)

ACTIVE_CALL(Send, (const void* buffer, int count, MPI_Datatype type, int dest,
                   int tag, MPI_Comm comm),
            (buffer, count, type, dest, tag, comm), remote_peer(comm, dest), true)
ACTIVE_CALL(Ssend, (const void* buffer, int count, MPI_Datatype type, int dest,
                    int tag, MPI_Comm comm),
            (buffer, count, type, dest, tag, comm), remote_peer(comm, dest), true)
ACTIVE_CALL(Recv, (void* buffer, int count, MPI_Datatype type, int source,
                   int tag, MPI_Comm comm, MPI_Status* status),
            (buffer, count, type, source, tag, comm, status),
            remote_peer(comm, source), true)
ACTIVE_CALL(Sendrecv, (const void* send_buffer, int send_count, MPI_Datatype send_type,
                       int dest, int send_tag, void* recv_buffer, int recv_count,
                       MPI_Datatype recv_type, int source, int recv_tag,
                       MPI_Comm comm, MPI_Status* status),
            (send_buffer, send_count, send_type, dest, send_tag, recv_buffer,
             recv_count, recv_type, source, recv_tag, comm, status),
            remote_peer(comm, dest) || remote_peer(comm, source), true)

ACTIVE_CALL(Isend, (const void* buffer, int count, MPI_Datatype type, int dest,
                    int tag, MPI_Comm comm, MPI_Request* request),
            (buffer, count, type, dest, tag, comm, request),
            remote_peer(comm, dest), false)
ACTIVE_CALL(Irecv, (void* buffer, int count, MPI_Datatype type, int source,
                    int tag, MPI_Comm comm, MPI_Request* request),
            (buffer, count, type, source, tag, comm, request),
            remote_peer(comm, source), false)
ACTIVE_CALL(Wait, (MPI_Request* request, MPI_Status* status), (request, status),
            saw_remote && request && *request != MPI_REQUEST_NULL, true)
ACTIVE_CALL(Waitall, (int count, MPI_Request requests[], MPI_Status statuses[]),
            (count, requests, statuses), saw_remote && count > 0 && requests, true)
ACTIVE_CALL(Waitany, (int count, MPI_Request requests[], int* index, MPI_Status* status),
            (count, requests, index, status), saw_remote && count > 0 && requests, true)
ACTIVE_CALL(Waitsome, (int count, MPI_Request requests[], int* outcount,
                       int indices[], MPI_Status statuses[]),
            (count, requests, outcount, indices, statuses),
            saw_remote && count > 0 && requests, true)

ACTIVE_CALL(Barrier, (MPI_Comm comm), (comm), network_comm(comm), true)
ACTIVE_CALL(Bcast, (void* buffer, int count, MPI_Datatype type, int root, MPI_Comm comm),
            (buffer, count, type, root, comm), network_comm(comm), true)
ACTIVE_CALL(Reduce, (const void* send_buffer, void* recv_buffer, int count,
                     MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm),
            (send_buffer, recv_buffer, count, type, op, root, comm), network_comm(comm), true)
ACTIVE_CALL(Allreduce, (const void* send_buffer, void* recv_buffer, int count,
                        MPI_Datatype type, MPI_Op op, MPI_Comm comm),
            (send_buffer, recv_buffer, count, type, op, comm), network_comm(comm), true)
ACTIVE_CALL(Allgather, (const void* send_buffer, int send_count, MPI_Datatype send_type,
                        void* recv_buffer, int recv_count, MPI_Datatype recv_type,
                        MPI_Comm comm),
            (send_buffer, send_count, send_type, recv_buffer, recv_count, recv_type, comm),
            network_comm(comm), true)
ACTIVE_CALL(Alltoall, (const void* send_buffer, int send_count, MPI_Datatype send_type,
                       void* recv_buffer, int recv_count, MPI_Datatype recv_type,
                       MPI_Comm comm),
            (send_buffer, send_count, send_type, recv_buffer, recv_count, recv_type, comm),
            network_comm(comm), true)

