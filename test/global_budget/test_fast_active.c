#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static atomic_uint false_active;
static void *writer(void *p){(void)p;for(unsigned i=0;i<10000;i++)publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=1,.granted_total_ns=1000000000});return NULL;}
static void *reader(void *p){(void)p;for(unsigned i=0;i<100000;i++)if(!peak_global_budget_active())atomic_fetch_add(&false_active,1);return NULL;}
int main(void){alarm(6);atomic_store(&owner,getpid());atomic_store(&local_attached,16);publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=1,.granted_total_ns=1000000000});pthread_t w,r[6];assert(!pthread_create(&w,NULL,writer,NULL));for(unsigned i=0;i<6;i++)assert(!pthread_create(&r[i],NULL,reader,NULL));assert(!pthread_join(w,NULL));for(unsigned i=0;i<6;i++)assert(!pthread_join(r[i],NULL));assert(!atomic_load(&false_active));atomic_fetch_add(&view_sequence,1);assert(!peak_global_budget_view().active);assert(peak_global_budget_active());assert(peak_global_budget_try_spend(1000));atomic_fetch_add(&view_sequence,1);puts("global_budget_fast_active_ok");}
