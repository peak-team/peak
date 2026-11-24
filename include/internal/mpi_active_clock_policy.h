#ifndef PEAK_MPI_ACTIVE_CLOCK_POLICY_H
#define PEAK_MPI_ACTIVE_CLOCK_POLICY_H

#include <float.h>
#include <math.h>
#include <stddef.h>

typedef struct {
    double ratio;
    double budget;
    int active;
} PeakMpiActiveClockPolicy;

/* Advisory scaling from rank-local wall time. elapsed and blocking describe
 * the same window. possible_overlap bounds already-charged time that may
 * overlap the measured MPI interval. This observation identifies neither
 * cross-rank dependencies nor independent work within this rank. */
static inline PeakMpiActiveClockPolicy peak_mpi_active_clock_policy(
    double q, double elapsed, double blocking, double possible_overlap)
{
    PeakMpiActiveClockPolicy decision = {.ratio = q};
    if (!(q > 0.0 && q <= 1.0) || !isfinite(elapsed) || elapsed <= 0.0)
        return decision;
    decision.budget = q * elapsed;
    if (!isfinite(blocking) || !isfinite(possible_overlap) ||
        possible_overlap < 0.0 || blocking <= possible_overlap ||
        blocking > elapsed)
        return decision;

    double eligible = (elapsed - blocking) + possible_overlap;
    decision.ratio = q * (eligible / elapsed);
    decision.budget = q * eligible;
    decision.active = 1;
    return decision;
}

/* One tick of uncertainty can restore recent detach allowance up to
 * the lifetime target. Observed active time can justify a higher rate.
 */
static inline double peak_mpi_active_clock_recent_ratio(
    double q, double elapsed, double blocking, double possible_overlap,
    double quantum, double lifetime_ratio)
{
    double measured = peak_mpi_active_clock_policy(
        q, elapsed, blocking, possible_overlap).ratio;
    double upper = peak_mpi_active_clock_policy(
        q, elapsed, fmax(0.0, blocking - quantum), possible_overlap).ratio;
    return fmax(measured, fmin(upper, lifetime_ratio));
}

typedef struct {
    double elapsed, blocked, overlap;
    double sampled_exposure, squared_weight;
} PeakMpiActiveClockRecent;

enum { PEAK_MPI_ACTIVE_CLOCK_RECENT_PROBES = 64 };

static inline double peak_mpi_active_clock_recent_horizon(
    double heartbeat_max, double probe_interval)
{
    /* Fixed heartbeats shorter than the throttle can wait nearly two probe
     * intervals between sampled sleeps. Leave one extra opportunity because
     * slight timing variation can make 64 probes fall just below the target. */
    return (PEAK_MPI_ACTIVE_CLOCK_RECENT_PROBES + 1) *
        fmax(heartbeat_max, 2.0 * probe_interval);
}

/* Keep costs and residence on the same interval. Raw cuts can bridge
 * sampled cuts, without increasing the weight of the sampled observations. */
static inline int peak_mpi_active_clock_recent_add(
    PeakMpiActiveClockRecent* recent, double max_elapsed, double elapsed,
    double blocked, double overlap, double sampled_exposure, unsigned probes)
{
    if (!probes && recent->elapsed == 0.0)
        return 0;
    if (recent->elapsed + elapsed > max_elapsed) {
        *recent = (PeakMpiActiveClockRecent){0};
        return 0;
    }
    recent->elapsed += elapsed;
    recent->blocked += blocked;
    recent->overlap += overlap;
    if (probes) {
        /* Each probe represents an equal share of its sleep exposure. */
        recent->sampled_exposure += sampled_exposure;
        recent->squared_weight += sampled_exposure * sampled_exposure / probes;
        /* Equal-weight probes count one each. Uneven exposure requires more
         * observations so a few unusually long sleeps cannot dominate. */
        double threshold = PEAK_MPI_ACTIVE_CLOCK_RECENT_PROBES * (1.0 - 8.0 * DBL_EPSILON);
        if (recent->squared_weight > 0.0 &&
            recent->sampled_exposure * recent->sampled_exposure >=
                threshold * recent->squared_weight)
            return 1;
    }
    return 0;
}

static inline double peak_mpi_active_clock_recent_sampled_ratio(
    double q, const PeakMpiActiveClockRecent* recent)
{
    return peak_mpi_active_clock_policy(
        q, recent->elapsed, fmin(recent->elapsed, recent->blocked),
        recent->overlap).ratio;
}

/* A missed snapshot carries the preceding allowance in seconds. A negative
 * budget denotes the absence of any valid clock observation. */
static inline double peak_mpi_active_clock_carried_ratio(
    double q, double elapsed, double last_budget)
{
    if (!(q > 0.0 && q <= 1.0) || !isfinite(last_budget) || last_budget < 0.0)
        return q;
    if (!isfinite(elapsed) || elapsed <= 0.0)
        return 0.0;
    return fmin(q, last_budget / elapsed);
}

/* Charge the rank's actual or forecast control windows at raw cost. The
 * caller separately admits the original wall-clock and local-rank risk. */
static inline int peak_mpi_active_clock_reattach_debit(
    double* remaining, double profile, double raw_stop, size_t windows)
{
    double stop = raw_stop * (double)windows;
    if (!isfinite(*remaining) || !isfinite(profile) ||
        !isfinite(stop) || raw_stop < 0.0 || profile < 0.0 ||
        profile > *remaining || stop > *remaining - profile)
        return 0;
    *remaining -= profile;
    *remaining -= stop;
    return 1;
}

#endif /* PEAK_MPI_ACTIVE_CLOCK_POLICY_H */
