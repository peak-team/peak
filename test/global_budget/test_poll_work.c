#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
int main(void){
 Peer p={0};assert(service_poll_timeout(&p,0,false,false)==100);
 atomic_store(&flush_requested,true);p.pending=true;assert(service_poll_timeout(&p,0,false,false)==100);/* Wait for POLLOUT, never spin. */
 p.pending=false;assert(service_poll_timeout(&p,0,false,false)==0);/* Old write completed: prepare pending FINAL immediately. */
 atomic_store(&flush_requested,false);assert(service_poll_timeout(&p,0,false,false)==100);
 atomic_store(&demand_generation,1);assert(service_poll_timeout(&p,0,false,false)==0);p.outgoing.demand_generation=1;assert(service_poll_timeout(&p,0,false,false)==100);
 Rank r={0};assert(reply_urgent(&p,&r,false,true,0,0));/* Pending old reply must be rebuilt for the cohort ACK. */
 assert(service_poll_timeout(&p,0,true,false)==0);puts("global_budget_poll_work_ok");
}
