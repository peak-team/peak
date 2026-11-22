/* Exercise the clock with deterministic wall times and no MPI run. */
#define clock_gettime fake_clock_gettime
#define clock_getres fake_clock_getres
#define PMPI_Query_thread fake_pmpi_query_thread
#define PMPI_Comm_size fake_pmpi_comm_size
#define PMPI_Comm_rank fake_pmpi_comm_rank
#include "../../src/mpi_active_clock.c"
#undef PMPI_Comm_rank
#undef PMPI_Comm_size
#undef PMPI_Query_thread
#undef clock_getres
#undef clock_gettime

#include <assert.h>

void
peak_log_message(PeakVerbosity verbosity, const char* format, ...)
{
    assert(verbosity == PEAK_VERBOSITY_DEBUG);
    assert(strstr(format, "reason=clock-qualification"));
}

enum { TICK_US = 1000 };
static const double epoch = 1.0;
static int wall_us;
static unsigned coarse_reads;
static bool coarse_available;
static bool fine_failure;
static unsigned delayed_reads;
static uint64_t opened;

int
fake_clock_gettime(clockid_t clock_id, struct timespec* time)
{
    *time = (struct timespec){0};
    if (clock_id == CLOCK_MONOTONIC && fine_failure)
        return -1;
    if (clock_id == CLOCK_MONOTONIC && delayed_reads) {
        --delayed_reads;
        wall_us += 1000;
    }
    int measured_us = wall_us;
    if (clock_id == CLOCK_MONOTONIC_COARSE) {
        if (!coarse_available)
            return -1;
        ++coarse_reads;
        measured_us -= measured_us % TICK_US;
    } else {
        assert(clock_id == CLOCK_MONOTONIC);
    }
    time->tv_sec = 1 + measured_us / 1000000;
    time->tv_nsec = (measured_us % 1000000) * 1000L;
    return 0;
}

int
fake_clock_getres(clockid_t clock_id, struct timespec* time)
{
    assert(clock_id == CLOCK_MONOTONIC_COARSE);
    time->tv_sec = 0;
    time->tv_nsec = TICK_US * 1000L;
    return 0;
}

int
fake_pmpi_query_thread(int* provided)
{
    *provided = MPI_THREAD_FUNNELED;
    return MPI_SUCCESS;
}

int
fake_pmpi_comm_size(MPI_Comm comm, int* size)
{
    assert(comm == MPI_COMM_WORLD);
    *size = 2;
    return MPI_SUCCESS;
}

int
fake_pmpi_comm_rank(MPI_Comm comm, int* rank)
{
    assert(comm == MPI_COMM_WORLD);
    *rank = 0;
    return MPI_SUCCESS;
}

static void
boundary_at(int at_us)
{
    assert(at_us >= wall_us);
    wall_us = at_us;
    if (opened) {
        close_interval(opened);
        opened = 0;
    } else {
        opened = open_interval();
        assert(opened != UINT64_MAX);
    }
}

static double
observe_at(int at_us, double previous)
{
    assert(at_us >= wall_us);
    wall_us = at_us;
    double now = epoch + wall_us * 1e-6;
    double blocked = peak_mpi_active_clock_blocked_seconds(epoch, &now);
    assert(blocked >= previous - 1e-9);
    assert(blocked <= now - epoch + 1e-9);
    return blocked;
}

static void
expect_added(double previous, double blocked, int expected_us, int tolerance_us)
{
    assert(fabs((blocked - previous) - expected_us * 1e-6) <=
           tolerance_us * 1e-6 + 1e-9);
}

int
main(void)
{
    double unavailable_cut = epoch + 0.001;
    coarse_available = true;
    assert(setenv("PEAK_MPI_ACTIVE_CLOCK", "0", 1) == 0);
    peak_mpi_active_clock_start(epoch);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &unavailable_cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);

    assert(unsetenv("PEAK_MPI_ACTIVE_CLOCK") == 0);
    coarse_available = false;
    peak_mpi_active_clock_start(epoch);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &unavailable_cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);

    coarse_available = true;
    peak_mpi_active_clock_start(epoch);
    double blocked = observe_at(1000, 0);
    expect_added(0, blocked, 0, 0);
    assert(fabs(peak_mpi_active_clock_quantum_seconds() - 0.001) < 1e-9);

    /* Sweep the phase of eight tiny calls followed by a 19ms wait. The
     * whole burst completes between cuts, including the last long call. */
    unsigned clock_reads_before = coarse_reads;
    for (int phase = 0; phase < TICK_US; phase += 23) {
        int start = (wall_us / TICK_US + 2) * TICK_US + phase;
        for (int call = 0; call < 8; ++call) {
            boundary_at(start + 70 * call);
            boundary_at(start + 70 * call + 50);
        }
        boundary_at(start + 560);
        boundary_at(start + 560 + 19000);
        double next = observe_at(start + 22000, blocked);
        expect_added(blocked, next, 19000, TICK_US);
        blocked = next;
    }
    assert(coarse_reads > clock_reads_before);

    /* Distribute short calls across coarse clock phases, then stop calling MPI.
     * Completed time must stay fixed when the application resumes computation. */
    int dense_start = (wall_us / TICK_US + 2) * TICK_US;
    for (int call = 0; call < 50; ++call) {
        int entry = dense_start + 2 * TICK_US * call + 20 * call;
        boundary_at(entry);
        boundary_at(entry + 400);
    }
    double dense_blocked = observe_at(dense_start + 101000, blocked);
    expect_added(blocked, dense_blocked, 20000, 0);
    blocked = observe_at(dense_start + 121000, dense_blocked);
    expect_added(dense_blocked, blocked, 0, 0);

    /* Two sparse waits share the window and their 15ms application gap
     * remains active time. Observe the second wait before it completes. */
    int start = (wall_us / TICK_US + 2) * TICK_US;
    boundary_at(start);
    boundary_at(start + 40000);
    boundary_at(start + 55000);
    double crossing = observe_at(start + 60000, blocked);
    expect_added(blocked, crossing, 45000, 0);
    double same_tick = observe_at(start + 60750, crossing);
    expect_added(crossing, same_tick, 0, 0);
    double still_waiting = observe_at(start + 90000, same_tick);
    expect_added(crossing, still_waiting, 30000, 0);
    boundary_at(start + 100000);
    blocked = observe_at(start + 115000, still_waiting);
    expect_added(still_waiting, blocked, 10000, 0);
    expect_added(blocked, observe_at(start + 130000, blocked), 0, 0);

    /* Retry once with a fresh bracket. A second preemption preserves the
     * caller's cut, and a later coherent sample can recover the estimate. */
    delayed_reads = 1;
    double cut = epoch + wall_us * 1e-6;
    double first_cut = cut;
    expect_added(blocked, peak_mpi_active_clock_blocked_seconds(epoch, &cut), 0, 0);
    assert(fabs(cut - first_cut - 0.001) < 1e-9);
    delayed_reads = 2;
    first_cut = cut;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) == -1.0);
    assert(cut == first_cut);
    expect_added(blocked, observe_at(wall_us, blocked), 0, 0);

    /* A failed fine-clock read is a transient wall cut failure. A failed
     * coarse-clock read means no sampled residence is usable at this cut. */
    cut = epoch + wall_us * 1e-6;
    first_cut = cut;
    fine_failure = true;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) == -1.0);
    assert(cut == first_cut);
    fine_failure = false;
    coarse_available = false;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
    assert(cut == first_cut);
    coarse_available = true;
    expect_added(blocked, observe_at(wall_us, blocked), 0, 0);

    /* Both entry and exit errors latch unavailability even if later clock
     * reads succeed. Reset the packed state to exercise each failure alone. */
    coarse_available = false;
    assert(open_interval() == UINT64_MAX);
    coarse_available = true;
    cut = epoch + wall_us * 1e-6;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);

    atomic_store(&residence_state, 0);
    uint64_t interval = open_interval();
    assert(interval != UINT64_MAX);
    coarse_available = false;
    close_interval(interval);
    coarse_available = true;
    assert(open_interval() == UINT64_MAX);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
}
