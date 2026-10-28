#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include <assert.h>
static void raw_send(int fd,const Frame *f){size_t sent=0;double end=seconds_now()+1;while(sent<sizeof(*f)&&seconds_now()<end){ssize_t n=send(fd,(const char*)f+sent,sizeof(*f)-sent,MSG_NOSIGNAL);if(n>0)sent+=(size_t)n;else usleep(100);}assert(sent==sizeof(*f));}
int main(void){
 alarm(6);Rank ranks[32]={0};assert(grace_reserve_ns(ranks,32)==16000000);/* Unknown ranks are conservative legacy. */
 for(unsigned i=0;i<32;i++){ranks[i].seen=true;ranks[i].latest=(PeakGlobalBudgetSample){.sequence=1,.initial_unexposed=true};ranks[i].wants_credit=true;ranks[i].requested_ns=1000000;}
 assert(grace_reserve_ns(ranks,32)==0);
 Rank short_rank=ranks[0];CreditGrant denied=grant_rank_credit(&short_rank,4499999);assert(!denied.credit&&!denied.closing&&!denied.grace&&!short_rank.registered&&!short_rank.grace_exposed);
 Rank active_first=ranks[1];active_first.latest.attached_targets=16;CreditGrant fragment=grant_rank_credit(&active_first,4000000);assert(!fragment.credit&&!active_first.registered&&!active_first.grace_exposed);
 Rank active_legacy=active_first;active_legacy.grace_exposed=true;CreditGrant partial=grant_rank_credit(&active_legacy,3500000);assert(partial.credit==500000&&partial.closing==3000000&&!partial.grace);
 CreditGrant exact=grant_rank_credit(&ranks[0],4500000);assert(exact.credit==1000000&&exact.closing==3000000&&exact.grace==500000);assert(ranks[0].grace_exposed&&grace_reserve_ns(ranks,32)==500000);
 PeakGlobalBudgetSample stale=ranks[0].latest;stale.sequence=2;assert(valid_sample(&stale,&ranks[0]));/* Grant may still be in flight: cached true is legal. */
 ranks[0].returned_ns=ranks[0].credit_total_ns;ranks[0].reservation=0;ranks[0].registered=false;assert(grace_reserve_ns(ranks,32)==500000);
 stale.initial_unexposed=false;assert(valid_sample(&stale,&ranks[0]));ranks[0].latest=stale;stale.initial_unexposed=true;assert(!valid_sample(&stale,&ranks[0]));
 for(unsigned i=0;i<32;i++){ranks[i]=(Rank){.seen=true,.wants_credit=true,.requested_ns=6000000,.latest={.sequence=1,.initial_unexposed=true}};}
 uint64_t pool=16532000;unsigned funded=0;for(unsigned i=0;i<32;i++){CreditGrant g=grant_rank_credit(&ranks[i],pool);pool-=g.credit+g.closing+g.grace;if(g.credit)funded++;}
 assert(funded==1&&pool==7032000&&grace_reserve_ns(ranks,32)==500000);
 for(unsigned i=0;i<32;i++)ranks[i]=(Rank){.seen=true,.grace_exposed=true,.wants_credit=true,.requested_ns=6000000,.latest={.sequence=1}};
 assert(grace_reserve_ns(ranks,32)==16000000);CreditGrant legacy=grant_rank_credit(&ranks[0],532000);assert(!legacy.credit&&!legacy.closing&&!legacy.grace);
 /* A real false -> true wire rollback degrades instead of releasing grace. */
 unsigned short port;assert(!peak_global_budget_listen(&port));assert(!peak_global_budget_start("127.0.0.1",port,0,2,.5,NULL));
 PeakGlobalBudgetSample own={.sequence=1,.initial_unexposed=true};peak_global_budget_publish(&own);int raw=connect_host("127.0.0.1",port);assert(raw>=0);
 Frame f={.magic=MAGIC,.version=PROTOCOL_VERSION,.nonce=job_nonce,.rank=1,.size=2,.message_sequence=1,.sample={.sequence=1}};raw_send(raw,&f);
 double end=seconds_now()+1;while(!peak_global_budget_active()&&seconds_now()<end)usleep(1000);assert(peak_global_budget_active());assert(fabs(peak_global_budget_view().control_reserve_seconds-.0005)<1e-12);
 f.message_sequence=2;f.sample.initial_unexposed=true;raw_send(raw,&f);end=seconds_now()+.5;while(peak_global_budget_active()&&seconds_now()<end)usleep(1000);assert(!peak_global_budget_active());close(raw);peak_global_budget_stop();puts("global_budget_initial_exposure_ok");
}
