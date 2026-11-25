#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Keep the three application targets visible to PEAK. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 9
#define FIXTURE_ENTRY __attribute__((noinline, noipa, used, visibility("default")))
#elif defined(__GNUC__) || defined(__clang__)
#define FIXTURE_ENTRY __attribute__((noinline, used, visibility("default")))
#else
#define FIXTURE_ENTRY
#endif

FIXTURE_ENTRY
uint64_t fixture_hot_a(uint64_t x)
{
    return x * UINT64_C(6364136223846793005) + 17;
}

FIXTURE_ENTRY
uint64_t fixture_hot_b(uint64_t x)
{
    return x * UINT64_C(6364136223846793005) + 23;
}

FIXTURE_ENTRY
uint64_t fixture_sentinel(uint64_t x)
{
    return (x >> 12) ^ x ^ UINT64_C(0x97e852660a6f9bab);
}

static uint64_t now_ns(clockid_t clock)
{
    struct timespec stamp;
    if (clock_gettime(clock, &stamp) != 0)
        abort();
    return (uint64_t)stamp.tv_sec * UINT64_C(1000000000) + stamp.tv_nsec;
}

static unsigned parse_unsigned(const char* value)
{
    char* end;
    errno = 0;
    if (*value < '0' || *value > '9')
        return 0;
    unsigned long n = strtoul(value, &end, 10);
    return errno || *end || n > UINT_MAX ? 0 : (unsigned)n;
}

static void mpi_check(int result)
{
    if (result != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 3);
        abort();
    }
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s {rotating|common} EVEN_ROUNDS BASE_ITERATIONS EVEN_HOT_CALLS\n",
                argv[0]);
        return 2;
    }
    int rotating = strcmp(argv[1], "rotating") == 0;
    if (!rotating && strcmp(argv[1], "common") != 0)
        return 2;
    unsigned rounds = parse_unsigned(argv[2]);
    unsigned base = parse_unsigned(argv[3]);
    unsigned hot = parse_unsigned(argv[4]);
    if (!rounds || rounds % 2 || !base || !hot || hot % 2)
        return 2;

    int rank, size, provided;
    if (MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided) != MPI_SUCCESS)
        return 3;
    mpi_check(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
    mpi_check(MPI_Comm_size(MPI_COMM_WORLD, &size));
    if (size != 2 || provided != MPI_THREAD_FUNNELED) {
        if (rank == 0)
            fprintf(stderr, "Expected two MPI ranks and MPI_THREAD_FUNNELED support\n");
        mpi_check(MPI_ERR_OTHER);
    }
    uint64_t (*hot_fn)(uint64_t) = rank ? fixture_hot_b : fixture_hot_a;
    mpi_check(MPI_Barrier(MPI_COMM_WORLD));

    uint64_t compute = rank + 1, leaves = rank + 1, sentinel = rank + 1;
    uint64_t mpi_ns = 0, coarse_ns = 0, hot_ns = 0, native_work_ns = 0, work_calls = 0;
    uint64_t started = now_ns(CLOCK_MONOTONIC);
    for (unsigned round = 0; round < rounds; ++round) {
        uint64_t t = now_ns(CLOCK_MONOTONIC);
        for (unsigned i = 0; i < base; ++i) {
            compute ^= compute >> 13;
            compute = compute * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        }
        native_work_ns += now_ns(CLOCK_MONOTONIC) - t;
        unsigned calls = rotating ? ((round & 1) == (unsigned)rank ? hot : 0) : hot / 2;
        t = now_ns(CLOCK_MONOTONIC);
        for (unsigned j = 0; j < calls; ++j)
            leaves = hot_fn(leaves);
        hot_ns += now_ns(CLOCK_MONOTONIC) - t;
        work_calls += calls;
        sentinel = fixture_sentinel(sentinel ^ round);

#ifdef CLOCK_MONOTONIC_COARSE
        uint64_t coarse_start = now_ns(CLOCK_MONOTONIC_COARSE);
#endif
        uint64_t mpi_start = now_ns(CLOCK_MONOTONIC);
        mpi_check(MPI_Barrier(MPI_COMM_WORLD));
        mpi_ns += now_ns(CLOCK_MONOTONIC) - mpi_start;
#ifdef CLOCK_MONOTONIC_COARSE
        coarse_ns += now_ns(CLOCK_MONOTONIC_COARSE) - coarse_start;
#endif
    }
    uint64_t elapsed = now_ns(CLOCK_MONOTONIC) - started;
    if (work_calls != (uint64_t)rounds * hot / 2)
        mpi_check(MPI_ERR_OTHER);
    printf("fixed rank=%d pid=%ld mode=%s rounds=%u base=%u calls=%" PRIu64
           " compute_checksum=%" PRIu64 " leaf_checksum=%" PRIu64 " sentinel_checksum=%" PRIu64
           " elapsed_ns=%" PRIu64 " mpi_ns=%" PRIu64 " coarse_ns=%" PRIu64
           " hot_ns=%" PRIu64 " native_work_ns=%" PRIu64 "\n",
           rank, (long)getpid(), rotating ? "rotating" : "common", rounds, base, work_calls, compute, leaves, sentinel,
           elapsed, mpi_ns, coarse_ns, hot_ns, native_work_ns);
    return MPI_Finalize() == MPI_SUCCESS ? 0 : 3;
}
