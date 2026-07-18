#include "internal/general_listener/report_model.h"

#include <float.h>
#include <stdint.h>

static bool
peak_report_positive_finite(double value)
{
    return value > 0.0 && value == value && value <= DBL_MAX;
}

static bool
peak_report_nonnegative_finite(double value)
{
    return value >= 0.0 && value == value && value <= DBL_MAX;
}

unsigned long
peak_report_calls_per_active_thread(unsigned long calls,
                                    unsigned long active_threads)
{
    if (calls == 0 || active_threads == 0) {
        return 0;
    }
    return calls / active_threads +
           (calls % active_threads != 0 ? 1 : 0);
}

void
peak_report_sanitize_times(size_t hook_count,
                           double* total_time,
                           double* exclusive_time)
{
    if (total_time == NULL || exclusive_time == NULL) {
        return;
    }

    for (size_t i = 0; i < hook_count; i++) {
        if (exclusive_time[i] < 0.0) {
            exclusive_time[i] = 0.0;
        }
        if (total_time[i] >= 0.0 && exclusive_time[i] > total_time[i]) {
            exclusive_time[i] = total_time[i];
        }
    }
}

PeakReportRankTuple
peak_report_overhead_rank_tuple(const PeakReportOverhead* report)
{
    PeakReportRankTuple tuple = {0};

    if (report == NULL) {
        return tuple;
    }
    tuple.accounting_valid = report->accounting_valid;
    tuple.local_ranks = report->local_ranks;
    tuple.stop_window_count = report->stop_window_count;
    tuple.failed_stop_window_count = report->failed_stop_window_count;
    tuple.elapsed_seconds = report->elapsed_seconds;
    tuple.profile_seconds = report->profile_seconds;
    tuple.control_seconds = report->control_seconds;
    tuple.management_seconds = report->management_seconds;
    tuple.control_risk_seconds = report->control_risk_seconds;
    tuple.profile_control_risk_seconds =
        report->profile_control_risk_seconds;
    tuple.profile_ratio = report->profile_ratio;
    tuple.control_ratio = report->control_ratio;
    tuple.profile_control_risk_ratio = report->profile_control_risk_ratio;
    tuple.control_risk_ratio = report->control_risk_ratio;
    tuple.management_ratio = report->management_ratio;
    tuple.ratio = report->ratio;
    return tuple;
}

bool
peak_report_rank_tuple_is_valid(const PeakReportRankTuple* tuple)
{
    return tuple != NULL &&
           tuple->local_ranks > 0U &&
           tuple->failed_stop_window_count != UINT64_MAX &&
           peak_report_positive_finite(tuple->elapsed_seconds) &&
           peak_report_nonnegative_finite(tuple->profile_seconds) &&
           peak_report_nonnegative_finite(tuple->control_seconds) &&
           peak_report_nonnegative_finite(tuple->management_seconds) &&
           peak_report_nonnegative_finite(tuple->control_risk_seconds) &&
           peak_report_nonnegative_finite(
               tuple->profile_control_risk_seconds) &&
           peak_report_nonnegative_finite(tuple->profile_ratio) &&
           peak_report_nonnegative_finite(tuple->control_ratio) &&
           peak_report_nonnegative_finite(
               tuple->profile_control_risk_ratio) &&
           peak_report_nonnegative_finite(tuple->control_risk_ratio) &&
           peak_report_nonnegative_finite(tuple->management_ratio) &&
           peak_report_nonnegative_finite(tuple->ratio);
}
