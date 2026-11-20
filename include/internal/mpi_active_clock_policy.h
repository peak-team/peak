#ifndef PEAK_MPI_ACTIVE_CLOCK_POLICY_H
#define PEAK_MPI_ACTIVE_CLOCK_POLICY_H

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
