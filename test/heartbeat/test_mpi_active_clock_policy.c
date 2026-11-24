#include "internal/mpi_active_clock_policy.h"
#include "internal/mpi_active_clock_sampling.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>

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

#ifndef PEAK_DETACH_TRACE_TRIALS
#define PEAK_DETACH_TRACE_TRIALS 256U
#endif

enum DetachHeartbeat {
    DETACH_FIXED,
    DETACH_VARIABLE,
    DETACH_DOMINANT_SLEEP,
};

enum DetachComparison {
    DETACH_LEGACY,
    DETACH_CURRENT,
    DETACH_EXACT,
    DETACH_COMPARISONS
};

typedef struct {
    unsigned decisions[DETACH_COMPARISONS];
    unsigned trials_with_request[DETACH_COMPARISONS];
    unsigned horizon_requests[DETACH_COMPARISONS];
    unsigned requests_within_two_seconds[DETACH_COMPARISONS];
    unsigned before_change_trials[DETACH_COMPARISONS];
    unsigned cuts_without_applied_sampling;
    unsigned observations;
    double first_request_sum[DETACH_COMPARISONS];
} DetachRun;

static void test_recent_probe_throttle_and_expiry(void)
{
    const uint64_t sleep_ns = UINT64_C(9000000);
    const double elapsed = sleep_ns * 1e-9;
    const double horizon = peak_mpi_active_clock_recent_horizon(
        elapsed, PEAK_MPI_CLOCK_PROBE_INTERVAL_NS * 1e-9);
    PeakMpiActiveClockRecent recent = {0};
    uint64_t next_probe_ns = 0;
    unsigned cuts = 0;
    unsigned observations = 0;
    for (uint64_t start_ns = 0; start_ns * 1e-9 < horizon; start_ns += sleep_ns) {
        unsigned probes = 0;
        if (start_ns >= next_probe_ns) {
            probes = peak_mpi_active_clock_probe_count(sleep_ns);
            next_probe_ns = start_ns + PEAK_MPI_CLOCK_PROBE_INTERVAL_NS;
        }
        ++cuts;
        observations += probes;
        if (peak_mpi_active_clock_recent_add(
                &recent, horizon, elapsed, probes ? elapsed : 0.0,
                elapsed / 20.0, probes ? elapsed : 0.0, probes))
            break;
    }
    assert(cuts == 127 && observations == 64);
    assert(equal(recent.elapsed, cuts * elapsed));
    assert(equal(recent.sampled_exposure, observations * elapsed));
    assert(equal(recent.blocked, observations * elapsed));
    assert(equal(recent.overlap, cuts * elapsed / 20.0));

    recent = (PeakMpiActiveClockRecent){0};
    assert(!peak_mpi_active_clock_recent_add(
        &recent, horizon, elapsed, elapsed, 0.0, elapsed, 1));
    assert(!peak_mpi_active_clock_recent_add(
        &recent, horizon, horizon, 0.0, 0.0, 0.0, 0));
    assert(recent.elapsed == 0.0 && recent.sampled_exposure == 0.0);
}

static void run_detach_trace(DetachRun* result, unsigned seconds, bool transition,
                             enum DetachHeartbeat heartbeat, unsigned stable_mpi_us)
{
    const uint64_t tick_ns = UINT64_C(4000000);
    const uint64_t variable_ticks[] = {25, 5, 13, 25, 9, 3};
    const uint64_t dominant_ticks[] = {100, 3, 3, 3, 3, 3};
    const double q = 0.1, pressure = 0.05, factor = 1.2;
    const uint64_t end_ns = seconds * UINT64_C(1000000000);
    const uint64_t transition_ns = UINT64_C(5000000000);
    const uint64_t begin_scoring_ns = transition ? transition_ns : 0;
    const double tick = tick_ns * 1e-9;
    const double horizon = peak_mpi_active_clock_recent_horizon(
        heartbeat == DETACH_DOMINANT_SLEEP ? 0.4 : 0.1,
        PEAK_MPI_CLOCK_PROBE_INTERVAL_NS * 1e-9);

    for (uint64_t trial = 0; trial < PEAK_DETACH_TRACE_TRIALS; ++trial) {
        /* Use the heartbeat's epoch/PID seed construction and SplitMix64.
         * All three decisions in each trial see the same probe stream. */
        double epoch = 1.0 + trial * 2e-6;
        PeakMpiClockSampler sampler = {
            .random = ((uint64_t)(epoch * 1e9) << 16) ^ (UINT64_C(1000) + trial),
        };
        PeakMpiClockResidence residence = {.epoch = epoch, .now = epoch, .match_sleep = true};
        uint64_t elapsed_ns = 0, truth_ns = 0, raw_ns = 0;
        unsigned beat = 0;
        double corrected = 0.0;
        PeakMpiActiveClockRecent block = {0};
        bool requested[DETACH_COMPARISONS] = {false};
        bool before_change[DETACH_COMPARISONS] = {false};

        while (elapsed_ns < end_ns) {
            unsigned index = beat % (sizeof(variable_ticks) / sizeof(variable_ticks[0]));
            uint64_t ticks = 25;
            if (heartbeat == DETACH_VARIABLE)
                ticks = variable_ticks[index];
            if (heartbeat == DETACH_DOMINANT_SLEEP)
                ticks = dominant_ticks[index];
            uint64_t sleep_ns = tick_ns * ticks;
            if (sleep_ns > end_ns - elapsed_ns)
                sleep_ns = end_ns - elapsed_ns;
            if (transition && elapsed_ns < transition_ns && sleep_ns > transition_ns - elapsed_ns)
                sleep_ns = transition_ns - elapsed_ns;
            uint64_t mpi_ns = (transition && elapsed_ns >= transition_ns
                               ? 3200 : stable_mpi_us) * UINT64_C(1000);
            uint64_t previous_elapsed_ns = elapsed_ns;

            sampler.start = residence.now;
            sampler.raw_start = raw_ns * 1e-9;
            sampler.probes = peak_mpi_active_clock_probe_count(sleep_ns);
            sampler.open_probes = 0;
            for (unsigned probe = 0; probe < sampler.probes; ++probe) {
                uint64_t offset = peak_mpi_active_clock_probe_offset(
                    &sampler, sleep_ns, probe, sampler.probes);
                /* Every short wait sits inside one coarse tick. Full MPI
                 * runs continuously and is charged by the coarse clock. */
                sampler.open_probes += mpi_ns == tick_ns ||
                    (offset % tick_ns >= UINT64_C(200000) &&
                     offset % tick_ns < UINT64_C(200000) + mpi_ns);
            }

            elapsed_ns += sleep_ns;
            truth_ns += (sleep_ns / tick_ns) * mpi_ns;
            raw_ns += mpi_ns == tick_ns ? sleep_ns : 0;
            double now = epoch + elapsed_ns * 1e-9;
            sampler.expected_end = sampler.start + sleep_ns * 1e-9;
            sampler.tolerance = 0.0005;
            double recent_elapsed = now - residence.now;
            double previous = corrected;
            corrected = peak_mpi_active_clock_accumulate(&residence, &sampler, now, raw_ns * 1e-9);
            double elapsed = now - epoch;
            /* The same still-attached work and recorded overlap are fed to
             * every decision so requests cannot change subsequent inputs. */
            double cost = pressure * elapsed;
            double recent_cost = pressure * recent_elapsed;
            double lifetime = peak_mpi_active_clock_policy(q, elapsed, corrected, cost).ratio;
            double exact_lifetime = peak_mpi_active_clock_policy(
                q, elapsed, fmin(truth_ns * 1e-9, elapsed), cost).ratio;
            double recent = corrected - previous;
            double exact_recent = fmin((sleep_ns / tick_ns) * mpi_ns * 1e-9, recent_elapsed);
            double legacy = peak_mpi_active_clock_recent_ratio(
                q, recent_elapsed, fmin(recent, recent_elapsed), recent_cost, tick, lifetime);
            unsigned applied_probes = sampler.applied_sleep_seconds > 0.0 ? sampler.probes : 0;
            double current = applied_probes ? q : legacy;
            if (beat && peak_mpi_active_clock_recent_add(
                    &block, horizon, recent_elapsed, recent, recent_cost,
                    sampler.applied_sleep_seconds, applied_probes)) {
                current = peak_mpi_active_clock_recent_sampled_ratio(q, &block);
                block = (PeakMpiActiveClockRecent){0};
            }
            double target[DETACH_COMPARISONS] = {
                [DETACH_LEGACY] = legacy,
                [DETACH_CURRENT] = current,
                [DETACH_EXACT] = peak_mpi_active_clock_policy(
                    q, recent_elapsed, exact_recent, recent_cost).ratio,
            };
            if (beat == 0) {
                target[DETACH_LEGACY] = q;
                target[DETACH_CURRENT] = q;
                target[DETACH_EXACT] = q;
            }
            ++beat;
            bool scoring = previous_elapsed_ns >= begin_scoring_ns;
            if (scoring) {
                ++result->observations;
                if (sampler.applied_sleep_seconds == 0.0)
                    ++result->cuts_without_applied_sampling;
            }
            for (unsigned variant = 0; variant < DETACH_COMPARISONS; ++variant) {
                double lifetime_target = variant == DETACH_EXACT ? exact_lifetime : lifetime;
                if (pressure <= target[variant] * factor && pressure <= lifetime_target * factor)
                    continue;
                if (!scoring) {
                    if (!before_change[variant]) {
                        before_change[variant] = true;
                        ++result->before_change_trials[variant];
                    }
                    continue;
                }
                ++result->decisions[variant];
                if (elapsed_ns == end_ns)
                    ++result->horizon_requests[variant];
                if (!requested[variant]) {
                    requested[variant] = true;
                    ++result->trials_with_request[variant];
                    uint64_t delay_ns = elapsed_ns - begin_scoring_ns;
                    if (transition && delay_ns <= UINT64_C(2000000000))
                        ++result->requests_within_two_seconds[variant];
                    result->first_request_sum[variant] += delay_ns * 1e-9;
                }
            }
        }
    }
}

static void print_detach_run(const char* label, const DetachRun* result, bool transition)
{
    printf("%s (%u decisions): legacy/current/exact detach counts", label, result->observations);
    for (unsigned i = 0; i < DETACH_COMPARISONS; ++i) {
        double mean = result->trials_with_request[i]
            ? result->first_request_sum[i] / result->trials_with_request[i] : 0.0;
        printf(" %u (%u/%u trials, %u at horizon, %.3fs mean first request",
               result->decisions[i], result->trials_with_request[i], (unsigned)PEAK_DETACH_TRACE_TRIALS,
               result->horizon_requests[i], mean);
        if (transition)
            printf(", %u within 2s, %u before change", result->requests_within_two_seconds[i],
                   result->before_change_trials[i]);
        printf(")");
    }
    printf("; %u cuts without applied occupancy samples\n", result->cuts_without_applied_sampling);
}

static void test_occupancy_recent_detach(void)
{
    DetachRun short_run = {0}, long_run = {0}, variable_run = {0};
    DetachRun transition = {0}, variable_transition = {0};
    DetachRun all_mpi = {0}, all_compute = {0};
    DetachRun dominant_run = {0}, dominant_transition = {0};
    run_detach_trace(&short_run, 5, false, DETACH_FIXED, 1600);
    run_detach_trace(&long_run, 60, false, DETACH_FIXED, 1600);
    run_detach_trace(&variable_run, 60, false, DETACH_VARIABLE, 1600);
    run_detach_trace(&transition, 7, true, DETACH_FIXED, 1600);
    run_detach_trace(&variable_transition, 7, true, DETACH_VARIABLE, 1600);
    run_detach_trace(&all_mpi, 5, false, DETACH_FIXED, 4000);
    run_detach_trace(&all_compute, 5, false, DETACH_FIXED, 0);

    run_detach_trace(&dominant_run, 60, false, DETACH_DOMINANT_SLEEP, 1600);
    run_detach_trace(&dominant_transition, 10, true, DETACH_DOMINANT_SLEEP, 1600);

    print_detach_run("40% MPI, 100ms, 5s", &short_run, false);
    print_detach_run("40% MPI, 100ms, 60s", &long_run, false);
    print_detach_run("40% MPI, variable heartbeat, 60s", &variable_run, false);
    print_detach_run("40% to 80% MPI, 100ms", &transition, true);
    print_detach_run("40% to 80% MPI, variable heartbeat", &variable_transition, true);
    print_detach_run("40% MPI, dominant 400ms heartbeat, 60s", &dominant_run, false);
    print_detach_run("40% to 80% MPI, dominant 400ms heartbeat", &dominant_transition, true);
    print_detach_run("100% MPI, 100ms", &all_mpi, false);
    print_detach_run("0% MPI, 100ms", &all_compute, false);
    assert(short_run.trials_with_request[DETACH_LEGACY] > PEAK_DETACH_TRACE_TRIALS / 2);
    assert(long_run.trials_with_request[DETACH_LEGACY] == PEAK_DETACH_TRACE_TRIALS);
    const DetachRun* stable[] = {&short_run, &long_run, &variable_run, &dominant_run};
    for (size_t i = 0; i < sizeof(stable) / sizeof(stable[0]); ++i) {
        assert(stable[i]->decisions[DETACH_EXACT] == 0);
        assert(stable[i]->trials_with_request[DETACH_CURRENT] <= PEAK_DETACH_TRACE_TRIALS / 25);
    }
    const DetachRun* changing[] = {&transition, &variable_transition, &dominant_transition};
    for (size_t i = 0; i < sizeof(changing) / sizeof(changing[0]); ++i) {
        assert(changing[i]->decisions[DETACH_EXACT] == changing[i]->observations);
        assert(changing[i]->cuts_without_applied_sampling == 0);
        assert(changing[i]->trials_with_request[DETACH_CURRENT] >= PEAK_DETACH_TRACE_TRIALS * 9 / 10);
    }
    assert(transition.requests_within_two_seconds[DETACH_CURRENT] == PEAK_DETACH_TRACE_TRIALS);
    assert(variable_transition.requests_within_two_seconds[DETACH_CURRENT] == PEAK_DETACH_TRACE_TRIALS);
    for (unsigned variant = 0; variant < DETACH_COMPARISONS; ++variant) {
        assert(all_mpi.decisions[variant] == all_mpi.observations);
        assert(all_compute.decisions[variant] == 0);
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
    test_recent_probe_throttle_and_expiry();
    test_occupancy_recent_detach();
}
