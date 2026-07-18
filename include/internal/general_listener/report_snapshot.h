#ifndef PEAK_REPORT_SNAPSHOT_H
#define PEAK_REPORT_SNAPSHOT_H

/**
 * @file report_snapshot.h
 * @brief Own immutable data captured for PEAK's final report.
 */

#include "internal/general_listener/report_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Owned final-report data captured from the live listener.
 *
 * The listener may populate and sanitize this object while capture is in
 * progress. After capture, formatters and transports receive it through
 * const-qualified interfaces and treat the complete object as read-only.
 */
typedef struct {
    size_t hook_count;
    char* program;
    char** names;
    int* instrumented;
    int* detached;
    int* reattached;
    int* revisited;
    unsigned long* num_calls;
    double* total_time;
    double* max_total_time;
    double* min_total_time;
    double* exclusive_time;
    float* max_time;
    float* min_time;
    unsigned long* thread_count;
    double overhead_per_call;
    int rank_count;
    PeakReportOverhead overhead;
} PeakReportSnapshot;

/** Allocates an empty owned snapshot for @p hook_count report slots. */
PeakReportSnapshot* peak_report_snapshot_create(size_t hook_count);

/** Copies the application name (`argv[0]`) displayed by the text report. */
bool peak_report_snapshot_set_program(PeakReportSnapshot* snapshot,
                                      const char* program);

/**
 * Copies one slot name into the snapshot.
 *
 * A NULL name is represented by an owned empty string so consumers never need
 * to fall back to live listener metadata.
 */
bool peak_report_snapshot_set_name(PeakReportSnapshot* snapshot,
                                   size_t hook_id,
                                   const char* name);

/** Creates an owned copy, including all report arrays and names. */
PeakReportSnapshot* peak_report_snapshot_clone(
    const PeakReportSnapshot* source);

/** Applies the established report-time timing sanitization in place. */
void peak_report_snapshot_prepare_for_render(PeakReportSnapshot* snapshot);

/** Returns PEAK's established FNV-1a-style identity hash for one slot name. */
uint64_t peak_report_snapshot_slot_identity_hash(
    const PeakReportSnapshot* snapshot,
    size_t hook_id);

/** Returns whether two captured slots have the same nonempty name. */
bool peak_report_snapshot_has_duplicate_names(
    const PeakReportSnapshot* snapshot);

/** Releases the snapshot and all memory it owns. */
void peak_report_snapshot_destroy(PeakReportSnapshot* snapshot);

#endif /* PEAK_REPORT_SNAPSHOT_H */
