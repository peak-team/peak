#ifndef PEAK_BURST_BUDGET_H
#define PEAK_BURST_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

/* Private to a thread. Neither hook calls nor checks contend on shared counters.
 * A new listener or lifecycle seeds a fresh block. CPU time excludes
 * MPI/scheduler waits; every block replaces its predecessor so warmup cannot
 * create credit for a later burst. This is a risk estimate, not slowdown. */
#define PEAK_BURST_BUDGET_STRIDE 64U
typedef struct {
    unsigned long long generation;
    uint64_t cpu_ns;
    unsigned int calls;
    bool seeded;
} PeakBurstBudgetSlot;

static inline bool
peak_burst_budget_due(PeakBurstBudgetSlot* slot)
{
    if (++slot->calls < PEAK_BURST_BUDGET_STRIDE) {
        return false;
    }
    slot->calls = 0;
    return true;
}

static inline bool
peak_burst_budget_sample(PeakBurstBudgetSlot* slot,
                        unsigned long long generation,
                        uint64_t cpu_ns,
                        double cost_per_call,
                        double rank_limit)
{
    bool exceeded = false;
    if (slot->seeded && slot->generation == generation &&
        cpu_ns > slot->cpu_ns) {
        double block_seconds = (double)(cpu_ns - slot->cpu_ns) / 1e9;
        exceeded = cost_per_call * PEAK_BURST_BUDGET_STRIDE >
                   rank_limit * block_seconds;
    }
    slot->generation = generation;
    slot->cpu_ns = cpu_ns;
    slot->seeded = true;
    return exceeded;
}

#endif
