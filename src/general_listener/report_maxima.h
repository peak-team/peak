#ifndef PEAK_REPORT_MAXIMA_H
#define PEAK_REPORT_MAXIMA_H

#include <stdbool.h>
#include <stdint.h>

/** Metrics for which the final report retains a per-rank maximum. */
typedef enum {
    PEAK_REPORT_METRIC_COMBINED = 0,
    PEAK_REPORT_METRIC_PROFILE,
    PEAK_REPORT_METRIC_CONTROL,
    PEAK_REPORT_METRIC_MANAGEMENT,
    PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK,
    PEAK_REPORT_METRIC_CONTROL_RISK,
    PEAK_REPORT_METRIC_COUNT,
} PeakReportMetric;

/** Complete final-report data associated with one rank. */
typedef struct {
    bool accounting_valid;
    unsigned int local_ranks;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
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
} PeakReportRankTuple;

/** Owner-consistent maxima for every final-report metric. */
typedef struct {
    PeakReportRankTuple tuples[PEAK_REPORT_METRIC_COUNT];
    int owner_ranks[PEAK_REPORT_METRIC_COUNT];
    bool present[PEAK_REPORT_METRIC_COUNT];
} PeakReportMaxima;

/** Clears all maxima and marks every metric as absent. */
void peak_report_maxima_reset(PeakReportMaxima* maxima);

/**
 * Resets @p maxima and uses @p candidate as the initial owner tuple.
 *
 * @return true on success; false for a null argument or negative rank.
 */
bool peak_report_maxima_initialize(PeakReportMaxima* maxima,
                                   const PeakReportRankTuple* candidate,
                                   int rank);

/**
 * Considers one rank for all report metrics.
 *
 * A greater metric value wins. Equal values select the lower rank, matching
 * MAXLOC semantics. Whenever a candidate wins, its complete tuple is copied.
 *
 * @return true on success; false for a null argument or negative rank.
 */
bool peak_report_maxima_consider(PeakReportMaxima* maxima,
                                 const PeakReportRankTuple* candidate,
                                 int rank);

/** Returns true when every report metric has an owner tuple. */
bool peak_report_maxima_complete(const PeakReportMaxima* maxima);

/**
 * Loads already-selected owner tuples, such as results from MPI MAXLOC.
 *
 * The destination is left unchanged if an owner rank is negative.
 *
 * @return true on success; false for invalid input.
 */
bool peak_report_maxima_load(
    PeakReportMaxima* maxima,
    const PeakReportRankTuple tuples[PEAK_REPORT_METRIC_COUNT],
    const int owner_ranks[PEAK_REPORT_METRIC_COUNT]);

/**
 * Copies all owner tuples and ranks when @p maxima is complete.
 *
 * The output arrays are left unchanged on failure.
 *
 * @return true on success; false for a null argument or incomplete maxima.
 */
bool peak_report_maxima_assign(
    const PeakReportMaxima* maxima,
    PeakReportRankTuple tuples[PEAK_REPORT_METRIC_COUNT],
    int owner_ranks[PEAK_REPORT_METRIC_COUNT]);

#endif /* PEAK_REPORT_MAXIMA_H */
