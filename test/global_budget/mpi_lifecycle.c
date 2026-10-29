#define _GNU_SOURCE
#include <mpi.h>
#include <omp.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include "internal/global_budget.h"
#define A UINT64_C(6364136223846793005)
#define B UINT64_C(1442695040888963407)
#define TARGET(name,id) __attribute__((noinline,noclone,noipa,used,visibility("default"),patchable_function_entry(16,0))) uint64_t pressure_##name(uint64_t x,unsigned work) { for(unsigned j=0;j<work;j++) x=A*x+B+(id); return x; }
#include "../../benchmarks/mpi_rank_budget/targets.inc"
#undef TARGET
static int (*coordinator_active)(void);
static uint32_t (*physical_targets)(void);
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
typedef struct { uint64_t a,b; } Aff;
static Aff compose(Aff f,Aff g) { return (Aff){f.a*g.a,f.a*g.b+f.b}; }
static uint64_t expected(uint64_t x,unsigned id,uint64_t n) { Aff f={A,B+id},r={1,0};while(n){if(n&1)r=compose(f,r);f=compose(f,f);n>>=1;}return r.a*x+r.b; }
/* Every interval performs real, checked arithmetic. There are no timing sleeps. */
static int work(double seconds,unsigned calls,int hot,
                PeakGlobalBudgetView (*view)(void),int rank,int monitor)
{
    double deadline=now()+seconds;int bad=0,inactive=0;uint64_t iterations=0;
#pragma omp parallel reduction(+:bad,iterations) reduction(|:inactive)
    {
        int tid=omp_get_thread_num();uint64_t x=1234u+(unsigned)rank*101u+(unsigned)tid;
        do {
            uint64_t before=x;unsigned id=hot&&tid==0?0:128;
            unsigned inner=hot?1:16384;
            for(unsigned i=0;i<calls;i++) x=id==0?pressure_000(x,inner):pressure_128(x,inner);
            if(x!=expected(before,id%128,(uint64_t)calls*inner)) bad++;
            iterations++;
            if(monitor&&tid==0&&!coordinator_active()) inactive=1;
        } while(now()<deadline);
    }
    PeakGlobalBudgetView v=view();
    printf("lifecycle rank=%d hot=%d seconds=%.9g iterations=%llu active=%d coherent=%d wave=%llu root_done=%llu credit=%llu physical=%u inactive=%d oracle_bad=%d\n",rank,hot,seconds,(unsigned long long)iterations,coordinator_active(),(int)v.active,(unsigned long long)v.wave,(unsigned long long)v.root_completed_wave,(unsigned long long)v.local_credit_ns,physical_targets(),inactive,bad);fflush(stdout);
    return bad || inactive;
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"clock-guard")) {
        int (*guard)(void)=dlsym(RTLD_DEFAULT,"mpi_interceptor_test_bootstrap_clock_guard");
        int (*grace)(void)=dlsym(RTLD_DEFAULT,"peak_general_listener_test_global_grace_faa");
        int (*admission)(void)=dlsym(RTLD_DEFAULT,"peak_general_listener_test_global_admission_ns");
        int (*idle)(void)=dlsym(RTLD_DEFAULT,"peak_general_listener_test_global_idle_guard");
        return guard&&grace&&admission&&idle ? (guard() || grace() || admission() || idle()) : 2;
    }
    unsigned char original_target[16];
    memcpy(original_target, (const void*)&pressure_000, sizeof(original_target));
    const char* activation = getenv("PEAK_MPI_ACTIVATION_POLICY");
    if (activation == NULL || !strcmp(activation, "prepare") || !strcmp(activation, "post-init")) {
        uint32_t (*physical)(void) = dlsym(RTLD_DEFAULT,"peak_general_listener_test_physical_targets");
        uint64_t (*calls)(void) = dlsym(RTLD_DEFAULT,"peak_general_listener_test_all_target_calls");
        int (*active)(void) = dlsym(RTLD_DEFAULT,"peak_runtime_is_active_for_checkpoint");
        double (*prepared)(void) = dlsym(RTLD_DEFAULT,"peak_mpi_runtime_preparation_seconds");
        int expect_deferred = activation && !strcmp(activation, "post-init");
        if (!physical || !calls || !active || !prepared || physical() != 0 || calls() != 0 ||
            (!!active() == expect_deferred) || (!expect_deferred && prepared() <= 0.0)) return 91;
        uint64_t x = 123;
        double until = now() + .05;
        do { x = pressure_000(x, 1); } while (now() < until);
        if (physical() != 0 || calls() != 0 ||
            memcmp(original_target, (const void*)&pressure_000, sizeof(original_target))) return 92;
        fprintf(stderr,"pre_mpi_native=1 pre_mpi_target_calls=0 oracle=%llu\n",(unsigned long long)x);
    }
    int provided,rank,size;MPI_Init_thread(&argc,&argv,MPI_THREAD_FUNNELED,&provided);
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);MPI_Comm_size(MPI_COMM_WORLD,&size);
    PeakGlobalBudgetView (*view)(void)=dlsym(RTLD_DEFAULT,"peak_general_listener_test_global_view");
    coordinator_active=dlsym(RTLD_DEFAULT,"peak_general_listener_test_global_active");
    physical_targets=dlsym(RTLD_DEFAULT,"peak_general_listener_test_physical_targets");
    if(!view||!coordinator_active||!physical_targets||provided<MPI_THREAD_FUNNELED||argc!=2) MPI_Abort(MPI_COMM_WORLD,2);
    cpu_set_t masks[2];
    if(omp_get_max_threads()!=2) MPI_Abort(MPI_COMM_WORLD,2);
#pragma omp parallel
    { int tid=omp_get_thread_num();CPU_ZERO(&masks[tid]);sched_getaffinity(0,sizeof(cpu_set_t),&masks[tid]); }
    printf("affinity rank=%d threads=2",rank);
    for(int t=0;t<2;t++){printf(" t%d=[",t);int first=1;for(int cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,&masks[t])){printf("%s%d",first?"":",",cpu);first=0;}printf("]");}printf("\n");fflush(stdout);
    int bad=0;
    if(!strcmp(argv[1],"unfunded")) {
        bad |= memcmp(original_target, (const void*)&pressure_000, sizeof(original_target)) != 0;
        bad |= physical_targets() != 0;
        bad |= work(.1,2000,1,view,rank,1);
        bad |= memcmp(original_target, (const void*)&pressure_000, sizeof(original_target)) != 0;
        printf("lifecycle rank=%d initially_unpatched=%d\n",rank,!bad);fflush(stdout);
        int sum;MPI_Allreduce(&bad,&sum,1,MPI_INT,MPI_MAX,MPI_COMM_WORLD);bad=sum;
    } else if(!strcmp(argv[1],"fallback")) {
        void (*disconnect)(void)=dlsym(RTLD_DEFAULT,"peak_general_listener_test_global_disconnect");
        if(!disconnect) MPI_Abort(MPI_COMM_WORLD,2);
        if(rank==0) disconnect();
        MPI_Barrier(MPI_COMM_WORLD);
        bad|=work(2.0,8,0,view,rank,0);
        int patched=memcmp(original_target,(const void*)&pressure_000,sizeof(original_target))!=0;
        printf("lifecycle rank=%d local_initial_patched=%d\n",rank,patched);fflush(stdout);
        bad|=!patched;
        bad|=work(.25,2000,1,view,rank,0);
        int sum;MPI_Allreduce(&bad,&sum,1,MPI_INT,MPI_MAX,MPI_COMM_WORLD);bad=sum;
    } else if(!strcmp(argv[1],"skew")) {
        MPI_Comm peers;MPI_Comm_split(MPI_COMM_WORLD,rank==0?MPI_UNDEFINED:1,rank,&peers);
        MPI_Barrier(MPI_COMM_WORLD);
        if(rank==0) { printf("lifecycle root_enter_finalize=1\n");fflush(stdout);MPI_Finalize();return 0; }
        double end=now()+4.0;
        while(now()<end) {
            bad|=work(.15,1000,1,view,rank,1);
            int sum;MPI_Allreduce(&bad,&sum,1,MPI_INT,MPI_MAX,peers);bad=sum;
            bad|=work(.15,8,0,view,rank,1);
        }
        MPI_Comm_free(&peers);
    } else if(!strcmp(argv[1],"recovery") || !strcmp(argv[1],"initial-once") || !strcmp(argv[1],"multiwindow")) {
        bad|=work(2.0,8,0,view,rank,1);
        MPI_Barrier(MPI_COMM_WORLD);
        bad|=work(.25,2000,1,view,rank,1);
        MPI_Barrier(MPI_COMM_WORLD);
        bad|=work(5.0,8,0,view,rank,1);
        MPI_Barrier(MPI_COMM_WORLD);
        bad|=work(.25,2000,1,view,rank,1);
        int sum;MPI_Allreduce(&bad,&sum,1,MPI_INT,MPI_MAX,MPI_COMM_WORLD);bad=sum;
    } else MPI_Abort(MPI_COMM_WORLD,2);
    printf("lifecycle rank=%d oracle=%s\n",rank,bad?"FAIL":"PASS");fflush(stdout);
    MPI_Finalize();return bad?1:0;
}
