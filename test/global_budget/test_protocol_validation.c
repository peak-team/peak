#define _GNU_SOURCE
/* White-box checks of wire validation, fragmentation and bounded hot reads. */
#include "../../src/global_budget.c"
#include <assert.h>
#include <sys/wait.h>
static void *consume(void *unused)
{
    (void)unused;
    for (unsigned i=0;i<1000;i++) (void)peak_global_budget_try_spend(1);
    return NULL;
}
int main(void)
{
    alarm(10);
    Rank r={0}; PeakGlobalBudgetSample s={.sequence=1};
    assert(valid_sample(&s,&r));
    s.profile_seconds=NAN; assert(!valid_sample(&s,&r));
    s.profile_seconds=-1; assert(!valid_sample(&s,&r));
    s.profile_seconds=1; s.control_seconds=2; s.management_seconds=3;
    s.elapsed_seconds=4; s.completed_wave=2; s.unfunded_seconds=.0004;
    r.seen=true;r.latest=s;
    assert(valid_sample(&s,&r));
    s.final=true;assert(!valid_sample(&s,&r));s.final=false;
    r.latest.retiring=true;s.retiring=true;assert(valid_sample(&s,&r));
    s.retiring=false;assert(!valid_sample(&s,&r));r.latest.retiring=false;
    s.profile_seconds=.9;assert(!valid_sample(&s,&r));s=r.latest;
    s.control_seconds=1;assert(!valid_sample(&s,&r));s=r.latest;
    s.management_seconds=2;assert(!valid_sample(&s,&r));s=r.latest;
    s.elapsed_seconds=3;assert(!valid_sample(&s,&r));s=r.latest;
    s.completed_wave=1;assert(!valid_sample(&s,&r));s=r.latest;
    s.unfunded_seconds=.0006;assert(!valid_sample(&s,&r));s=r.latest;
    s.unfunded_seconds=.0003;assert(!valid_sample(&s,&r));
    world=4;job_nonce=123;
    Frame f={.magic=MAGIC,.version=PROTOCOL_VERSION,.rank=3,.size=4,.nonce=123};
    assert(header(&f));f.nonce++;assert(!header(&f));f.nonce=123;
    f.rank=4;assert(!header(&f));f.rank=3;f.version=1;assert(!header(&f));
    f.version=PROTOCOL_VERSION;
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,pair));
    Peer p={.fd=pair[0],.rank=-1};size_t offset=0;
    while(offset<sizeof(f)){
        size_t n=sizeof(f)-offset;if(n>7)n=7;
        assert(send(pair[1],(char*)&f+offset,n,0)==(ssize_t)n);offset+=n;
        int result=read_frame(&p);assert(result==(offset==sizeof(f)?1:0));
    }
    assert(!memcmp(&f,&p.incoming,sizeof(f)));
    close(pair[1]);assert(read_frame(&p)==-1);close(pair[0]);
    atomic_store(&owner,getpid());
    publish_view((PeakGlobalBudgetView){.active=true,.granted_total_seconds=1e-6,.granted_total_ns=1000});
    assert(peak_global_budget_view().active);
    pthread_t t[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&t[i],NULL,consume,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(t[i],NULL));
    assert(atomic_load(&consumed_ns)==1000);
    assert(!peak_global_budget_try_spend(UINT64_MAX));
    atomic_store(&public_timestamp,ns_now(CLOCK_MONOTONIC)-FRESH_NS-1);
    assert(!peak_global_budget_view().active);
    atomic_fetch_add(&view_sequence,1);assert(!peak_global_budget_view().active);
    atomic_fetch_add(&view_sequence,1);
    pid_t pid=fork();assert(pid>=0);if(!pid){assert(!peak_global_budget_view().active);assert(!peak_global_budget_try_spend(1));peak_global_budget_stop();_exit(0);}
    int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    puts("global_budget_validation_ok");return 0;
}
