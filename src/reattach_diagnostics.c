#define _GNU_SOURCE
#include "internal/general_listener/reattach_diagnostics.h"
#include "internal/exec_raw_syscall.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

typedef struct {
    uint64_t gen, request, rounds, samples, reuse, reasons[10];
    uint64_t sample_round, sample_gen, budget_round, budget_gen;
    uint64_t budget_sample_round, budget_sample_gen;
    uint64_t reason_genmask[10], reason_first_round[10], reason_last_round[10];
    uint64_t calls;
    double profile, budget_profile, elapsed, sample_elapsed, budget_elapsed, control, spent_ratio, gate_ratio, headroom, stop;
    int ready, reattach_enabled;
    unsigned interval, counter;
    double target_ratio, global_ratio, factor;
} Hook;
typedef struct {
    size_t hook;
    uint64_t gen, request, calls;
    const char* kind;
    int state, operation;
    double profile, saved, elapsed, a, b;
    unsigned reason;
} Event;
static const char* const reason_names[10] = {
    "global_denied", "accounting_incoherent", "state_filter",
    "cooldown_filter", "per_target_filter", "pending_budget",
    "candidate_headroom", "queue_reject", "request_accept", "other"
};
static Hook hooks[PEAK_RD_HOOKS];
static Event events[PEAK_RD_EVENTS];
static size_t event_count, hook_count;
static char output_path[1024];
static long owner_pid;
static atomic_bool enabled, sealed, emitted;
static atomic_uint active;
static atomic_flag recording = ATOMIC_FLAG_INIT;
static atomic_ullong busy_drops, after_cut, overflow, out_of_range;

static void atomic_increment(atomic_ullong* value);
static void increment(uint64_t* value) { if (*value != UINT64_MAX) ++*value; else atomic_increment(&overflow); }
static void atomic_increment(atomic_ullong* value)
{
    unsigned long long old = atomic_load(value);
    while (old != ULLONG_MAX && !atomic_compare_exchange_weak(value, &old, old + 1)) {}
}
static long process_id(void)
{
#if defined(__linux__)
    return peak_exec_raw_syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0);
#else
    return -1;
#endif
}
/* seq_cst entry/seal protocol: if emit observes active==0 after sealing,
 * every writer which saw unsealed has finished. A later entrant sees sealed
 * before touching payload. A busy writer drops instead of waiting. */
static int begin(void)
{
    if (!atomic_load_explicit(&enabled, memory_order_acquire)) return 0;
    if (process_id() != owner_pid) return 0; /* inherited fork image */
    atomic_fetch_add(&active, 1);
    if (atomic_load(&sealed)) {
        atomic_increment(&after_cut); atomic_fetch_sub(&active, 1); return 0;
    }
    if (atomic_flag_test_and_set(&recording)) {
        atomic_increment(&busy_drops); atomic_fetch_sub(&active, 1); return 0;
    }
    return 1;
}
static void end(void)
{
    atomic_flag_clear(&recording);
    atomic_fetch_sub(&active, 1);
}
static Hook* hook_at(size_t hook)
{
    if (hook >= PEAK_RD_HOOKS) { atomic_increment(&out_of_range); return NULL; }
    if (hook_count <= hook) hook_count = hook + 1;
    return &hooks[hook];
}
int peak_rd_init(const char* flag, const char* trace)
{
    int saved_errno = errno;
    if (flag != NULL && strcmp(flag, "1") == 0 && trace != NULL && trace[0]) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
        size_t length = strlen(trace);
        const char suffix[] = ".reattach-diagnostics";
        if (length + sizeof(suffix) <= sizeof(output_path)) {
            memcpy(output_path, trace, length);
            memcpy(output_path + length, suffix, sizeof(suffix));
            owner_pid = process_id();
            atomic_store(&enabled, true);
        }
#endif
    }
    errno = saved_errno;
    return atomic_load(&enabled);
}
static void note_reason(Hook* h, unsigned index)
{
    if(h->reasons[index]==0)h->reason_first_round[index]=h->rounds;
    increment(&h->reasons[index]);
    if(h->gen<64) h->reason_genmask[index] |= UINT64_C(1)<<h->gen;
    else atomic_increment(&overflow);
    h->reason_last_round[index]=h->rounds;
}
void peak_rd_event(size_t hook, const char* kind, int state, int operation, uint64_t calls,
    double profile, double saved, double elapsed, double a, double b, unsigned reason)
{
    int saved_errno = errno;
    if (!begin()) { errno = saved_errno; return; }
    Hook* h = hook_at(hook);
    if (h != NULL) {
        if (strcmp(kind, "seed") == 0) increment(&h->gen);
        if (strcmp(kind, "request_accept") == 0) {
            increment(&h->request); note_reason(h,8);
        }
        if (event_count < PEAK_RD_EVENTS) {
            Event* e = &events[event_count++];
            *e = (Event){hook,h->gen,h->request,calls,kind,state,operation,
                         profile,saved,elapsed,a,b,reason};
        } else atomic_increment(&overflow);
    }
    end(); errno = saved_errno;
}
void peak_rd_round(size_t count, double elapsed, int ready, int on,
    unsigned interval, unsigned counter, double target, double global, double factor)
{
    int saved_errno = errno;
    if (!begin()) { errno = saved_errno; return; }
    if (count > PEAK_RD_HOOKS) atomic_increment(&out_of_range);
    for (size_t i=0; i<count && i<PEAK_RD_HOOKS; ++i) {
        Hook* h=hook_at(i); increment(&h->rounds); h->elapsed=elapsed;
        h->ready=ready; h->reattach_enabled=on; h->interval=interval;
        h->counter=counter; h->target_ratio=target; h->global_ratio=global; h->factor=factor;
    }
    end(); errno = saved_errno;
}
void peak_rd_sample(size_t hook, int reuse, uint64_t calls, double profile)
{
    int saved_errno = errno;
    if (!begin()) { errno = saved_errno; return; }
    Hook* h=hook_at(hook);
    if(h){increment(&h->samples);if(reuse)increment(&h->reuse);h->calls=calls;h->profile=profile;h->sample_round=h->rounds;h->sample_gen=h->gen;h->sample_elapsed=h->elapsed;}
    end(); errno = saved_errno;
}
void peak_rd_reason(size_t hook, const char* reason)
{
    int saved_errno = errno;
    if (!begin()) { errno = saved_errno; return; }
    Hook* h=hook_at(hook); unsigned index=9;
    for(unsigned i=0;i<10;i++)if(strcmp(reason,reason_names[i])==0){index=i;break;}
    if(h)note_reason(h,index);
    end(); errno = saved_errno;
}
void peak_rd_global(size_t count, int coherent, int admitted, double elapsed,
    double profile, double control, double ratio, double gate, double headroom, double stop)
{
    int saved_errno = errno;
    if (!begin()) { errno = saved_errno; return; }
    if(count>PEAK_RD_HOOKS)atomic_increment(&out_of_range);
    for(size_t i=0;i<count && i<PEAK_RD_HOOKS;i++){
        Hook*h=hook_at(i);if(!coherent)note_reason(h,1);if(!admitted)note_reason(h,0);
        h->budget_round=h->rounds;h->budget_gen=h->gen;h->budget_sample_round=h->sample_round;h->budget_sample_gen=h->sample_gen;h->budget_elapsed=elapsed;h->budget_profile=profile;h->control=control;h->spent_ratio=ratio;
        h->gate_ratio=gate;h->headroom=headroom;h->stop=stop;
    }
    end(); errno = saved_errno;
}
void peak_rd_pending_budget(size_t count)
{
    int saved_errno=errno;
    if(!begin()){errno=saved_errno;return;}
    if(count>PEAK_RD_HOOKS)atomic_increment(&out_of_range);
    for(size_t i=0;i<count&&i<PEAK_RD_HOOKS;i++)note_reason(hook_at(i),5);
    end();errno=saved_errno;
}
#if defined(__linux__)
static int write_line(int fd, const char* line, int length, size_t capacity)
{
    if(length<0 || (size_t)length>=capacity)return 0;
    return peak_exec_raw_syscall6(SYS_write,fd,(long)line,length,0,0,0)==length;
}
#endif
static void close_diagnostic_fd(void* argument)
{
    int saved_errno=errno;
    int* fd=argument;
#if defined(__linux__)
    if(*fd>=0)(void)peak_exec_raw_syscall6(SYS_close,*fd,0,0,0,0,0);
#endif
    *fd=-1;
    errno=saved_errno;
}
void peak_rd_emit(void)
{
    int saved_errno = errno;
    if(!atomic_load(&enabled)||process_id()!=owner_pid||atomic_load(&emitted)) {errno=saved_errno;return;}
    /* Acquire no owned fd while async cancellation is enabled. No new
     * deferred cancellation point is introduced; restore type after close. */
    int original_type;
    if(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED,&original_type)!=0){errno=saved_errno;return;}
    if(atomic_exchange(&emitted,true)){
        (void)pthread_setcanceltype(original_type,NULL);errno=saved_errno;return;
    }
    atomic_store(&sealed,true);
    /* Never read racing payload, even for an invalid capture. */
    unsigned concurrent=atomic_load(&active);
#if defined(__linux__)
    int fd=(int)peak_exec_raw_syscall6(SYS_openat,AT_FDCWD,(long)output_path,
        O_WRONLY|O_CREAT|O_EXCL|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600,0,0);
    if(fd>=0){
        pthread_cleanup_push(close_diagnostic_fd,&fd);
        struct stat info;
        if(peak_exec_raw_syscall6(SYS_fstat,fd,(long)&info,0,0,0,0)==0 && S_ISREG(info.st_mode)){
            char line[1024];int n,ok=1;
            n=snprintf(line,sizeof(line),"RD_BEGIN v=1 pid=%ld capture=init-to-first-controller-stop hooks_cap=%u events_cap=%u concurrent=%u\n",owner_pid,PEAK_RD_HOOKS,PEAK_RD_EVENTS,concurrent);
            ok=write_line(fd,line,n,sizeof(line));
            if(!concurrent){
                for(size_t i=0;ok&&i<event_count;i++){
                    Event*e=&events[i];n=snprintf(line,sizeof(line),"RD_EVENT v=1 seq=%zu hook=%zu gen=%llu req=%llu kind=%s state=%d operation=%d calls=%llu profile=%.17g saved=%.17g elapsed=%.17g a=%.17g b=%.17g reason=%u\n",i+1,e->hook,(unsigned long long)e->gen,(unsigned long long)e->request,e->kind,e->state,e->operation,(unsigned long long)e->calls,e->profile,e->saved,e->elapsed,e->a,e->b,e->reason);ok=write_line(fd,line,n,sizeof(line));
                }
                for(size_t i=0;ok&&i<hook_count;i++){
                    Hook*h=&hooks[i];n=snprintf(line,sizeof(line),"RD_HOOK v=1 hook=%zu gen=%llu req=%llu rounds=%llu samples=%llu reuse=%llu sample_round=%llu sample_gen=%llu budget_round=%llu budget_gen=%llu budget_sample_round=%llu budget_sample_gen=%llu calls=%llu profile=%.17g budget_profile=%.17g elapsed=%.17g sample_elapsed=%.17g budget_elapsed=%.17g control=%.17g spent_ratio=%.17g gate_ratio=%.17g headroom=%.17g predicted_stop=%.17g ready=%d reattach=%d interval=%u counter=%u target_ratio=%.17g global_ratio=%.17g factor=%.17g\n",i,(unsigned long long)h->gen,(unsigned long long)h->request,(unsigned long long)h->rounds,(unsigned long long)h->samples,(unsigned long long)h->reuse,(unsigned long long)h->sample_round,(unsigned long long)h->sample_gen,(unsigned long long)h->budget_round,(unsigned long long)h->budget_gen,(unsigned long long)h->budget_sample_round,(unsigned long long)h->budget_sample_gen,(unsigned long long)h->calls,h->profile,h->budget_profile,h->elapsed,h->sample_elapsed,h->budget_elapsed,h->control,h->spent_ratio,h->gate_ratio,h->headroom,h->stop,h->ready,h->reattach_enabled,h->interval,h->counter,h->target_ratio,h->global_ratio,h->factor);ok=write_line(fd,line,n,sizeof(line));
                    for(unsigned k=0;ok&&k<10;k++){n=snprintf(line,sizeof(line),"RD_REASON v=1 hook=%zu name=%s count=%llu generation_mask=%llu first_round=%llu last_round=%llu\n",i,reason_names[k],(unsigned long long)h->reasons[k],(unsigned long long)h->reason_genmask[k],(unsigned long long)h->reason_first_round[k],(unsigned long long)h->reason_last_round[k]);ok=write_line(fd,line,n,sizeof(line));}
                }
            }
            if(ok){n=snprintf(line,sizeof(line),"RD_END v=1 events=%zu hooks=%zu busy_drops=%llu overflow=%llu out_of_range=%llu after_cut=%llu concurrent=%u complete=%d\n",concurrent?0:event_count,concurrent?0:hook_count,atomic_load(&busy_drops),atomic_load(&overflow),atomic_load(&out_of_range),atomic_load(&after_cut),concurrent,!concurrent&&!atomic_load(&busy_drops)&&!atomic_load(&overflow)&&!atomic_load(&out_of_range));(void)write_line(fd,line,n,sizeof(line));}
        }
        close_diagnostic_fd(&fd);
        pthread_cleanup_pop(0);
    }
#endif
    (void)pthread_setcanceltype(original_type,NULL);
    errno=saved_errno;
}
