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
 * Write an aggregate report CSV selected by `PEAK_STATSLOG_PATH` and the
 * process ID.
 *
 * No file is created when the snapshot has no instrumented calls. Values are
 * rendered exactly as captured; the coordinator decides whether to sanitize
 * the final snapshot before calling this function. The complete CSV is first
 * written to a unique same-directory temporary file and atomically published,
 * so the final pathname never exposes a partial write. A failed update leaves
 * any existing completed CSV unchanged. Newly created files use mode `0666`
 * filtered by the process umask.
 *
 * @return true when no CSV is needed or the complete CSV was written.
 */
bool peak_report_formatter_write_csv(const PeakReportSnapshot* snapshot);

/**
 * Write a rank-local report CSV without cross-node PID collisions.
 *
 * A multi-rank launcher job retains the aggregate-compatible `-pPID` portion
 * and appends a validated `-rRANK` suffix. When the launcher reports a
 * multi-rank job but no valid rank, a sanitized hostname suffix is used. A
 * single-process job keeps the aggregate-compatible filename. Publication is
 * atomic with the same guarantees as peak_report_formatter_write_csv().
 *
 * @return true when no CSV is needed or the complete CSV was written.
 */
bool peak_report_formatter_write_rank_local_csv(
    const PeakReportSnapshot* snapshot);

/** Write the stable key/value lines for per-rank maximum overhead owners. */
void peak_report_formatter_write_rank_maxima(
    const PeakReportRankTuple maximum[PEAK_REPORT_METRIC_COUNT],
    const int owner_rank[PEAK_REPORT_METRIC_COUNT]);

/**
 * Write and flush the human-readable report through the runtime logging
 * interface.
 *
 * The report is suppressed when the snapshot contains no reportable data or
 * when @p options disables text output. Values are rendered exactly as
 * captured; the coordinator decides whether to sanitize first. A suppressed
 * report is treated as successfully complete.
 *
 * @return true when no text is needed or the complete report was flushed;
 *         false when the output stream reports an error.
 */
bool peak_report_formatter_write_text(
    const PeakReportSnapshot* snapshot,
    const PeakReportFormatOptions* options);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_REPORT_FORMATTER_H */
