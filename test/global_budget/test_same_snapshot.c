#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static void raw_send(int s,Frame *f)
{
 size_t used=0;double deadline=seconds_now()+.5;
 while(used<sizeof(*f)&&seconds_now()<deadline){ssize_t n=send(s,(char*)f+used,sizeof(*f)-used,MSG_NOSIGNAL);if(n>0)used+=(size_t)n;else usleep(1000);}
 assert(used==sizeof(*f));
}
static Frame raw_receive(int s,double minimum)
{
 Peer p={.fd=s};double deadline=seconds_now()+.8;
 while(seconds_now()<deadline){int n=read_frame(&p);assert(n>=0);if(n==1){Frame f=p.incoming;p.received=0;if(f.view.active&&f.view.granted_total_seconds>=minimum)return f;}usleep(1000);}
 assert(!"no funded reply");return (Frame){0};
}
static Frame raw_wave(int s,uint64_t wave,uint64_t completed)
{
 Peer p={.fd=s};double deadline=seconds_now()+.7;
 while(seconds_now()<deadline){int n=read_frame(&p);assert(n>=0);if(n==1){Frame f=p.incoming;p.received=0;if(f.view.wave>=wave&&f.view.root_completed_wave>=completed)return f;}usleep(1000);}
 assert(!"wave confirmation missing");return (Frame){0};
}
int main(void)
{
 alarm(8);unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,2,.5,NULL));
 PeakGlobalBudgetSample s={.sequence=1,.elapsed_seconds=.1};peak_global_budget_publish(&s);
 int raw=connect_host("127.0.0.1",port);assert(raw>=0);
 Frame f={.magic=MAGIC,.version=1,.nonce=job_nonce,.rank=1,.size=2,.message_sequence=1,.sample={.sequence=1,.elapsed_seconds=.1}};
 raw_send(raw,&f);usleep(150000);
 f.message_sequence=2;f.request_credit=true;f.demand_generation=1;f.requested_ns=1000;raw_send(raw,&f);
 Frame before=raw_receive(raw,.000099999);
 /* Cost snapshot revision remains ONE; credit consumption must still settle. */
 f.message_sequence=3;f.consumed_ns=1000;raw_send(raw,&f);
 Frame after=raw_receive(raw,before.view.granted_total_seconds+.0000009);
 assert(after.view.global_spent_seconds>=1e-6);
 assert(after.view.granted_total_seconds<before.view.granted_total_seconds+.0000011);
 /* Identical wire replay cannot settle the same consumption twice. */
 raw_send(raw,&f);Frame duplicate=raw_receive(raw,after.view.granted_total_seconds);
 assert(duplicate.view.granted_total_seconds==after.view.granted_total_seconds);
 assert(duplicate.view.wave==0);
 /* Force an actual unfunded request, then preserve the SAME request after
  * DONE while attached remains stale-positive. It must not create wave two. */
 f.message_sequence=4;f.demand_generation=2;f.requested_ns=10000000;
 f.sample.profile_seconds=2;f.sample.attached_targets=16;raw_send(raw,&f);
 Frame wave=raw_wave(raw,1,0);assert(wave.view.wave==1);
 peak_global_budget_complete(1);f.message_sequence=5;f.sample.completed_wave=1;raw_send(raw,&f);
 wave=raw_wave(raw,1,1);assert(wave.view.wave==1);
 f.message_sequence=6;raw_send(raw,&f);usleep(30000);
 wave=raw_wave(raw,1,1);assert(wave.view.wave==1);
 /* A genuinely new demand generation can cause a second wave; old ACK one
  * cannot complete it. */
 f.message_sequence=7;f.demand_generation=3;raw_send(raw,&f);
 wave=raw_wave(raw,2,1);assert(wave.view.wave==2&&wave.view.root_completed_wave==1);
 peak_global_budget_complete(2);f.message_sequence=8;f.sample.completed_wave=2;raw_send(raw,&f);
 wave=raw_wave(raw,2,2);assert(wave.view.wave_span_seconds>0);
 assert(peak_global_budget_flush(100));
 /* An ACK for a never-issued future wave is rejected, not counted DONE. */
 f.message_sequence=9;f.sample.completed_wave=3;raw_send(raw,&f);
 usleep(150000);assert(!peak_global_budget_view().active);
 s.sequence=2;peak_global_budget_publish(&s);
 assert(!peak_global_budget_flush(100));
 close(raw);
 peak_global_budget_stop();puts("global_budget_same_snapshot_ok");return 0;
}
