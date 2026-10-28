#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
#define N 6
static pthread_barrier_t barrier;
static atomic_uint misses,successes;
static bool mixed;
static void *consume(void *arg){(void)arg;pthread_barrier_wait(&barrier);for(unsigned i=0;i<5000;i++){if((mixed&&(i&1)?peak_global_budget_try_spend_control(1000):peak_global_budget_try_spend(1000)))atomic_fetch_add(&successes,1);else atomic_fetch_add(&misses,1);}return NULL;}
static void *grants(void *arg){(void)arg;for(unsigned i=1;i<=100;i++){publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=(double)(i*1000000)/1e9,.granted_total_ns=i*1000000});usleep(10);}return NULL;}
static void *returns(void *arg){(void)arg;for(unsigned i=0;i<1000;i++)peak_global_budget_return_idle();return NULL;}
static void launch(void){pthread_t t[N];assert(!pthread_barrier_init(&barrier,NULL,N));for(unsigned i=0;i<N;i++)assert(!pthread_create(&t[i],NULL,consume,NULL));for(unsigned i=0;i<N;i++)assert(!pthread_join(t[i],NULL));assert(!pthread_barrier_destroy(&barrier));}
int main(int argc,char **argv){(void)argv;mixed=argc>1;
 alarm(8);atomic_store(&owner,getpid());atomic_store(&running,true);atomic_store(&local_attached,16);atomic_store(&low_water_ns,0);
 publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=1,.granted_total_ns=1000000000});launch();
 assert(!atomic_load(&misses));assert(atomic_load(&consumed_ns)+atomic_load(&consumed_control_ns)==30000000);if(mixed)assert(atomic_load(&consumed_control_ns)==15000000);assert(atomic_load(&available_ns)==970000000);
 atomic_store(&available_ns,0);atomic_store(&granted_ns,0);atomic_store(&debit_ns,0);atomic_store(&consumed_ns,0);atomic_store(&consumed_control_ns,0);atomic_store(&returned_ns,0);atomic_store(&successes,0);atomic_store(&misses,0);
 pthread_t g,r;assert(!pthread_create(&g,NULL,grants,NULL));assert(!pthread_create(&r,NULL,returns,NULL));launch();assert(!pthread_join(g,NULL));assert(!pthread_join(r,NULL));
 uint64_t consumed=atomic_load(&consumed_ns)+atomic_load(&consumed_control_ns),returned=atomic_load(&returned_ns),grant=atomic_load(&granted_ns);int64_t available=atomic_load(&available_ns);
 assert(available>=0);assert((uint64_t)available+consumed+returned==grant);assert(consumed+returned==atomic_load(&debit_ns));assert(consumed==(uint64_t)atomic_load(&successes)*1000);
 assert(!peak_global_budget_try_spend(MAX_SPEND_NS+1));assert(atomic_load(&rejected_spend_count)==1);assert(atomic_load(&available_ns)==available);
 publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=1e12});assert(!peak_global_budget_active());assert(atomic_load(&available_ns)==available);assert(peak_global_budget_view().accounting_invalid);
 atomic_store(&running,false);puts("global_budget_credit_concurrency_ok");
}
