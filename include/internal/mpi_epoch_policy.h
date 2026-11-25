#ifndef PEAK_MPI_EPOCH_POLICY_H
#define PEAK_MPI_EPOCH_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/mpi_active_clock.h"

/* Bind the nomination to the first successful qualified collective. */
static inline bool
peak_mpi_epoch_sample_kind(unsigned* selected, unsigned kind, bool succeeded)
{
    if (*selected == PEAK_MPI_EPOCH_FAILED && succeeded)
        *selected = kind;
    return *selected != PEAK_MPI_EPOCH_FAILED && *selected == kind;
}

/* If profiling adds C to a completed epoch of duration T, its modeled
 * slowdown is C / (T - C). Solve for the observed cost fraction C / T. */
static inline double
peak_mpi_epoch_cost_fraction(double q)
{
    return q / (1.0 + q);
}

typedef struct {
    size_t hook;
    const void* listener;
    uint64_t control;
    uint64_t provider_generation;
} PeakMpiEpochHook;

typedef struct {
    PeakMpiEpochHook nominee;
    double nominated_at;
    double expires;
    double min_calls_per_second;
    double confirmed_at;
    double previous_end;
    unsigned long previous_calls;
    unsigned previous_kind;
    unsigned previous_attempt;
    unsigned corroborations;
    unsigned completed_intervals;
} PeakMpiEpoch;

static inline bool
peak_mpi_epoch_same_hook(PeakMpiEpochHook first, PeakMpiEpochHook second)
{
    return first.listener && first.listener == second.listener &&
           first.hook == second.hook && first.control == second.control &&
           first.provider_generation == second.provider_generation;
}

/* Return whether to disarm. Only adjacent successful boundaries of one
 * collective kind contribute. A missed trylock leaves a gap in attempts. */
static inline bool
peak_mpi_epoch_observe(PeakMpiEpoch* epoch, PeakMpiEpochHook hook, unsigned kind,
                       unsigned attempt, double now, unsigned long calls)
{
    if (!(now > epoch->nominated_at && now <= epoch->expires) ||
        !peak_mpi_epoch_same_hook(epoch->nominee, hook))
        return true;

    if (kind == PEAK_MPI_EPOCH_FAILED || kind != epoch->previous_kind ||
        attempt != epoch->previous_attempt + 1 || !(now > epoch->previous_end) ||
        calls < epoch->previous_calls) {
        epoch->corroborations = 0;
    } else {
        ++epoch->completed_intervals;
        if ((double)(calls - epoch->previous_calls) >
                (now - epoch->previous_end) * epoch->min_calls_per_second &&
            ++epoch->corroborations == 2) {
            epoch->confirmed_at = now;
            return true;
        }
    }

    epoch->previous_kind = kind;
    epoch->previous_end = now;
    epoch->previous_calls = calls;
    epoch->previous_attempt = attempt;
    return attempt == PEAK_MPI_EPOCH_MAX_ATTEMPTS;
}

static inline bool
peak_mpi_epoch_ready(const PeakMpiEpoch* epoch, PeakMpiEpochHook hook, double now)
{
    return epoch->confirmed_at > epoch->nominated_at &&
           now >= epoch->confirmed_at && now <= epoch->expires &&
           peak_mpi_epoch_same_hook(epoch->nominee, hook);
}

#endif /* PEAK_MPI_EPOCH_POLICY_H */
