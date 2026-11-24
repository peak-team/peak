#ifndef PEAK_MPI_ACTIVE_CLOCK_SAMPLING_H
#define PEAK_MPI_ACTIVE_CLOCK_SAMPLING_H

#include <stdbool.h>
#include <math.h>
#include <stdint.h>

#define PEAK_MPI_CLOCK_PROBE_INTERVAL_NS UINT64_C(10000000)
#define PEAK_MPI_CLOCK_MAX_PROBES 8U

typedef struct PeakMpiClockSampler {
    uint64_t random;
    double next_probe_sleep;
    double start, raw_start;
    double expected_end, tolerance;
    unsigned probes, open_probes;
    bool incomplete;
    bool use_occupancy;
    unsigned evidence_probes;
    double evidence_error[2], evidence_variance;
    uint64_t attempted_probes, sampled_windows, corrected_windows;
    double sampled_seconds, sampled_residence, sampled_raw, occupancy_seconds;
} PeakMpiClockSampler;

static inline unsigned peak_mpi_active_clock_probe_count(uint64_t sleep_ns)
{
    uint64_t probes = sleep_ns / PEAK_MPI_CLOCK_PROBE_INTERVAL_NS;
    if (probes > PEAK_MPI_CLOCK_MAX_PROBES)
        return PEAK_MPI_CLOCK_MAX_PROBES;
    return probes || !sleep_ns ? (unsigned)probes : 1U;
}

static inline uint64_t peak_mpi_active_clock_stratum_edge(
    uint64_t sleep_ns, unsigned i, unsigned probes)
{
    return sleep_ns * i / probes;
}

static inline uint64_t peak_mpi_active_clock_probe_offset(
    PeakMpiClockSampler* sampler, uint64_t sleep_ns, unsigned i, unsigned probes)
{
    /* SplitMix64, local to the admitted heartbeat thread. */
    uint64_t bits = (sampler->random += UINT64_C(0x9e3779b97f4a7c15));
    bits = (bits ^ (bits >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    bits = (bits ^ (bits >> 27)) * UINT64_C(0x94d049bb133111eb);
    bits ^= bits >> 31;
    uint64_t left = peak_mpi_active_clock_stratum_edge(sleep_ns, i, probes);
    uint64_t width = peak_mpi_active_clock_stratum_edge(sleep_ns, i + 1, probes) - left;
    double unit = (double)(bits >> 11) * 0x1p-53;
    return left + (uint64_t)(unit * (double)width);
}

static inline bool peak_mpi_active_clock_probe_ontime(
    uint64_t sleep_ns, unsigned i, unsigned probes, uint64_t requested_ns,
    double observed, double tolerance)
{
    double left = peak_mpi_active_clock_stratum_edge(sleep_ns, i, probes) * 1e-9;
    double right = peak_mpi_active_clock_stratum_edge(sleep_ns, i + 1, probes) * 1e-9;
    return observed >= left && observed < right &&
        fabs(observed - requested_ns * 1e-9) <= tolerance;
}

static inline double peak_mpi_active_clock_sampled_seconds(
    double elapsed, double raw, unsigned probes, unsigned open_probes)
{
    if (elapsed <= 0.0)
        return 0.0;
    if (!probes || open_probes > probes)
        return fmin(elapsed, fmax(0.0, raw));
    return elapsed * ((double)open_probes / probes);
}

static inline void peak_mpi_active_clock_clear_evidence(PeakMpiClockSampler* sampler)
{
    sampler->evidence_probes = 0;
    sampler->evidence_error[0] = 0.0;
    sampler->evidence_error[1] = 0.0;
    sampler->evidence_variance = 0.0;
}

static inline void peak_mpi_active_clock_reset_evidence(PeakMpiClockSampler* sampler)
{
    sampler->use_occupancy = false;
    peak_mpi_active_clock_clear_evidence(sampler);
}

static inline void peak_mpi_active_clock_add_evidence(
    PeakMpiClockSampler* sampler, double elapsed, double coarse, double sampled)
{
    enum { MIN_PROBES = 8, EVIDENCE_DECAY_PROBES = 64 };
    double probability = coarse / elapsed;
    /* Age past evidence in proportion to new independent probes. A missed
     * sleep adds no evidence and a new decision affects only future sleeps. */
    double weight = 1.0 - (double)sampler->probes / EVIDENCE_DECAY_PROBES;
    sampler->evidence_error[0] *= weight;
    sampler->evidence_error[1] *= weight;
    sampler->evidence_error[probability >= 0.5] += sampled - coarse;
    sampler->evidence_variance = weight * weight * sampler->evidence_variance +
        elapsed * elapsed *
        probability * (1.0 - probability) / sampler->probes;
    if (sampler->evidence_probes < MIN_PROBES)
        sampler->evidence_probes += sampler->probes;
    if (sampler->evidence_probes < MIN_PROBES)
        return;

    /* Keep opposite low/high duty errors visible. Allow for both sign
     * combinations in the noise guard. */
    double error = fabs(sampler->evidence_error[0]) + fabs(sampler->evidence_error[1]);
    double margin = elapsed / (8.0 * sampler->probes);
    sampler->use_occupancy = error > 2.25 * sqrt(sampler->evidence_variance) + margin;
}

static inline double peak_mpi_active_clock_sampled_delta(
    PeakMpiClockSampler* sampler, double previous_now, double previous_raw,
    double now, double raw)
{
    double delta = raw - previous_raw;
    /* A timing miss cannot change the evidence from earlier valid sleeps. */
    if (!sampler->probes || sampler->incomplete)
        return delta;
    if (sampler->start < previous_now || now <= sampler->start ||
        sampler->raw_start < previous_raw || sampler->raw_start > raw) {
        peak_mpi_active_clock_reset_evidence(sampler);
        return delta;
    }
    if (fabs(now - sampler->expected_end) > sampler->tolerance)
        return delta;

    double elapsed = now - sampler->start;
    double raw_sleep = raw - sampler->raw_start;
    double coarse = fmin(elapsed, raw_sleep);
    double sampled = peak_mpi_active_clock_sampled_seconds(
        elapsed, raw_sleep, sampler->probes, sampler->open_probes);
    bool use_occupancy = sampler->use_occupancy;
    peak_mpi_active_clock_add_evidence(sampler, elapsed, coarse, sampled);
    sampler->sampled_windows++;
    sampler->sampled_seconds += elapsed;
    sampler->sampled_residence += sampled;
    sampler->sampled_raw += raw_sleep;
    if (!use_occupancy)
        return delta;
    sampler->occupancy_seconds += elapsed;
    if (sampled == raw_sleep)
        return delta;
    sampler->corrected_windows++;
    return delta - raw_sleep + sampled;
}

typedef struct {
    double epoch, now, raw, corrected;
    bool match_sleep;
} PeakMpiClockResidence;

static inline double peak_mpi_active_clock_accumulate(
    PeakMpiClockResidence* residence, PeakMpiClockSampler* sampler,
    double now, double raw)
{
    bool raw_regressed = raw < residence->raw;
    if (raw < 0.0 || raw_regressed)
        peak_mpi_active_clock_reset_evidence(sampler);
    if (raw < 0.0) {
        residence->match_sleep = false;
        return raw;
    }

    /* Preserve coarse deltas outside replaced spans, including quantized
     * steps larger than a heartbeat. Keep the raw high-water mark through
     * a regression so a later cut cannot bill it again. */
    double delta = residence->match_sleep && !raw_regressed
        ? peak_mpi_active_clock_sampled_delta(
            sampler, residence->now, residence->raw, now, raw)
        : raw - residence->raw;
    residence->corrected = fmin(residence->corrected + fmax(0.0, delta),
                                now - residence->epoch);
    residence->now = now;
    residence->raw = fmax(residence->raw, raw);
    residence->match_sleep = true;
    return residence->corrected;
}

#endif /* PEAK_MPI_ACTIVE_CLOCK_SAMPLING_H */
