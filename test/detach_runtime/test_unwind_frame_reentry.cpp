#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static std::atomic<unsigned> calls, destroyed, errors, stage, release_target;
static volatile sig_atomic_t signal_calls, signal_errors;
static pthread_key_t key;
static thread_local unsigned passes;
static void (*diagnose)(uint64_t*,uint64_t*,uint64_t*);
static int (*reap)();
static std::atomic<bool> stop;
struct Guard { ~Guard() { destroyed.fetch_add(1); } };

#if defined(__GNUC__) && !defined(__clang__)
#define NOIPA __attribute__((noipa))
#else
#define NOIPA
#endif
extern "C" __attribute__((noinline,visibility("default"))) NOIPA
unsigned peak_frame_signal_target(unsigned value) {
  __asm__ volatile("" : "+r"(value) :: "memory");
  __asm__ volatile(".rept 32; nop; .endr" ::: "memory");
  return value+4;
}
static void handler(int) {
  int saved=errno;
  if (peak_frame_signal_target(67)!=71) ++signal_errors;
  ++signal_calls;
  errno=saved;
}
extern "C" __attribute__((noinline,visibility("default"))) NOIPA
unsigned peak_frame_scope_target(unsigned mode,unsigned depth) {
  calls.fetch_add(1);
  Guard guard;
  if (depth) return peak_frame_scope_target(mode,depth-1)+1;
  if (raise(SIGUSR1)) errors.fetch_add(1);
  if (mode==1) { errno=EDOM;throw 29u; }
  if (mode==2) {
    stage.store(1,std::memory_order_release);
    while (!release_target.load(std::memory_order_acquire)) sched_yield();
  }
  if (mode==3) pthread_exit(reinterpret_cast<void*>(uintptr_t(0x51)));
  if (mode==4) {
    stage.store(2,std::memory_order_release);
    for (;;) { pthread_testcancel();sched_yield(); }
  }
  errno=ERANGE;
  return 19;
}
static void destructor(void* p) {
  ++passes;
  unsigned mode=passes==2?1:passes==4?2:0;
  try {
    unsigned r=peak_frame_scope_target(mode,1);
    if (mode==1 || r!=20 || errno!=ERANGE) errors.fetch_add(1);
  } catch (unsigned n) {
    if (mode!=1 || n!=29 || errno!=EDOM) errors.fetch_add(1);
  }
  if (passes<4 && pthread_setspecific(key,p)) errors.fetch_add(1);
}
static void* worker(void*) {
  if (pthread_setspecific(key,reinterpret_cast<void*>(uintptr_t(1)))) errors.fetch_add(1);
  for (unsigned i=0;i<32;i++) {
    if (peak_frame_scope_target(0,3)!=22 || errno!=ERANGE) errors.fetch_add(1);
    try { (void)peak_frame_scope_target(1,3);errors.fetch_add(1); }
    catch (unsigned n) { if (n!=29 || errno!=EDOM) errors.fetch_add(1); }
  }
  return reinterpret_cast<void*>(uintptr_t(0x31));
}
static void* forced(void* p) {
  (void)peak_frame_scope_target(unsigned(uintptr_t(p)),2);
  return nullptr;
}
static void* observer(void*) {
  while (!stop.load(std::memory_order_acquire)) {
    uint64_t owners,blocks,active;
    diagnose(&owners,&blocks,&active);
    sched_yield();
  }
  return nullptr;
}
int main(int argc, char**) {
  struct sigaction sa={};sa.sa_handler=handler;sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1,&sa,nullptr)) return 2;
  // Warm the ordinary target, handler target, pool and key before worker exit.
  if (peak_frame_signal_target(3)!=7 || peak_frame_scope_target(0,0)!=19) return 3;
  diagnose=reinterpret_cast<decltype(diagnose)>(dlsym(RTLD_DEFAULT,"gum_interceptor_peak_unwind_diagnostics"));
  reap=reinterpret_cast<decltype(reap)>(dlsym(RTLD_DEFAULT,"gum_interceptor_peak_unwind_reap_threads"));
  if (!!diagnose!=!!reap || (argc>1 && !diagnose) ||
      pthread_key_create(&key,destructor)) return 4;
  pthread_t t,o;
  if (diagnose && pthread_create(&o,nullptr,observer,nullptr)) return 5;
  if (pthread_create(&t,nullptr,worker,nullptr)) return 6;
  while (stage.load(std::memory_order_acquire)!=1) sched_yield();
  // This is a quiescent checkpoint inside a live fourth key destructor.
  // The PEAK key has retired this owner, but the thread is still alive.
  if (diagnose) {
    for(unsigned i=0;i<32;i++) {
      reap();uint64_t owners,blocks,active;diagnose(&owners,&blocks,&active);
      if (active!=2 || owners<2 || blocks<2) errors.fetch_add(1);
    }
  }
  release_target.store(1,std::memory_order_release);
  void* result=nullptr;
  if (pthread_join(t,&result) || result!=reinterpret_cast<void*>(uintptr_t(0x31))) return 7;
  if (pthread_create(&t,nullptr,forced,reinterpret_cast<void*>(uintptr_t(3))) || pthread_join(t,&result) || result!=reinterpret_cast<void*>(uintptr_t(0x51))) return 8;
  if (pthread_create(&t,nullptr,forced,reinterpret_cast<void*>(uintptr_t(4)))) return 9;
  while (stage.load(std::memory_order_acquire)!=2) sched_yield();
  if (pthread_cancel(t) || pthread_join(t,&result) || result!=PTHREAD_CANCELED) return 10;
  stop.store(true,std::memory_order_release);
  if (diagnose && pthread_join(o,nullptr)) return 11;
  uint64_t owners=0,blocks=0,active=0;
  if (diagnose) {reap();diagnose(&owners,&blocks,&active);if(active || owners>1 || blocks>1) errors.fetch_add(1);}
  bool ok=calls==271 && destroyed==271 && signal_calls==71 && !signal_errors && !errors;
  std::printf("FRAME_REENTRY calls=%u destroyed=%u signals=%d signal_errors=%d errors=%u active=%lu owners=%lu blocks=%lu pass=%d\n",calls.load(),destroyed.load(),signal_calls,signal_errors,errors.load(),(unsigned long)active,(unsigned long)owners,(unsigned long)blocks,ok);
  return ok?0:1;
}
