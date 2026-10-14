#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fake_ptrace_errno;
static int fake_ptrace_calls;
static int protocol_failure_mode;
static long fake_time_ms;
static int scripted, signal_calls, wait_calls, ready_b, ready_a,unknown_proc;
static pid_t fail_detach_tid;
static int open_for_test(const char*,int,...);
static long fixed_now;
static int wait_mode;
static pid_t wait_tids[16],detach_tids[16];
static int detach_signals[16],detach_count;
static int sigtimedwait_for_test(const sigset_t*,siginfo_t*,const struct timespec*);

static pid_t waitpid_for_test(pid_t pid, int* status, int options);
static int clock_gettime_for_test(clockid_t clock_id, struct timespec* ts);

#define PEAK_DETACH_HELPER_PTRACE_STOP_WAIT_UNIT_TEST 1
#define open open_for_test
#define sigtimedwait sigtimedwait_for_test
#define ptrace ptrace_for_test
#define waitpid waitpid_for_test
#define clock_gettime clock_gettime_for_test
#include "../../src/detach_helper.c"
#undef open
#undef sigtimedwait
#undef clock_gettime
#undef waitpid
#undef ptrace

long
ptrace_for_test(enum __ptrace_request request, ...)
{
    if (scripted) {
        va_list ap;va_start(ap,request);pid_t tid=va_arg(ap,pid_t);(void)va_arg(ap,void*);void*data=va_arg(ap,void*);va_end(ap);
        if(request!=PTRACE_DETACH){errno=EINVAL;return -1;}
        detach_tids[detach_count]=tid;detach_signals[detach_count++]=(int)(intptr_t)data;
        if(fake_ptrace_errno && (!fail_detach_tid || fail_detach_tid==tid)){errno=fake_ptrace_errno;return -1;}return 0;
    }
    if (protocol_failure_mode &&
        (request == PTRACE_SEIZE || request == PTRACE_INTERRUPT)) {
        return 0;
    }
    if (request != PTRACE_DETACH) {
        errno = EINVAL;
        return -1;
    }
    fake_ptrace_calls++;
    if (fake_ptrace_errno != 0) {
        errno = fake_ptrace_errno;
        return -1;
    }
    return 0;
}

static pid_t
waitpid_for_test(pid_t pid, int* status, int options)
{
    (void)options;
    if(scripted) {
        wait_tids[wait_calls++]=pid;
        if(wait_mode==5){if(pid==99999999 || pid==getpid()){errno=ECHILD;return -1;}*status=(SIGTRAP<<8)|0x7f;return pid;}
        if(wait_mode==4){if(wait_calls==1){*status=0;return pid;}errno=ECHILD;return -1;}
        if(wait_mode==1){errno=ECHILD;return -1;}
        if(wait_mode==2){errno=ESRCH;return -1;}
        if(wait_mode==3)return 0;
        if(pid==111 && !ready_a)return 0;
        if(pid==222 && !ready_b){errno=EIO;return -1;}
        *status=(pid==111 ? (SIGUSR1<<8)|0x7f : (128<<16)|(SIGTRAP<<8)|0x7f);
        return pid;
    }
    (void)pid;(void)status;return 0;
}

static int
clock_gettime_for_test(clockid_t clock_id, struct timespec* ts)
{
    (void)clock_id;
    if(scripted){ts->tv_sec=fixed_now/1000;ts->tv_nsec=(fixed_now%1000)*1000000;return 0;}
    fake_time_ms += 10000;
    ts->tv_sec = fake_time_ms / 1000;
    ts->tv_nsec = (fake_time_ms % 1000) * 1000000;
    return 0;
}

static int open_for_test(const char*path,int flags,...) {
 if(scripted && unknown_proc && strstr(path,"/proc/")==path){errno=EACCES;return -1;}
 if(flags&O_CREAT){va_list ap;va_start(ap,flags);int mode=va_arg(ap,int);va_end(ap);return open(path,flags,mode);}
 return open(path,flags);
}
static int sigtimedwait_for_test(const sigset_t*s,siginfo_t*i,const struct timespec*t) {
 (void)s;(void)i;(void)t;signal_calls++;ready_a=1;return SIGCHLD;
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return 1; \
} } while (0)

static int
test_esrch_running_tracee_is_release_failure(void)
{
    int detail = 0;

    held_thread_count = 1;
    held_threads[0] = (PeakHeldThread){.tid = getpid(), .attached = 1};
    fake_ptrace_errno = ESRCH;
    fake_ptrace_calls = 0;
    CHECK(cleanup_held_threads_or_release_failed(
              PEAK_DETACH_HELPER_STATUS_TIMEOUT, &detail) ==
          PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
    CHECK(detail == ESRCH);
    CHECK(held_thread_count == 1);
    CHECK(fake_ptrace_calls == 1);
    return 0;
}

static int
test_esrch_exited_tracee_is_released(void)
{
    int detail = 0;

    held_thread_count = 1;
    held_threads[0] = (PeakHeldThread){.tid = 99999999, .attached = 1};
    fake_ptrace_errno = ESRCH;
    CHECK(cleanup_held_threads_or_release_failed(
              PEAK_DETACH_HELPER_STATUS_TIMEOUT, &detail) ==
          PEAK_DETACH_HELPER_STATUS_TIMEOUT);
    CHECK(held_thread_count == 0);
    return 0;
}

static int
test_normal_detach_releases_tracee(void)
{
    int detail = 0;

    held_thread_count = 1;
    held_threads[0] = (PeakHeldThread){.tid = getpid(), .attached = 1};
    fake_ptrace_errno = 0;
    CHECK(cleanup_held_threads_or_release_failed(
              PEAK_DETACH_HELPER_STATUS_OK, &detail) ==
          PEAK_DETACH_HELPER_STATUS_OK);
    CHECK(held_thread_count == 0);
    return 0;
}

static int
test_stop_release_failure_exits_helper(void)
{
    int pair[2];
    pid_t child;
    int status;
    PeakDetachHelperRequest request = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .command = PEAK_DETACH_HELPER_CMD_STOP,
        .pid = (int32_t)getpid(),
        .controller_tid = 0,
        .instruction_count = 0
    };
    PeakDetachHelperResponse response;
    char trailing;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int result;
        close(pair[0]);
        protocol_failure_mode = 1;
        fake_ptrace_errno = ESRCH;
        fake_time_ms = 0;
        result = serve_protocol(pair[1]);
        close(pair[1]);
        _exit(result);
    }
    close(pair[1]);
    CHECK(write_exact(pair[0], &request, sizeof(request)) == 0);
    CHECK(read_exact(pair[0], &response, sizeof(response)) == 1);
    CHECK(response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
    CHECK(response.errno_value == ETIMEDOUT);
    CHECK(read(pair[0], &trailing, 1) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    close(pair[0]);
    return 0;
}


static void reset_pending(void) {
 scripted=1;unknown_proc=0;fail_detach_tid=0;signal_calls=wait_calls=detach_count=0;ready_a=0;ready_b=1;fixed_now=50;wait_mode=0;fake_ptrace_errno=0;held_thread_count=0;
}
static PeakHeldThread pending(pid_t tid,long deadline) {
 return (PeakHeldThread){.tid=tid,.attached=1,.interrupt_requested=1,.stop_observed=0,.pending_cleanup_deadline=deadline};
}
static int test_pending_reverse_ready_and_signal(void) {
 int detail=0;reset_pending();held_threads[0]=pending(111,100);held_threads[1]=pending(222,100);held_thread_count=2;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(held_thread_count==0);
 CHECK(wait_calls==3&&wait_tids[0]==111&&wait_tids[1]==111&&wait_tids[2]==222);CHECK(signal_calls==1);
 CHECK(detach_count==2&&detach_tids[0]==111&&detach_tids[1]==222);
 CHECK(detach_signals[0]==SIGUSR1&&detach_signals[1]==0);return 0;
}
static int test_pending_expired_not_refreshed(void) {
 int detail=0;reset_pending();fixed_now=100;wait_mode=3;held_threads[0]=pending(111,100);held_threads[1]=pending(222,90);held_thread_count=2;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);CHECK(detail==ETIMEDOUT);
 CHECK(held_thread_count==2&&wait_calls==2&&signal_calls==0&&detach_count==0);
 CHECK(held_threads[0].pending_cleanup_deadline==100&&held_threads[1].pending_cleanup_deadline==90);
 detail=0;CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);CHECK(wait_calls==4&&signal_calls==0);
 CHECK(held_threads[0].pending_cleanup_deadline==100);return 0;
}
static int test_pending_echild_gone(void) {
 int detail=0;reset_pending();wait_mode=1;held_threads[0]=pending(99999999,100);held_thread_count=1;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);CHECK(held_thread_count==0&&detach_count==0);return 0;
}
static int test_exit_consumed_then_echild_gone(void) {
 int detail=0,signal=-1;reset_pending();wait_mode=4;held_threads[0]=pending(99999999,100);held_thread_count=1;
 CHECK(wait_for_ptrace_stop_until(99999999,&signal,100)==-1);CHECK(errno==ESRCH);
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);
 CHECK(wait_calls==2&&held_thread_count==0&&detach_count==0);return 0;
}
static int test_pending_echild_extant(void) {
 int detail=0;reset_pending();wait_mode=1;held_threads[0]=pending(getpid(),100);held_thread_count=1;
 CHECK(cleanup_held_threads_or_release_failed(PEAK_DETACH_HELPER_STATUS_TIMEOUT,&detail)==PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED);
 CHECK(detail==ECHILD&&held_thread_count==1&&detach_count==0);CHECK(!held_threads[0].stop_observed);return 0;
}
static int test_pending_esrch_extant_compaction(void) {
 int detail=0;reset_pending();wait_mode=2;held_threads[0]=pending(99999999,100);held_threads[1]=pending(getpid(),200);held_thread_count=2;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);CHECK(detail==ESRCH&&held_thread_count==1);
 CHECK(held_threads[0].tid==getpid()&&held_threads[0].pending_cleanup_deadline==200&&held_threads[0].interrupt_requested==1);
 CHECK(detach_count==0);return 0;
}

static int test_pending_echild_unknown_proc(void) {
 int detail=0;reset_pending();wait_mode=1;unknown_proc=1;held_threads[0]=pending(99999999,100);held_thread_count=1;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);
 CHECK(detail==ECHILD&&held_thread_count==1&&detach_count==0);return 0;
}
static int test_observed_detach_failure_compaction(void) {
 int detail=0;reset_pending();held_threads[0]=pending(111,123);held_threads[0].stop_observed=1;held_threads[0].detach_signal=SIGUSR1;
 held_threads[1]=pending(222,456);held_threads[1].stop_observed=1;held_thread_count=2;fake_ptrace_errno=EIO;fail_detach_tid=111;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);
 CHECK(detail==EIO&&held_thread_count==1&&wait_calls==0&&detach_count==2);
 CHECK(held_threads[0].tid==111&&held_threads[0].pending_cleanup_deadline==123&&held_threads[0].detach_signal==SIGUSR1&&held_threads[0].stop_observed);
 fake_ptrace_errno=0;detail=0;CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_OK);
 CHECK(held_thread_count==0&&wait_calls==0&&detach_count==3&&detach_signals[2]==SIGUSR1);return 0;
}
static int test_gone_cleanup_retry_and_outer_deadline(void) {
 int detail=ESRCH,retry=-1;reset_pending();wait_mode=1;held_threads[0]=pending(99999999,100);held_thread_count=1;
 CHECK(cleanup_or_retry_stop_snapshot(PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR,&detail,100,&retry)==PEAK_DETACH_HELPER_STATUS_OK);
 CHECK(retry==1&&detail==0&&held_thread_count==0);
 reset_pending();wait_mode=1;fixed_now=100;held_threads[0]=pending(99999999,100);held_thread_count=1;detail=ESRCH;retry=-1;
 CHECK(cleanup_or_retry_stop_snapshot(PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR,&detail,100,&retry)==PEAK_DETACH_HELPER_STATUS_TIMEOUT);
 CHECK(retry==0&&detail==ETIMEDOUT&&held_thread_count==0);return 0;
}
static int test_three_pending_cleanup_continues_after_failure(void) {
 int detail=0;reset_pending();wait_mode=5;held_threads[0]=pending(99999999,100);held_threads[1]=pending(getpid(),200);held_threads[2]=pending(333,300);held_thread_count=3;
 CHECK(detach_held_threads(&detail)==PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR);
 CHECK(detail==ECHILD&&held_thread_count==1&&wait_calls==3&&detach_count==1&&detach_tids[0]==333);
 CHECK(held_threads[0].tid==getpid()&&held_threads[0].pending_cleanup_deadline==200&&held_threads[0].interrupt_requested&&!held_threads[0].stop_observed);return 0;
}
int
main(void)
{
    if(test_pending_echild_unknown_proc()!=0 || test_observed_detach_failure_compaction()!=0 ||
       test_gone_cleanup_retry_and_outer_deadline()!=0 || test_three_pending_cleanup_continues_after_failure()!=0)return 1;
    if(test_pending_reverse_ready_and_signal()!=0 || test_pending_expired_not_refreshed()!=0 ||
       test_pending_echild_gone()!=0 || test_exit_consumed_then_echild_gone()!=0 || test_pending_echild_extant()!=0 || test_pending_esrch_extant_compaction()!=0)return 1;
    scripted=0;
    if (test_esrch_running_tracee_is_release_failure() != 0 ||
        test_esrch_exited_tracee_is_released() != 0 ||
        test_normal_detach_releases_tracee() != 0 ||
        test_stop_release_failure_exits_helper() != 0) {
        return 1;
    }
    puts("detach_helper_release_ok original4 plus_pending10");
    return 0;
}
