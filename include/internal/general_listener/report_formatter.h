#ifndef PEAK_REPORT_FORMATTER_H
#define PEAK_REPORT_FORMATTER_H

/**
 * @file report_formatter.h
 * @brief Render immutable general-listener report snapshots.
 */

#include "internal/general_listener/report_snapshot.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Final-output policy selected after transport aggregation completes. */
typedef struct {
    bool print_text;
    bool truncate_names;
} PeakReportFormatOptions;

/**
 * Write the report CSV selected by `PEAK_STATSLOG_PATH` and the process ID.
 *
 * No file is created when the snapshot has no instrumented calls. Values are
 * rendered exactly as captured; the coordinator decides whether to sanitize
 * the final snapshot before calling this function.
 *
 * @return true when no CSV is needed or the complete CSV was written.
 */
bool peak_report_formatter_write_csv(const PeakReportSnapshot* snapshot);

/**
 * Remove the report CSV selected by `PEAK_STATSLOG_PATH` and the process ID.
 *
 * A missing file is treated as a successful removal.
 */
bool peak_report_formatter_remove_csv(void);

/** Write the stable key/value lines for per-rank maximum overhead owners. */
void peak_report_formatter_write_rank_maxima(
    const PeakReportRankTuple maximum[PEAK_REPORT_METRIC_COUNT],
    const int owner_rank[PEAK_REPORT_METRIC_COUNT]);

/**
 * Write the human-readable report through the runtime logging interface.
 *
 * The report is suppressed when the snapshot contains no reportable data or
 * when @p options disables text output. Values are rendered exactly as
 * captured; the coordinator decides whether to sanitize first.
 */
void peak_report_formatter_write_text(
    const PeakReportSnapshot* snapshot,
    const PeakReportFormatOptions* options);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_REPORT_FORMATTER_H */
