#define _GNU_SOURCE
#include <unistd.h>
static ssize_t interleaved_write(int fd,const void *data,size_t count);
#define write interleaved_write
#include "../../src/global_budget.c"
#undef write
#include <assert.h>
static atomic_uint poll_entries;
static bool armed,observed_running,forced_reentry;
static ssize_t interleaved_write(int fd,const void *data,size_t count){
 bool is_stop=armed&&fd==wake_fd;unsigned before=atomic_load(&poll_entries);bool prior=atomic_load(&running);
 ssize_t n=write(fd,data,count);
 if(is_stop){observed_running=prior;if(prior){
  /* Force the scheduler ordering seen on a shared rank CPU: the service
   * consumes this wake and enters poll again before write returns to stop. */
  double deadline=seconds_now()+1;while(atomic_load(&poll_entries)<=before&&seconds_now()<deadline)usleep(100);
  forced_reentry=atomic_load(&poll_entries)>before;
 }}
 return n;
}
static void *parked_service(void *unused){(void)unused;while(atomic_load(&running)){
 atomic_fetch_add(&poll_entries,1);struct pollfd p={wake_fd,POLLIN,0};int n=poll(&p,1,100);
 if(n>0){uint64_t value;(void)read(wake_fd,&value,sizeof(value));}
 }return NULL;}
int main(void){
 alarm(4);atomic_store(&owner,getpid());wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);assert(wake_fd>=0);atomic_store(&running,true);
 assert(!pthread_create(&worker,NULL,parked_service,NULL));while(!atomic_load(&poll_entries))usleep(100);usleep(1000);
 unsigned before=atomic_load(&poll_entries);armed=true;double started=seconds_now();peak_global_budget_stop();double elapsed=seconds_now()-started;
 fprintf(stderr,"stop running_at_wake=%d forced_reentry=%d poll_before=%u poll_after=%u join_ms=%.3f\n",observed_running,forced_reentry,before,atomic_load(&poll_entries),elapsed*1000);
#ifdef PEAK_TEST_OLD_STOP
 assert(observed_running&&forced_reentry&&elapsed>.08);
#else
 assert(!observed_running&&!forced_reentry&&atomic_load(&poll_entries)==before);
#endif
 puts("global_budget_stop_wake_ok");
}
