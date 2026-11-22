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

#if defined(__linux__) && defined(__GNUC__) && !defined(PEAK_ATTACH_RUNTIME)
#define ACTIVE_CLOCK_TLS __attribute__((tls_model("initial-exec")))
#else
#define ACTIVE_CLOCK_TLS
#endif

#if defined(__linux__) && defined(CLOCK_MONOTONIC_COARSE)
#define ACTIVE_CLOCK_ID CLOCK_MONOTONIC_COARSE
#elif defined(__APPLE__)
#define ACTIVE_CLOCK_ID CLOCK_MONOTONIC_RAW_APPROX
#endif

/* The low bit marks an open MPI call. The remaining bits hold completed
 * residence when closed and completed residence minus entry time when open.
 * Unsigned arithmetic keeps the signed offset in the same atomic word.
 * UINT64_MAX marks permanent failure of the selected clock.
 */
static _Atomic uint64_t residence_state;
static _Atomic pid_t clock_owner;
enum { WRITER_INACTIVE, WRITER_IDLE, WRITER_IN_MPI };
static _Thread_local unsigned writer_state ACTIVE_CLOCK_TLS;
static _Thread_local int world_rank ACTIVE_CLOCK_TLS;
static double clock_epoch;
static double clock_quantum;

static inline uint64_t
clock_ns(clockid_t clock)
{
    struct timespec now;
    if (clock_gettime(clock, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static inline uint64_t
sample_clock_ns(void)
{
#ifdef ACTIVE_CLOCK_ID
    return clock_ns(ACTIVE_CLOCK_ID);
#else
    return 0;
#endif
}

static bool
sample_clock_available(void)
{
    enum { BATCH_SIZE = 128, MAX_CLOCK_NS = 20 };
    if (!sample_clock_ns())
        return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint64_t before = clock_ns(CLOCK_MONOTONIC);
        for (unsigned i = 0; i < BATCH_SIZE; ++i) {
            if (!sample_clock_ns())
                return false;
        }
        uint64_t after = clock_ns(CLOCK_MONOTONIC);
        if (before && after >= before && after - before <= BATCH_SIZE * MAX_CLOCK_NS)
            return true;
    }
    return false;
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
        PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS ||
        !sample_clock_available())
        return;

#ifdef ACTIVE_CLOCK_ID
    struct timespec resolution;
    if (clock_getres(ACTIVE_CLOCK_ID, &resolution) != 0)
        return;
    clock_quantum = (double)resolution.tv_sec + (double)resolution.tv_nsec * 1e-9;
#endif
    clock_epoch = runtime_start;
    writer_state = WRITER_IDLE;
    atomic_store_explicit(&clock_owner, getpid(), memory_order_release);
}

static inline uint64_t
open_interval(void)
{
    uint64_t completed = atomic_load_explicit(&residence_state, memory_order_relaxed);
    if (completed == UINT64_MAX)
        return UINT64_MAX;
    uint64_t tick = sample_clock_ns();
    uint64_t opened = tick ? (completed - (tick << 1)) | 1 : UINT64_MAX;
    atomic_store_explicit(&residence_state, opened, memory_order_release);
    return opened;
}

static inline void
close_interval(uint64_t opened)
{
    uint64_t tick = sample_clock_ns();
    uint64_t completed = tick ? opened + (tick << 1) - 1 : UINT64_MAX;
    atomic_store_explicit(&residence_state, completed, memory_order_release);
}

double
peak_mpi_active_clock_quantum_seconds(void)
{
    /* The caller has acquired the clock owner in a valid observation. */
    return clock_quantum;
}

static inline uint64_t
residence_at(uint64_t state, uint64_t tick)
{
    return ((state & 1) ? state + (tick << 1) : state) >> 1;
}

double
peak_mpi_active_clock_blocked_seconds(double runtime_start, double* now)
{
    pid_t owner = atomic_load_explicit(&clock_owner, memory_order_acquire);
    if (!owner || owner != getpid() || runtime_start != clock_epoch)
        return PEAK_MPI_CLOCK_UNAVAILABLE;
    if (!isfinite(*now) || *now <= runtime_start)
        return -1.0;

    /* Match residence and wall cuts within a short bracket. A dense MPI
     * loop may change the open bit without changing residence at this tick.
     */
    uint64_t before = (uint64_t)(*now * 1e9);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        uint64_t first = atomic_load_explicit(&residence_state, memory_order_acquire);
        uint64_t tick = sample_clock_ns();
        uint64_t after = clock_ns(CLOCK_MONOTONIC);
        uint64_t second = atomic_load_explicit(&residence_state, memory_order_acquire);
        if (first == UINT64_MAX || second == UINT64_MAX)
            return PEAK_MPI_CLOCK_UNAVAILABLE;
        if (!tick || !after)
            return -1.0;
        uint64_t residence = residence_at(first, tick);
        if (after >= before && after - before <= 100000 &&
            residence == residence_at(second, tick)) {
            *now = (double)(after / UINT64_C(1000000000)) +
                   (double)(after % UINT64_C(1000000000)) * 1e-9;
            return fmin((double)residence * 1e-9, *now - runtime_start);
        }
        before = after;
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
        uint64_t opened = (Eligible) ? open_interval() : UINT64_MAX;          \
        int result = next Arguments;                                          \
        if (opened != UINT64_MAX)                                             \
            close_interval(opened);                                           \
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

