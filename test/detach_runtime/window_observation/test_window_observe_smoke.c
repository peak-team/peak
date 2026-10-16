#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "general_listener.h"
#include "window_observe.h"

static atomic_int stop,ready;
static atomic_ullong worker_calls,reader_calls;
static int(*hook_state)(size_t);
__attribute__((noinline,noclone,used,visibility("default")))
uint64_t window_observation_target(uint64_t x)
{
    asm volatile("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop" ::: "memory");
    return x*6364136223846793005ULL;
}
static uint64_t monotonic_ns(void)
{
    struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return(uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
static void*worker(void*p)
{
    (void)p;uint64_t n=0;atomic_fetch_add(&ready,1);
    while(!atomic_load(&stop)){
        assert(window_observation_target(n)==n*6364136223846793005ULL);n++;
    }
    atomic_store(&worker_calls,n);return NULL;
}
static void*reader(void*p)
{
    (void)p;uint64_t n=0;atomic_fetch_add(&ready,1);
    while(!atomic_load(&stop)){
        int state=hook_state(0);
        assert(state>=PEAK_HOOK_UNRESOLVED&&state<=PEAK_HOOK_SHUTDOWN);n++;
    }
    atomic_fetch_add(&reader_calls,n);return NULL;
}
static void*symbol(const char*name)
{
    void*p=dlsym(RTLD_DEFAULT,name);assert(p);return p;
}
static int attached(void)
{
    const unsigned char*p=(const unsigned char*)window_observation_target;
    return p[0]==0xe9||(p[0]==0xff&&p[1]==0x25);
}
int main(void)
{
    hook_state=symbol("peak_general_listener_hook_state");
    int(*detach)(size_t)=symbol("peak_general_listener_request_detach");
    int(*reattach)(size_t)=symbol("peak_general_listener_request_reattach");
    int(*drain)(unsigned)=symbol("peak_general_listener_controller_drain");
    assert(window_observation_target(7)==7ULL*6364136223846793005ULL);
    assert(hook_state(0)==PEAK_HOOK_ATTACHED&&attached());
    pthread_t wt,rt[2];assert(!pthread_create(&wt,NULL,worker,NULL));
    for(int i=0;i<2;i++)assert(!pthread_create(&rt[i],NULL,reader,NULL));
    uint64_t deadline=monotonic_ns()+10000000000ULL;
    while(atomic_load(&ready)!=3){assert(monotonic_ns()<deadline);sched_yield();}
#if PEAK_FIXED_WINDOW_OBSERVE
    window_observe_readers_init(rt);
    window_observe_readers_cut(0);
#endif
    uint64_t start=monotonic_ns();
    for(int cycle=0;cycle<2;cycle++){
        errno=E2BIG;
        assert(WINDOW_REQUEST(detach,0,"detach"));
        assert(WINDOW_DRAIN(drain,1000));
        assert(hook_state(0)==PEAK_HOOK_DETACHED&&!attached());
        assert(window_observation_target(11)==11ULL*6364136223846793005ULL);
        usleep(2000);
        assert(WINDOW_REQUEST(reattach,0,"reattach"));
        assert(WINDOW_DRAIN(drain,1000));
        assert(hook_state(0)==PEAK_HOOK_ATTACHED&&attached());
        usleep(2000);
    }
    uint64_t end=monotonic_ns();
#if PEAK_FIXED_WINDOW_OBSERVE
    window_observe_readers_cut(1);
#endif
    atomic_store(&stop,1);assert(!pthread_join(wt,NULL));
    for(int i=0;i<2;i++)assert(!pthread_join(rt[i],NULL));
    printf("WINDOW schema=1 rank=0 start_ns=%llu end_ns=%llu elapsed_ns=%llu\n",
           (unsigned long long)start,(unsigned long long)end,(unsigned long long)(end-start));
#if PEAK_FIXED_WINDOW_OBSERVE
    window_observe_print(0);
#endif
    assert(atomic_load(&worker_calls)>0&&atomic_load(&reader_calls)>0);
    printf("WINDOW_OBSERVE_SMOKE_OK cycles=2 physical=1 worker_calls=%llu reader_calls=%llu\n",
           (unsigned long long)atomic_load(&worker_calls),(unsigned long long)atomic_load(&reader_calls));
    return 0;
}
