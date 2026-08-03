#include "internal/general_listener/report_snapshot.h"

#include <cassert>
#include <cstdio>

int
main()
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(1);
    PeakReportMaxima maxima;

    assert(snapshot != nullptr);
    assert(peak_report_snapshot_set_program(snapshot, "cxx-link"));
    assert(peak_report_snapshot_set_name(snapshot, 0, "slot"));
    peak_report_maxima_reset(&maxima);
    assert(peak_report_calls_per_active_thread(9, 2) == 5);
    peak_report_snapshot_destroy(snapshot);
    std::puts("report_snapshot_cxx_link_test_ok");
    return 0;
}
