#define _GNU_SOURCE
static void commit_between_generation_and_cost(void);
#define PEAK_GLOBAL_BUDGET_TEST_SNAPSHOT_HOOK() commit_between_generation_and_cost()
#include "../../src/global_budget.c"
#include <assert.h>
static bool interleave;
static void commit_between_generation_and_cost(void){if(interleave){interleave=false;atomic_fetch_add(&consumed_ns,1000);atomic_fetch_add(&consumed_control_ns,500);atomic_fetch_add_explicit(&demand_generation,1,memory_order_release);}}
int main(void){
 alarm(5);atomic_store(&owner,getpid());atomic_store(&demand_generation,7);atomic_store(&consumed_ns,1000);atomic_store(&consumed_control_ns,500);local_sample.sequence=1;
 interleave=true;Frame first=request_snapshot();assert(first.demand_generation==7&&first.consumed_profile_ns==2000&&first.consumed_control_ns==1000);
 Frame next=request_snapshot();assert(next.demand_generation==8&&next.consumed_profile_ns==2000&&next.consumed_control_ns==1000);
 atomic_store(&pressure_notice,true);atomic_store(&request_started_ns,1000);atomic_store(&available_ns,12345);acknowledge_request(8,2000);
 assert(!atomic_load(&pressure_notice));assert(!atomic_load(&request_started_ns));assert(atomic_load(&available_ns)==12345);/* ACK without a new grant still completes the request. */
 atomic_store(&demand_generation,9);atomic_store(&pressure_notice,true);atomic_store(&request_started_ns,3000);acknowledge_request(8,4000);
 assert(atomic_load(&pressure_notice)&&atomic_load(&request_started_ns)==3000);puts("global_budget_request_snapshot_ok");
}
