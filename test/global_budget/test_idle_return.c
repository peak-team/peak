#define _GNU_SOURCE
static void publication_during_return(void);
#define PEAK_GLOBAL_BUDGET_TEST_IDLE_COMMIT_HOOK publication_during_return
#include "../../src/global_budget.c"
#include <assert.h>

static pthread_mutex_t controller_lock = PTHREAD_MUTEX_INITIALIZER;
static bool mutation_pending;
static bool publication_blocked;

bool peak_global_budget_listener_idle_try_guard(void)
{
    if (pthread_mutex_trylock(&controller_lock))
        return false;
    if (mutation_pending) {
        pthread_mutex_unlock(&controller_lock);
        return false;
    }
    return true;
}

void peak_global_budget_listener_idle_unguard(void)
{
    assert(!pthread_mutex_unlock(&controller_lock));
}

static void publication_during_return(void)
{
    /* Physical publication uses the controller lock, including the interval
     * between the return's activity check and wallet commit. */
    assert(pthread_mutex_trylock(&controller_lock) == EBUSY);
    publication_blocked = true;
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
    PhysicalIdleSnapshot physical = physical_idle_snapshot(atomic_load(&physical_targets), 0,
                                                           400000000, &seen, &last, &returned);
    assert(!physical.attached);
    /* A grant waited 400ms for installation. That detached wait must not
     * reclaim the freshly installed hook's operating credit immediately. */
    peak_global_budget_physical_targets(16);
    physical = physical_idle_snapshot(atomic_load(&physical_targets), 0, 400000000,
                                      &seen, &last, &returned);
    assert(!idle_return_due(physical, 499999999, last, false, 20000000, 1000000));
    assert(idle_return_due(physical, 500000000, last, false, 20000000, 1000000));
    mutation_pending = true;
    assert(!idle_return_commit(physical, 20000000, 1000000));
    assert(atomic_load(&available_ns) == 20000000);
    mutation_pending = false;
    assert(idle_return_commit(physical, 20000000, 1000000) == 19000000);
    assert(publication_blocked);
    assert((uint64_t)atomic_load(&available_ns) + atomic_load(&debit_ns) ==
           atomic_load(&granted_ns));
    /* The helper may miss the detached publication between two equal counts. */
    PhysicalIdleSnapshot old = physical;
    peak_global_budget_physical_targets(0);
    peak_global_budget_physical_targets(16);
    assert(!idle_return_commit(old, 1000000, 100000));
    returned = true;
    physical = physical_idle_snapshot(atomic_load(&physical_targets), 0, 900000000,
                                      &seen, &last, &returned);
    assert(physical.word != old.word && last == 900000000 && !returned);
    assert(!idle_return_due(physical, 999999999, last, false, 1000000, 100000));
    assert(idle_return_due(physical, 1000000000, last, false, 1000000, 100000));
    atomic_store(&running, false);
    puts("global_budget_idle_return_ok");
}
