#ifndef PEAK_MPI_ACTIVE_CLOCK_POLICY_H
#define PEAK_MPI_ACTIVE_CLOCK_POLICY_H

#include <math.h>

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

#endif /* PEAK_MPI_ACTIVE_CLOCK_POLICY_H */
