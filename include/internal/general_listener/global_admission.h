#ifndef PEAK_GLOBAL_ADMISSION_H
#define PEAK_GLOBAL_ADMISSION_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
/* UINT64_MAX denotes an invalid/unrepresentable estimate, never free credit. */
static inline uint64_t peak_global_admission_seconds_ns(double seconds)
{
    if (!isfinite(seconds) || seconds < 0.0) return UINT64_MAX;
    double ns = ceil(seconds * 1e9);
    return isfinite(ns) && ns >= 0.0 && ns < (double)UINT64_MAX
        ? (uint64_t)ns : UINT64_MAX;
}
static inline bool peak_global_admission_claim_ns(uint64_t* headroom,
    uint64_t observation, uint64_t mutation)
{
    if (observation == UINT64_MAX || mutation == UINT64_MAX ||
        mutation > *headroom || observation > *headroom - mutation)
        return false;
    *headroom -= mutation + observation;
    return true;
}
static inline uint64_t peak_global_admission_closing_windows(uint64_t targets)
{
    return targets / 64 + (targets % 64 != 0);
}
static inline double peak_global_admission_closing_forecast(double previous,
    double per_window, uint64_t targets)
{
    return fmax(previous, fmax(.003, per_window) *
        (double)peak_global_admission_closing_windows(targets));
}
#endif /* PEAK_GLOBAL_ADMISSION_H */
