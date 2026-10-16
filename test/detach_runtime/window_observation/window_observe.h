#ifndef FIXED_WINDOW_OBSERVE_H
#define FIXED_WINDOW_OBSERVE_H
#ifndef PEAK_FIXED_WINDOW_OBSERVE
#define PEAK_FIXED_WINDOW_OBSERVE 0
#endif
#if PEAK_FIXED_WINDOW_OBSERVE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
/* Fixture main-thread only; no target/reader hot-loop bookkeeping. */
typedef struct {
    uint64_t wall0,cpu0,cpu1,wall1;
    size_t hook;
    int kind,result,errors;
} WindowApiObservation;
typedef struct {uint64_t lo,cpu,hi;int rc,lo_rc,hi_rc;} WindowCpuObservation;
static WindowApiObservation window_api_observations[128];
static unsigned window_api_count,window_api_dropped;
static clockid_t window_reader_clocks[2];
static int window_reader_clock_rc[2];
static int window_reader_clock_initialized[2];
static WindowCpuObservation window_reader_cpu[2][2];
static int window_observe_clock(clockid_t id,uint64_t*out){
    struct timespec t;int rc=clock_gettime(id,&t);
    if(!rc)*out=(uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
    return rc;
}
static void window_observe_readers_init(pthread_t*r){
    int saved=errno;
    for(int i=0;i<2;i++){
        window_reader_clock_rc[i]=pthread_getcpuclockid(r[i],&window_reader_clocks[i]);
        window_reader_clock_initialized[i]=1;
    }
    errno=saved;
}
static void window_observe_readers_cut(int end){
    int saved=errno;
    for(int i=0;i<2;i++){
        WindowCpuObservation*p=&window_reader_cpu[end][i];
        p->lo_rc=window_observe_clock(CLOCK_MONOTONIC,&p->lo);
        p->rc=!window_reader_clock_initialized[i] ? -1 : window_reader_clock_rc[i] ? window_reader_clock_rc[i] : window_observe_clock(window_reader_clocks[i],&p->cpu);
        p->hi_rc=window_observe_clock(CLOCK_MONOTONIC,&p->hi);
    }
    errno=saved;
}
static WindowApiObservation*window_observe_begin(int kind,size_t hook){
    int saved=errno;
    if(window_api_count>=128){window_api_dropped++;errno=saved;return NULL;}
    WindowApiObservation*p=&window_api_observations[window_api_count++];
    p->kind=kind;p->hook=hook;
    p->errors|=window_observe_clock(CLOCK_MONOTONIC,&p->wall0)!=0;
    p->errors|=(window_observe_clock(CLOCK_THREAD_CPUTIME_ID,&p->cpu0)!=0)<<1;
    errno=saved;return p;
}
static void window_observe_end(WindowApiObservation*p,int result){
    int saved=errno;
    if(p){
        p->errors|=(window_observe_clock(CLOCK_THREAD_CPUTIME_ID,&p->cpu1)!=0)<<2;
        p->errors|=(window_observe_clock(CLOCK_MONOTONIC,&p->wall1)!=0)<<3;
        p->result=result;
    }
    errno=saved;
}
static int window_observe_request(int(*fn)(size_t),size_t hook,const char*op){
    WindowApiObservation*p=window_observe_begin(op[0]=='d'?1:2,hook);
    int result=fn(hook);window_observe_end(p,result);return result;
}
static int window_observe_drain(int(*fn)(unsigned),unsigned timeout){
    WindowApiObservation*p=window_observe_begin(3,timeout);
    int result=fn(timeout);window_observe_end(p,result);return result;
}
static void window_observe_print(int rank){
    int saved=errno;
    for(unsigned i=0;i<window_api_count;i++){
        WindowApiObservation*p=&window_api_observations[i];
        printf("WINDOW_API schema=1 rank=%d index=%u kind=%d arg=%zu wall0=%llu cpu0=%llu cpu1=%llu wall1=%llu result=%d errors=%d\n",
               rank,i,p->kind,p->hook,(unsigned long long)p->wall0,(unsigned long long)p->cpu0,
               (unsigned long long)p->cpu1,(unsigned long long)p->wall1,p->result,p->errors);
    }
    for(int cut=0;cut<2;cut++)for(int i=0;i<2;i++){
        WindowCpuObservation*p=&window_reader_cpu[cut][i];
        printf("WINDOW_READER_CPU schema=1 rank=%d reader=%d cut=%d initialized=%d id_rc=%d lo=%llu cpu=%llu hi=%llu rc=%d lo_rc=%d hi_rc=%d\n",
               rank,i,cut,window_reader_clock_initialized[i],window_reader_clock_rc[i],(unsigned long long)p->lo,(unsigned long long)p->cpu,
               (unsigned long long)p->hi,p->rc,p->lo_rc,p->hi_rc);
    }
    printf("WINDOW_OBSERVE_END schema=1 rank=%d api_count=%u dropped=%u\n",rank,window_api_count,window_api_dropped);
    errno=saved;
}
#define WINDOW_REQUEST(fn,id,op) window_observe_request(fn,id,op)
#define WINDOW_DRAIN(fn,timeout) window_observe_drain(fn,timeout)
#else
#define WINDOW_REQUEST(fn,id,op) fn(id)
#define WINDOW_DRAIN(fn,timeout) fn(timeout)
#endif
#endif
