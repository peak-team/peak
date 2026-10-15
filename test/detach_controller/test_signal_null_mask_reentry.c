#define _GNU_SOURCE
#include "internal/signal_policy_internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int peak_process_profile_enabled(void) { return 1; }
int peak_process_requests_work(void) { return 1; }
int peak_exec_raw_syscall_handle(long n,long a,long b,long c,long d,long e,long f,long *r)
{ (void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)r;return 0; }
static long (*native_syscall)(long,...);
static int (*native_mask)(int,const sigset_t *,sigset_t *);
static int (*native_pthread_mask)(int,const sigset_t *,sigset_t *);
static volatile sig_atomic_t armed,returned,deliveries;
static int mode,event_fd;
struct kernel_action { uintptr_t handler,flags,restorer; uint64_t mask; };
static int query(int native, void *old)
{
    if (mode == 1) return native ? native_syscall(SYS_rt_sigprocmask,SIG_BLOCK,NULL,old,8)
                                : syscall(SYS_rt_sigprocmask,SIG_BLOCK,NULL,old,8);
    if (mode == 2) return native ? native_pthread_mask(SIG_BLOCK,NULL,old)
                                : pthread_sigmask(SIG_BLOCK,NULL,old);
    return native ? native_mask(SIG_BLOCK,NULL,old) : sigprocmask(SIG_BLOCK,NULL,old);
}
static void handler(int sig)
{
    (void)sig; deliveries++; write(event_fd,"E",1);
    sigset_t native_old,wrapped_old;
    memset(&native_old,0xa5,sizeof(native_old));
    memset(&wrapped_old,0xa5,sizeof(wrapped_old));
    uint64_t before=0,after=0;
    native_syscall(SYS_rt_sigprocmask,SIG_SETMASK,NULL,&before,8);
    errno=EDOM; int nr=query(1,&native_old),ne=errno;
    write(event_fd,nr==0 && ne==EDOM ? "N":"F",1);
    errno=EDOM; int wr=query(0,&wrapped_old),we=errno;
    native_syscall(SYS_rt_sigprocmask,SIG_SETMASK,NULL,&after,8);
    returned=nr==0 && wr==nr && ne==EDOM && we==ne && before==after &&
        (before & (UINT64_C(1)<<(SIGUSR1-1))) &&
        memcmp(&native_old,&wrapped_old,sizeof(native_old))==0;
    dprintf(1,"query mode=%d native=%d/%d wrapped=%d/%d oldset_equal=%d mask_equal=%d\n",
        mode,nr,ne,wr,we,memcmp(&native_old,&wrapped_old,sizeof(native_old))==0,before==after);
    write(event_fd,returned?"R":"F",1);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m)
{
    int rc=__real_pthread_mutex_lock(m);
    if (!rc && armed) { armed=0;write(event_fd,"L",1);
        native_syscall(SYS_tgkill,getpid(),native_syscall(SYS_gettid),SIGUSR1);
        write(event_fd,returned?"S":"F",1); }
    return rc;
}
static int run(int kind)
{
    int p[2]; if(pipe(p))return 1;
    pid_t child=fork(); if(child<0)return 1;
    if(!child){ close(p[0]);event_fd=p[1];mode=kind;
        struct sigaction sa={.sa_handler=handler};sigemptyset(&sa.sa_mask);
        if(sigaction(SIGUSR1,&sa,NULL))_exit(2);
        struct kernel_action before={0},after={0};
        native_syscall(SYS_rt_sigaction,SIGUSR1,NULL,&before,8);
        int reserved=peak_signal_policy_choose_reserved_signal();
        int migrations=peak_signal_policy_migration_count();
        sa.sa_handler=SIG_IGN;armed=1;
        int rc=sigaction(reserved,&sa,NULL);
        native_syscall(SYS_rt_sigaction,SIGUSR1,NULL,&after,8);
        int delta=peak_signal_policy_migration_count()-migrations;
        printf("outer mode=%d rc=%d migrations=%d handler_equal=%d deliveries=%d\n",
            kind,rc,delta,memcmp(&before,&after,sizeof(before))==0,deliveries);fflush(stdout);
        _exit(rc==0 && returned && deliveries==1 && delta==1 &&
            peak_signal_policy_reserved_signal()!=reserved &&
            memcmp(&before,&after,sizeof(before))==0?0:3);
    }
    close(p[1]);char events[32]={0};int used=0,status=0,finished=0;
    for(int i=0;i<100;i++){
        struct pollfd fd={.fd=p[0],.events=POLLIN};
        if(poll(&fd,1,10)>0 && (fd.revents&POLLIN) && used<31){
            int n=read(p[0],events+used,31-used);if(n>0)used+=n;}
        if(waitpid(child,&status,WNOHANG)==child){finished=1;break;}
        usleep(1000);
    }
    if(!finished){kill(child,SIGKILL);waitpid(child,&status,0);}
    while(used<31){int n=read(p[0],events+used,31-used);if(n<=0)break;used+=n;}
    close(p[0]);
    int pass=finished && WIFEXITED(status) && WEXITSTATUS(status)==0 && !strcmp(events,"LENRS");
    printf("mode=%d events=%s completed=%d status=%d reaped=1 pass=%d\n",kind,events,finished,status,pass);
    fflush(stdout);return !pass;
}
static int errors(void)
{
    uint64_t old=0;int failures=0;
    for(int kind=0;kind<2;kind++){
        void *out=kind ? &old : (void *)1;size_t size=kind?7:8;
        errno=EDOM;long nr=native_syscall(SYS_rt_sigprocmask,123,NULL,out,size);int ne=errno;
        errno=EDOM;long wr=syscall(SYS_rt_sigprocmask,123,NULL,out,size);int we=errno;
        int expected=kind?EINVAL:EFAULT;
        int pass=nr==-1 && wr==nr && ne==expected && we==ne;
        printf("error kind=%d native=%ld/%d wrapped=%ld/%d pass=%d\n",kind,nr,ne,wr,we,pass);
        failures+=!pass;
    }
    return failures;
}
int main(void)
{
    native_syscall=dlsym(RTLD_NEXT,"syscall");native_mask=dlsym(RTLD_NEXT,"sigprocmask");
    native_pthread_mask=dlsym(RTLD_NEXT,"pthread_sigmask");
    if(!native_syscall || !native_mask || !native_pthread_mask)return 2;
    unsetenv("PEAK_DETACH_SIGNAL");int failures=errors();
    for(int i=0;i<3;i++)failures+=run(i);
    return failures?1:0;
}
