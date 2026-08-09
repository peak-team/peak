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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional profiler facilities must never make the application unavailable
 * before they have changed application code.  Keep this compact, allocation-
 * free diagnostic in the report model so a completed report records every
 * facility that was deliberately disabled during startup.
 */
typedef enum {
    PEAK_PROFILER_DEGRADED_NONE             = 0,
    PEAK_PROFILER_DEGRADED_HEARTBEAT        = 1u << 0,
    PEAK_PROFILER_DEGRADED_REPORT           = 1u << 1,
    PEAK_PROFILER_DEGRADED_MEMORY_TRACKING  = 1u << 2,
    PEAK_PROFILER_DEGRADED_EXIT_INTERPOSER  = 1u << 3,
} PeakProfilerDegradedMask;

/** Records one non-critical subsystem that was safely disabled. */
void peak_report_snapshot_note_degraded(uint32_t mask, const char* reason);

/** Returns the process-wide immutable-after-set degraded-mode bitset. */
uint32_t peak_report_snapshot_degraded_mask(void);

/** Writes a stable comma-separated reason list to @p buffer. */
void peak_report_snapshot_format_degraded_reasons(char* buffer,
                                                  size_t buffer_size);

/** Formats the supplied captured bitset, rather than live process state. */
void peak_report_snapshot_format_degraded_mask(uint32_t mask,
                                               char* buffer,
                                               size_t buffer_size);

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
    /* Global diagnostics for user callers that had no assignable PEAK slot. */
    uint64_t dropped_calls;
    uint64_t dropped_threads;
    /** Non-critical profiler facilities disabled without mutating user code. */
    uint32_t degraded_mask;
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

/** Stores the latest real local transport context for optional report domains. */
void peak_report_snapshot_set_transport_overhead(const PeakReportOverhead* overhead);

/** Returns the stored context, or a deterministic invalid zero context. */
PeakReportOverhead peak_report_snapshot_get_transport_overhead(void);

/** Releases the snapshot and all memory it owns. */
void peak_report_snapshot_destroy(PeakReportSnapshot* snapshot);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_REPORT_SNAPSHOT_H */
