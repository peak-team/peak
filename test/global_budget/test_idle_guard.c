#define _GNU_SOURCE
static void try_concurrent_publication(void);
#define PEAK_GLOBAL_BUDGET_TEST_IDLE_COMMIT_HOOK try_concurrent_publication
#include "../../src/global_budget.c"
#include <assert.h>
static pthread_mutex_t listener_lock = PTHREAD_MUTEX_INITIALIZER;
static bool pending, blocked;
bool peak_global_budget_listener_idle_try_guard(void)
{
    if (pthread_mutex_trylock(&listener_lock))
        return false;
    if (pending) {
        pthread_mutex_unlock(&listener_lock);
        return false;
    }
    return true;
}
void peak_global_budget_listener_idle_unguard(void)
{
    assert(!pthread_mutex_unlock(&listener_lock));
}
static void try_concurrent_publication(void)
{
    /* Exactly after the physical recheck, the same controller lock prevents
     * a publication from slipping in before the wallet CAS/commit. */
    assert(pthread_mutex_trylock(&listener_lock) == EBUSY);
    blocked = true;
}
int main(void)
{
    atomic_store(&owner, getpid());
    atomic_store(&running, true);
    atomic_store(&available_ns, 20000000);
    atomic_store(&granted_ns, 20000000);
    peak_global_budget_physical_targets(8);
    uint64_t seen = 0, last = 0;
    bool returned = false;
    PhysicalIdleSnapshot physical =
        physical_idle_snapshot(atomic_load(&physical_targets), 8, 1, &seen, &last, &returned);
    assert(idle_return_due(physical, 100000001, last, false, 20000000, 1000000));
    assert(!pthread_mutex_lock(&listener_lock));
    assert(!idle_return_commit(physical, 20000000, 1000000));
    assert(atomic_load(&available_ns) == 20000000);
    assert(!pthread_mutex_unlock(&listener_lock));
    /* A prepaid partial attach can still have the same physical count. */
    pending = true;
    assert(!idle_return_commit(physical, 20000000, 1000000));
    assert(atomic_load(&returned_ns) == 0);
    pending = false;
    assert(idle_return_commit(physical, 20000000, 1000000) == 19000000);
    assert(blocked);
    assert((uint64_t)atomic_load(&available_ns) + atomic_load(&debit_ns) ==
           atomic_load(&granted_ns));
    assert(!pthread_mutex_trylock(&listener_lock));
    assert(!pthread_mutex_unlock(&listener_lock));
    /* New publications after releasing the guard invalidate old snapshots. */
    peak_global_budget_physical_targets(0);
    peak_global_budget_physical_targets(8);
    atomic_fetch_add(&available_ns, 4000000);
    atomic_fetch_add(&granted_ns, 4000000);
    assert(!idle_return_commit(physical, 5000000, 1000000));
    atomic_store(&running, false);
    puts("global_budget_idle_guard_ok");
}
