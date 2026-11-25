#include "internal/general_listener/heartbeat_detach_policy.h"

#include <assert.h>

static void
test_recent_only(void)
{
    OverheadEntry entries[] = {
        {0, 0.085, 0.0000, 0.085},
        {1, 0.012, 0.0001, 0.012},
        {2, 0.006, 0.0060, 0.001},
        {3, 0.006, 0.0055, 0.001},
    };

    peak_heartbeat_sort_detach_entries(entries, 4, true, false);
    assert(entries[0].index == 2);
    assert(entries[1].index == 3);
}

static void
test_lifetime_only(void)
{
    OverheadEntry entries[] = {
        {0, 0.085, 0.085, 0.0001},
        {1, 0.006, 0.001, 0.0060},
        {2, 0.055, 0.055, 0.0060},
    };

    peak_heartbeat_sort_detach_entries(entries, 3, false, true);
    assert(entries[0].index == 1);
    assert(entries[1].index == 2);
}

static void
test_reorder_remaining_entries(bool recent_exceeded)
{
    OverheadEntry entries[] = {
        {0, 0.84, 0.0020, 0.084},
        {1, 0.65, 0.0001, 0.065},
        {2, 0.60, 0.0060, 0.001},
        {3, 0.50, 0.0050, 0.001},
    };

    if (!recent_exceeded) {
        for (size_t i = 0; i < 4; i++) {
            double recent = entries[i].rate;
            entries[i].rate = entries[i].lifetime;
            entries[i].lifetime = recent;
        }
    }

    peak_heartbeat_sort_detach_entries(entries, 4, true, true);
    assert(entries[0].index == 0);
    assert(entries[1].index == 1);

    peak_heartbeat_sort_detach_entries(entries + 1, 3, recent_exceeded, !recent_exceeded);
    assert(entries[0].index == 0);
    assert(entries[1].index == 2);
    assert(entries[2].index == 3);
}

int
main(void)
{
    test_recent_only();
    test_lifetime_only();
    test_reorder_remaining_entries(true);
    test_reorder_remaining_entries(false);
    return 0;
}
