#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
int main(void)
{
    alarm(5);
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    assert(!peak_global_budget_start("127.0.0.1",port,0,2,.5,NULL));
    PeakGlobalBudgetSample s={.sequence=1,.elapsed_seconds=.1};
    peak_global_budget_publish(&s);
    int raw=connect_host("127.0.0.1",port);assert(raw>=0);
    Frame f={.magic=MAGIC,.version=1,.nonce=job_nonce,.rank=1,.size=2,
        .message_sequence=1,.sample={.sequence=1,.elapsed_seconds=.1}};
    size_t sent=0;double deadline=seconds_now()+1;
    while(sent<sizeof(f)&&seconds_now()<deadline){
        ssize_t n=send(raw,(char*)&f+sent,sizeof(f)-sent,MSG_NOSIGNAL);
        if(n>0)sent+=(size_t)n;else usleep(1000);
    }
    assert(sent==sizeof(f));
    while(!peak_global_budget_view().active&&seconds_now()<deadline)usleep(1000);
    assert(peak_global_budget_view().active);
    /* Other participant leaves first. Our final snapshot has not been sent. */
    close(raw);
    deadline=seconds_now()+.5;
    while(peak_global_budget_view().active&&seconds_now()<deadline)usleep(1000);
    assert(!peak_global_budget_view().active);
    s.sequence=2;s.elapsed_seconds=.2;s.profile_seconds=.001;
    peak_global_budget_publish(&s);
    assert(peak_global_budget_flush(100));
    PeakGlobalBudgetView v=peak_global_budget_view();
    assert(!v.active&&v.acknowledged_sequence>=2);
    peak_global_budget_stop();
    puts("global_budget_peer_shutdown_ok");return 0;
}
