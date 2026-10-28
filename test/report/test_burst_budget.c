#include "internal/general_listener/burst_budget.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    PeakBurstBudgetSlot slot = {0};
    for (unsigned int i = 1; i <= 64; i++) {
        assert(peak_burst_budget_due(&slot) == (i == 64));
    }
    /* First block only seeds. Prior warmup/wait cannot create CPU credit. */
    assert(!peak_burst_budget_sample(&slot, 128, 2000000000, 1e-7, .1 / 32));
    assert(peak_burst_budget_sample(&slot, 128, 2000010000, 1e-7, .1 / 32));
    /* Expensive useful computation between calls leaves the hook attached. */
    assert(!peak_burst_budget_sample(&slot, 128, 3000010000, 1e-7, .1 / 32));
    /* Long history is replaced by each block, so the next burst still fires. */
    assert(peak_burst_budget_sample(&slot, 128, 3000020000, 1e-7, .1 / 32));
    /* Fresh lifecycle seeds; stale blocks are never charged to a reattach. */
    assert(!peak_burst_budget_sample(&slot, 256, 3000030000, 1e-7, .1 / 32));
    assert(peak_burst_budget_sample(&slot, 256, 3000040000, 1e-7, .1 / 32));
    /* Clock regression/zero interval resets baseline without division. */
    assert(!peak_burst_budget_sample(&slot, 256, 1, 1e-7, .1 / 32));
    assert(!peak_burst_budget_sample(&slot, 256, 1, 1e-7, .1 / 32));
    /* Thread retirement/reused slot is equivalent to its initial zero state. */
    slot = (PeakBurstBudgetSlot){0};
    assert(!peak_burst_budget_sample(&slot, 256, 10, 1e-7, .1 / 32));
    /* A configured zero budget retains its zero tolerance semantics. */
    assert(peak_burst_budget_sample(&slot, 256, 20, 1e-7, 0));
    assert(!peak_burst_budget_sample(&slot, 256, 30, 0, 0));
    /* Hundreds of independent hooks have independent slots, no cache aliases. */
    PeakBurstBudgetSlot hooks[256] = {0};
    for (unsigned int round = 0; round < 128; round++) {
        for (unsigned int hook = 0; hook < 256; hook++) {
            if (peak_burst_budget_due(&hooks[hook])) {
                bool exceeded = peak_burst_budget_sample(&hooks[hook], 128,
                    (round + 1) * 1000, 1e-7, .1 / 32);
                assert(exceeded == (round == 127));
            }
        }
    }
    puts("burst_budget_test_ok");
    return 0;
}
