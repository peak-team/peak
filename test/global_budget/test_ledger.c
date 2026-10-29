#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>

static void *spend_credit(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1000; i++)
        assert(i & 1 ? peak_global_budget_try_spend_control(1000)
                     : peak_global_budget_try_spend(1000));
    return NULL;
}

static void check_credit_conservation(void)
{
    atomic_store(&owner, getpid());
    atomic_store(&running, true);
    atomic_store(&local_attached, 1);
    publish_view((PeakGlobalBudgetView){
        .active = true, .granted_total_ns = 10000000, .granted_total_seconds = .01});
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, spend_credit, NULL));
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&consumed_ns) == 2000000);
    assert(atomic_load(&consumed_control_ns) == 2000000);
    assert(atomic_load(&available_ns) == 6000000);
    peak_global_budget_return_idle();
    assert(atomic_load(&available_ns) == 0);
    assert(atomic_load(&returned_ns) == 6000000);
    assert(atomic_load(&consumed_ns) + atomic_load(&consumed_control_ns) +
               atomic_load(&returned_ns) == atomic_load(&granted_ns));
    peak_global_budget_return_idle();
    assert(atomic_load(&returned_ns) == 6000000);
    atomic_store(&running, false);
}

static void receive(Rank *rank, PeakGlobalBudgetSample sample)
{
    assert(valid_sample(&sample, rank));
    rank->latest = sample;
    rank->seen = true;
    rank->previous_cost = fmax(rank->previous_cost, ledger_cost(&sample, rank->profile_ns));
}

static void check_control_settlement(void)
{
    Rank rank = {.credit_total_ns = 6000000, .consumed_ns = 3000000,
                 .control_ns = 3000000, .reservation = .003};
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 1});
    assert(fabs(control_escrow(&rank) - .003) < 1e-12);
    /* The controller finishes after heartbeat captured its settlement mark.
     * Actual cost must not permanently absorb the still-pending prepayment. */
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 2, .control_seconds = .003});
    assert(fabs(rank.previous_cost - .003) < 1e-12);
    assert(fabs(control_escrow(&rank) - .003) < 1e-12);
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 3, .control_seconds = .003,
                                          .settled_control_credit_ns = 3000000});
    assert(fabs(rank.previous_cost - .003) < 1e-12);
    assert(control_escrow(&rank) == 0);
    assert(rank.credit_total_ns - rank.consumed_ns - rank.returned_ns == 3000000);
}

int main(void)
{
    check_credit_conservation();
    check_control_settlement();
    puts("global_budget_ledger_ok");
}
