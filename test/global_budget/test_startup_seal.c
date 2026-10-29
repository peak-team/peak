#define _GNU_SOURCE
#define PEAK_GLOBAL_BUDGET_NATIVE 1
static void seal_during_snapshot(void);
#define PEAK_GLOBAL_BUDGET_TEST_SEAL_SNAPSHOT_HOOK seal_during_snapshot
#include "../../src/global_budget.c"
#include <assert.h>
static bool inject_seal;
static void seal_during_snapshot(void)
{
    if (inject_seal) {
        atomic_store(&startup_charge, .123);
        atomic_store_explicit(&startup_sealed, true, memory_order_release);
        inject_seal = false;
    }
}
static void run(double fee, bool funded)
{
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    assert(!peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL));
    peak_global_budget_origin(100);
    peak_global_budget_physical_targets(0);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .initial_unexposed = true});
    peak_global_budget_request_credit(6000000);
    uint64_t limit = ns_now(CLOCK_MONOTONIC) + UINT64_C(1000000000);
    while (!peak_global_budget_active() && ns_now(CLOCK_MONOTONIC) < limit)
        usleep(1000);
    assert(peak_global_budget_active());
    /* Multiple normal report periods ensure the request reached an active
     * broker with ample gross earned budget, before its startup commitment. */
    usleep(220000);
    PeakGlobalBudgetView v = peak_global_budget_view();
    assert(v.active && !v.startup_sealed && v.granted_total_ns == 0 && v.wave == 0);
    assert(effective_targets(99) == 0);
    peak_global_budget_startup(fee);
    limit = ns_now(CLOCK_MONOTONIC) + UINT64_C(1000000000);
    do {
        v = peak_global_budget_view();
        if (v.startup_sealed && (!funded || v.granted_total_ns >= 6000000))
            break;
        usleep(1000);
    } while (ns_now(CLOCK_MONOTONIC) < limit);
    assert(v.startup_sealed && v.global_spent_seconds >= fee);
    assert(funded ? v.granted_total_ns >= 6000000 : v.granted_total_ns == 0);
    peak_global_budget_retire();
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 2, .initial_unexposed = true, .retiring = true, .final = true});
    assert(peak_global_budget_final_flush(2000));
    peak_global_budget_stop();
}
int main(int argc, char **argv)
{
    alarm(8);
    assert(argc == 2);
    assert(!setenv("PEAK_DETACH_HELPER", argv[1], 1));
    run(10, false);
    run(.001, true);
    /* Force a producer seal precisely after the consumer acquired false.
     * Safe result: false + new fee; next snapshot: true + committed fee. */
    rank_id = 0;
    atomic_store(&startup_sealed, false);
    atomic_store(&startup_charge, 0);
    inject_seal = true;
    Frame before = request_snapshot();
    assert(!before.root_startup_sealed && before.root_startup_seconds == .123);
    Frame after = request_snapshot();
    assert(after.root_startup_sealed && after.root_startup_seconds == .123);
    broker_only = true;
    atomic_store(&earned_origin, 100);
    atomic_store(&startup_charge, 10);
    atomic_store(&startup_sealed, true);
    Frame root = {
        .root_origin_seconds = 100, .root_startup_seconds = 10, .root_startup_sealed = true};
    assert(root_metadata_valid(&root));
    root.root_startup_sealed = false;
    assert(!root_metadata_valid(&root));
    root.root_startup_sealed = true;
    root.root_startup_seconds = 0;
    assert(!root_metadata_valid(&root));
    root = (Frame){.rank = 1, .root_startup_sealed = true};
    assert(!root_metadata_valid(&root));
    broker_only = false;
    puts("global_budget_startup_seal_ok");
}
