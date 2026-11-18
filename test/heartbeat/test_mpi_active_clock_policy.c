#include "internal/mpi_active_clock_policy.h"

#include <assert.h>
#include <float.h>

static int equal(double a, double b)
{
    return fabs(a - b) <= 16.0 * DBL_EPSILON * fmax(1.0, fabs(b));
}

static void expect(double elapsed, double blocking, double overlap,
                   double ratio, double budget, int active)
{
    PeakMpiActiveClockPolicy decision =
        peak_mpi_active_clock_policy(0.1, elapsed, blocking, overlap);
    assert(equal(decision.ratio, ratio));
    assert(equal(decision.budget, budget));
    assert(decision.active == active);
}

static void test_reattach_raw_budget(void)
{
    /* Vista long-chain observation: raw cost fits the active budget while
     * two local-rank-scaled STOP windows fit only the wall-clock budget. */
    const double profile = 0.0009, stop = 0.002315, lease = 0.000003;
    const double active_budget = 0.509, wall_budget = 20.4;
    const size_t local_ranks = 128;
    double active_remaining = active_budget;
    double combined_remaining = active_budget;
    double wall_remaining = wall_budget;
    assert(peak_mpi_active_clock_reattach_debit(
        &active_remaining, profile, stop, 1));
    assert(peak_mpi_active_clock_reattach_debit(
        &combined_remaining, profile, stop * local_ranks, 1));
    assert(!peak_mpi_active_clock_reattach_debit(
        &combined_remaining, lease, stop * local_ranks, 1));
    assert(peak_mpi_active_clock_reattach_debit(
        &wall_remaining, profile, stop * local_ranks, 1));
    assert(peak_mpi_active_clock_reattach_debit(
        &wall_remaining, lease, stop * local_ranks, 1));
    assert(peak_mpi_active_clock_reattach_debit(
        &active_remaining, lease, stop, 1));
    /* Another hook in the same batch consumes its own lease only. */
    assert(peak_mpi_active_clock_reattach_debit(
        &active_remaining, lease, stop, 0));
    assert(equal(active_remaining, active_budget - profile - 2 * stop - 2 * lease));

    /* A refused forecast must not consume headroom. */
    double shortfall = 0.001;
    assert(!peak_mpi_active_clock_reattach_debit(&shortfall, lease, stop, 1));
    assert(!peak_mpi_active_clock_reattach_debit(&shortfall, 0.0, -1.0, 0));
    assert(!peak_mpi_active_clock_reattach_debit(&shortfall, NAN, stop, 1));
    assert(equal(shortfall, 0.001));
}

int main(void)
{
    expect(20.0, 0.0, 0.0, 0.1, 2.0, 0);
    expect(20.0, 15.0, 0.0, 0.025, 0.5, 1);
    expect(20.0, 15.0, 2.0, 0.035, 0.7, 1);
    expect(20.0, 1.0, 2.0, 0.1, 2.0, 0);
    expect(5.0, 3.0, 0.5, 0.05, 0.25, 1);

    /* A rank-local clock cannot distinguish concurrent native waiting from
     * a serial baton. Both partial windows have the same advisory budget. */
    expect(20.0, 15.0, 0.0, 0.025, 0.5, 1);
    /* A fully blocked window adds no budget beyond potential overlap. */
    expect(20.0, 20.0, 0.0, 0.0, 0.0, 1);
    expect(20.0, 20.0, 1.0, 0.005, 0.1, 1);
    expect(20.0, 20.0, 20.0, 0.1, 2.0, 0);
    expect(20.0, 20.0, 21.0, 0.1, 2.0, 0);
    expect(20.0, 21.0, 1.0, 0.1, 2.0, 0);
    expect(20.0, -1.0, 0.0, 0.1, 2.0, 0);
    expect(20.0, 1.0, -1.0, 0.1, 2.0, 0);
    expect(20.0, NAN, 0.0, 0.1, 2.0, 0);
    expect(20.0, 1.0, INFINITY, 0.1, 2.0, 0);
    expect(0.0, 0.0, 0.0, 0.1, 0.0, 0);
    expect(NAN, 0.0, 0.0, 0.1, 0.0, 0);
    test_reattach_raw_budget();
}
