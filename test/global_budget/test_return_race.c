#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static void *consume(void *unused) {
 (void)unused;
 for(unsigned i=0;i<10000;i++) (void)peak_global_budget_try_spend(100);
 return NULL;
}
int main(void) {
 alarm(5);atomic_store(&owner,getpid());atomic_store(&local_attached,16);atomic_store(&low_water_ns,0);
 publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=.001,.granted_total_ns=1000000});
 pthread_t t;assert(!pthread_create(&t,NULL,consume,NULL));
 peak_global_budget_return_idle();assert(!pthread_join(t,NULL));peak_global_budget_return_idle();
 uint64_t consumed=atomic_load(&consumed_ns),returned=atomic_load(&returned_ns);
 assert(consumed+returned==atomic_load(&debit_ns));assert(consumed+returned==atomic_load(&granted_ns));
 peak_global_budget_return_idle();assert(returned==atomic_load(&returned_ns));
 assert(!peak_global_budget_try_spend(1));
 peak_global_budget_return_idle();peak_global_budget_request_credit(1000);
 uint64_t generation=atomic_load(&demand_generation);
 peak_global_budget_request_credit(2000);assert(atomic_load(&demand_generation)>generation);
 generation=atomic_load(&demand_generation);peak_global_budget_request_credit(2000);
 assert(atomic_load(&demand_generation)==generation);puts("global_budget_return_race_ok");
}
