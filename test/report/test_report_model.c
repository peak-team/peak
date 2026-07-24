#include "internal/general_listener/report_model.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int
double_array_equal(const double* actual,
                   const double* expected,
                   size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (actual[i] != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static int
check_calls_per_active_thread(void)
{
    return peak_report_calls_per_active_thread(0, 0) != 0 ||
           peak_report_calls_per_active_thread(5, 0) != 0 ||
           peak_report_calls_per_active_thread(0, 3) != 0 ||
           peak_report_calls_per_active_thread(6, 3) != 2 ||
           peak_report_calls_per_active_thread(7, 3) != 3;
}

static int
check_time_sanitization(void)
{
    double total[] = {1.0, 2.0, -1.0, 4.0};
    double exclusive[] = {-0.5, 3.0, 5.0, 2.0};
    const double expected[] = {0.0, 2.0, 5.0, 2.0};

    peak_report_sanitize_times(4, total, exclusive);
    if (!double_array_equal(exclusive, expected, 4)) {
        return 1;
    }

    peak_report_sanitize_times(4, NULL, exclusive);
    peak_report_sanitize_times(4, total, NULL);
    return !double_array_equal(exclusive, expected, 4);
}

static PeakReportOverhead
fixture_overhead(void)
{
    PeakReportOverhead report = {0};

    report.accounting_valid = true;
    report.local_ranks = 3U;
    report.stop_window_count = 4U;
    report.failed_stop_window_count = 5U;
    report.elapsed_seconds = 6.0;
    report.profile_seconds = 7.0;
    report.control_seconds = 8.0;
    report.management_seconds = 9.0;
    report.control_risk_seconds = 10.0;
    report.profile_control_risk_seconds = 11.0;
    report.profile_ratio = 12.0;
    report.control_ratio = 13.0;
    report.profile_control_risk_ratio = 14.0;
    report.control_risk_ratio = 15.0;
    report.management_ratio = 16.0;
    report.ratio = 17.0;
    return report;
}

static int
tuple_equal(const PeakReportRankTuple* actual,
            const PeakReportRankTuple* expected)
{
    return actual->accounting_valid == expected->accounting_valid &&
           actual->local_ranks == expected->local_ranks &&
           actual->stop_window_count == expected->stop_window_count &&
           actual->failed_stop_window_count ==
               expected->failed_stop_window_count &&
           actual->elapsed_seconds == expected->elapsed_seconds &&
           actual->profile_seconds == expected->profile_seconds &&
           actual->control_seconds == expected->control_seconds &&
           actual->management_seconds == expected->management_seconds &&
           actual->control_risk_seconds == expected->control_risk_seconds &&
           actual->profile_control_risk_seconds ==
               expected->profile_control_risk_seconds &&
           actual->profile_ratio == expected->profile_ratio &&
           actual->control_ratio == expected->control_ratio &&
           actual->profile_control_risk_ratio ==
               expected->profile_control_risk_ratio &&
           actual->control_risk_ratio == expected->control_risk_ratio &&
           actual->management_ratio == expected->management_ratio &&
           actual->ratio == expected->ratio;
}

static int
check_tuple_conversion(void)
{
    PeakReportOverhead report = fixture_overhead();
    PeakReportRankTuple tuple = peak_report_overhead_rank_tuple(&report);
    PeakReportRankTuple zero = peak_report_overhead_rank_tuple(NULL);
    PeakReportRankTuple expected_zero = {0};

    return !tuple.accounting_valid || tuple.local_ranks != 3U ||
           tuple.stop_window_count != 4U ||
           tuple.failed_stop_window_count != 5U ||
           tuple.elapsed_seconds != 6.0 ||
           tuple.profile_seconds != 7.0 ||
           tuple.control_seconds != 8.0 ||
           tuple.management_seconds != 9.0 ||
           tuple.control_risk_seconds != 10.0 ||
           tuple.profile_control_risk_seconds != 11.0 ||
           tuple.profile_ratio != 12.0 ||
           tuple.control_ratio != 13.0 ||
           tuple.profile_control_risk_ratio != 14.0 ||
           tuple.control_risk_ratio != 15.0 ||
           tuple.management_ratio != 16.0 || tuple.ratio != 17.0 ||
           !tuple_equal(&zero, &expected_zero);
}

static PeakReportRankTuple
valid_tuple(void)
{
    PeakReportRankTuple tuple = {0};

    tuple.local_ranks = 1U;
    tuple.elapsed_seconds = 1.0;
    return tuple;
}

static int
check_tuple_validation(void)
{
    PeakReportRankTuple tuple = valid_tuple();

    if (!peak_report_rank_tuple_is_valid(&tuple) ||
        peak_report_rank_tuple_is_valid(NULL)) {
        return 1;
    }

    tuple.local_ranks = 0U;
    if (peak_report_rank_tuple_is_valid(&tuple)) {
        return 1;
    }
    tuple = valid_tuple();
    tuple.failed_stop_window_count = UINT64_MAX;
    if (peak_report_rank_tuple_is_valid(&tuple)) {
        return 1;
    }
    tuple = valid_tuple();
    tuple.elapsed_seconds = 0.0;
    if (peak_report_rank_tuple_is_valid(&tuple)) {
        return 1;
    }
    tuple = valid_tuple();
    tuple.profile_seconds = -1.0;
    if (peak_report_rank_tuple_is_valid(&tuple)) {
        return 1;
    }
    tuple = valid_tuple();
    tuple.control_ratio = NAN;
    if (peak_report_rank_tuple_is_valid(&tuple)) {
        return 1;
    }
    tuple = valid_tuple();
    tuple.management_seconds = INFINITY;
    return peak_report_rank_tuple_is_valid(&tuple);
}

int
main(void)
{
    if (check_calls_per_active_thread() ||
        check_time_sanitization() ||
        check_tuple_conversion() ||
        check_tuple_validation()) {
        fputs("report_model_test_failed\n", stderr);
        return 1;
    }

    puts("report_model_test_ok");
    return 0;
}
