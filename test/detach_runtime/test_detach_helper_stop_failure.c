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
static ssize_t fake_write(int,const void*,size_t);
static int fail_log_write;
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
#define write fake_write
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
#undef write
#undef waitpid
#undef clock_gettime
#undef sigtimedwait
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__func__,__LINE__,#x);return 1;}}while(0)
typedef struct {pid_t tid;int owned,requested,observed,gone,reaped,signo;int waits,detaches;} Target;
static Target targets[PEAK_DETACH_HELPER_MAX_THREADS+1];
static unsigned target_count,visible,position,scan;
static int churn,census_error,fail_request,fail_n,fail_errno,fail_once,op_counts[4];
static int opendir_fail_scan,vanish_on_fail,fail_detach_tid;
static int never_ready_tid,initial_unready_tid,signal_ready,signal_calls,bad_registers;
static int exit_tid,clock_step,advance_after_fanout,fail_wait_tid;
static long now_ms,last_timeout_ms;
static char ops[32768];static pid_t tids[32768];static unsigned op_count;
static void log_op(char op,pid_t tid){if(op_count<32768){ops[op_count]=op;tids[op_count++]=tid;}}
static Target* target(pid_t tid){for(unsigned i=0;i<target_count;i++)if(targets[i].tid==tid)return &targets[i];return NULL;}
static void reset(unsigned n){
 memset(targets,0,sizeof(targets));target_count=visible=n;position=0;scan=0;churn=census_error=0;fail_request=fail_n=fail_errno=fail_once=0;
 memset(op_counts,0,sizeof(op_counts));never_ready_tid=initial_unready_tid=signal_ready=signal_calls=bad_registers=0;exit_tid=clock_step=advance_after_fanout=fail_wait_tid=0;
 opendir_fail_scan=vanish_on_fail=fail_detach_tid=0;now_ms=100;last_timeout_ms=-1;op_count=0;held_thread_count=0;
 for(unsigned i=0;i<n;i++){targets[i].tid=111+(pid_t)i*111;targets[i].signo=SIGTRAP;}
 if(n>1)targets[1].signo=SIGUSR1;
}
static DIR* fake_opendir(const char*path){(void)path;if(opendir_fail_scan==scan+1){scan++;errno=EPERM;return NULL;}if(churn&&scan==1)visible=target_count;position=0;scan++;return (DIR*)(uintptr_t)1;}
static struct dirent* fake_readdir(DIR*d){
 (void)d;static struct dirent e;
 if(census_error&&scan==1&&position==3){errno=EIO;return NULL;}
 while(position<=visible){unsigned p=position++;pid_t tid=p==0?42:targets[p-1].tid;if(p&&targets[p-1].gone)continue;snprintf(e.d_name,sizeof(e.d_name),"%ld",(long)tid);return &e;}
 errno=0;return NULL;
}
static int fake_closedir(DIR*d){(void)d;return 0;}
static int fake_open(const char*path,int flags,...){
 if(!strcmp(path,"/proc/self/fd/2"))return open(path,flags);
 (void)flags;long tid=0;if(sscanf(path,"/proc/%ld/status",&tid)==1){Target*t=target((pid_t)tid);if(t&&t->gone){errno=ENOENT;return -1;}return 9876;}
 errno=EACCES;return -1;
}
static ssize_t fake_write(int fd,const void*b,size_t n){if(fail_log_write){errno=ENOSPC;return -1;}return write(fd,b,n);}
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
 if(kind){op_counts[kind]++;log_op(kind==1?'S':kind==2?'I':'G',tid);if(fail_request==kind&&op_counts[kind]==fail_n){if(vanish_on_fail){t->gone=1;t->owned=0;}errno=fail_errno;if(fail_once)fail_request=0;return -1;}}
 if(request==PTRACE_SEIZE){if(t->owned){errno=EPERM;return -1;}t->owned=1;return 0;}
 if(request==PTRACE_INTERRUPT){t->requested=1;if(advance_after_fanout&&op_counts[2]==(int)visible)now_ms=10000;return 0;}
 if(get_registers){if(!t->observed)bad_registers++;memset(regs,0,sizeof(*regs));return peak_regs_set_pc(regs,(uint64_t)(0x1000+tid));}
 if(request==PTRACE_DETACH){log_op('D',tid);t->detaches++;if(tid==fail_detach_tid){errno=EIO;return -1;}if(!t->requested||!t->observed){errno=ESRCH;return -1;}if((int)(intptr_t)data!=(t->signo==SIGTRAP?0:t->signo)){errno=EINVAL;return -1;}t->owned=0;return 0;}
 errno=EINVAL;return -1;
}
static pid_t fake_waitpid(pid_t tid,int*status,int options){
 (void)options;Target*t=target(tid);log_op('W',tid);if(!t){errno=ECHILD;return -1;}t->waits++;
 if(tid==fail_wait_tid){fail_wait_tid=0;errno=EINVAL;return -1;}
 if(tid==exit_tid){if(!t->reaped){*status=0;t->gone=t->reaped=1;t->owned=0;return tid;}errno=ECHILD;return -1;}
 if(tid==never_ready_tid)return 0;
 if(tid==initial_unready_tid&&!signal_ready)return 0;
 *status=(t->signo<<8)|0x7f;t->observed=1;return tid;
}
static int fake_signal_wait(const sigset_t*s,siginfo_t*i,const struct timespec*t){(void)s;(void)i;signal_calls++;last_timeout_ms=t->tv_sec*1000+t->tv_nsec/1000000;signal_ready=1;return SIGCHLD;}
static int fake_clock(clockid_t id,struct timespec*t){(void)id;now_ms+=clock_step;t->tv_sec=now_ms/1000;t->tv_nsec=(now_ms%1000)*1000000;return 0;}
static char diagnostic[768];
static int call_stop(int*detail,int*peer){int pair[2],logpipe[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)||pipe(logpipe))return -99;int saved=dup(2);if(saved<0||dup2(logpipe[1],2)<0)return -99;close(logpipe[1]);*peer=pair[1];int status=stop_target_threads(pair[0],99999,42,detail);int observed_errno=errno;dup2(saved,2);close(saved);ssize_t n=read(logpipe[0],diagnostic,sizeof(diagnostic)-1);close(logpipe[0]);diagnostic[n>0?n:0]=0;close(pair[0]);errno=observed_errno;return status;}
static int diagnostic_matches(const char*op,const char*stage,int tid,int before,int status,int after){char wanted[512];snprintf(wanted,sizeof(wanted),"operation=%s stage=%s target_pid=99999 tid=%d first_errno=1 held_before_cleanup=%d last_operation=%s last_stage=%s last_tid=%d last_errno=1 last_held=%d final_status=%d final_errno=1 held_after_cleanup=%d\n",op,stage,tid,before,op,stage,tid,before,status,after);CHECK(strstr(diagnostic,wanted)!=NULL);CHECK(strchr(diagnostic,'\n')==diagnostic+strlen(diagnostic)-1);return 0;}

static int no_success_bytes(int peer){char b;ssize_t n=recv(peer,&b,1,MSG_DONTWAIT);close(peer);return n==0;}
static int read_success(int peer,unsigned n){
 PeakDetachHelperResponse response;ssize_t got=read(peer,&response,sizeof(response));if(got!=sizeof(response)||response.status!=PEAK_DETACH_HELPER_STATUS_OK||response.thread_count!=n){close(peer);return 0;}
 PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];got=read(peer,snapshots,n*sizeof(*snapshots));close(peer);if(got!=(ssize_t)(n*sizeof(*snapshots)))return 0;
 unsigned index=0;for(unsigned i=0;i<target_count;i++)if(!targets[i].gone){if(snapshots[index].tid!=targets[i].tid||snapshots[index].pc!=(uint64_t)(0x1000+targets[i].tid))return 0;index++;}return index==n;
}
static int no_owned(void){for(unsigned i=0;i<target_count;i++)if(targets[i].owned)return 0;return 1;}
/* Historical13 batch-only test bodies omitted: these7 scenarios are source-compatible with44. */
static int test_initial_opendir_eperm(void){int detail=0,peer;reset(3);opendir_fail_scan=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);CHECK(detail==EPERM&&held_thread_count==0&&no_owned()&&no_success_bytes(peer));CHECK(op_counts[1]==0&&op_counts[2]==0&&now_ms==100);CHECK(diagnostic_matches("opendir","snapshot",99999,0,3,0)==0);return 0;}
static int test_seize_eperm_live(void){int detail=0,peer;reset(3);fail_request=1;fail_n=3;fail_errno=EPERM;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);CHECK(detail==EPERM&&held_thread_count==0&&no_owned()&&no_success_bytes(peer));CHECK(!targets[2].gone&&targets[2].detaches==0&&targets[0].observed&&targets[1].observed&&targets[0].detaches==1&&targets[1].detaches==1&&now_ms==100);CHECK(diagnostic_matches("PTRACE_SEIZE","snapshot",333,2,3,0)==0);return 0;}
static int test_seize_eperm_vanished(void){int detail=0,peer;reset(3);fail_request=1;fail_n=3;fail_errno=EPERM;vanish_on_fail=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);CHECK(detail==EPERM&&held_thread_count==0&&no_owned()&&no_success_bytes(peer));CHECK(targets[2].gone&&targets[0].observed&&targets[1].observed&&targets[0].detaches==1&&targets[1].detaches==1&&now_ms==100);CHECK(diagnostic_matches("PTRACE_SEIZE","snapshot",333,2,3,0)==0);return 0;}
static int test_interrupt_eperm_live(void){int detail=0,peer;reset(3);fail_request=2;fail_n=3;fail_errno=EPERM;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);CHECK(detail==EPERM&&held_thread_count==1&&no_success_bytes(peer));CHECK(targets[2].owned&&!targets[2].requested&&!targets[2].observed&&!targets[2].gone&&held_threads[0].tid==333);CHECK(!targets[0].owned&&!targets[1].owned&&targets[0].detaches==1&&targets[1].detaches==1&&now_ms==100);held_thread_count=0;CHECK(diagnostic_matches("PTRACE_INTERRUPT","snapshot",333,3,8,1)==0);return 0;}
static int test_interrupt_eperm_vanished(void){int detail=0,peer;reset(3);fail_request=2;fail_n=3;fail_errno=EPERM;vanish_on_fail=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);CHECK(detail==EPERM&&held_thread_count==0&&no_owned()&&no_success_bytes(peer));CHECK(targets[2].gone&&!targets[2].requested&&targets[0].observed&&targets[1].observed&&targets[0].detaches==1&&targets[1].detaches==1&&now_ms==100);CHECK(diagnostic_matches("PTRACE_INTERRUPT","snapshot",333,3,3,0)==0);return 0;}
static int test_rescan_opendir_eperm_releases(void){int detail=0,peer;reset(3);visible=2;churn=1;opendir_fail_scan=3;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);CHECK(detail==EPERM&&no_success_bytes(peer));printf("RESCAN_OBSERVATION held=%zu owned0=%d owned1=%d detaches0=%d detaches1=%d\n",held_thread_count,targets[0].owned,targets[1].owned,targets[0].detaches,targets[1].detaches);CHECK(held_thread_count==0&&no_owned());CHECK(targets[0].detaches==1&&targets[1].detaches==1&&op_counts[1]==2&&now_ms==100);CHECK(diagnostic_matches("opendir","rescan",99999,2,3,0)==0);return 0;}
static int test_rescan_opendir_eperm_release_failure(void){int detail=0,peer;reset(3);visible=2;churn=1;opendir_fail_scan=3;fail_detach_tid=111;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);CHECK(detail==EPERM&&no_success_bytes(peer));CHECK(held_thread_count==1&&held_threads[0].tid==111&&targets[0].owned&&!targets[1].owned);CHECK(targets[0].detaches==1&&targets[1].detaches==1&&held_threads[0].detach_signal==0&&now_ms==100);held_thread_count=0;CHECK(diagnostic_matches("opendir","rescan",99999,2,8,1)==0);return 0;}
static int test_logging_failure_preserves_operation(void){int detail=0,peer;reset(3);opendir_fail_scan=1;fail_log_write=1;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED);fail_log_write=0;CHECK(detail==EPERM&&no_success_bytes(peer)&&diagnostic[0]==0);return 0;}
static int test_verify_opendir_failure(void){int detail=0,peer;reset(2);opendir_fail_scan=2;CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);CHECK(detail==EPERM&&no_owned()&&held_thread_count==0&&no_success_bytes(peer));CHECK(diagnostic_matches("opendir","verify",99999,2,5,0)==0);return 0;}
static int test_success_is_silent(void){int detail=0,peer;reset(2);CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(diagnostic[0]==0&&read_success(peer,2));int e=0;CHECK(detach_held_threads(&e)==PEAK_DETACH_HELPER_STATUS_OK);return 0;}
static int test_retry_preserves_first_and_terminal_failures(void){
 int detail=0,peer;reset(2);fail_request=3;fail_n=1;fail_errno=ESRCH;fail_once=1;opendir_fail_scan=3;
 CHECK(call_stop(&detail,&peer)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);
 CHECK(detail==EPERM&&held_thread_count==0&&no_owned()&&no_success_bytes(peer));
 CHECK(strstr(diagnostic,"operation=get_registers stage=snapshot target_pid=99999 tid=111 first_errno=3 held_before_cleanup=1 last_operation=opendir last_stage=verify last_tid=99999 last_errno=1 last_held=2 final_status=5 final_errno=1 held_after_cleanup=0\n")!=NULL);
 CHECK(strchr(diagnostic,'\n')==diagnostic+strlen(diagnostic)-1);
 CHECK(targets[0].detaches==2&&targets[1].detaches==1&&scan==3);return 0;
}
static int test_first_record_is_preserved(void){PeakStopFailure f={0};reset(2);held_thread_count=1;record_stop_failure(&f,"PTRACE_SEIZE","snapshot",111,ESRCH);held_thread_count=0;record_stop_failure(&f,"PTRACE_INTERRUPT","snapshot",222,EPERM);CHECK(!strcmp(f.operation,"PTRACE_SEIZE")&&f.error==ESRCH&&f.tid==111&&f.held==1&&f.last_tid==222&&f.last_error==EPERM);return 0;}
static int test_failure_logger_io(void){
 int saved=dup(2),p[2];CHECK(saved>=0&&pipe(p)==0);CHECK(dup2(p[1],2)>=0);close(p[1]);int flags=fcntl(2,F_GETFL);CHECK(fcntl(2,F_SETFL,flags|O_NONBLOCK)==0);char b[4096]={0};while(write(2,b,sizeof(b))>0){}CHECK(errno==EAGAIN);CHECK(fcntl(2,F_SETFL,flags)==0);errno=EDOM;write_stop_failure_diagnostic("full\n",5);CHECK(errno==EDOM&&fcntl(2,F_GETFL)==flags);CHECK(dup2(saved,2)>=0);close(p[0]);
 FILE*f=tmpfile();CHECK(f!=NULL&&dup2(fileno(f),2)>=0);CHECK(write(2,"prefix",6)==6);errno=ERANGE;write_stop_failure_diagnostic("record\n",7);CHECK(errno==ERANGE&&lseek(2,0,SEEK_CUR)==13);fail_log_write=1;errno=EPERM;write_stop_failure_diagnostic("failed\n",7);CHECK(errno==EPERM);fail_log_write=0;CHECK(dup2(saved,2)>=0);close(saved);rewind(f);CHECK(fread(b,1,13,f)==13&&!memcmp(b,"prefixrecord\n",13));fclose(f);return 0;
}
int main(void){
#define RUN(f) do{if(f())return 1;puts(#f " PASS");}while(0)
 RUN(test_retry_preserves_first_and_terminal_failures);RUN(test_first_record_is_preserved);RUN(test_success_is_silent);RUN(test_verify_opendir_failure);RUN(test_logging_failure_preserves_operation);RUN(test_failure_logger_io);RUN(test_initial_opendir_eperm);RUN(test_seize_eperm_live);RUN(test_seize_eperm_vanished);RUN(test_interrupt_eperm_live);RUN(test_interrupt_eperm_vanished);RUN(test_rescan_opendir_eperm_releases);
#ifdef TEST_RESCAN_CLEANUP_FAILURE
 RUN(test_rescan_opendir_eperm_release_failure);
 puts("stop_failure_7_mock_paths_ok");
#else
 puts("stop_failure_6_mock_paths_ok");
#endif
 return 0;
}
