/* Exercise the writer and reader with deterministic wall times and no MPI run. */
#define clock_gettime fake_clock_gettime
#include "../../src/mpi_active_clock.c"
#undef clock_gettime

#include <assert.h>

static const double epoch = 1.0;
static int wall_ms;
static unsigned clock_reads;

int
fake_clock_gettime(clockid_t clock_id, struct timespec* time)
{
    assert(clock_id == CLOCK_MONOTONIC);
    assert(wall_ms >= 0 && wall_ms < 1000);
    ++clock_reads;
    time->tv_sec = 1;
    time->tv_nsec = wall_ms * 1000000L;
    return 0;
}

static void
boundary_at(int at_ms)
{
    wall_ms = at_ms;
    publish_boundary();
}

static void
expect_cut(int before_ms, int cut_ms, unsigned expected_us,
           int blocked_ms, bool expected_saturation)
{
    wall_ms = cut_ms;
    double now = epoch + before_ms * 0.001;
    bool saturated;
    double blocked =
        peak_mpi_active_clock_blocked_seconds(epoch, &now, expected_us, &saturated);
    assert(saturated == expected_saturation);
    assert(fabs(blocked - blocked_ms * 0.001) < 1e-9);
    assert(fabs(now - (epoch + cut_ms * 0.001)) < 1e-9);
}

int
main(void)
{
    /* Supply admission state directly. The launcher test covers real MPI init. */
    clock_epoch = epoch;
    last_cut_ns = UINT64_C(1000000000);
    last_limit = BOUNDARIES_PER_HEARTBEAT;
    atomic_store(&edge_limit, last_limit);
    atomic_store(&clock_owner, getpid());

    expect_cut(10, 10, 20000, 0, false);

    /* Both cut endpoints are outside, but the edge changes. Only the bounded
     * prefix gets timestamps, and a late cut credits at most one expected
     * sleep after the prefix. */
    unsigned previous_reads = clock_reads;
    for (int i = 1; i <= 50; ++i)
        boundary_at(10 + i);
    assert(clock_reads - previous_reads == BOUNDARIES_PER_HEARTBEAT);
    expect_cut(200, 220, 20000, 182, true);

    /* An unchanged outside boundary confirms active time and lets the rank
     * earn more budget without calling MPI again. */
    expect_cut(240, 240, 20000, 182, false);

    /* Both waits share one heartbeat window. The adjacent boundaries between
     * the waits retain the 15ms gap independently of the time of the cut. */
    boundary_at(250);
    boundary_at(290);
    boundary_at(305);
    expect_cut(310, 310, 20000, 227, false);
    expect_cut(340, 340, 20000, 257, false);
    boundary_at(350);
    expect_cut(365, 365, 20000, 267, false);
    expect_cut(380, 380, 20000, 267, false);
}
