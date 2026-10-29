#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static bool return_surplus(PhysicalIdleSnapshot physical, uint64_t now, uint64_t last,
                           bool returned)
{
    int64_t balance = atomic_load(&available_ns);
    uint64_t keep = 1000000;
    if (!idle_return_due(physical, now, last, returned, balance, keep))
        return false;
    return idle_return_commit(physical, balance, keep) != 0;
}
int main(void)
{
    atomic_store(&owner, getpid());
    atomic_store(&running, true);
    atomic_store(&available_ns, 20000000);
    atomic_store(&granted_ns, 20000000);
    uint64_t seen = 0, last = 0;
    bool returned = false;
    peak_global_budget_physical_targets(0);
    PhysicalIdleSnapshot snapshot = physical_idle_snapshot(atomic_load(&physical_targets), 0,
                                                           400000000, &seen, &last, &returned);
    assert(!snapshot.attached);
    /* Credit was issued 400ms ago while detached. Installing now must begin
     * the original idle interval, instead of immediately returning its lease. */
    peak_global_budget_physical_targets(16);
    snapshot = physical_idle_snapshot(atomic_load(&physical_targets), 0, 400000000, &seen, &last,
                                      &returned);
    assert(snapshot.attached && last == 400000000);
    assert(!return_surplus(snapshot, 400000000, last, returned));
    assert(!return_surplus(snapshot, 499999999, last, returned));
    assert(return_surplus(snapshot, 500000000, last, returned));
    assert(atomic_load(&available_ns) == 1000000 && atomic_load(&returned_ns) == 19000000);
    assert((uint64_t)atomic_load(&available_ns) + atomic_load(&debit_ns) ==
           atomic_load(&granted_ns));
    returned = true;
    /* Service misses both the close and restore publications; same count,
     * different revision still resets activity and invalidates old returns. */
    PhysicalIdleSnapshot old = snapshot;
    peak_global_budget_physical_targets(0);
    peak_global_budget_physical_targets(16);
    assert(!return_surplus(old, 900000000, last, false));
    snapshot = physical_idle_snapshot(atomic_load(&physical_targets), 99, 900000000, &seen, &last,
                                      &returned);
    assert(snapshot.attached && snapshot.word != old.word && last == 900000000 && !returned);
    atomic_fetch_add(&available_ns, 4000000);
    atomic_fetch_add(&granted_ns, 4000000);
    assert(!return_surplus(snapshot, 999999999, last, returned));
    assert(return_surplus(snapshot, 1000000000, last, returned));
    assert((uint64_t)atomic_load(&available_ns) + atomic_load(&debit_ns) ==
           atomic_load(&granted_ns));
    uint64_t saved_seen = seen, saved_last = last;
    PhysicalIdleSnapshot invalid =
        physical_idle_snapshot(atomic_load(&physical_targets), 99, 0, &seen, &last, &returned);
    assert(!invalid.attached && seen == saved_seen && last == saved_last);
    invalid = physical_idle_snapshot(atomic_load(&physical_targets), 99, saved_last - 1, &seen,
                                     &last, &returned);
    assert(!invalid.attached && seen == saved_seen && last == saved_last);
    atomic_store(&local_retiring, true);
    snapshot = physical_idle_snapshot(atomic_load(&physical_targets), 99, 1200000000, &seen, &last,
                                      &returned);
    assert(!snapshot.attached);
    atomic_store(&running, false);
    puts("global_budget_physical_idle_epoch_ok");
}
