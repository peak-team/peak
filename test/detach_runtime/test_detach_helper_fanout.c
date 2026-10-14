#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static DIR* fake_opendir(const char*);
static struct dirent* fake_readdir(DIR*);
static int fake_closedir(DIR*);
static int fake_open(const char*,int,...);
static int fake_close(int);
static pid_t fake_waitpid(pid_t,int*,int);
static int fake_clock(clockid_t,struct timespec*);
static int fake_signal_wait(const sigset_t*,siginfo_t*,const struct timespec*);
#define PEAK_DETACH_HELPER_PTRACE_STOP_WAIT_UNIT_TEST 1
#define ptrace fake_ptrace
#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define open fake_open
#define close fake_close
#define waitpid fake_waitpid
#define clock_gettime fake_clock
#define sigtimedwait fake_signal_wait
#include "../../src/detach_helper.c"
#undef ptrace
#undef opendir
#undef readdir
#undef closedir
#undef open
#undef close
#undef waitpid
#undef clock_gettime
#undef sigtimedwait
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__func__,__LINE__,#x);return 1;}}while(0)
typedef struct {pid_t tid;int owned,requested,observed,gone,reaped,signo;int waits,detaches;} Target;
static Target targets[PEAK_DETACH_HELPER_MAX_THREADS+1];
static unsigned target_count,visible,position,scan;
static int churn,census_error,fail_request,fail_n,fail_errno,fail_once,op_counts[4];
static int never_ready_tid,initial_unready_tid,signal_ready,signal_calls,bad_registers;
static int exit_tid,clock_step,advance_after_fanout;
static long now_ms,last_timeout_ms;
static char ops[32768];static pid_t tids[32768];static unsigned op_count;
static void log_op(char op,pid_t tid){if(op_count<32768){ops[op_count]=op;tids[op_count++]=tid;}}
static Target* target(pid_t tid){for(unsigned i=0;i<target_count;i++)if(targets[i].tid==tid)return &targets[i];return NULL;}
static void reset(unsigned n){
 memset(targets,0,sizeof(targets));target_count=visible=n;position=0;scan=0;churn=census_error=0;fail_request=fail_n=fail_errno=fail_once=0;
 memset(op_counts,0,sizeof(op_counts));never_ready_tid=initial_unready_tid=signal_ready=signal_calls=bad_registers=0;exit_tid=clock_step=advance_after_fanout=0;
 now_ms=100;last_timeout_ms=-1;op_count=0;held_thread_count=0;
 for(unsigned i=0;i<n;i++){targets[i].tid=111+(pid_t)i*111;targets[i].signo=SIGTRAP;}
 if(n>1)targets[1].signo=SIGUSR1;
}
static DIR* fake_opendir(const char*path){(void)path;if(churn&&scan==1)visible=target_count;position=0;scan++;return (DIR*)(uintptr_t)1;}
static struct dirent* fake_readdir(DIR*d){
 (void)d;static struct dirent e;
 if(census_error&&scan==1&&position==3){errno=EIO;return NULL;}
 while(position<=visible){unsigned p=position++;pid_t tid=p==0?42:targets[p-1].tid;if(p&&targets[p-1].gone)continue;snprintf(e.d_name,sizeof(e.d_name),"%ld",(long)tid);return &e;}
 errno=0;return NULL;
}
static int fake_closedir(DIR*d){(void)d;return 0;}
static int fake_open(const char*path,int flags,...){
 (void)flags;long tid=0;if(sscanf(path,"/proc/%ld/status",&tid)==1){Target*t=target((pid_t)tid);if(t&&t->gone){errno=ENOENT;return -1;}return 9876;}
 errno=EACCES;return -1;
}
static int fake_close(int fd){return fd==9876?0:close(fd);}
long fake_ptrace(enum __ptrace_request request,...){
 va_list ap;va_start(ap,request);pid_t tid=va_arg(ap,pid_t);(void)va_arg(ap,void*);void*data=va_arg(ap,void*);va_end(ap);Target*t=target(tid);
 if(!t){errno=ESRCH;return -1;}
 PeakRegs* regs = data;
#if defined(__aarch64__)
 int get_registers = request == PTRACE_GETREGSET;
 if (get_registers) regs = ((struct iovec*)data)->iov_base;
#else
 int get_registers = request == PTRACE_GETREGS;
#endif
 int kind=request==PTRACE_SEIZE?1:request==PTRACE_INTERRUPT?2:get_registers?3:0;
 if(kind){op_counts[kind]++;log_op(kind==1?'S':kind==2?'I':'G',tid);if(fail_request==kind&&op_counts[kind]==fail_n){errno=fail_errno;if(fail_once)fail_request=0;return -1;}}
 if(request==PTRACE_SEIZE){if(t->owned){errno=EPERM;return -1;}t->owned=1;return 0;}
 if(request==PTRACE_INTERRUPT){t->requested=1;if(advance_after_fanout&&op_counts[2]==(int)visible)now_ms=10000;return 0;}
 if(get_registers){if(!t->observed)bad_registers++;memset(regs,0,sizeof(*regs));return peak_regs_set_pc(regs,(uint64_t)(0x1000+tid));}
 if(request==PTRACE_DETACH){log_op('D',tid);t->detaches++;if(!t->requested){errno=ESRCH;return -1;}if((int)(intptr_t)data!=(t->signo==SIGTRAP?0:t->signo)){errno=EINVAL;return -1;}t->owned=0;return 0;}
 errno=EINVAL;return -1;
}
static pid_t fake_waitpid(pid_t tid,int*status,int options){
 (void)options;Target*t=target(tid);log_op('W',tid);if(!t){errno=ECHILD;return -1;}t->waits++;
 if(tid==exit_tid){if(!t->reaped){*status=0;t->gone=t->reaped=1;t->owned=0;return tid;}errno=ECHILD;return -1;}
 if(tid==never_ready_tid)return 0;
 if(tid==initial_unready_tid&&!signal_ready)return 0;
 *status=(t->signo<<8)|0x7f;t->observed=1;return tid;
}
static int fake_signal_wait(const sigset_t*s,siginfo_t*i,const struct timespec*t){(void)s;(void)i;signal_calls++;last_timeout_ms=t->tv_sec*1000+t->tv_nsec/1000000;signal_ready=1;return SIGCHLD;}
static int fake_clock(clockid_t id,struct timespec*t){(void)id;now_ms+=clock_step;t->tv_sec=now_ms/1000;t->tv_nsec=(now_ms%1000)*1000000;return 0;}
static int call_stop(int*detail,int*peer){int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)){return -99;}*peer=pair[1];int status=stop_target_threads(pair[0],99999,42,detail);close(pair[0]);return status;}
static int no_success_bytes(int peer){char b;ssize_t n=recv(peer,&b,1,MSG_DONTWAIT);close(peer);return n==0;}
static int read_success(int peer,unsigned n){
 PeakDetachHelperResponse response;ssize_t got=read(peer,&response,sizeof(response));if(got!=sizeof(response)||response.status!=PEAK_DETACH_HELPER_STATUS_OK||response.thread_count!=n){close(peer);return 0;}
 PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];got=read(peer,snapshots,n*sizeof(*snapshots));close(peer);if(got!=(ssize_t)(n*sizeof(*snapshots)))return 0;
 unsigned index=0;for(unsigned i=0;i<target_count;i++)if(!targets[i].gone){if(snapshots[index].tid!=targets[i].tid||snapshots[index].pc!=(uint64_t)(0x1000+targets[i].tid))return 0;index++;}return index==n;
}
static int no_owned(void){for(unsigned i=0;i<target_count;i++)if(targets[i].owned)return 0;return 1;}
static int test_full_success(void){
 int detail=0,peer;reset(3);CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(read_success(peer,3));CHECK(bad_registers==0&&held_thread_count==3);
 unsigned first_wait=0;while(first_wait<op_count&&ops[first_wait]!='W')first_wait++;unsigned interrupts=0;for(unsigned i=0;i<first_wait;i++)if(ops[i]=='I')interrupts++;CHECK(interrupts==3);
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(no_owned());return 0;
}
static int test_nth_seize(void){int detail=0,peer;reset(3);fail_request=1;fail_n=3;fail_errno=EPERM;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);CHECK(detail==EPERM&&no_owned()&&no_success_bytes(peer));CHECK(targets[0].detaches==1&&targets[1].detaches==1&&targets[2].detaches==0);return 0;}
static int test_nth_interrupt_legacy_boundary(void){int detail=0,peer;reset(3);fail_request=2;fail_n=3;fail_errno=EIO;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);CHECK(detail==EIO&&no_success_bytes(peer));CHECK(held_thread_count==1&&held_threads[0].tid==333&&!held_threads[0].interrupt_requested);CHECK(!targets[0].owned&&!targets[1].owned&&targets[2].owned);return 0;}
static int test_nth_getregs(void){int detail=0,peer;reset(3);fail_request=3;fail_n=2;fail_errno=EINVAL;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR);CHECK(detail==EINVAL&&no_owned()&&no_success_bytes(peer));CHECK(targets[2].waits==1&&targets[2].detaches==1);return 0;}
static int test_register_retry(void){int detail=0,peer;reset(3);fail_request=3;fail_n=2;fail_errno=EIO;fail_once=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(read_success(peer,3));CHECK(op_counts[1]==6);CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);return 0;}
static int test_census_error(void){int detail=0,peer;reset(3);census_error=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);CHECK(detail==EIO&&no_owned()&&no_success_bytes(peer));CHECK(op_counts[1]==2);return 0;}
static int test_limit(void){int detail=0,peer;reset(PEAK_DETACH_HELPER_MAX_THREADS+1);CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_THREAD_LIMIT);CHECK(detail==E2BIG&&no_owned()&&no_success_bytes(peer));CHECK(op_counts[2]==PEAK_DETACH_HELPER_MAX_THREADS);return 0;}
static int test_churn(void){int detail=0,peer;reset(3);visible=2;churn=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(read_success(peer,3));CHECK(op_counts[1]==3&&scan>=4);CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);return 0;}
static int test_exit_reap_retry(void){int detail=0,peer;reset(3);exit_tid=222;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(read_success(peer,2));CHECK(targets[1].reaped&&!targets[1].owned);CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);return 0;}
static int test_partial_timeout_retained(void){int detail=0,peer;reset(3);clock_step=10000;fail_request=3;fail_n=1;fail_errno=EINVAL;never_ready_tid=222;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);CHECK(detail==EINVAL&&no_success_bytes(peer));CHECK(held_thread_count==1&&held_threads[0].tid==222&&held_threads[0].interrupt_requested&&!held_threads[0].stop_observed);long saved=held_threads[0].pending_cleanup_deadline;CHECK(targets[2].detaches==1&&signal_calls==0);CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);CHECK(saved==held_threads[0].pending_cleanup_deadline&&signal_calls==0);return 0;}
static int test_normal_fresh_vs_cleanup(void){int detail=0,peer;reset(3);advance_after_fanout=1;initial_unready_tid=111;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(read_success(peer,3));CHECK(signal_calls==1&&last_timeout_ms==5000);CHECK(held_threads[0].pending_cleanup_deadline==5100&&now_ms==10000);CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);return 0;}
int main(void){
#define RUN(f) do{if(f())return 1;puts(#f " PASS");}while(0)
 RUN(test_full_success);RUN(test_nth_seize);RUN(test_nth_interrupt_legacy_boundary);RUN(test_nth_getregs);RUN(test_register_retry);RUN(test_census_error);RUN(test_limit);RUN(test_churn);RUN(test_exit_reap_retry);RUN(test_partial_timeout_retained);RUN(test_normal_fresh_vs_cleanup);
 puts("full_fanout_11_mock_paths_ok");return 0;
}
