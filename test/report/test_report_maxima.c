#include "internal/general_listener/report_maxima.h"

#include <stdio.h>
#include <string.h>

static PeakReportRankTuple
fixture_tuple(int rank)
{
    PeakReportRankTuple tuple = {0};

    tuple.accounting_valid = rank != 1;
    tuple.local_ranks = 3U;
    tuple.stop_window_count = 100U + (uint64_t)rank;
    tuple.failed_stop_window_count = 10U + (uint64_t)rank;
    if (rank == 0) {
        tuple.elapsed_seconds = 10.0;
        tuple.profile_seconds = 1.0;
        tuple.control_seconds = 1.0;
        tuple.management_seconds = 0.5;
    } else if (rank == 1) {
        tuple.elapsed_seconds = 20.0;
        tuple.profile_seconds = 6.0;
        tuple.control_seconds = 2.0;
        tuple.management_seconds = 4.0;
    } else {
        tuple.elapsed_seconds = 30.0 + (double)rank;
    }
    tuple.control_risk_seconds =
        tuple.control_seconds * (double)tuple.local_ranks;
    tuple.profile_control_risk_seconds =
        tuple.profile_seconds + tuple.control_risk_seconds;
    tuple.profile_ratio = tuple.profile_seconds / tuple.elapsed_seconds;
    tuple.control_ratio = tuple.control_seconds / tuple.elapsed_seconds;
    tuple.profile_control_risk_ratio =
        tuple.profile_control_risk_seconds / tuple.elapsed_seconds;
    tuple.control_risk_ratio =
        tuple.control_risk_seconds / tuple.elapsed_seconds;
    tuple.management_ratio =
        tuple.management_seconds / tuple.elapsed_seconds;
    tuple.ratio =
        (tuple.profile_seconds + tuple.control_seconds) /
        tuple.elapsed_seconds;
    return tuple;
}

static bool
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
check_distinct_owners_and_assignment(void)
{
    static const int expected_owners[PEAK_REPORT_METRIC_COUNT] = {
        1, 1, 0, 1, 1, 0,
    };
    PeakReportMaxima maxima;
    PeakReportRankTuple rank_zero = fixture_tuple(0);
    PeakReportRankTuple rank_one = fixture_tuple(1);
    PeakReportRankTuple assigned[PEAK_REPORT_METRIC_COUNT] = {{0}};
    int owners[PEAK_REPORT_METRIC_COUNT] = {0};

    if (!peak_report_maxima_initialize(&maxima, &rank_zero, 0) ||
        !peak_report_maxima_consider(&maxima, &rank_one, 1) ||
        !peak_report_maxima_complete(&maxima) ||
        !peak_report_maxima_assign(&maxima, assigned, owners)) {
        return 1;
    }

    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        PeakReportRankTuple expected = fixture_tuple(expected_owners[metric]);

        if (owners[metric] != expected_owners[metric] ||
            !tuple_equal(&assigned[metric], &expected)) {
            return 1;
        }
    }
    return 0;
}

static int
check_tie_prefers_lower_rank_and_copies_tuple(void)
{
    PeakReportMaxima maxima;
    PeakReportRankTuple higher_rank = fixture_tuple(0);
    PeakReportRankTuple lower_rank = higher_rank;
    PeakReportRankTuple later_higher_rank = higher_rank;

    higher_rank.stop_window_count = 700U;
    lower_rank.stop_window_count = 300U;
    lower_rank.accounting_valid = false;
    later_higher_rank.stop_window_count = 500U;

    if (!peak_report_maxima_initialize(&maxima, &higher_rank, 7) ||
        !peak_report_maxima_consider(&maxima, &lower_rank, 3) ||
        !peak_report_maxima_consider(&maxima, &later_higher_rank, 5)) {
        return 1;
    }
    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        if (maxima.owner_ranks[metric] != 3 ||
            !tuple_equal(&maxima.tuples[metric], &lower_rank)) {
            return 1;
        }
    }
    return 0;
}

static int
check_incomplete_assignment(void)
{
    PeakReportMaxima maxima;
    PeakReportRankTuple untouched[PEAK_REPORT_METRIC_COUNT];
    PeakReportRankTuple before[PEAK_REPORT_METRIC_COUNT];
    int owners[PEAK_REPORT_METRIC_COUNT];
    int owners_before[PEAK_REPORT_METRIC_COUNT];

    memset(untouched, 0x5a, sizeof(untouched));
    memcpy(before, untouched, sizeof(before));
    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        owners[metric] = 100 + metric;
        owners_before[metric] = owners[metric];
    }

    peak_report_maxima_reset(&maxima);
    maxima.present[PEAK_REPORT_METRIC_COMBINED] = true;
    if (peak_report_maxima_complete(&maxima) ||
        peak_report_maxima_assign(&maxima, untouched, owners) ||
        memcmp(untouched, before, sizeof(untouched)) != 0 ||
        memcmp(owners, owners_before, sizeof(owners)) != 0) {
        return 1;
    }
    return 0;
}

static int
check_load_selected_tuples(void)
{
    PeakReportMaxima maxima;
    PeakReportRankTuple tuples[PEAK_REPORT_METRIC_COUNT];
    int owners[PEAK_REPORT_METRIC_COUNT];

    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        owners[metric] = metric + 2;
        tuples[metric] = fixture_tuple(owners[metric]);
    }
    if (!peak_report_maxima_load(&maxima, tuples, owners) ||
        !peak_report_maxima_complete(&maxima)) {
        return 1;
    }
    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        if (maxima.owner_ranks[metric] != owners[metric] ||
            !tuple_equal(&maxima.tuples[metric], &tuples[metric])) {
            return 1;
        }
    }

    PeakReportMaxima before = maxima;
    owners[PEAK_REPORT_METRIC_CONTROL] = -1;
    return peak_report_maxima_load(&maxima, tuples, owners) ||
           memcmp(&maxima, &before, sizeof(maxima)) != 0;
}

static int
check_invalid_arguments(void)
{
    PeakReportMaxima maxima;
    PeakReportRankTuple tuple = fixture_tuple(0);

    peak_report_maxima_reset(NULL);
    return peak_report_maxima_initialize(NULL, &tuple, 0) ||
           peak_report_maxima_initialize(&maxima, NULL, 0) ||
           peak_report_maxima_initialize(&maxima, &tuple, -1) ||
           peak_report_maxima_consider(NULL, &tuple, 0) ||
           peak_report_maxima_consider(&maxima, NULL, 0) ||
           peak_report_maxima_consider(&maxima, &tuple, -1) ||
           peak_report_maxima_complete(NULL) ||
           peak_report_maxima_assign(NULL, NULL, NULL);
}

int
main(void)
{
    if (check_distinct_owners_and_assignment() != 0 ||
        check_tie_prefers_lower_rank_and_copies_tuple() != 0 ||
        check_incomplete_assignment() != 0 ||
        check_load_selected_tuples() != 0 ||
        check_invalid_arguments() != 0) {
        fputs("report_maxima_test_failed\n", stderr);
        return 1;
    }

    puts("report_maxima_test_ok");
    return 0;
}
