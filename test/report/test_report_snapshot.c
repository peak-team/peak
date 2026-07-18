#include "internal/general_listener/report_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(2);
    PeakReportSnapshot* copy;

    assert(snapshot != NULL);
    assert(snapshot->hook_count == 2);
    assert(snapshot->rank_count == 1);
    assert(peak_report_snapshot_set_program(snapshot, "milc -i input"));
    assert(peak_report_snapshot_set_name(snapshot, 0, "first"));
    assert(peak_report_snapshot_set_name(snapshot, 1, "second"));
    snapshot->instrumented[0] = 1;
    snapshot->detached[0] = 1;
    snapshot->num_calls[0] = 9;
    snapshot->total_time[0] = 2.0;
    snapshot->exclusive_time[0] = 3.0;
    snapshot->overhead.valid = true;
    snapshot->overhead.elapsed_seconds = 4.0;
    snapshot->overhead_per_call = 0.25;

    assert(!peak_report_snapshot_has_duplicate_names(snapshot));
    assert(peak_report_snapshot_slot_identity_hash(snapshot, 0) != 0);
    assert(peak_report_snapshot_slot_identity_hash(snapshot, 2) == 0);

    copy = peak_report_snapshot_clone(snapshot);
    assert(copy != NULL);
    assert(strcmp(copy->program, "milc -i input") == 0);
    assert(copy->program != snapshot->program);
    assert(strcmp(copy->names[0], "first") == 0);
    assert(copy->names[0] != snapshot->names[0]);
    assert(copy->instrumented[0] == 1);
    assert(copy->detached[0] == 1);
    assert(copy->num_calls[0] == 9);
    assert(copy->overhead.valid);
    assert(copy->overhead.elapsed_seconds == 4.0);
    assert(copy->overhead_per_call == 0.25);

    peak_report_snapshot_prepare_for_render(copy);
    assert(strcmp(copy->program, "milc -i input") == 0);
    assert(copy->exclusive_time[0] == 2.0);
    assert(snapshot->exclusive_time[0] == 3.0);
    assert(peak_report_snapshot_set_name(copy, 1, "first"));
    assert(peak_report_snapshot_has_duplicate_names(copy));

    peak_report_snapshot_destroy(copy);
    peak_report_snapshot_destroy(snapshot);
    puts("report_snapshot_test_ok");
    return 0;
}
