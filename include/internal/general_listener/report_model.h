#ifndef PEAK_REPORT_MODEL_H
#define PEAK_REPORT_MODEL_H

#include "internal/general_listener/report_maxima.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Final overhead data consumed by local and aggregated report renderers. */
typedef struct {
    bool valid;
    bool accounting_valid;
    bool per_rank_max;
    unsigned int local_ranks;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
    double elapsed_min_seconds;
    double elapsed_max_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double control_risk_seconds;
    double profile_control_risk_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    double ratio;
    PeakReportMaxima per_rank_maxima;
} PeakReportOverhead;

/** Returns the ceiling of calls divided by active threads, or zero. */
unsigned long peak_report_calls_per_active_thread(
    unsigned long calls,
    unsigned long active_threads);

/**
 * Clamps each exclusive time to the range represented by its total time.
 *
 * Negative totals are preserved because they are invalid upstream evidence;
 * only nonnegative totals act as an upper bound.
 */
void peak_report_sanitize_times(size_t hook_count,
                                double* total_time,
                                double* exclusive_time);

/** Converts final overhead data into the transport-neutral per-rank tuple. */
PeakReportRankTuple peak_report_overhead_rank_tuple(
    const PeakReportOverhead* report);

/** Validates a tuple received from an aggregation transport. */
bool peak_report_rank_tuple_is_valid(const PeakReportRankTuple* tuple);

#endif /* PEAK_REPORT_MODEL_H */
