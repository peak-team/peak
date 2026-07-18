#include "internal/general_listener/report_maxima.h"

#include <string.h>

static double
peak_report_tuple_metric(const PeakReportRankTuple* tuple,
                         PeakReportMetric metric)
{
    switch (metric) {
    case PEAK_REPORT_METRIC_COMBINED:
        return tuple->ratio;
    case PEAK_REPORT_METRIC_PROFILE:
        return tuple->profile_ratio;
    case PEAK_REPORT_METRIC_CONTROL:
        return tuple->control_ratio;
    case PEAK_REPORT_METRIC_MANAGEMENT:
        return tuple->management_ratio;
    case PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK:
        return tuple->profile_control_risk_ratio;
    case PEAK_REPORT_METRIC_CONTROL_RISK:
        return tuple->control_risk_ratio;
    case PEAK_REPORT_METRIC_COUNT:
        break;
    }

    return 0.0;
}

void
peak_report_maxima_reset(PeakReportMaxima* maxima)
{
    if (maxima == NULL) {
        return;
    }

    memset(maxima, 0, sizeof(*maxima));
    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        maxima->owner_ranks[metric] = -1;
    }
}

bool
peak_report_maxima_initialize(PeakReportMaxima* maxima,
                              const PeakReportRankTuple* candidate,
                              int rank)
{
    if (maxima == NULL) {
        return false;
    }

    peak_report_maxima_reset(maxima);
    return peak_report_maxima_consider(maxima, candidate, rank);
}

bool
peak_report_maxima_consider(PeakReportMaxima* maxima,
                            const PeakReportRankTuple* candidate,
                            int rank)
{
    if (maxima == NULL || candidate == NULL || rank < 0) {
        return false;
    }

    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        double candidate_value = peak_report_tuple_metric(
            candidate, (PeakReportMetric)metric);
        double current_value = peak_report_tuple_metric(
            &maxima->tuples[metric], (PeakReportMetric)metric);
        bool replace =
            !maxima->present[metric] || candidate_value > current_value ||
            (candidate_value == current_value &&
             rank < maxima->owner_ranks[metric]);

        if (replace) {
            maxima->tuples[metric] = *candidate;
            maxima->owner_ranks[metric] = rank;
            maxima->present[metric] = true;
        }
    }

    return true;
}

bool
peak_report_maxima_complete(const PeakReportMaxima* maxima)
{
    if (maxima == NULL) {
        return false;
    }

    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        if (!maxima->present[metric]) {
            return false;
        }
    }

    return true;
}

bool
peak_report_maxima_load(
    PeakReportMaxima* maxima,
    const PeakReportRankTuple tuples[PEAK_REPORT_METRIC_COUNT],
    const int owner_ranks[PEAK_REPORT_METRIC_COUNT])
{
    PeakReportMaxima loaded;

    if (maxima == NULL || tuples == NULL || owner_ranks == NULL) {
        return false;
    }
    peak_report_maxima_reset(&loaded);
    for (int metric = 0; metric < PEAK_REPORT_METRIC_COUNT; metric++) {
        if (owner_ranks[metric] < 0) {
            return false;
        }
        loaded.tuples[metric] = tuples[metric];
        loaded.owner_ranks[metric] = owner_ranks[metric];
        loaded.present[metric] = true;
    }
    *maxima = loaded;
    return true;
}

bool
peak_report_maxima_assign(
    const PeakReportMaxima* maxima,
    PeakReportRankTuple tuples[PEAK_REPORT_METRIC_COUNT],
    int owner_ranks[PEAK_REPORT_METRIC_COUNT])
{
    if (tuples == NULL || owner_ranks == NULL ||
        !peak_report_maxima_complete(maxima)) {
        return false;
    }

    memcpy(tuples, maxima->tuples, sizeof(maxima->tuples));
    memcpy(owner_ranks, maxima->owner_ranks, sizeof(maxima->owner_ranks));
    return true;
}
