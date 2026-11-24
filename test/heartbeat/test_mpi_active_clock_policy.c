#include "internal/mpi_active_clock_policy.h"
#include "internal/mpi_active_clock_sampling.h"

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

    double remaining = second_miss * 12.0;
    assert(peak_mpi_active_clock_reattach_debit(&remaining, 0.05, 0.0, 0));
    assert(!peak_mpi_active_clock_reattach_debit(&remaining, 0.06, 0.0, 0));

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

static void test_recent_quantum_margin(void)
{
    const double q = 0.1;
    const double tick = 0.010;

    /* A heartbeat with one full tick of MPI cannot grant the wall rate. */
    assert(equal(peak_mpi_active_clock_recent_ratio(
        q, tick, tick, 0.0, tick, 0.003), 0.003));
    /* A 19 ms wait still leaves measured credit in a 20 ms window. */
    assert(equal(peak_mpi_active_clock_recent_ratio(
        q, 0.020, 0.019, 0.0, tick, 0.003), 0.005));
    /* A recent wait after compute cannot spend more than one uncertainty tick. */
    assert(equal(peak_mpi_active_clock_recent_ratio(
        q, 0.020, 0.020, 0.0, tick, 0.080), 0.050));
    /* Recent compute after a long wait restores the measured rate immediately. */
    assert(equal(peak_mpi_active_clock_recent_ratio(
        q, tick, 0.0, 0.0, tick, 0.003), q));
    /* Overlapping profiling cost stays in both allowances. */
    assert(equal(peak_mpi_active_clock_recent_ratio(
        q, tick, tick, 0.001, tick, 0.003), 0.010));
}

static void test_phase_estimate_fallback(void)
{
    const double span = 0.125;
    const double raw = 0.0625;

    assert(equal(peak_mpi_active_clock_sampled_seconds(span, raw, 0, 0), raw));
    assert(equal(peak_mpi_active_clock_sampled_seconds(span, raw, 2, 3), raw));
    assert(equal(peak_mpi_active_clock_sampled_seconds(span, 0.0, 8, 0), 0.0));
    assert(equal(peak_mpi_active_clock_sampled_seconds(span, span, 8, 8), span));

    PeakMpiClockResidence residence = {.epoch = 1.0, .now = 1.0, .match_sleep = true};
    PeakMpiClockSampler window = {0};
    /* A small sample supplies evidence for later cuts. Short contradictions
     * cannot retroactively change the residence of a span already counted. */
    window.start = 1.0;
    window.expected_end = 1.0005;
    window.probes = 1;
    window.open_probes = 1;
    assert(peak_mpi_active_clock_accumulate(&residence, &window, window.expected_end, 0.0) == 0.0);
    assert(!window.use_occupancy);
    assert(window.evidence_probes == 1);

    /* Balanced random disagreement around the measured residence never
     * warrants replacing it. */
    window.incomplete = true;
    assert(peak_mpi_active_clock_accumulate(&residence, &window, 1.125, 0.0) == 0.0);
    assert(window.evidence_probes == 1);
    window.incomplete = false;
    for (unsigned i = 0; i < 8; ++i) {
        window.start = 1.125 + i * span;
        window.expected_end = window.start + span;
        window.raw_start = i * raw;
        double cut_raw = window.raw_start + raw;
        window.probes = 8;
        window.open_probes = i % 2 == 0 ? 3 : 5;
        assert(equal(peak_mpi_active_clock_accumulate(
                         &residence, &window, window.expected_end, cut_raw), cut_raw));
        assert(!window.use_occupancy && window.corrected_windows == 0);
    }
}

static void test_phase_residence_accumulation(void)
{
    const double tick = 0.001;
    const double q = 0.1;
    PeakMpiClockResidence residence = {.epoch = 1.0, .now = 1.0, .match_sleep = true};
    PeakMpiClockSampler window = {
        .start = 1.0, .expected_end = 1.125, .probes = 8, .open_probes = 4,
    };
    double blocked = peak_mpi_active_clock_accumulate(&residence, &window, window.expected_end, 0.0);
    assert(equal(blocked, 0.0));
    assert(window.use_occupancy && window.corrected_windows == 0);
    window.start = window.expected_end;
    window.expected_end = 1.25;
    blocked = peak_mpi_active_clock_accumulate(&residence, &window, window.expected_end, 0.0);
    assert(equal(blocked, 0.0625));
    assert(window.corrected_windows == 1);

    /* A window extending beyond the observation cannot adjust that cut. */
    window.start = 1.25;
    window.expected_end = 1.4375;
    const double raw = 0.03125;
    double previous = blocked;
    blocked = peak_mpi_active_clock_accumulate(&residence, &window, 1.375, 0.015625);
    assert(equal(blocked - previous, 0.015625));
    assert(window.use_occupancy && window.corrected_windows == 1);

    /* The missed cut preserves lifetime residence. The next valid cut bills
     * raw time across the gap even if it has an otherwise usable sample. */
    assert(peak_mpi_active_clock_accumulate(&residence, &window, 1.4375, -1.0) == -1.0);
    assert(equal(residence.corrected, blocked));
    assert(!window.use_occupancy && window.evidence_probes == 0);
    window.start = 1.4375;
    window.expected_end = 1.5;
    window.raw_start = 0.0234375;
    previous = blocked;
    blocked = peak_mpi_active_clock_accumulate(&residence, &window, window.expected_end, raw);
    assert(equal(blocked - previous, 0.015625));
    assert(window.evidence_probes == 0 && window.corrected_windows == 1);

    /* New evidence allows a later correction. A subsequent compute span
     * keeps that correction in the lifetime total and restores recent rate. */
    window.start = 1.5;
    window.expected_end = 1.625;
    window.raw_start = raw;
    previous = blocked;
    blocked = peak_mpi_active_clock_accumulate(&residence, &window, window.expected_end, raw);
    assert(equal(blocked, previous));
    assert(window.use_occupancy);
    window.start = window.expected_end;
    window.expected_end = 1.75;
    blocked = peak_mpi_active_clock_accumulate(&residence, &window, window.expected_end, raw);
    assert(equal(blocked - previous, 0.0625));
    assert(window.corrected_windows == 2);

    window.probes = 0;
    previous = blocked;
    blocked = peak_mpi_active_clock_accumulate(&residence, &window, 1.875, raw);
    assert(equal(blocked, previous));
    double lifetime = peak_mpi_active_clock_policy(q, 0.875, blocked, 0.0).ratio;
    assert(lifetime < q);
    assert(equal(peak_mpi_active_clock_recent_ratio(
                     q, 0.125, blocked - previous, 0.0, tick, lifetime), q));
}

static void test_opposed_phase_across_heartbeats(void)
{
    const double tick = 0.001;
    const double q = 0.1;
    const double span = 0.125;
    PeakMpiClockResidence residence = {.epoch = 1.0, .now = 1.0, .match_sleep = true};
    PeakMpiClockSampler window = {.probes = 8, .open_probes = 4};
    double raw = 0.0;
    double blocked = 0.0;

    /* Every heartbeat is half MPI. Alternating ticks still give the right
     * lifetime total in pairs while reporting the wrong recent rate in both. */
    for (unsigned i = 0; i < 12; ++i) {
        window.start = 1.0 + i * span;
        window.expected_end = window.start + span;
        window.raw_start = raw;
        double raw_delta = i % 2 == 0 ? 0.0 : span;
        raw += raw_delta;
        bool correcting = window.use_occupancy;
        double previous = blocked;
        blocked = peak_mpi_active_clock_accumulate(
            &residence, &window, window.expected_end, raw);
        double recent = blocked - previous;
        assert(equal(recent, correcting ? span / 2.0 : raw_delta));
        if (i >= 4) {
            assert(correcting && window.use_occupancy);
            assert(equal(peak_mpi_active_clock_recent_ratio(
                             q, span, recent, 0.0, tick, q / 2.0), q / 2.0));
        }
    }
    assert(fabs(blocked - 12 * span / 2.0) <= span / 2.0);
}

static void test_phase_probe_stratum_boundary(void)
{
    const uint64_t span_ns = 20000000;
    const uint64_t requested_ns = 9950000;
    const double tolerance = 0.0005;
    assert(peak_mpi_active_clock_probe_ontime(
        span_ns, 0, 2, requested_ns, 0.00999, tolerance));
    /* Reads at or beyond the edge are still within the timing tolerance,
     * but belong to the next stratum. */
    assert(!peak_mpi_active_clock_probe_ontime(
        span_ns, 0, 2, requested_ns, 0.01000, tolerance));
    assert(!peak_mpi_active_clock_probe_ontime(
        span_ns, 0, 2, requested_ns, 0.01001, tolerance));
    assert(!peak_mpi_active_clock_probe_ontime(
        span_ns, 1, 2, 10050000, 0.00999, tolerance));
}

static void test_timing_rejections_preserve_phase_evidence(void)
{
    const double span = 0.125;
    const double raw_wait = 0.015625;
    PeakMpiClockResidence residence = {.epoch = 1.0, .now = 1.0, .match_sleep = true};
    PeakMpiClockSampler window = {.probes = 2, .open_probes = 1, .tolerance = 0.0005};
    double raw = 0.0;
    double blocked = 0.0;

    /* After every six valid probes, alternate a partial sample with a late
     * complete sample. No uninterrupted run meets the learning threshold. */
    for (unsigned i = 0; i < 63; ++i) {
        window.start = 1.0 + i * span;
        double cut = window.start + span;
        bool rejected = i % 4 == 3;
        window.incomplete = rejected && i % 8 == 3;
        window.expected_end = rejected && !window.incomplete ? cut - span / 2.0 : cut;
        window.raw_start = raw;
        raw += rejected ? raw_wait : 0.0;
        unsigned evidence = window.evidence_probes;
        uint64_t sampled = window.sampled_windows;
        uint64_t corrected = window.corrected_windows;
        bool correcting = window.use_occupancy;
        double previous = blocked;
        blocked = peak_mpi_active_clock_accumulate(&residence, &window, cut, raw);
        if (rejected) {
            assert(equal(blocked - previous, raw_wait));
            assert(window.evidence_probes == evidence);
            assert(window.sampled_windows == sampled);
            assert(window.corrected_windows == corrected);
            assert(window.use_occupancy == correcting);
        } else {
            assert(equal(blocked - previous, correcting ? span / 2.0 : 0.0));
            if (i >= 8)
                assert(correcting && window.use_occupancy);
        }
    }
    assert(window.corrected_windows > 32);
    assert(window.evidence_probes > 0);
    assert(peak_mpi_active_clock_accumulate(&residence, &window, residence.now + span, -1.0)
           == -1.0);
    assert(window.evidence_probes == 0 && !window.use_occupancy);
    assert(equal(residence.corrected, blocked));
}

static void test_phase_full_sample_bounds_raw_overrun(void)
{
    PeakMpiClockResidence residence = {.epoch = 1.0, .now = 1.5, .match_sleep = true};
    PeakMpiClockSampler window = {
        .start = 1.5, .expected_end = 1.625, .probes = 8, .open_probes = 8,
        .use_occupancy = true,
    };
    /* Prior compute gives the lifetime total room for the overshoot. The
     * sampled window must still charge at most its own elapsed time. */
    assert(equal(peak_mpi_active_clock_accumulate(&residence, &window, 1.625, 0.140625),
                 0.125));
    assert(window.corrected_windows == 1);
}

static void test_phase_selection_adapts_to_workload_change(void)
{
    const double span = 0.125;
    PeakMpiClockResidence residence = {.epoch = 1.0, .now = 1.0, .match_sleep = true};
    PeakMpiClockSampler window = {.probes = 8, .open_probes = 4};
    double raw = 0.0;
    double blocked = 0.0;

    for (unsigned i = 0; i < 96; ++i) {
        /* Half the wall time is truly blocked throughout. The coarse clock
         * first undercounts, recovers, then overcounts after a phase change. */
        double raw_delta = i < 16 ? 0.0 : i < 80 ? span / 2.0 : span;
        window.start = 1.0 + i * span;
        window.expected_end = window.start + span;
        window.raw_start = raw;
        raw += raw_delta;
        bool correcting = window.use_occupancy;
        double previous = blocked;
        blocked = peak_mpi_active_clock_accumulate(
            &residence, &window, window.expected_end, raw);
        assert(equal(blocked - previous, correcting ? span / 2.0 : raw_delta));
        if (i == 15 || i == 95)
            assert(correcting && window.use_occupancy);
        if (i == 79)
            assert(!correcting && !window.use_occupancy);
    }
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
    test_recent_quantum_margin();
    test_phase_estimate_fallback();
    test_phase_residence_accumulation();
    test_opposed_phase_across_heartbeats();
    test_phase_probe_stratum_boundary();
    test_timing_rejections_preserve_phase_evidence();
    test_phase_full_sample_bounds_raw_overrun();
    test_phase_selection_adapts_to_workload_change();
}
