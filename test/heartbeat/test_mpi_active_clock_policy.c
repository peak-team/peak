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

static void test_missed_clock_snapshot(void)
{
    const double q = 0.1;
    assert(equal(peak_mpi_active_clock_carried_ratio(q, 9.0, -1.0), q));

    PeakMpiActiveClockPolicy observed =
        peak_mpi_active_clock_policy(q, 10.0, 9.0, 0.05);
    assert(equal(observed.budget, 0.105));
    double first_miss = peak_mpi_active_clock_carried_ratio(q, 11.0, observed.budget);
    double second_miss = peak_mpi_active_clock_carried_ratio(q, 12.0, observed.budget);
    assert(equal(first_miss * 11.0, observed.budget));
    assert(equal(second_miss * 12.0, observed.budget));
    assert(second_miss < first_miss && first_miss < observed.ratio);

    PeakMpiActiveClockPolicy recovered =
        peak_mpi_active_clock_policy(q, 13.0, 11.0, 0.05);
    assert(equal(recovered.budget, 0.205));
    assert(equal(peak_mpi_active_clock_carried_ratio(q, 14.0, recovered.budget)
                     * 14.0, recovered.budget));

    assert(peak_mpi_active_clock_carried_ratio(q, 11.0, 0.0) == 0.0);
    assert(peak_mpi_active_clock_carried_ratio(q, 0.0, observed.budget) == 0.0);
    assert(peak_mpi_active_clock_carried_ratio(q, NAN, observed.budget) == 0.0);
    assert(equal(peak_mpi_active_clock_carried_ratio(q, 11.0, INFINITY), q));
    assert(equal(peak_mpi_active_clock_carried_ratio(2.0, 11.0, 0.0), 2.0));
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
    test_missed_clock_snapshot();
}
