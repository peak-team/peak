#define _GNU_SOURCE
/* Application MPI causal metadata. Native operations and return values are
 * preserved; unsupported/ambiguous semantics explicitly seal a SUM epoch. */
#include "internal/native_thread_pointer.h"
#include "internal/mpi_budget_events.h"
#include <mpi.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dlfcn.h>
#include <stdatomic.h>

/* Configure against the actual installed prototypes, including pre-errata
 * MPI-4 headers. Standalone frontend probes default to corrected qualifiers;
 * optional APIs require an explicit header/link availability result. */
#ifndef PEAK_MPI_PSEND_INIT_CONST_BUFFER
#define PEAK_MPI_PSEND_INIT_CONST_BUFFER 1
#endif
#ifndef PEAK_MPI_PREADY_LIST_CONST_PARTITIONS
#define PEAK_MPI_PREADY_LIST_CONST_PARTITIONS 1
#endif
#ifndef PEAK_MPI_HAVE_STATUS_SET_ELEMENTS_C
#define PEAK_MPI_HAVE_STATUS_SET_ELEMENTS_C 0
#endif
#if PEAK_MPI_PSEND_INIT_CONST_BUFFER
#define PEAK_MPI_PSEND_BUFFER_TYPE const void
#else
#define PEAK_MPI_PSEND_BUFFER_TYPE void
#endif
#if PEAK_MPI_PREADY_LIST_CONST_PARTITIONS
#define PEAK_MPI_PREADY_PARTITION_TYPE const int
#else
#define PEAK_MPI_PREADY_PARTITION_TYPE int
#endif

#if defined(__GNUC__)
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif

typedef struct MessageCounter {
    int peer, tag;
    PeakMpiEventOp op;
    uint64_t next;
    struct MessageCounter *link;
} MessageCounter;
typedef struct GroupCreation {
    int tag, size, *members;
    uint64_t ordinal;
    struct GroupCreation *next;
} GroupCreation;
typedef struct {
    uint32_t parent_owner;
    uint64_t parent_id, ordinal;
    unsigned kind;
    int tag, valid;
} CreationIdentity;
typedef struct Context {
    MPI_Comm handle;
    uint64_t id, round, creation_ordinal;
    uint32_t owner;
    int size, *members, retired;
    MessageCounter *messages;
    GroupCreation *groups;
    struct Context *next;
} Context;
typedef struct Request {
    MPI_Request handle;
    PeakMpiBudgetEvent post;
    struct Request *next;
} Request;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Context *contexts;
static Request *requests, *recycled_requests;
static size_t retired_contexts;
static uint64_t sequence, last_mpi, next_request;
static int world_rank;
static unsigned active_calls;
static uint64_t runtime_epoch, runtime_raw[PEAK_RUNTIME_COMPONENTS];
static uint64_t runtime_outside[PEAK_RUNTIME_COMPONENTS], runtime_sum[PEAK_RUNTIME_COMPONENTS];
static unsigned runtime_active[PEAK_RUNTIME_COMPONENTS];
static bool runtime_closed;
static void runtime_snapshot_locked(PeakMpiBudgetCostSnapshot *snapshot);
static int stream_closed;
static _Atomic int frontend_ready;
static int uncertifiable_state;
static int needs_strong_fence;
static _Thread_local int application_active __attribute__((tls_model("initial-exec")));
int peak_mpi_application_active(void){return application_active;}

static _Thread_local uint64_t wrapper_started, native_elapsed_ns;
static _Thread_local int clock_failed;
/* Non-native monotonic elapsed time is an upper bound on added thread CPU.
 * It deliberately includes lock waits/descheduling; native MPI waits are
 * excluded. Unlike THREAD_CPUTIME_ID this clock commonly uses the vDSO. */
static uint64_t metadata_now(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)!=0 || t.tv_sec<0 ||
       (uint64_t)t.tv_sec>UINT64_MAX/1000000000U){clock_failed=1;return 0;}
    return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec;
}
/* Exclude only the real application MPI implementation. Lazy symbol lookup
 * and every wrapper instruction remain part of charged frontend cost. */
static _Thread_local int native_exclusion;
static void native_start(int active){native_exclusion=active;}
static void native_stop(int active){(void)active;native_exclusion=0;}
static void native_elapsed(uint64_t started)
{
    uint64_t now=metadata_now();if(now<started)clock_failed=1;
    else if(UINT64_MAX-native_elapsed_ns<now-started)clock_failed=1;
    else native_elapsed_ns+=now-started;
}

#define PEAK_NATIVE(N, DECL, ARGS) \
    static __typeof__(PMPI_##N) *native_##N; \
    static pthread_once_t resolve_##N##_once=PTHREAD_ONCE_INIT; \
    static void resolve_##N(void){native_##N=(__typeof__(PMPI_##N) *)dlsym(RTLD_NEXT,"PMPI_" #N);} \
    static int call_##N DECL { \
        pthread_once(&resolve_##N##_once,resolve_##N); \
        if(!native_##N)return MPI_ERR_OTHER; \
        int exclude=native_exclusion;native_exclusion=0; \
        uint64_t started=exclude?metadata_now():0; \
        int result=native_##N ARGS; \
        if(exclude)native_elapsed(started); \
        native_exclusion=exclude;return result; \
    }
#define PEAK_MPI_APIS_3(X) \
    X(Buffer_detach,(void *buffer_addr, int *size),(buffer_addr,size)) \
    X(Barrier,(MPI_Comm comm),(comm)) \
    X(Ibarrier,(MPI_Comm comm, MPI_Request *request),(comm,request)) \
    X(Bcast,(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm),(buffer,count,datatype,root,comm)) \
    X(Ibcast,(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Request *request),(buffer,count,datatype,root,comm,request)) \
    X(Allreduce,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,comm)) \
    X(Iallreduce,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,request)) \
    X(Reduce,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,root,comm)) \
    X(Ireduce,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,root,comm,request)) \
    X(Allgather,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Iallgather,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Allgatherv,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm)) \
    X(Iallgatherv,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request)) \
    X(Alltoall,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Ialltoall,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Alltoallv,(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm)) \
    X(Ialltoallv,(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request)) \
    X(Alltoallw,(const void *sendbuf, const int sendcounts[], const int sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const int rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm)) \
    X(Ialltoallw,(const void *sendbuf, const int sendcounts[], const int sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const int rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request)) \
    X(Gather,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm)) \
    X(Igather,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request)) \
    X(Gatherv,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm)) \
    X(Igatherv,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,request)) \
    X(Scatter,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm)) \
    X(Iscatter,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request)) \
    X(Scatterv,(const void *sendbuf, const int sendcounts[], const int displs[], MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm)) \
    X(Iscatterv,(const void *sendbuf, const int sendcounts[], const int displs[], MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,request)) \
    X(Reduce_scatter,(const void *sendbuf, void *recvbuf, const int recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,recvcounts,datatype,op,comm)) \
    X(Ireduce_scatter,(const void *sendbuf, void *recvbuf, const int recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,recvcounts,datatype,op,comm,request)) \
    X(Reduce_scatter_block,(const void *sendbuf, void *recvbuf, int recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,recvcount,datatype,op,comm)) \
    X(Ireduce_scatter_block,(const void *sendbuf, void *recvbuf, int recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,recvcount,datatype,op,comm,request)) \
    X(Scan,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,comm)) \
    X(Iscan,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,request)) \
    X(Exscan,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,comm)) \
    X(Iexscan,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,request)) \
    X(Isend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,peer,tag,comm,request)) \
    X(Issend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,peer,tag,comm,request)) \
    X(Ibsend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,peer,tag,comm,request)) \
    X(Irsend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,peer,tag,comm,request)) \
    X(Irecv,(void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,peer,tag,comm,request)) \
    X(Send,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm),(buf,count,datatype,peer,tag,comm)) \
    X(Ssend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm),(buf,count,datatype,peer,tag,comm)) \
    X(Bsend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm),(buf,count,datatype,peer,tag,comm)) \
    X(Rsend,(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm),(buf,count,datatype,peer,tag,comm)) \
    X(Recv,(void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Status *status),(buf,count,datatype,peer,tag,comm,status)) \
    X(Wait,(MPI_Request *request,MPI_Status *status),(request,status)) \
    X(Test,(MPI_Request *request,int *flag,MPI_Status *status),(request,flag,status)) \
    X(Waitall,(int count, MPI_Request array[], MPI_Status *statuses),(count,array,statuses)) \
    X(Waitany,(int count, MPI_Request array[], int *index, MPI_Status *status),(count,array,index,status)) \
    X(Waitsome,(int count, MPI_Request array[], int *outcount, int indices[], MPI_Status *statuses),(count,array,outcount,indices,statuses)) \
    X(Testall,(int count, MPI_Request array[], int *flag, MPI_Status *statuses),(count,array,flag,statuses)) \
    X(Testany,(int count, MPI_Request array[], int *index, int *flag, MPI_Status *status),(count,array,index,flag,status)) \
    X(Testsome,(int count, MPI_Request array[], int *outcount, int indices[], MPI_Status *statuses),(count,array,outcount,indices,statuses)) \
    X(Comm_dup,(MPI_Comm comm,MPI_Comm *newcomm),(comm,newcomm)) \
    X(Comm_split,(MPI_Comm comm,int color,int key,MPI_Comm *newcomm),(comm,color,key,newcomm)) \
    X(Comm_create,(MPI_Comm comm,MPI_Group group,MPI_Comm *newcomm),(comm,group,newcomm)) \
    X(Comm_create_group,(MPI_Comm comm,MPI_Group group,int tag,MPI_Comm *newcomm),(comm,group,tag,newcomm)) \
    X(Comm_split_type,(MPI_Comm comm,int type,int key,MPI_Info info,MPI_Comm *newcomm),(comm,type,key,info,newcomm)) \
    X(Comm_free,(MPI_Comm *comm),(comm)) \
    X(Request_free,(MPI_Request *request),(request)) \
    X(Cancel,(MPI_Request *request),(request)) \
    X(Start,(MPI_Request *request),(request)) \
    X(Startall,(int count,MPI_Request requests_[]),(count,requests_)) \
    X(Sendrecv,(const void *sendbuf,int sendcount,MPI_Datatype sendtype,int dest,int sendtag,void *recvbuf,int recvcount,MPI_Datatype recvtype,int source,int recvtag,MPI_Comm comm,MPI_Status *status),(sendbuf,sendcount,sendtype,dest,sendtag,recvbuf,recvcount,recvtype,source,recvtag,comm,status)) \
    X(Sendrecv_replace,(void *buf,int count,MPI_Datatype datatype,int dest,int sendtag,int source,int recvtag,MPI_Comm comm,MPI_Status *status),(buf,count,datatype,dest,sendtag,source,recvtag,comm,status)) \
    X(Win_create,(void *base, MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, MPI_Win *win),(base,size,disp_unit,info,comm,win)) \
    X(Win_allocate,(MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win),(size,disp_unit,info,comm,baseptr,win)) \
    X(Win_allocate_shared,(MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win),(size,disp_unit,info,comm,baseptr,win)) \
    X(Win_create_dynamic,(MPI_Info info, MPI_Comm comm, MPI_Win *win),(info,comm,win)) \
    X(Win_free,(MPI_Win *win),(win)) \
    X(Win_fence,(int assert, MPI_Win win),(assert,win)) \
    X(Win_lock,(int lock_type, int rank, int assert, MPI_Win win),(lock_type,rank,assert,win)) \
    X(Win_lock_all,(int assert, MPI_Win win),(assert,win)) \
    X(Win_unlock,(int rank, MPI_Win win),(rank,win)) \
    X(Win_unlock_all,(MPI_Win win),(win)) \
    X(Win_flush,(int rank, MPI_Win win),(rank,win)) \
    X(Win_flush_all,(MPI_Win win),(win)) \
    X(Win_flush_local,(int rank, MPI_Win win),(rank,win)) \
    X(Win_flush_local_all,(MPI_Win win),(win)) \
    X(Win_sync,(MPI_Win win),(win)) \
    X(Win_start,(MPI_Group group, int assert, MPI_Win win),(group,assert,win)) \
    X(Win_post,(MPI_Group group, int assert, MPI_Win win),(group,assert,win)) \
    X(Win_complete,(MPI_Win win),(win)) \
    X(Win_wait,(MPI_Win win),(win)) \
    X(Win_test,(MPI_Win win, int *flag),(win,flag)) \
    X(Win_attach,(MPI_Win win, void *base, MPI_Aint size),(win,base,size)) \
    X(Win_detach,(MPI_Win win, const void *base),(win,base)) \
    X(Put,(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win)) \
    X(Get,(void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win)) \
    X(Accumulate,(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win)) \
    X(Get_accumulate,(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, void *result_addr, int result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win),(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win)) \
    X(Fetch_and_op,(const void *origin_addr, void *result_addr, MPI_Datatype datatype, int target_rank, MPI_Aint target_disp, MPI_Op op, MPI_Win win),(origin_addr,result_addr,datatype,target_rank,target_disp,op,win)) \
    X(Compare_and_swap,(const void *origin_addr, const void *compare_addr, void *result_addr, MPI_Datatype datatype, int target_rank, MPI_Aint target_disp, MPI_Win win),(origin_addr,compare_addr,result_addr,datatype,target_rank,target_disp,win)) \
    X(Rput,(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request)) \
    X(Rget,(void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request)) \
    X(Raccumulate,(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request)) \
    X(Rget_accumulate,(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, void *result_addr, int result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request)) \
    X(Send_init,(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Ssend_init,(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Rsend_init,(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Bsend_init,(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Recv_init,(void *buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,source,tag,comm,request)) \
    X(Comm_idup,(MPI_Comm comm, MPI_Comm *newcomm, MPI_Request *request),(comm,newcomm,request)) \
    X(Intercomm_create,(MPI_Comm local_comm, int local_leader, MPI_Comm peer_comm, int remote_leader, int tag, MPI_Comm *newintercomm),(local_comm,local_leader,peer_comm,remote_leader,tag,newintercomm)) \
    X(Intercomm_merge,(MPI_Comm intercomm, int high, MPI_Comm *newintracomm),(intercomm,high,newintracomm)) \
    X(Comm_spawn,(const char *command, char *argv[], int maxprocs, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *intercomm, int array_of_errcodes[]),(command,argv,maxprocs,info,root,comm,intercomm,array_of_errcodes)) \
    X(Comm_spawn_multiple,(int count, char *array_of_commands[], char **array_of_argv[], const int array_of_maxprocs[], const MPI_Info array_of_info[], int root, MPI_Comm comm, MPI_Comm *intercomm, int array_of_errcodes[]),(count,array_of_commands,array_of_argv,array_of_maxprocs,array_of_info,root,comm,intercomm,array_of_errcodes)) \
    X(Comm_connect,(const char *port_name, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *newcomm),(port_name,info,root,comm,newcomm)) \
    X(Comm_accept,(const char *port_name, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *newcomm),(port_name,info,root,comm,newcomm)) \
    X(Comm_join,(int fd, MPI_Comm *intercomm),(fd,intercomm)) \
    X(Mprobe,(int source, int tag, MPI_Comm comm, MPI_Message *message, MPI_Status *status),(source,tag,comm,message,status)) \
    X(Improbe,(int source, int tag, MPI_Comm comm, int *flag, MPI_Message *message, MPI_Status *status),(source,tag,comm,flag,message,status)) \
    X(Mrecv,(void *buf, int count, MPI_Datatype datatype, MPI_Message *message, MPI_Status *status),(buf,count,datatype,message,status)) \
    X(Imrecv,(void *buf, int count, MPI_Datatype datatype, MPI_Message *message, MPI_Request *request),(buf,count,datatype,message,request)) \
    X(Neighbor_allgather,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Neighbor_allgatherv,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm)) \
    X(Neighbor_alltoall,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Neighbor_alltoallv,(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm)) \
    X(Neighbor_alltoallw,(const void *sendbuf, const int sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm)) \
    X(Ineighbor_allgather,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Ineighbor_allgatherv,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request)) \
    X(Ineighbor_alltoall,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Ineighbor_alltoallv,(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request)) \
    X(Ineighbor_alltoallw,(const void *sendbuf, const int sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request)) \
    X(Cart_create,(MPI_Comm comm_old, int ndims, const int dims[], const int periods[], int reorder, MPI_Comm *comm_cart),(comm_old,ndims,dims,periods,reorder,comm_cart)) \
    X(Cart_sub,(MPI_Comm comm, const int remain_dims[], MPI_Comm *newcomm),(comm,remain_dims,newcomm)) \
    X(Graph_create,(MPI_Comm comm_old, int nnodes, const int indx[], const int edges[], int reorder, MPI_Comm *comm_graph),(comm_old,nnodes,indx,edges,reorder,comm_graph)) \
    X(Dist_graph_create,(MPI_Comm comm_old, int n, const int sources[], const int degrees[], const int destinations[], const int weights[], MPI_Info info, int reorder, MPI_Comm *comm_dist_graph),(comm_old,n,sources,degrees,destinations,weights,info,reorder,comm_dist_graph)) \
    X(Dist_graph_create_adjacent,(MPI_Comm comm_old, int indegree, const int sources[], const int sourceweights[], int outdegree, const int destinations[], const int destweights[], MPI_Info info, int reorder, MPI_Comm *comm_dist_graph),(comm_old,indegree,sources,sourceweights,outdegree,destinations,destweights,info,reorder,comm_dist_graph)) \
    X(Comm_dup_with_info,(MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm),(comm,info,newcomm)) \
    X(Request_get_status,(MPI_Request request, int *flag, MPI_Status *status),(request,flag,status)) \
    X(Probe,(int source, int tag, MPI_Comm comm, MPI_Status *status),(source,tag,comm,status)) \
    X(Iprobe,(int source, int tag, MPI_Comm comm, int *flag, MPI_Status *status),(source,tag,comm,flag,status)) \
    X(Win_shared_query,(MPI_Win win, int rank, MPI_Aint *size, int *disp_unit, void *baseptr),(win,rank,size,disp_unit,baseptr)) \
    X(Comm_disconnect,(MPI_Comm *comm),(comm))

#define PEAK_MPI_APIS_4(X) \
    X(Allgather_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Allgatherv_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request)) \
    X(Allreduce_init,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,info,request)) \
    X(Alltoall_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Alltoallv_init,(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request)) \
    X(Alltoallw_init,(const void *sendbuf, const int sendcounts[], const int sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const int rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request)) \
    X(Barrier_init,(MPI_Comm comm, MPI_Info info, MPI_Request *request),(comm,info,request)) \
    X(Bcast_init,(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(buffer,count,datatype,root,comm,info,request)) \
    X(Exscan_init,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,info,request)) \
    X(Gather_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request)) \
    X(Gatherv_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,info,request)) \
    X(Neighbor_allgather_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Neighbor_allgatherv_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request)) \
    X(Neighbor_alltoall_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Neighbor_alltoallv_init,(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request)) \
    X(Neighbor_alltoallw_init,(const void *sendbuf, const int sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request)) \
    X(Reduce_init,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,root,comm,info,request)) \
    X(Reduce_scatter_block_init,(const void *sendbuf, void *recvbuf, int recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,recvcount,datatype,op,comm,info,request)) \
    X(Reduce_scatter_init,(const void *sendbuf, void *recvbuf, const int recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,recvcounts,datatype,op,comm,info,request)) \
    X(Scan_init,(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,info,request)) \
    X(Scatter_init,(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request)) \
    X(Scatterv_init,(const void *sendbuf, const int sendcounts[], const int displs[], MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request)) \
    X(Session_init,(MPI_Info info, MPI_Errhandler errhandler, MPI_Session *session),(info,errhandler,session)) \
    X(Precv_init,(void *buf, int partitions, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info, MPI_Request *request),(buf,partitions,count,datatype,dest,tag,comm,info,request)) \
    X(Psend_init,(PEAK_MPI_PSEND_BUFFER_TYPE *buf, int partitions, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info, MPI_Request *request),(buf,partitions,count,datatype,dest,tag,comm,info,request)) \
    X(Allgather_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Allgather_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Allgatherv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm)) \
    X(Allgatherv_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request)) \
    X(Allreduce_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,comm)) \
    X(Allreduce_init_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,info,request)) \
    X(Alltoall_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Alltoall_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Alltoallv_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm)) \
    X(Alltoallv_init_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request)) \
    X(Alltoallw_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm)) \
    X(Alltoallw_init_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request)) \
    X(Bcast_c,(void *buffer, MPI_Count count, MPI_Datatype datatype, int root, MPI_Comm comm),(buffer,count,datatype,root,comm)) \
    X(Bcast_init_c,(void *buffer, MPI_Count count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(buffer,count,datatype,root,comm,info,request)) \
    X(Exscan_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,comm)) \
    X(Exscan_init_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,info,request)) \
    X(Gather_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm)) \
    X(Gather_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request)) \
    X(Gatherv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm)) \
    X(Gatherv_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,info,request)) \
    X(Iallgather_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Iallgatherv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request)) \
    X(Iallreduce_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,request)) \
    X(Ialltoall_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Ialltoallv_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request)) \
    X(Ialltoallw_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request)) \
    X(Ibcast_c,(void *buffer, MPI_Count count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Request *request),(buffer,count,datatype,root,comm,request)) \
    X(Iexscan_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,request)) \
    X(Igather_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request)) \
    X(Igatherv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,request)) \
    X(Ineighbor_allgather_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Ineighbor_allgatherv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request)) \
    X(Ineighbor_alltoall_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request)) \
    X(Ineighbor_alltoallv_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request)) \
    X(Ineighbor_alltoallw_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request)) \
    X(Ireduce_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,root,comm,request)) \
    X(Ireduce_scatter_c,(const void *sendbuf, void *recvbuf, const MPI_Count recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,recvcounts,datatype,op,comm,request)) \
    X(Ireduce_scatter_block_c,(const void *sendbuf, void *recvbuf, MPI_Count recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,recvcount,datatype,op,comm,request)) \
    X(Iscan_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,request)) \
    X(Iscatter_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request)) \
    X(Iscatterv_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint displs[], MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,request)) \
    X(Neighbor_allgather_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Neighbor_allgather_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Neighbor_allgatherv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm)) \
    X(Neighbor_allgatherv_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request)) \
    X(Neighbor_alltoall_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm)) \
    X(Neighbor_alltoall_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request)) \
    X(Neighbor_alltoallv_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm)) \
    X(Neighbor_alltoallv_init_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request)) \
    X(Neighbor_alltoallw_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm)) \
    X(Neighbor_alltoallw_init_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request)) \
    X(Reduce_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,root,comm)) \
    X(Reduce_init_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,root,comm,info,request)) \
    X(Reduce_local_c,(const void *inbuf, void *inoutbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op),(inbuf,inoutbuf,count,datatype,op)) \
    X(Reduce_scatter_c,(const void *sendbuf, void *recvbuf, const MPI_Count recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,recvcounts,datatype,op,comm)) \
    X(Reduce_scatter_block_c,(const void *sendbuf, void *recvbuf, MPI_Count recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,recvcount,datatype,op,comm)) \
    X(Reduce_scatter_block_init_c,(const void *sendbuf, void *recvbuf, MPI_Count recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,recvcount,datatype,op,comm,info,request)) \
    X(Reduce_scatter_init_c,(const void *sendbuf, void *recvbuf, const MPI_Count recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,recvcounts,datatype,op,comm,info,request)) \
    X(Scan_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm),(sendbuf,recvbuf,count,datatype,op,comm)) \
    X(Scan_init_c,(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,recvbuf,count,datatype,op,comm,info,request)) \
    X(Scatter_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm)) \
    X(Scatter_init_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request)) \
    X(Scatterv_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint displs[], MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm),(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm)) \
    X(Scatterv_init_c,(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint displs[], MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request),(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request)) \
    X(Get_count_c,(const MPI_Status *status, MPI_Datatype datatype, MPI_Count *count),(status,datatype,count)) \
    X(Get_elements_c,(const MPI_Status *status, MPI_Datatype datatype, MPI_Count *count),(status,datatype,count)) \
    X(Pack_c,(const void *inbuf, MPI_Count incount, MPI_Datatype datatype, void *outbuf, MPI_Count outsize, MPI_Count *position, MPI_Comm comm),(inbuf,incount,datatype,outbuf,outsize,position,comm)) \
    X(Pack_external_c,(const char *datarep, const void *inbuf, MPI_Count incount, MPI_Datatype datatype, void *outbuf, MPI_Count outsize, MPI_Count *position),(datarep,inbuf,incount,datatype,outbuf,outsize,position)) \
    X(Pack_external_size_c,(const char *datarep, MPI_Count incount, MPI_Datatype datatype, MPI_Count *size),(datarep,incount,datatype,size)) \
    X(Pack_size_c,(MPI_Count incount, MPI_Datatype datatype, MPI_Comm comm, MPI_Count *size),(incount,datatype,comm,size)) \
    X(Type_contiguous_c,(MPI_Count count, MPI_Datatype oldtype, MPI_Datatype *newtype),(count,oldtype,newtype)) \
    X(Type_create_darray_c,(int size, int rank, int ndims, const MPI_Count array_of_gsizes[], const int array_of_distribs[], const int array_of_dargs[], const int array_of_psizes[], int order, MPI_Datatype oldtype, MPI_Datatype *newtype),(size,rank,ndims,array_of_gsizes,array_of_distribs,array_of_dargs,array_of_psizes,order,oldtype,newtype)) \
    X(Type_create_hindexed_c,(MPI_Count count, const MPI_Count array_of_blocklengths[], const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype),(count,array_of_blocklengths,array_of_displacements,oldtype,newtype)) \
    X(Type_create_hindexed_block_c,(MPI_Count count, MPI_Count blocklength, const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype),(count,blocklength,array_of_displacements,oldtype,newtype)) \
    X(Type_create_hvector_c,(MPI_Count count, MPI_Count blocklength, MPI_Count stride, MPI_Datatype oldtype, MPI_Datatype *newtype),(count,blocklength,stride,oldtype,newtype)) \
    X(Type_create_indexed_block_c,(MPI_Count count, MPI_Count blocklength, const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype),(count,blocklength,array_of_displacements,oldtype,newtype)) \
    X(Type_create_resized_c,(MPI_Datatype oldtype, MPI_Count lb, MPI_Count extent, MPI_Datatype *newtype),(oldtype,lb,extent,newtype)) \
    X(Type_create_struct_c,(MPI_Count count, const MPI_Count array_of_blocklengths[], const MPI_Count array_of_displacements[], const MPI_Datatype array_of_types[], MPI_Datatype *newtype),(count,array_of_blocklengths,array_of_displacements,array_of_types,newtype)) \
    X(Type_create_subarray_c,(int ndims, const MPI_Count array_of_sizes[], const MPI_Count array_of_subsizes[], const MPI_Count array_of_starts[], int order, MPI_Datatype oldtype, MPI_Datatype *newtype),(ndims,array_of_sizes,array_of_subsizes,array_of_starts,order,oldtype,newtype)) \
    X(Type_get_contents_c,(MPI_Datatype datatype, MPI_Count max_integers, MPI_Count max_addresses, MPI_Count max_large_counts, MPI_Count max_datatypes, int array_of_integers[], MPI_Aint array_of_addresses[], MPI_Count array_of_large_counts[], MPI_Datatype array_of_datatypes[]),(datatype,max_integers,max_addresses,max_large_counts,max_datatypes,array_of_integers,array_of_addresses,array_of_large_counts,array_of_datatypes)) \
    X(Type_get_envelope_c,(MPI_Datatype datatype, MPI_Count *num_integers, MPI_Count *num_addresses, MPI_Count *num_large_counts, MPI_Count *num_datatypes, int *combiner),(datatype,num_integers,num_addresses,num_large_counts,num_datatypes,combiner)) \
    X(Type_get_extent_c,(MPI_Datatype datatype, MPI_Count *lb, MPI_Count *extent),(datatype,lb,extent)) \
    X(Type_get_true_extent_c,(MPI_Datatype datatype, MPI_Count *true_lb, MPI_Count *true_extent),(datatype,true_lb,true_extent)) \
    X(Type_indexed_c,(MPI_Count count, const MPI_Count array_of_blocklengths[], const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype),(count,array_of_blocklengths,array_of_displacements,oldtype,newtype)) \
    X(Type_size_c,(MPI_Datatype datatype, MPI_Count *size),(datatype,size)) \
    X(Type_vector_c,(MPI_Count count, MPI_Count blocklength, MPI_Count stride, MPI_Datatype oldtype, MPI_Datatype *newtype),(count,blocklength,stride,oldtype,newtype)) \
    X(Unpack_c,(const void *inbuf, MPI_Count insize, MPI_Count *position, void *outbuf, MPI_Count outcount, MPI_Datatype datatype, MPI_Comm comm),(inbuf,insize,position,outbuf,outcount,datatype,comm)) \
    X(Unpack_external_c,(const char datarep[], const void *inbuf, MPI_Count insize, MPI_Count *position, void *outbuf, MPI_Count outcount, MPI_Datatype datatype),(datarep,inbuf,insize,position,outbuf,outcount,datatype)) \
    X(Op_create_c,(MPI_User_function_c *user_fn, int commute, MPI_Op *op),(user_fn,commute,op)) \
    X(Bsend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm),(buf,count,datatype,dest,tag,comm)) \
    X(Bsend_init_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Buffer_attach_c,(void *buffer, MPI_Count size),(buffer,size)) \
    X(Buffer_detach_c,(void *buffer_addr, MPI_Count *size),(buffer_addr,size)) \
    X(Ibsend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Imrecv_c,(void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Message *message, MPI_Request *request),(buf,count,datatype,message,request)) \
    X(Irecv_c,(void *buf, MPI_Count count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,source,tag,comm,request)) \
    X(Irsend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Isend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Isendrecv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm, MPI_Request *request),(sendbuf,sendcount,sendtype,dest,sendtag,recvbuf,recvcount,recvtype,source,recvtag,comm,request)) \
    X(Isendrecv_replace_c,(void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int sendtag, int source, int recvtag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,sendtag,source,recvtag,comm,request)) \
    X(Issend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Mrecv_c,(void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Message *message, MPI_Status *status),(buf,count,datatype,message,status)) \
    X(Recv_c,(void *buf, MPI_Count count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Status *status),(buf,count,datatype,source,tag,comm,status)) \
    X(Recv_init_c,(void *buf, MPI_Count count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,source,tag,comm,request)) \
    X(Rsend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm),(buf,count,datatype,dest,tag,comm)) \
    X(Rsend_init_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Send_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm),(buf,count,datatype,dest,tag,comm)) \
    X(Send_init_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Sendrecv_c,(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm, MPI_Status *status),(sendbuf,sendcount,sendtype,dest,sendtag,recvbuf,recvcount,recvtype,source,recvtag,comm,status)) \
    X(Sendrecv_replace_c,(void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int sendtag, int source, int recvtag, MPI_Comm comm, MPI_Status *status),(buf,count,datatype,dest,sendtag,source,recvtag,comm,status)) \
    X(Ssend_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm),(buf,count,datatype,dest,tag,comm)) \
    X(Ssend_init_c,(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request),(buf,count,datatype,dest,tag,comm,request)) \
    X(Accumulate_c,(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win)) \
    X(Get_c,(void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win)) \
    X(Get_accumulate_c,(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, void *result_addr, MPI_Count result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win),(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win)) \
    X(Put_c,(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win)) \
    X(Raccumulate_c,(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request)) \
    X(Rget_c,(void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request)) \
    X(Rget_accumulate_c,(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, void *result_addr, MPI_Count result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request)) \
    X(Rput_c,(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request),(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request)) \
    X(Win_allocate_c,(MPI_Aint size, MPI_Aint disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win),(size,disp_unit,info,comm,baseptr,win)) \
    X(Win_allocate_shared_c,(MPI_Aint size, MPI_Aint disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win),(size,disp_unit,info,comm,baseptr,win)) \
    X(Win_create_c,(void *base, MPI_Aint size, MPI_Aint disp_unit, MPI_Info info, MPI_Comm comm, MPI_Win *win),(base,size,disp_unit,info,comm,win)) \
    X(Win_shared_query_c,(MPI_Win win, int rank, MPI_Aint *size, MPI_Aint *disp_unit, void *baseptr),(win,rank,size,disp_unit,baseptr)) \
    X(Comm_idup_with_info,(MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm, MPI_Request *request),(comm,info,newcomm,request)) \
    X(Comm_create_from_group,(MPI_Group group, const char *stringtag, MPI_Info info, MPI_Errhandler errhandler, MPI_Comm *newcomm),(group,stringtag,info,errhandler,newcomm)) \
    X(Intercomm_create_from_groups,(MPI_Group local_group, int local_leader, MPI_Group remote_group, int remote_leader, const char *stringtag, MPI_Info info, MPI_Errhandler errhandler, MPI_Comm *newintercomm),(local_group,local_leader,remote_group,remote_leader,stringtag,info,errhandler,newintercomm)) \
    X(Session_finalize,(MPI_Session *session),(session)) \
    X(Pready,(int partition, MPI_Request request),(partition,request)) \
    X(Pready_range,(int partition_low, int partition_high, MPI_Request request),(partition_low,partition_high,request)) \
    X(Pready_list,(int length, PEAK_MPI_PREADY_PARTITION_TYPE array_of_partitions[], MPI_Request request),(length,array_of_partitions,request)) \
    X(Parrived,(MPI_Request request, int partition, int *flag),(request,partition,flag))

#if MPI_VERSION >= 4 && PEAK_MPI_HAVE_STATUS_SET_ELEMENTS_C
#define PEAK_MPI_OPTIONAL_STATUS_API(X) \
    X(Status_set_elements_c,(MPI_Status *status,MPI_Datatype datatype,MPI_Count count),(status,datatype,count))
PEAK_MPI_OPTIONAL_STATUS_API(PEAK_NATIVE)
#endif
PEAK_MPI_APIS_3(PEAK_NATIVE)
#if MPI_VERSION >= 4
PEAK_MPI_APIS_4(PEAK_NATIVE)
#endif
#if MPI_VERSION >= 2
#define PEAK_MPI_IO_APIS_2(X) \
    X(File_open,(MPI_Comm comm, const char *filename, int amode, MPI_Info info, MPI_File *fh),(comm,filename,amode,info,fh)) \
    X(File_close,(MPI_File *fh),(fh)) \
    X(File_delete,(const char *filename, MPI_Info info),(filename,info)) \
    X(File_set_size,(MPI_File fh, MPI_Offset size),(fh,size)) \
    X(File_preallocate,(MPI_File fh, MPI_Offset size),(fh,size)) \
    X(File_set_info,(MPI_File fh, MPI_Info info),(fh,info)) \
    X(File_set_view,(MPI_File fh, MPI_Offset disp, MPI_Datatype etype, MPI_Datatype filetype, const char *datarep, MPI_Info info),(fh,disp,etype,filetype,datarep,info)) \
    X(File_read_at,(MPI_File fh, MPI_Offset offset, void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_read_at_all,(MPI_File fh, MPI_Offset offset, void * buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_write_at,(MPI_File fh, MPI_Offset offset, const void * buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_write_at_all,(MPI_File fh, MPI_Offset offset, const void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_iread_at,(MPI_File fh, MPI_Offset offset, void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iwrite_at,(MPI_File fh, MPI_Offset offset, const void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_read,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_read_all,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_all,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_iread,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iwrite,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_read_shared,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_shared,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_iread_shared,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iwrite_shared,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_read_ordered,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_ordered,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_seek_shared,(MPI_File fh, MPI_Offset offset, int whence),(fh,offset,whence)) \
    X(File_get_position_shared,(MPI_File fh, MPI_Offset *offset),(fh,offset)) \
    X(File_read_at_all_begin,(MPI_File fh, MPI_Offset offset, void *buf, int count, MPI_Datatype datatype),(fh,offset,buf,count,datatype)) \
    X(File_read_at_all_end,(MPI_File fh, void *buf, MPI_Status *status),(fh,buf,status)) \
    X(File_write_at_all_begin,(MPI_File fh, MPI_Offset offset, const void *buf, int count, MPI_Datatype datatype),(fh,offset,buf,count,datatype)) \
    X(File_write_at_all_end,(MPI_File fh, const void *buf, MPI_Status *status),(fh,buf,status)) \
    X(File_read_all_begin,(MPI_File fh, void *buf, int count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_read_all_end,(MPI_File fh, void *buf, MPI_Status *status),(fh,buf,status)) \
    X(File_write_all_begin,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_write_all_end,(MPI_File fh, const void *buf, MPI_Status *status),(fh,buf,status)) \
    X(File_read_ordered_begin,(MPI_File fh, void *buf, int count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_read_ordered_end,(MPI_File fh, void *buf, MPI_Status *status),(fh,buf,status)) \
    X(File_write_ordered_begin,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_write_ordered_end,(MPI_File fh, const void *buf, MPI_Status *status),(fh,buf,status)) \
    X(File_set_atomicity,(MPI_File fh, int flag),(fh,flag)) \
    X(File_sync,(MPI_File fh),(fh)) \
    X(Grequest_start,(MPI_Grequest_query_function *query_fn, MPI_Grequest_free_function *free_fn, MPI_Grequest_cancel_function *cancel_fn, void *extra_state, MPI_Request *request),(query_fn,free_fn,cancel_fn,extra_state,request)) \
    X(Grequest_complete,(MPI_Request request),(request))
PEAK_MPI_IO_APIS_2(PEAK_NATIVE)
#endif
#if MPI_VERSION > 3 || (MPI_VERSION == 3 && MPI_SUBVERSION >= 1)
#define PEAK_MPI_IO_APIS_31(X) \
    X(File_iread_at_all,(MPI_File fh, MPI_Offset offset, void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iwrite_at_all,(MPI_File fh, MPI_Offset offset, const void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iread_all,(MPI_File fh, void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iwrite_all,(MPI_File fh, const void *buf, int count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request))
PEAK_MPI_IO_APIS_31(PEAK_NATIVE)
#endif
#if MPI_VERSION >= 4
#define PEAK_MPI_IO_APIS_4(X) \
    X(File_read_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_read_all_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_read_all_begin_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_read_at_c,(MPI_File fh, MPI_Offset offset, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_read_at_all_c,(MPI_File fh, MPI_Offset offset, void * buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_read_at_all_begin_c,(MPI_File fh, MPI_Offset offset, void *buf, MPI_Count count, MPI_Datatype datatype),(fh,offset,buf,count,datatype)) \
    X(File_read_ordered_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_read_ordered_begin_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_read_shared_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_all_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_all_begin_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_write_at_c,(MPI_File fh, MPI_Offset offset, const void * buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_write_at_all_c,(MPI_File fh, MPI_Offset offset, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,offset,buf,count,datatype,status)) \
    X(File_write_at_all_begin_c,(MPI_File fh, MPI_Offset offset, const void *buf, MPI_Count count, MPI_Datatype datatype),(fh,offset,buf,count,datatype)) \
    X(File_write_ordered_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_write_ordered_begin_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype),(fh,buf,count,datatype)) \
    X(File_write_shared_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Status *status),(fh,buf,count,datatype,status)) \
    X(File_iread_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iread_all_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iread_at_c,(MPI_File fh, MPI_Offset offset, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iread_at_all_c,(MPI_File fh, MPI_Offset offset, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iread_shared_c,(MPI_File fh, void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iwrite_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iwrite_all_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request)) \
    X(File_iwrite_at_c,(MPI_File fh, MPI_Offset offset, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iwrite_at_all_c,(MPI_File fh, MPI_Offset offset, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,offset,buf,count,datatype,request)) \
    X(File_iwrite_shared_c,(MPI_File fh, const void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Request *request),(fh,buf,count,datatype,request))
PEAK_MPI_IO_APIS_4(PEAK_NATIVE)
#endif
#if MPI_VERSION >= 3
#define PEAK_MPI_IO_APIS_3(X) \
    X(Comm_set_info,(MPI_Comm comm, MPI_Info info),(comm,info))
PEAK_MPI_IO_APIS_3(PEAK_NATIVE)
#endif
#undef PEAK_NATIVE

static void charge_frontend(void)
{
    uint64_t now=metadata_now();
    if(now<wrapper_started || now-wrapper_started<native_elapsed_ns)clock_failed=1;
    peak_global_budget_mpi_frontend_charge(clock_failed?UINT64_MAX:now-wrapper_started-native_elapsed_ns);
}

/* The metadata lock orders event allocation and publication across app threads.
 * It is never held across native application MPI calls. */
static void emit(PeakMpiBudgetEvent e)
{
    if(e.kind==PEAK_MPI_EVENT_COMPLETE && !e.count && e.request){e.requests=&e.request;e.count=1;}
    e.rank = world_rank;
    e.seq = ++sequence;
    e.last_mpi_seq = last_mpi;
    if (e.kind != PEAK_MPI_EVENT_FRONTIER)
        last_mpi = e.seq;
    PeakMpiBudgetCostSnapshot cost={0};
    peak_mpi_budget_cost_snapshot(&cost);
    runtime_snapshot_locked(&cost);
    uint64_t parts[]={cost.active_outside_ns,cost.active_inside_ns,
                      cost.ignored_outside_ns,cost.ignored_inside_ns};
    e.cumulative_cost_ns=0;
    for(size_t i=0;i<4;i++)e.cumulative_cost_ns=UINT64_MAX-e.cumulative_cost_ns<parts[i]?UINT64_MAX:e.cumulative_cost_ns+parts[i];
    peak_global_budget_mpi_event_submit(&e,&cost);
}
static uint64_t runtime_add(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX-a ? UINT64_MAX : a+b;
}
static void runtime_snapshot_locked(PeakMpiBudgetCostSnapshot *snapshot)
{
    snapshot->runtime_raw_ns=0;
    snapshot->runtime_outside_ns=0;
    snapshot->runtime_sum_ns=0;
    for(unsigned i=0;i<PEAK_RUNTIME_COMPONENTS;i++) {
        snapshot->runtime_raw_ns=runtime_add(snapshot->runtime_raw_ns,runtime_raw[i]);
        snapshot->runtime_outside_ns=runtime_add(snapshot->runtime_outside_ns,runtime_outside[i]);
        snapshot->runtime_sum_ns=runtime_add(snapshot->runtime_sum_ns,runtime_sum[i]);
    }
}
static void runtime_observe_locked(unsigned component, uint64_t raw)
{
    if(raw>runtime_raw[component])runtime_raw[component]=raw;
    uint64_t classified=runtime_add(runtime_outside[component],runtime_sum[component]);
    if(!runtime_active[component] && runtime_raw[component]>classified)
        runtime_sum[component]=runtime_add(runtime_sum[component],runtime_raw[component]-classified);
}
PeakMpiRuntimeSpan peak_mpi_budget_runtime_begin(PeakMpiRuntimeComponent component)
{
    PeakMpiRuntimeSpan span={0};
    if(component>=PEAK_RUNTIME_COMPONENTS)return span;
    pthread_mutex_lock(&lock);
    span=(PeakMpiRuntimeSpan){.mpi_sequence=last_mpi,.lifecycle_epoch=runtime_epoch,
        .component=(unsigned)component,.registered=true,
        .eligible=atomic_load_explicit(&frontend_ready,memory_order_acquire) &&
            peak_global_budget_mpi_events_enabled() && !active_calls &&
            !stream_closed && !uncertifiable_state && !runtime_closed};
    ++runtime_active[component];
    pthread_mutex_unlock(&lock);
    return span;
}
void peak_mpi_budget_runtime_end(PeakMpiRuntimeSpan *span, uint64_t own_ns,
    uint64_t raw_after)
{
    if(!span || !span->registered)return;
    pthread_mutex_lock(&lock);
    unsigned component=span->component;
    bool outside=span->eligible && span->mpi_sequence==last_mpi &&
        span->lifecycle_epoch==runtime_epoch && !active_calls && !runtime_closed &&
        !stream_closed && !uncertifiable_state && peak_global_budget_mpi_events_enabled();
    if(raw_after>runtime_raw[component])runtime_raw[component]=raw_after;
    uint64_t *classified=outside?runtime_outside:runtime_sum;
    classified[component]=runtime_add(classified[component],own_ns);
    --runtime_active[component];
    span->registered=false;
    runtime_observe_locked(component,raw_after);
    /* Control and transport only update counters. Their prefixes are captured
     * before the next MPI edge or by the genuine management frontier; this
     * avoids extra per-window traffic and send/classification feedback. */
    if(own_ns && own_ns!=UINT64_MAX && component==PEAK_RUNTIME_MANAGEMENT &&
       atomic_load_explicit(&frontend_ready,memory_order_acquire) &&
       !runtime_closed && !stream_closed && !uncertifiable_state &&
       peak_global_budget_mpi_events_enabled())
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_FRONTIER});
    pthread_mutex_unlock(&lock);
}
void peak_mpi_budget_runtime_observe(PeakMpiRuntimeComponent component, uint64_t raw)
{
    if(component>=PEAK_RUNTIME_COMPONENTS)return;
    pthread_mutex_lock(&lock);
    runtime_observe_locked((unsigned)component,raw);
    pthread_mutex_unlock(&lock);
}
void peak_mpi_budget_runtime_snapshot(PeakMpiBudgetCostSnapshot *snapshot)
{
    if(!snapshot)return;
    pthread_mutex_lock(&lock);
    runtime_snapshot_locked(snapshot);
    pthread_mutex_unlock(&lock);
}
void peak_mpi_budget_runtime_classification_close(void)
{
    pthread_mutex_lock(&lock);
    if(!runtime_closed){runtime_closed=true;++runtime_epoch;}
    pthread_mutex_unlock(&lock);
}
static void unknown_plain(void) { emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_UNKNOWN}); }
/* Unmodeled synchronous dependencies require an actual WORLD entry fence. */
static void unknown(void) { needs_strong_fence=1;unknown_plain(); }
/* Untracked live state has no closure proof in the first implementation. */
static void unknown_live(void)
{
    uncertifiable_state=1;needs_strong_fence=1;
    emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_UNKNOWN,.closure_flags=PEAK_MPI_OPAQUE_LIVE});
}
static Context *context(MPI_Comm comm)
{
    for (Context *c=contexts;c;c=c->next) if(!c->retired && c->handle==comm) return c;
    return NULL;
}
/* Canonical byte arithmetic is independent of native handle representation
 * and host byte order. Child serials occupy a namespace disjoint from WORLD
 * and SELF. As with the transport nonce, collisions are probabilistic; live
 * identity collisions and membership disagreement fail metadata closed. */
static void identity_word(uint64_t *a,uint64_t *b,uint64_t value)
{
    for(unsigned i=0;i<8;i++){
        uint64_t byte=(value>>(8*i))&255U;
        *a=(*a^byte)*UINT64_C(1099511628211);
        *b=(*b^(byte+UINT64_C(0x9d)))*UINT64_C(14029467366897019727);
    }
}
static void child_identity(const CreationIdentity *key,const int *members,int size,uint32_t *owner,uint64_t *serial)
{
    uint64_t a=UINT64_C(14695981039346656037),b=UINT64_C(0x6a09e667f3bcc909);
    identity_word(&a,&b,UINT64_C(0x5045414b43545831)); /* PEAKCTX1 */
    identity_word(&a,&b,key->parent_owner);identity_word(&a,&b,key->parent_id);
    identity_word(&a,&b,key->ordinal);identity_word(&a,&b,key->kind);
    identity_word(&a,&b,(uint32_t)key->tag);identity_word(&a,&b,(uint64_t)size);
    for(int i=0;i<size;i++)identity_word(&a,&b,(uint32_t)members[i]);
    *owner=(uint32_t)(b^(b>>32));
    *serial=a|UINT64_C(0x8000000000000000);
}
static int register_context(MPI_Comm comm, uint32_t owner, uint64_t id,const CreationIdentity *key)
{
    int inter=0, size=0;
    if(comm==MPI_COMM_NULL) return 1;
    if(PMPI_Comm_test_inter(comm,&inter)!=MPI_SUCCESS || inter ||
       PMPI_Comm_size(comm,&size)!=MPI_SUCCESS || size<1 ||
       (size_t)size>SIZE_MAX/sizeof(int)) return 0;
    Context *c=calloc(1,sizeof *c);
    int *local=malloc((size_t)size*sizeof *local);
    if(!c || !local) {free(c);free(local);return 0;}
    c->members=malloc((size_t)size*sizeof *c->members);
    if(!c->members) {free(local);free(c);return 0;}
    MPI_Group g=MPI_GROUP_NULL,w=MPI_GROUP_NULL;
    for(int i=0;i<size;i++) local[i]=i;
    int ok=PMPI_Comm_group(comm,&g)==MPI_SUCCESS &&
        PMPI_Comm_group(MPI_COMM_WORLD,&w)==MPI_SUCCESS &&
        PMPI_Group_translate_ranks(g,size,local,w,c->members)==MPI_SUCCESS;
    if(g!=MPI_GROUP_NULL) PMPI_Group_free(&g);
    if(w!=MPI_GROUP_NULL) PMPI_Group_free(&w);
    free(local);
    if(!ok) {free(c->members);free(c);return 0;}
    for(int i=0;i<size;i++)if(c->members[i]<0){free(c->members);free(c);return 0;}
    if(key)child_identity(key,c->members,size,&owner,&id);
    for(Context *old=contexts;old;old=old->next)if(old->owner==owner && old->id==id){free(c->members);free(c);return 0;}
    c->handle=comm;c->id=id;c->owner=owner;c->size=size;c->next=contexts;contexts=c;
    peak_global_budget_mpi_context_register(owner,id,c->members,(size_t)size);
    return 1;
}
/* Terminal UNKNOWN is accepted by the sink even after its gate closes. All
 * previously admitted operations finish their records; no stream can resume
 * across unobserved communicator/request changes. */
static void seal_stream(void)
{
    pthread_mutex_lock(&lock);
    if(!stream_closed){stream_closed=1;unknown();}
    pthread_mutex_unlock(&lock);
}
static void seal_and_charge(void)
{
    uint64_t started=metadata_now();
    seal_stream();
    uint64_t finished=metadata_now();
    peak_global_budget_mpi_frontend_charge(clock_failed || finished<started?UINT64_MAX:finished-started);
}
void peak_mpi_budget_events_initialize(void)
{
    if(!peak_global_budget_mpi_events_enabled()) return;
    clock_failed=0;wrapper_started=metadata_now();native_elapsed_ns=0;
    pthread_mutex_lock(&lock);
    if(PMPI_Comm_rank(MPI_COMM_WORLD,&world_rank)!=MPI_SUCCESS){
        uncertifiable_state=1;clock_failed=1;pthread_mutex_unlock(&lock);
        charge_frontend();return;
    }
    if(!register_context(MPI_COMM_WORLD,UINT32_MAX,1,NULL) ||
       !register_context(MPI_COMM_SELF,(uint32_t)world_rank,0,NULL)) unknown_live();
    pthread_mutex_unlock(&lock);
    charge_frontend();
    if(!peak_global_budget_mpi_events_enabled())seal_and_charge();
    atomic_store_explicit(&frontend_ready,1,memory_order_release);
}
/* Private heartbeat entry: the caller meters this entire call as background
 * management SUM cost, including snapshots and queue publication. It is not a
 * foreground operation and must not consume its leases or settle its costs. */
void peak_mpi_budget_events_frontier(void)
{
    if(!atomic_load_explicit(&frontend_ready,memory_order_acquire) ||
       !peak_global_budget_mpi_events_enabled())return;
    pthread_mutex_lock(&lock);
    emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_FRONTIER});
    pthread_mutex_unlock(&lock);
}
static int begin(void)
{
    if(!atomic_load_explicit(&frontend_ready,memory_order_acquire) ||
       !peak_global_budget_mpi_events_enabled() || !peak_mpi_application_enter()) return 0;
    clock_failed=0;wrapper_started=metadata_now();native_elapsed_ns=0;
    if(!peak_global_budget_mpi_trace_begin()){
        seal_stream();charge_frontend();peak_mpi_application_leave();return 0;
    }
    application_active=1;
    pthread_mutex_lock(&lock);
    if(active_calls++) unknown_live();
    emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_ENTER});
    pthread_mutex_unlock(&lock);
    return 1;
}
static void end_impl(int active,int result,const PeakMpiBudgetEvent *collective,int strong_fence)
{
    if(!active) return;
    pthread_mutex_lock(&lock);
    if(result!=MPI_SUCCESS || clock_failed) unknown_live();
    if(active_calls) active_calls--;
    PeakMpiBudgetEvent exit={.kind=PEAK_MPI_EVENT_EXIT};
    if(collective){exit.op=collective->op;exit.comm=collective->comm;exit.context_owner=collective->context_owner;exit.round=collective->round;}
    if(collective && result==MPI_SUCCESS && !clock_failed &&
       collective->comm==1 && collective->context_owner==UINT32_MAX &&
       !active_calls && !requests && !uncertifiable_state && !stream_closed &&
       peak_global_budget_mpi_events_enabled()) {
        if(strong_fence)needs_strong_fence=0;
        if(!needs_strong_fence)exit.closure_flags=PEAK_MPI_CLOSED_WORLD_PREFIX |
            (strong_fence?PEAK_MPI_ENTRY_FENCE:0);
    }
    emit(exit);
    pthread_mutex_unlock(&lock);
    charge_frontend();
    if(!peak_global_budget_mpi_events_enabled())seal_and_charge();
    application_active=0;
    peak_mpi_application_leave();
}
static void end(int active,int result){end_impl(active,result,NULL,0);}
static void end_collective(int active,int result,const PeakMpiBudgetEvent *e)
{end_impl(active,result,e,0);}
/* Ambiguous blocking and nonblocking sends share the same MPI FIFO lane. */
static uint64_t message_round(Context *c,PeakMpiEventOp op,int peer,int tag)
{
    if(op==PEAK_MPI_OP_AMBIGUOUS_SEND)op=PEAK_MPI_OP_SEND;
    MessageCounter *m=c->messages;
    while(m && (m->peer!=peer || m->tag!=tag || m->op!=op))m=m->link;
    if(!m){m=calloc(1,sizeof *m);if(m){m->peer=peer;m->tag=tag;m->op=op;m->link=c->messages;c->messages=m;}}
    if(!m){unknown_live();return 0;}
    uint64_t round=++m->next;
    if(!round)unknown_live();
    return round;
}
static void end_barrier(int active,int result,const PeakMpiBudgetEvent *e)
{end_impl(active,result,e,1);}
static PeakMpiBudgetEvent post(MPI_Comm comm,PeakMpiEventOp op,int peer,int tag)
{
    Context *c=context(comm);
    PeakMpiBudgetEvent e={.kind=PEAK_MPI_EVENT_POST,.op=op,
        .request=++next_request,.peer=peer,.tag=(uint64_t)(int64_t)tag};
    if(op!=PEAK_MPI_OP_COLLECTIVE && peer==MPI_PROC_NULL){e.request=0;return e;}
    if(!c) {unknown_live();return e;}
    e.comm=c->id;e.context_owner=c->owner;
    if(op==PEAK_MPI_OP_COLLECTIVE) e.round=++c->round;
    else {
        int resolved=peer>=0 && peer<c->size;
        e.peer=resolved?c->members[peer]:-1; /* SIZE_MAX on the neutral wire. */
        if(resolved && tag>=0)e.round=message_round(c,op,peer,tag);
        else unknown_plain();
    }
    emit(e);return e;
}
static void resolve_receive(PeakMpiBudgetEvent *e,Context *c,const MPI_Status *status)
{
    int peer=status->MPI_SOURCE,tag=status->MPI_TAG;
    e->peer=c && peer>=0 && peer<c->size?c->members[peer]:peer;
    e->tag=(uint64_t)(int64_t)tag;
    if(!e->round && e->request && c && peer>=0 && peer<c->size && tag>=0)
        e->round=message_round(c,PEAK_MPI_OP_RECV,peer,tag);
}
static void remember(MPI_Request handle,PeakMpiBudgetEvent e)
{
    if(!e.request)return;
    if(handle==MPI_REQUEST_NULL) {
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});
        e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);return;
    }
    Request *r=recycled_requests;
    if(r)recycled_requests=r->next;else r=malloc(sizeof *r);
    if(!r) {unknown_live();return;}
    r->handle=handle;r->post=e;r->next=requests;requests=r;
}
static uint64_t request_id(MPI_Request handle)
{
    if(handle==MPI_REQUEST_NULL) return 0;
    for(Request *r=requests;r;r=r->next) if(r->handle==handle) return r->post.request;
    unknown();return 0;
}
static void collect_contexts(void)
{
    Context **p=&contexts;
    if(!retired_contexts)return;
    while(*p){Context *c=*p;int used=0;
        if(!c->retired){p=&c->next;continue;}
        for(Request *r=requests;r;r=r->next)
            if(r->post.comm==c->id && r->post.context_owner==c->owner){used=1;break;}
        if(used){p=&c->next;continue;}
        *p=c->next;retired_contexts--;free(c->members);MessageCounter *m=c->messages;
        while(m){MessageCounter *n=m->link;free(m);m=n;}
        GroupCreation *g=c->groups;while(g){GroupCreation *n=g->next;free(g->members);free(g);g=n;}free(c);
    }
}
static void complete(uint64_t id, const MPI_Status *status)
{
    if(!id) return;
    Request **p=&requests;
    while(*p && (*p)->post.request!=id) p=&(*p)->next;
    if(!*p) {unknown_live();return;}
    Request *r=*p;PeakMpiBudgetEvent e=r->post;
    *p=r->next;r->next=recycled_requests;recycled_requests=r;
    e.kind=PEAK_MPI_EVENT_COMPLETE;
    if(e.op==PEAK_MPI_OP_RECV && status) {
        Context *c=contexts;while(c && (c->id!=e.comm || c->owner!=e.context_owner))c=c->next;
        resolve_receive(&e,c,status);
    }
    emit(e);
    collect_contexts();
}
typedef struct {
    uint64_t *ids, *waiting;
    size_t id_capacity, waiting_capacity, status_capacity;
    MPI_Status *statuses;
} WaitScratch;
static _Thread_local WaitScratch *thread_scratch;
static pthread_key_t scratch_key;
static pthread_once_t scratch_once=PTHREAD_ONCE_INIT;
static int scratch_key_ready;
static void scratch_destroy(void *value)
{
    WaitScratch *s=value;
    if(!s)return;
    uint64_t start=metadata_now();
    free(s->ids);free(s->waiting);free(s->statuses);free(s);
    uint64_t finish=metadata_now();
    peak_global_budget_mpi_frontend_charge(clock_failed || finish<start?UINT64_MAX:finish-start);
}
static void scratch_initialize(void)
{scratch_key_ready=pthread_key_create(&scratch_key,scratch_destroy)==0;}
static WaitScratch *scratch(void)
{
    if(thread_scratch)return thread_scratch;
    pthread_once(&scratch_once,scratch_initialize);
    if(!scratch_key_ready)return NULL;
    WaitScratch *s=calloc(1,sizeof *s);
    if(!s)return NULL;
    if(pthread_setspecific(scratch_key,s)!=0){free(s);return NULL;}
    thread_scratch=s;return s;
}
static int grow_scratch(void **data,size_t *capacity,size_t count,size_t element)
{
    if(count<=*capacity)return 1;
    if(count>SIZE_MAX/element)return 0;
    void *grown=realloc(*data,count*element);
    if(!grown)return 0;
    *data=grown;*capacity=count;return 1;
}
static MPI_Status *wait_statuses(int count)
{
    WaitScratch *s=scratch();
    if(!s || count<1 || !grow_scratch((void **)&s->statuses,&s->status_capacity,(size_t)count,sizeof *s->statuses))return NULL;
    return s->statuses;
}
static uint64_t *wait_ids(int count,const MPI_Request *array)
{
    if(count<0 || (count>0 && !array)) {unknown();return NULL;}
    WaitScratch *s=scratch();size_t needed=count?(size_t)count:1;
    if(!s || !grow_scratch((void **)&s->ids,&s->id_capacity,needed,sizeof *s->ids) ||
       !grow_scratch((void **)&s->waiting,&s->waiting_capacity,needed,sizeof *s->waiting)){unknown_live();return NULL;}
    uint64_t *ids=s->ids,*waiting=s->waiting;
    size_t n=0;
    for(int i=0;i<count;i++){ids[i]=request_id(array[i]);if(ids[i])waiting[n++]=ids[i];}
    emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=waiting,.count=n});
    return ids;
}
static int peak_mpi_Barrier(MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Barrier(comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_barrier(active,result,&e);return result;
}

static int peak_mpi_Ibarrier(MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ibarrier(comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Bcast(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Bcast(buffer,count,datatype,root,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ibcast(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ibcast(buffer,count,datatype,root,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Allreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Allreduce(sendbuf,recvbuf,count,datatype,op,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iallreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iallreduce(sendbuf,recvbuf,count,datatype,op,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Reduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Reduce(sendbuf,recvbuf,count,datatype,op,root,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ireduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ireduce(sendbuf,recvbuf,count,datatype,op,root,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Allgather(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iallgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iallgather(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Allgatherv(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iallgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iallgatherv(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Alltoall(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ialltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ialltoall(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Alltoallv(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Alltoallv(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ialltoallv(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ialltoallv(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Alltoallw(const void *sendbuf, const int sendcounts[], const int sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const int rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Alltoallw(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ialltoallw(const void *sendbuf, const int sendcounts[], const int sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const int rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ialltoallw(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Gather(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Igather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Igather(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Gatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Gatherv(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Igatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Igatherv(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Scatter(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iscatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iscatter(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Scatterv(const void *sendbuf, const int sendcounts[], const int displs[], MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Scatterv(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iscatterv(const void *sendbuf, const int sendcounts[], const int displs[], MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iscatterv(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Reduce_scatter(const void *sendbuf, void *recvbuf, const int recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Reduce_scatter(sendbuf,recvbuf,recvcounts,datatype,op,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ireduce_scatter(const void *sendbuf, void *recvbuf, const int recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ireduce_scatter(sendbuf,recvbuf,recvcounts,datatype,op,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Reduce_scatter_block(const void *sendbuf, void *recvbuf, int recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Reduce_scatter_block(sendbuf,recvbuf,recvcount,datatype,op,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Ireduce_scatter_block(const void *sendbuf, void *recvbuf, int recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ireduce_scatter_block(sendbuf,recvbuf,recvcount,datatype,op,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Scan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Scan(sendbuf,recvbuf,count,datatype,op,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iscan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iscan(sendbuf,recvbuf,count,datatype,op,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Exscan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Exscan(sendbuf,recvbuf,count,datatype,op,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
    end_collective(active,result,&e);return result;
}

static int peak_mpi_Iexscan(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{
    int active=begin(); PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Iexscan(sendbuf,recvbuf,count,datatype,op,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS && request) {pthread_mutex_lock(&lock);remember(*request,e);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}

static int peak_mpi_Isend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_SEND,peer,tag);
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Isend(buf,count,datatype,peer,tag,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        remember(*request,e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Issend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_SEND,peer,tag);
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Issend(buf,count,datatype,peer,tag,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        remember(*request,e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Ibsend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        unknown_live();
        e=post(comm,PEAK_MPI_OP_SEND,peer,tag);
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ibsend(buf,count,datatype,peer,tag,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        remember(*request,e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Irsend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        unknown_live();
        e=post(comm,PEAK_MPI_OP_SEND,peer,tag);
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Irsend(buf,count,datatype,peer,tag,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        remember(*request,e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Irecv(void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Request *request)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_RECV,peer,tag);
        if(peer==MPI_ANY_SOURCE || tag==MPI_ANY_TAG) unknown_live();
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Irecv(buf,count,datatype,peer,tag,comm,request);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        remember(*request,e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Send(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_AMBIGUOUS_SEND,peer,tag);
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Send(buf,count,datatype,peer,tag,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Ssend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_AMBIGUOUS_SEND,peer,tag);
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Ssend(buf,count,datatype,peer,tag,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Bsend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_AMBIGUOUS_SEND,peer,tag);
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Bsend(buf,count,datatype,peer,tag,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Rsend(const void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_AMBIGUOUS_SEND,peer,tag);
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Rsend(buf,count,datatype,peer,tag,comm);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Recv(void *buf, int count, MPI_Datatype datatype, int peer, int tag, MPI_Comm comm, MPI_Status *status)
{
    int active=begin();PeakMpiBudgetEvent e={0};
    MPI_Status temporary;MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&temporary:status;
    if(active) {pthread_mutex_lock(&lock);
        e=post(comm,PEAK_MPI_OP_RECV,peer,tag);
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});
        pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Recv(buf,count,datatype,peer,tag,comm,actual);native_stop(active);
    if(active && result==MPI_SUCCESS) {pthread_mutex_lock(&lock);
        resolve_receive(&e,context(comm),actual);
        e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Wait(MPI_Request *request,MPI_Status *status)
{
    int active=begin();uint64_t *ids=NULL;MPI_Status tmp;
    MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&tmp:status;
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(1,request);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Wait(request,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids)complete(ids[0],actual);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Test(MPI_Request *request,int *flag,MPI_Status *status)
{
    int active=begin();uint64_t *ids=NULL;MPI_Status tmp;
    MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&tmp:status;
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(1,request);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Test(request,flag,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && *flag && ids)complete(ids[0],actual);pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Waitall(int count, MPI_Request array[], MPI_Status *statuses)
{
    int active=begin();uint64_t *ids=NULL;
    MPI_Status *temporary=NULL,*actual=statuses;
    if(active && statuses==MPI_STATUSES_IGNORE && count>0) {temporary=wait_statuses(count);if(temporary)actual=temporary;}
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(count,array);if(count>0 && actual==MPI_STATUSES_IGNORE)unknown_live();pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Waitall(count,array,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids){for(int i=0;i<count;i++)complete(ids[i],actual==MPI_STATUSES_IGNORE?NULL:&actual[i]);}pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Waitany(int count, MPI_Request array[], int *index, MPI_Status *status)
{
    int active=begin();uint64_t *ids=NULL;
    MPI_Status tmp;MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&tmp:status;
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(count,array);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Waitany(count,array,index,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids){if(*index!=MPI_UNDEFINED && *index>=0 && *index<count)complete(ids[*index],actual);}pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Waitsome(int count, MPI_Request array[], int *outcount, int indices[], MPI_Status *statuses)
{
    int active=begin();uint64_t *ids=NULL;
    MPI_Status *temporary=NULL,*actual=statuses;
    if(active && statuses==MPI_STATUSES_IGNORE && count>0) {temporary=wait_statuses(count);if(temporary)actual=temporary;}
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(count,array);if(count>0 && actual==MPI_STATUSES_IGNORE)unknown_live();pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Waitsome(count,array,outcount,indices,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids){if(*outcount!=MPI_UNDEFINED)for(int i=0;i<*outcount;i++){if(indices[i]>=0 && indices[i]<count)complete(ids[indices[i]],actual==MPI_STATUSES_IGNORE?NULL:&actual[i]);else unknown();}}pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Testall(int count, MPI_Request array[], int *flag, MPI_Status *statuses)
{
    int active=begin();uint64_t *ids=NULL;
    MPI_Status *temporary=NULL,*actual=statuses;
    if(active && statuses==MPI_STATUSES_IGNORE && count>0) {temporary=wait_statuses(count);if(temporary)actual=temporary;}
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(count,array);if(count>0 && actual==MPI_STATUSES_IGNORE)unknown_live();pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Testall(count,array,flag,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids && *flag){for(int i=0;i<count;i++)complete(ids[i],actual==MPI_STATUSES_IGNORE?NULL:&actual[i]);}pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Testany(int count, MPI_Request array[], int *index, int *flag, MPI_Status *status)
{
    int active=begin();uint64_t *ids=NULL;
    MPI_Status tmp;MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&tmp:status;
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(count,array);pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Testany(count,array,index,flag,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids && *flag){if(*index!=MPI_UNDEFINED && *index>=0 && *index<count)complete(ids[*index],actual);}pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Testsome(int count, MPI_Request array[], int *outcount, int indices[], MPI_Status *statuses)
{
    int active=begin();uint64_t *ids=NULL;
    MPI_Status *temporary=NULL,*actual=statuses;
    if(active && statuses==MPI_STATUSES_IGNORE && count>0) {temporary=wait_statuses(count);if(temporary)actual=temporary;}
    if(active){pthread_mutex_lock(&lock);ids=wait_ids(count,array);if(count>0 && actual==MPI_STATUSES_IGNORE)unknown_live();pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Testsome(count,array,outcount,indices,actual);native_stop(active);
    if(active){pthread_mutex_lock(&lock);if(result==MPI_SUCCESS && ids){if(*outcount!=MPI_UNDEFINED)for(int i=0;i<*outcount;i++){if(indices[i]>=0 && indices[i]<count)complete(ids[indices[i]],actual==MPI_STATUSES_IGNORE?NULL:&actual[i]);else unknown();}}pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
/* All added MPI calls here are local group/membership queries. In particular
 * no private collective may depend on rank-local metadata admission. */
static CreationIdentity creation_identity(int active,MPI_Comm parent,unsigned kind)
{
    CreationIdentity key={.kind=kind};
    if(!active)return key;
    pthread_mutex_lock(&lock);Context *c=context(parent);
    if(c){key.parent_owner=c->owner;key.parent_id=c->id;
        if(c->creation_ordinal!=UINT64_MAX){key.ordinal=++c->creation_ordinal;key.valid=1;}}
    if(!key.valid)unknown_live();
    pthread_mutex_unlock(&lock);return key;
}
static CreationIdentity group_creation_identity(int active,MPI_Comm parent,MPI_Group group,int tag)
{
    CreationIdentity key={.kind=5,.tag=tag};
    if(!active)return key;
    pthread_mutex_lock(&lock);Context *c=context(parent);
    if(c){key.parent_owner=c->owner;key.parent_id=c->id;}
    else unknown_live();
    pthread_mutex_unlock(&lock);if(!c)return key;
    int size=0;MPI_Group world_group=MPI_GROUP_NULL;
    if(PMPI_Group_size(group,&size)!=MPI_SUCCESS || size<1 ||
       (size_t)size>SIZE_MAX/sizeof(int)){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);return key;}
    int *indices=malloc((size_t)size*sizeof *indices),*members=malloc((size_t)size*sizeof *members);
    int ok=indices && members;
    if(ok){for(int i=0;i<size;i++)indices[i]=i;
        ok=PMPI_Comm_group(MPI_COMM_WORLD,&world_group)==MPI_SUCCESS &&
           PMPI_Group_translate_ranks(group,size,indices,world_group,members)==MPI_SUCCESS;
        for(int i=0;ok && i<size;i++)if(members[i]<0)ok=0;}
    if(world_group!=MPI_GROUP_NULL)PMPI_Group_free(&world_group);
    free(indices);
    pthread_mutex_lock(&lock);c=context(parent);
    if(ok && c){GroupCreation *g=c->groups;
        while(g && (g->tag!=tag || g->size!=size || memcmp(g->members,members,(size_t)size*sizeof *members)))g=g->next;
        if(!g){g=calloc(1,sizeof *g);if(g){g->tag=tag;g->size=size;g->members=members;members=NULL;g->next=c->groups;c->groups=g;}}
        if(g && g->ordinal!=UINT64_MAX){key.ordinal=++g->ordinal;key.valid=1;}}
    if(!key.valid)unknown_live();
    pthread_mutex_unlock(&lock);free(members);return key;
}
static void created(MPI_Comm comm,const CreationIdentity *key)
{
    if(comm==MPI_COMM_NULL || !key->valid)return;
    pthread_mutex_lock(&lock);
    if(!register_context(comm,0,0,key))unknown_live();
    pthread_mutex_unlock(&lock);
}
static PeakMpiBudgetEvent creation_enter(int active,MPI_Comm comm)
{
    PeakMpiBudgetEvent e={0};
    if(active){pthread_mutex_lock(&lock);e=post(comm,PEAK_MPI_OP_COLLECTIVE,0,0);
        emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=e.request?&e.request:NULL,.count=e.request?1:0});pthread_mutex_unlock(&lock);}
    return e;
}
static void creation_exit(int active,int result,PeakMpiBudgetEvent e)
{
    if(active && result==MPI_SUCCESS){pthread_mutex_lock(&lock);e.kind=PEAK_MPI_EVENT_COMPLETE;emit(e);pthread_mutex_unlock(&lock);}
}
static int peak_mpi_Comm_dup(MPI_Comm comm,MPI_Comm *newcomm)
{int active=begin();CreationIdentity creation_key=creation_identity(active,comm,1);PeakMpiBudgetEvent e=creation_enter(active,comm);native_start(active);int result=call_Comm_dup(comm,newcomm);native_stop(active);if(active && result==MPI_SUCCESS)created(*newcomm,&creation_key);creation_exit(active,result,e);end(active,result);return result;}
static int peak_mpi_Comm_split(MPI_Comm comm,int color,int key,MPI_Comm *newcomm)
{int active=begin();CreationIdentity creation_key=creation_identity(active,comm,2);PeakMpiBudgetEvent e=creation_enter(active,comm);native_start(active);int result=call_Comm_split(comm,color,key,newcomm);native_stop(active);if(active && result==MPI_SUCCESS)created(*newcomm,&creation_key);creation_exit(active,result,e);end(active,result);return result;}
static int peak_mpi_Comm_create(MPI_Comm comm,MPI_Group group,MPI_Comm *newcomm)
{int active=begin();CreationIdentity creation_key=creation_identity(active,comm,3);PeakMpiBudgetEvent e=creation_enter(active,comm);native_start(active);int result=call_Comm_create(comm,group,newcomm);native_stop(active);if(active && result==MPI_SUCCESS)created(*newcomm,&creation_key);creation_exit(active,result,e);end(active,result);return result;}
static int peak_mpi_Comm_create_group(MPI_Comm comm,MPI_Group group,int tag,MPI_Comm *newcomm)
{int active=begin();CreationIdentity creation_key=group_creation_identity(active,comm,group,tag);if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_create_group(comm,group,tag,newcomm);native_stop(active);if(active && result==MPI_SUCCESS)created(*newcomm,&creation_key);end(active,result);return result;}
static int peak_mpi_Comm_split_type(MPI_Comm comm,int type,int key,MPI_Info info,MPI_Comm *newcomm)
{int active=begin();CreationIdentity creation_key=creation_identity(active,comm,4);PeakMpiBudgetEvent e=creation_enter(active,comm);native_start(active);int result=call_Comm_split_type(comm,type,key,info,newcomm);native_stop(active);if(active && result==MPI_SUCCESS)created(*newcomm,&creation_key);creation_exit(active,result,e);end(active,result);return result;}
static int peak_mpi_Comm_free(MPI_Comm *comm)
{
    int active=begin();MPI_Comm old=comm?*comm:MPI_COMM_NULL;PeakMpiBudgetEvent e=creation_enter(active,old);native_start(active);int result=call_Comm_free(comm);native_stop(active);
    creation_exit(active,result,e);
    if(active && result==MPI_SUCCESS){pthread_mutex_lock(&lock);Context **p=&contexts;while(*p && (*p)->handle!=old)p=&(*p)->next;
        if(*p){Context *c=*p;c->retired=1;retired_contexts++;}
        collect_contexts();
        pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Request_free(MPI_Request *request)
{
    int active=begin();MPI_Request old=request?*request:MPI_REQUEST_NULL;
    if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Request_free(request);native_stop(active);
    if(active && result==MPI_SUCCESS){pthread_mutex_lock(&lock);Request **p=&requests;
        while(*p && (*p)->handle!=old)p=&(*p)->next;
        if(*p){Request *r=*p;*p=r->next;r->next=recycled_requests;recycled_requests=r;}collect_contexts();pthread_mutex_unlock(&lock);}
    end(active,result);return result;
}
static int peak_mpi_Cancel(MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Cancel(request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Start(MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Start(request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Startall(int count,MPI_Request requests_[])
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Startall(count,requests_);native_stop(active);end(active,result);return result;}
/* Sendrecv is one native call but two immutable logical operations. */
static void paired_post(int active,MPI_Comm comm,int dest,int sendtag,int source,int recvtag,PeakMpiBudgetEvent events[2])
{
    if(!active)return;
    pthread_mutex_lock(&lock);
    events[0]=post(comm,PEAK_MPI_OP_SEND,dest,sendtag);
    events[1]=post(comm,PEAK_MPI_OP_RECV,source,recvtag);
    uint64_t ids[2];size_t count=0;
    for(size_t i=0;i<2;i++)if(events[i].request)ids[count++]=events[i].request;
    emit((PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_WAIT,.requests=ids,.count=count});
    pthread_mutex_unlock(&lock);
}
static void paired_complete(int active,int result,MPI_Comm comm,const MPI_Status *status,PeakMpiBudgetEvent events[2])
{
    if(!active || result!=MPI_SUCCESS)return;
    pthread_mutex_lock(&lock);
    if(events[0].request){events[0].kind=PEAK_MPI_EVENT_COMPLETE;emit(events[0]);}
    if(events[1].request){
        resolve_receive(&events[1],context(comm),status);
        events[1].kind=PEAK_MPI_EVENT_COMPLETE;emit(events[1]);
    }
    pthread_mutex_unlock(&lock);
}
static int peak_mpi_Buffer_detach(void *buffer_addr,int *size)
{
    int active=begin();
    if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}
    native_start(active);int result=call_Buffer_detach(buffer_addr,size);native_stop(active);
    end(active,result);return result;
}
static int peak_mpi_Sendrecv(const void *sendbuf,int sendcount,MPI_Datatype sendtype,int dest,int sendtag,void *recvbuf,int recvcount,MPI_Datatype recvtype,int source,int recvtag,MPI_Comm comm,MPI_Status *status)
{
    int active=begin();PeakMpiBudgetEvent events[2]={{0}};MPI_Status temporary;
    MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&temporary:status;
    paired_post(active,comm,dest,sendtag,source,recvtag,events);
    native_start(active);int result=call_Sendrecv(sendbuf,sendcount,sendtype,dest,sendtag,recvbuf,recvcount,recvtype,source,recvtag,comm,actual);native_stop(active);
    paired_complete(active,result,comm,actual,events);
    end(active,result);return result;
}
static int peak_mpi_Sendrecv_replace(void *buf,int count,MPI_Datatype datatype,int dest,int sendtag,int source,int recvtag,MPI_Comm comm,MPI_Status *status)
{
    int active=begin();PeakMpiBudgetEvent events[2]={{0}};MPI_Status temporary;
    MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&temporary:status;
    paired_post(active,comm,dest,sendtag,source,recvtag,events);
    native_start(active);int result=call_Sendrecv_replace(buf,count,datatype,dest,sendtag,source,recvtag,comm,actual);native_stop(active);
    paired_complete(active,result,comm,actual,events);
    end(active,result);return result;
}
static int peak_mpi_Win_create(void *base, MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_create(base,size,disp_unit,info,comm,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_allocate(MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_allocate(size,disp_unit,info,comm,baseptr,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_allocate_shared(MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_allocate_shared(size,disp_unit,info,comm,baseptr,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_create_dynamic(MPI_Info info, MPI_Comm comm, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_create_dynamic(info,comm,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_free(MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_free(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_fence(int assert, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_fence(assert,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_lock(int lock_type, int rank, int assert, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_lock(lock_type,rank,assert,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_lock_all(int assert, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_lock_all(assert,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_unlock(int rank, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_unlock(rank,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_unlock_all(MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_unlock_all(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_flush(int rank, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_flush(rank,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_flush_all(MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_flush_all(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_flush_local(int rank, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_flush_local(rank,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_flush_local_all(MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_flush_local_all(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_sync(MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_sync(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_start(MPI_Group group, int assert, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_start(group,assert,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_post(MPI_Group group, int assert, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_post(group,assert,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_complete(MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_complete(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_wait(MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_wait(win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_test(MPI_Win win, int *flag)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_test(win,flag);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_attach(MPI_Win win, void *base, MPI_Aint size)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_attach(win,base,size);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_detach(MPI_Win win, const void *base)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_detach(win,base);native_stop(active);end(active,result);return result;}
static int peak_mpi_Put(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Put(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Get(void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Get(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Accumulate(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Accumulate(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Get_accumulate(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, void *result_addr, int result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Get_accumulate(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Fetch_and_op(const void *origin_addr, void *result_addr, MPI_Datatype datatype, int target_rank, MPI_Aint target_disp, MPI_Op op, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Fetch_and_op(origin_addr,result_addr,datatype,target_rank,target_disp,op,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Compare_and_swap(const void *origin_addr, const void *compare_addr, void *result_addr, MPI_Datatype datatype, int target_rank, MPI_Aint target_disp, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Compare_and_swap(origin_addr,compare_addr,result_addr,datatype,target_rank,target_disp,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rput(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rput(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rget(void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rget(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Raccumulate(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Raccumulate(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rget_accumulate(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, void *result_addr, int result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rget_accumulate(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Send_init(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Send_init(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ssend_init(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ssend_init(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rsend_init(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rsend_init(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Bsend_init(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Bsend_init(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Recv_init(void *buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Recv_init(buf,count,datatype,source,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_idup(MPI_Comm comm, MPI_Comm *newcomm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_idup(comm,newcomm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Intercomm_create(MPI_Comm local_comm, int local_leader, MPI_Comm peer_comm, int remote_leader, int tag, MPI_Comm *newintercomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Intercomm_create(local_comm,local_leader,peer_comm,remote_leader,tag,newintercomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Intercomm_merge(MPI_Comm intercomm, int high, MPI_Comm *newintracomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Intercomm_merge(intercomm,high,newintracomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_spawn(const char *command, char *argv[], int maxprocs, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *intercomm, int array_of_errcodes[])
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_spawn(command,argv,maxprocs,info,root,comm,intercomm,array_of_errcodes);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_spawn_multiple(int count, char *array_of_commands[], char **array_of_argv[], const int array_of_maxprocs[], const MPI_Info array_of_info[], int root, MPI_Comm comm, MPI_Comm *intercomm, int array_of_errcodes[])
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_spawn_multiple(count,array_of_commands,array_of_argv,array_of_maxprocs,array_of_info,root,comm,intercomm,array_of_errcodes);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_connect(const char *port_name, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *newcomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_connect(port_name,info,root,comm,newcomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_accept(const char *port_name, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *newcomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_accept(port_name,info,root,comm,newcomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_join(int fd, MPI_Comm *intercomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_join(fd,intercomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Mprobe(int source, int tag, MPI_Comm comm, MPI_Message *message, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Mprobe(source,tag,comm,message,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Improbe(int source, int tag, MPI_Comm comm, int *flag, MPI_Message *message, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Improbe(source,tag,comm,flag,message,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Mrecv(void *buf, int count, MPI_Datatype datatype, MPI_Message *message, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Mrecv(buf,count,datatype,message,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Imrecv(void *buf, int count, MPI_Datatype datatype, MPI_Message *message, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Imrecv(buf,count,datatype,message,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgather(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgatherv(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoall(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallv(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallv(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallw(const void *sendbuf, const int sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallw(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_allgather(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_allgatherv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_allgatherv(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_alltoall(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_alltoallv(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_alltoallv(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_alltoallw(const void *sendbuf, const int sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_alltoallw(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Cart_create(MPI_Comm comm_old, int ndims, const int dims[], const int periods[], int reorder, MPI_Comm *comm_cart)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Cart_create(comm_old,ndims,dims,periods,reorder,comm_cart);native_stop(active);end(active,result);return result;}
static int peak_mpi_Cart_sub(MPI_Comm comm, const int remain_dims[], MPI_Comm *newcomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Cart_sub(comm,remain_dims,newcomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Graph_create(MPI_Comm comm_old, int nnodes, const int indx[], const int edges[], int reorder, MPI_Comm *comm_graph)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Graph_create(comm_old,nnodes,indx,edges,reorder,comm_graph);native_stop(active);end(active,result);return result;}
static int peak_mpi_Dist_graph_create(MPI_Comm comm_old, int n, const int sources[], const int degrees[], const int destinations[], const int weights[], MPI_Info info, int reorder, MPI_Comm *comm_dist_graph)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Dist_graph_create(comm_old,n,sources,degrees,destinations,weights,info,reorder,comm_dist_graph);native_stop(active);end(active,result);return result;}
static int peak_mpi_Dist_graph_create_adjacent(MPI_Comm comm_old, int indegree, const int sources[], const int sourceweights[], int outdegree, const int destinations[], const int destweights[], MPI_Info info, int reorder, MPI_Comm *comm_dist_graph)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Dist_graph_create_adjacent(comm_old,indegree,sources,sourceweights,outdegree,destinations,destweights,info,reorder,comm_dist_graph);native_stop(active);end(active,result);return result;}

#if MPI_VERSION >= 3
static int peak_mpi_Comm_dup_with_info(MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_dup_with_info(comm,info,newcomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Request_get_status(MPI_Request request, int *flag, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Request_get_status(request,flag,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Probe(int source, int tag, MPI_Comm comm, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Probe(source,tag,comm,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iprobe(int source, int tag, MPI_Comm comm, int *flag, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iprobe(source,tag,comm,flag,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_shared_query(MPI_Win win, int rank, MPI_Aint *size, int *disp_unit, void *baseptr)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_shared_query(win,rank,size,disp_unit,baseptr);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_disconnect(MPI_Comm *comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_disconnect(comm);native_stop(active);end(active,result);return result;}
#endif
#if MPI_VERSION >= 4
static int peak_mpi_Allgather_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allgather_init(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allgatherv_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allgatherv_init(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allreduce_init(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allreduce_init(sendbuf,recvbuf,count,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoall_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoall_init(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoallv_init(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoallv_init(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoallw_init(const void *sendbuf, const int sendcounts[], const int sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const int rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoallw_init(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Barrier_init(MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Barrier_init(comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Bcast_init(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Bcast_init(buffer,count,datatype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Exscan_init(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Exscan_init(sendbuf,recvbuf,count,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Gather_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Gather_init(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Gatherv_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Gatherv_init(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgather_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgather_init(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgatherv_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgatherv_init(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoall_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoall_init(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallv_init(const void *sendbuf, const int sendcounts[], const int sdispls[], MPI_Datatype sendtype, void *recvbuf, const int recvcounts[], const int rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallv_init(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallw_init(const void *sendbuf, const int sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const int recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallw_init(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_init(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_init(sendbuf,recvbuf,count,datatype,op,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_scatter_block_init(const void *sendbuf, void *recvbuf, int recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_scatter_block_init(sendbuf,recvbuf,recvcount,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_scatter_init(const void *sendbuf, void *recvbuf, const int recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_scatter_init(sendbuf,recvbuf,recvcounts,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scan_init(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scan_init(sendbuf,recvbuf,count,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scatter_init(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scatter_init(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scatterv_init(const void *sendbuf, const int sendcounts[], const int displs[], MPI_Datatype sendtype, void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scatterv_init(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Session_init(MPI_Info info, MPI_Errhandler errhandler, MPI_Session *session)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Session_init(info,errhandler,session);native_stop(active);end(active,result);return result;}
static int peak_mpi_Precv_init(void *buf, int partitions, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Precv_init(buf,partitions,count,datatype,dest,tag,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Psend_init(PEAK_MPI_PSEND_BUFFER_TYPE *buf, int partitions, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Psend_init(buf,partitions,count,datatype,dest,tag,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allgather_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allgather_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allgather_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allgather_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allgatherv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allgatherv_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allgatherv_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allgatherv_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allreduce_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allreduce_c(sendbuf,recvbuf,count,datatype,op,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Allreduce_init_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Allreduce_init_c(sendbuf,recvbuf,count,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoall_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoall_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoall_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoall_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoallv_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoallv_c(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoallv_init_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoallv_init_c(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoallw_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoallw_c(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Alltoallw_init_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Alltoallw_init_c(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Bcast_c(void *buffer, MPI_Count count, MPI_Datatype datatype, int root, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Bcast_c(buffer,count,datatype,root,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Bcast_init_c(void *buffer, MPI_Count count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Bcast_init_c(buffer,count,datatype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Exscan_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Exscan_c(sendbuf,recvbuf,count,datatype,op,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Exscan_init_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Exscan_init_c(sendbuf,recvbuf,count,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Gather_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Gather_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Gather_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Gather_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Gatherv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, int root, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Gatherv_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Gatherv_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Gatherv_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iallgather_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iallgather_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iallgatherv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iallgatherv_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iallreduce_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iallreduce_c(sendbuf,recvbuf,count,datatype,op,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ialltoall_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ialltoall_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ialltoallv_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ialltoallv_c(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ialltoallw_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ialltoallw_c(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ibcast_c(void *buffer, MPI_Count count, MPI_Datatype datatype, int root, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ibcast_c(buffer,count,datatype,root,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iexscan_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iexscan_c(sendbuf,recvbuf,count,datatype,op,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Igather_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Igather_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Igatherv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Igatherv_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,root,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_allgather_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_allgather_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_allgatherv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_allgatherv_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_alltoall_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_alltoall_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_alltoallv_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_alltoallv_c(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ineighbor_alltoallw_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ineighbor_alltoallw_c(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ireduce_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ireduce_c(sendbuf,recvbuf,count,datatype,op,root,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ireduce_scatter_c(const void *sendbuf, void *recvbuf, const MPI_Count recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ireduce_scatter_c(sendbuf,recvbuf,recvcounts,datatype,op,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ireduce_scatter_block_c(const void *sendbuf, void *recvbuf, MPI_Count recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ireduce_scatter_block_c(sendbuf,recvbuf,recvcount,datatype,op,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iscan_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iscan_c(sendbuf,recvbuf,count,datatype,op,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iscatter_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iscatter_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Iscatterv_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint displs[], MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Iscatterv_c(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgather_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgather_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgather_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgather_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgatherv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgatherv_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_allgatherv_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint displs[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_allgatherv_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcounts,displs,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoall_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoall_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoall_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoall_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallv_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallv_c(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallv_init_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], MPI_Datatype sendtype, void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], MPI_Datatype recvtype, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallv_init_c(sendbuf,sendcounts,sdispls,sendtype,recvbuf,recvcounts,rdispls,recvtype,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallw_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallw_c(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Neighbor_alltoallw_init_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint sdispls[], const MPI_Datatype sendtypes[], void *recvbuf, const MPI_Count recvcounts[], const MPI_Aint rdispls[], const MPI_Datatype recvtypes[], MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Neighbor_alltoallw_init_c(sendbuf,sendcounts,sdispls,sendtypes,recvbuf,recvcounts,rdispls,recvtypes,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_c(sendbuf,recvbuf,count,datatype,op,root,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_init_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_init_c(sendbuf,recvbuf,count,datatype,op,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_local_c(const void *inbuf, void *inoutbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_local_c(inbuf,inoutbuf,count,datatype,op);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_scatter_c(const void *sendbuf, void *recvbuf, const MPI_Count recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_scatter_c(sendbuf,recvbuf,recvcounts,datatype,op,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_scatter_block_c(const void *sendbuf, void *recvbuf, MPI_Count recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_scatter_block_c(sendbuf,recvbuf,recvcount,datatype,op,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_scatter_block_init_c(const void *sendbuf, void *recvbuf, MPI_Count recvcount, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_scatter_block_init_c(sendbuf,recvbuf,recvcount,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Reduce_scatter_init_c(const void *sendbuf, void *recvbuf, const MPI_Count recvcounts[], MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Reduce_scatter_init_c(sendbuf,recvbuf,recvcounts,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scan_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scan_c(sendbuf,recvbuf,count,datatype,op,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scan_init_c(const void *sendbuf, void *recvbuf, MPI_Count count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scan_init_c(sendbuf,recvbuf,count,datatype,op,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scatter_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scatter_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scatter_init_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scatter_init_c(sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scatterv_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint displs[], MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scatterv_c(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Scatterv_init_c(const void *sendbuf, const MPI_Count sendcounts[], const MPI_Aint displs[], MPI_Datatype sendtype, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Info info, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Scatterv_init_c(sendbuf,sendcounts,displs,sendtype,recvbuf,recvcount,recvtype,root,comm,info,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Get_count_c(const MPI_Status *status, MPI_Datatype datatype, MPI_Count *count)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Get_count_c(status,datatype,count);native_stop(active);end(active,result);return result;}
static int peak_mpi_Get_elements_c(const MPI_Status *status, MPI_Datatype datatype, MPI_Count *count)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Get_elements_c(status,datatype,count);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pack_c(const void *inbuf, MPI_Count incount, MPI_Datatype datatype, void *outbuf, MPI_Count outsize, MPI_Count *position, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pack_c(inbuf,incount,datatype,outbuf,outsize,position,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pack_external_c(const char *datarep, const void *inbuf, MPI_Count incount, MPI_Datatype datatype, void *outbuf, MPI_Count outsize, MPI_Count *position)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pack_external_c(datarep,inbuf,incount,datatype,outbuf,outsize,position);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pack_external_size_c(const char *datarep, MPI_Count incount, MPI_Datatype datatype, MPI_Count *size)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pack_external_size_c(datarep,incount,datatype,size);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pack_size_c(MPI_Count incount, MPI_Datatype datatype, MPI_Comm comm, MPI_Count *size)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pack_size_c(incount,datatype,comm,size);native_stop(active);end(active,result);return result;}
#if PEAK_MPI_HAVE_STATUS_SET_ELEMENTS_C
static int peak_mpi_Status_set_elements_c(MPI_Status *status, MPI_Datatype datatype, MPI_Count count)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Status_set_elements_c(status,datatype,count);native_stop(active);end(active,result);return result;}
#endif
static int peak_mpi_Type_contiguous_c(MPI_Count count, MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_contiguous_c(count,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_darray_c(int size, int rank, int ndims, const MPI_Count array_of_gsizes[], const int array_of_distribs[], const int array_of_dargs[], const int array_of_psizes[], int order, MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_darray_c(size,rank,ndims,array_of_gsizes,array_of_distribs,array_of_dargs,array_of_psizes,order,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_hindexed_c(MPI_Count count, const MPI_Count array_of_blocklengths[], const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_hindexed_c(count,array_of_blocklengths,array_of_displacements,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_hindexed_block_c(MPI_Count count, MPI_Count blocklength, const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_hindexed_block_c(count,blocklength,array_of_displacements,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_hvector_c(MPI_Count count, MPI_Count blocklength, MPI_Count stride, MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_hvector_c(count,blocklength,stride,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_indexed_block_c(MPI_Count count, MPI_Count blocklength, const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_indexed_block_c(count,blocklength,array_of_displacements,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_resized_c(MPI_Datatype oldtype, MPI_Count lb, MPI_Count extent, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_resized_c(oldtype,lb,extent,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_struct_c(MPI_Count count, const MPI_Count array_of_blocklengths[], const MPI_Count array_of_displacements[], const MPI_Datatype array_of_types[], MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_struct_c(count,array_of_blocklengths,array_of_displacements,array_of_types,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_create_subarray_c(int ndims, const MPI_Count array_of_sizes[], const MPI_Count array_of_subsizes[], const MPI_Count array_of_starts[], int order, MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_create_subarray_c(ndims,array_of_sizes,array_of_subsizes,array_of_starts,order,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_get_contents_c(MPI_Datatype datatype, MPI_Count max_integers, MPI_Count max_addresses, MPI_Count max_large_counts, MPI_Count max_datatypes, int array_of_integers[], MPI_Aint array_of_addresses[], MPI_Count array_of_large_counts[], MPI_Datatype array_of_datatypes[])
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_get_contents_c(datatype,max_integers,max_addresses,max_large_counts,max_datatypes,array_of_integers,array_of_addresses,array_of_large_counts,array_of_datatypes);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_get_envelope_c(MPI_Datatype datatype, MPI_Count *num_integers, MPI_Count *num_addresses, MPI_Count *num_large_counts, MPI_Count *num_datatypes, int *combiner)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_get_envelope_c(datatype,num_integers,num_addresses,num_large_counts,num_datatypes,combiner);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_get_extent_c(MPI_Datatype datatype, MPI_Count *lb, MPI_Count *extent)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_get_extent_c(datatype,lb,extent);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_get_true_extent_c(MPI_Datatype datatype, MPI_Count *true_lb, MPI_Count *true_extent)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_get_true_extent_c(datatype,true_lb,true_extent);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_indexed_c(MPI_Count count, const MPI_Count array_of_blocklengths[], const MPI_Count array_of_displacements[], MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_indexed_c(count,array_of_blocklengths,array_of_displacements,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_size_c(MPI_Datatype datatype, MPI_Count *size)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_size_c(datatype,size);native_stop(active);end(active,result);return result;}
static int peak_mpi_Type_vector_c(MPI_Count count, MPI_Count blocklength, MPI_Count stride, MPI_Datatype oldtype, MPI_Datatype *newtype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Type_vector_c(count,blocklength,stride,oldtype,newtype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Unpack_c(const void *inbuf, MPI_Count insize, MPI_Count *position, void *outbuf, MPI_Count outcount, MPI_Datatype datatype, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Unpack_c(inbuf,insize,position,outbuf,outcount,datatype,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Unpack_external_c(const char datarep[], const void *inbuf, MPI_Count insize, MPI_Count *position, void *outbuf, MPI_Count outcount, MPI_Datatype datatype)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Unpack_external_c(datarep,inbuf,insize,position,outbuf,outcount,datatype);native_stop(active);end(active,result);return result;}
static int peak_mpi_Op_create_c(MPI_User_function_c *user_fn, int commute, MPI_Op *op)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Op_create_c(user_fn,commute,op);native_stop(active);end(active,result);return result;}
static int peak_mpi_Bsend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Bsend_c(buf,count,datatype,dest,tag,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Bsend_init_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Bsend_init_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Buffer_attach_c(void *buffer, MPI_Count size)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Buffer_attach_c(buffer,size);native_stop(active);end(active,result);return result;}
static int peak_mpi_Buffer_detach_c(void *buffer_addr, MPI_Count *size)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Buffer_detach_c(buffer_addr,size);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ibsend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ibsend_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Imrecv_c(void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Message *message, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Imrecv_c(buf,count,datatype,message,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Irecv_c(void *buf, MPI_Count count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Irecv_c(buf,count,datatype,source,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Irsend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Irsend_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Isend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Isend_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Isendrecv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Isendrecv_c(sendbuf,sendcount,sendtype,dest,sendtag,recvbuf,recvcount,recvtype,source,recvtag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Isendrecv_replace_c(void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int sendtag, int source, int recvtag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Isendrecv_replace_c(buf,count,datatype,dest,sendtag,source,recvtag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Issend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Issend_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Mrecv_c(void *buf, MPI_Count count, MPI_Datatype datatype, MPI_Message *message, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Mrecv_c(buf,count,datatype,message,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Recv_c(void *buf, MPI_Count count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Status *status)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Recv_c(buf,count,datatype,source,tag,comm,status);native_stop(active);end(active,result);return result;}
static int peak_mpi_Recv_init_c(void *buf, MPI_Count count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Recv_init_c(buf,count,datatype,source,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rsend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rsend_c(buf,count,datatype,dest,tag,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rsend_init_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rsend_init_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Send_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Send_c(buf,count,datatype,dest,tag,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Send_init_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Send_init_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Sendrecv_c(const void *sendbuf, MPI_Count sendcount, MPI_Datatype sendtype, int dest, int sendtag, void *recvbuf, MPI_Count recvcount, MPI_Datatype recvtype, int source, int recvtag, MPI_Comm comm, MPI_Status *status)
{
    int active=begin();PeakMpiBudgetEvent events[2]={{0}};MPI_Status temporary;
    MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&temporary:status;
    paired_post(active,comm,dest,sendtag,source,recvtag,events);
    native_start(active);int result=call_Sendrecv_c(sendbuf,sendcount,sendtype,dest,sendtag,recvbuf,recvcount,recvtype,source,recvtag,comm,actual);native_stop(active);
    paired_complete(active,result,comm,actual,events);
    end(active,result);return result;
}
static int peak_mpi_Sendrecv_replace_c(void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int sendtag, int source, int recvtag, MPI_Comm comm, MPI_Status *status)
{
    int active=begin();PeakMpiBudgetEvent events[2]={{0}};MPI_Status temporary;
    MPI_Status *actual=active && status==MPI_STATUS_IGNORE?&temporary:status;
    paired_post(active,comm,dest,sendtag,source,recvtag,events);
    native_start(active);int result=call_Sendrecv_replace_c(buf,count,datatype,dest,sendtag,source,recvtag,comm,actual);native_stop(active);
    paired_complete(active,result,comm,actual,events);
    end(active,result);return result;
}
static int peak_mpi_Ssend_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ssend_c(buf,count,datatype,dest,tag,comm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Ssend_init_c(const void *buf, MPI_Count count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Ssend_init_c(buf,count,datatype,dest,tag,comm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Accumulate_c(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Accumulate_c(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Get_c(void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Get_c(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Get_accumulate_c(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, void *result_addr, MPI_Count result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Get_accumulate_c(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Put_c(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Put_c(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Raccumulate_c(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Raccumulate_c(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rget_c(void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rget_c(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rget_accumulate_c(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, void *result_addr, MPI_Count result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rget_accumulate_c(origin_addr,origin_count,origin_datatype,result_addr,result_count,result_datatype,target_rank,target_disp,target_count,target_datatype,op,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Rput_c(const void *origin_addr, MPI_Count origin_count, MPI_Datatype origin_datatype, int target_rank, MPI_Aint target_disp, MPI_Count target_count, MPI_Datatype target_datatype, MPI_Win win, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Rput_c(origin_addr,origin_count,origin_datatype,target_rank,target_disp,target_count,target_datatype,win,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_allocate_c(MPI_Aint size, MPI_Aint disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_allocate_c(size,disp_unit,info,comm,baseptr,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_allocate_shared_c(MPI_Aint size, MPI_Aint disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_allocate_shared_c(size,disp_unit,info,comm,baseptr,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_create_c(void *base, MPI_Aint size, MPI_Aint disp_unit, MPI_Info info, MPI_Comm comm, MPI_Win *win)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_create_c(base,size,disp_unit,info,comm,win);native_stop(active);end(active,result);return result;}
static int peak_mpi_Win_shared_query_c(MPI_Win win, int rank, MPI_Aint *size, MPI_Aint *disp_unit, void *baseptr)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Win_shared_query_c(win,rank,size,disp_unit,baseptr);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_idup_with_info(MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm, MPI_Request *request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_idup_with_info(comm,info,newcomm,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Comm_create_from_group(MPI_Group group, const char *stringtag, MPI_Info info, MPI_Errhandler errhandler, MPI_Comm *newcomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Comm_create_from_group(group,stringtag,info,errhandler,newcomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Intercomm_create_from_groups(MPI_Group local_group, int local_leader, MPI_Group remote_group, int remote_leader, const char *stringtag, MPI_Info info, MPI_Errhandler errhandler, MPI_Comm *newintercomm)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Intercomm_create_from_groups(local_group,local_leader,remote_group,remote_leader,stringtag,info,errhandler,newintercomm);native_stop(active);end(active,result);return result;}
static int peak_mpi_Session_finalize(MPI_Session *session)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Session_finalize(session);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pready(int partition, MPI_Request request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pready(partition,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pready_range(int partition_low, int partition_high, MPI_Request request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pready_range(partition_low,partition_high,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Pready_list(int length, PEAK_MPI_PREADY_PARTITION_TYPE array_of_partitions[], MPI_Request request)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Pready_list(length,array_of_partitions,request);native_stop(active);end(active,result);return result;}
static int peak_mpi_Parrived(MPI_Request request, int partition, int *flag)
{int active=begin();if(active){pthread_mutex_lock(&lock);unknown_live();pthread_mutex_unlock(&lock);}native_start(active);int result=call_Parrived(request,partition,flag);native_stop(active);end(active,result);return result;}
#endif
#define PEAK_UNSUPPORTED_CLASS_File_open unknown
#define PEAK_UNSUPPORTED_CLASS_File_close unknown
#define PEAK_UNSUPPORTED_CLASS_File_delete unknown
#define PEAK_UNSUPPORTED_CLASS_File_set_size unknown
#define PEAK_UNSUPPORTED_CLASS_File_preallocate unknown
#define PEAK_UNSUPPORTED_CLASS_File_set_info unknown
#define PEAK_UNSUPPORTED_CLASS_File_set_view unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_at unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_at_all unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_at unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_at_all unknown
#define PEAK_UNSUPPORTED_CLASS_File_iread_at unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_at unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_all unknown
#define PEAK_UNSUPPORTED_CLASS_File_write unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_all unknown
#define PEAK_UNSUPPORTED_CLASS_File_iread unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_shared unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_shared unknown
#define PEAK_UNSUPPORTED_CLASS_File_iread_shared unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_shared unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_ordered unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_ordered unknown
#define PEAK_UNSUPPORTED_CLASS_File_seek_shared unknown
#define PEAK_UNSUPPORTED_CLASS_File_get_position_shared unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_at_all_begin unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_at_all_end unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_at_all_begin unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_at_all_end unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_all_begin unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_all_end unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_all_begin unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_all_end unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_ordered_begin unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_ordered_end unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_ordered_begin unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_ordered_end unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_set_atomicity unknown
#define PEAK_UNSUPPORTED_CLASS_File_sync unknown
#define PEAK_UNSUPPORTED_CLASS_Grequest_start unknown_live
#define PEAK_UNSUPPORTED_CLASS_Grequest_complete unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iread_at_all unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_at_all unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iread_all unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_all unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_all_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_all_begin_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_at_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_at_all_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_at_all_begin_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_ordered_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_read_ordered_begin_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_read_shared_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_all_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_all_begin_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_at_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_at_all_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_at_all_begin_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_ordered_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_write_ordered_begin_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_write_shared_c unknown
#define PEAK_UNSUPPORTED_CLASS_File_iread_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iread_all_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iread_at_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iread_at_all_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iread_shared_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_all_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_at_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_at_all_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_File_iwrite_shared_c unknown_live
#define PEAK_UNSUPPORTED_CLASS_Comm_set_info unknown
/* Unsupported MPI-IO synchronization and shared-position operations must
 * seal a SUM epoch; their arguments/status/results still pass through intact. */
#define PEAK_UNSUPPORTED(N, DECL, ARGS) \
    static int peak_mpi_##N DECL { \
        int active=begin(); \
        if(active){pthread_mutex_lock(&lock);PEAK_UNSUPPORTED_CLASS_##N();pthread_mutex_unlock(&lock);} \
        native_start(active);int result=call_##N ARGS;native_stop(active); \
        end(active,result);return result; \
    }
#if MPI_VERSION >= 2
PEAK_MPI_IO_APIS_2(PEAK_UNSUPPORTED)
#endif
#if MPI_VERSION > 3 || (MPI_VERSION == 3 && MPI_SUBVERSION >= 1)
PEAK_MPI_IO_APIS_31(PEAK_UNSUPPORTED)
#endif
#if MPI_VERSION >= 4
PEAK_MPI_IO_APIS_4(PEAK_UNSUPPORTED)
#endif
#if MPI_VERSION >= 3
PEAK_MPI_IO_APIS_3(PEAK_UNSUPPORTED)
#endif
#undef PEAK_UNSUPPORTED
/* Both public entry points share one guarded body; the actual native PMPI
 * implementation is resolved after this object's symbols with RTLD_NEXT. */
#define PEAK_ALIAS(N, DECL, ARGS) \
    EXPORT int MPI_##N DECL { return peak_mpi_##N ARGS; } \
    EXPORT int PMPI_##N DECL { return peak_mpi_##N ARGS; }
#if MPI_VERSION >= 4 && PEAK_MPI_HAVE_STATUS_SET_ELEMENTS_C
PEAK_MPI_OPTIONAL_STATUS_API(PEAK_ALIAS)
#endif
PEAK_MPI_APIS_3(PEAK_ALIAS)
#if MPI_VERSION >= 4
PEAK_MPI_APIS_4(PEAK_ALIAS)
#endif
#if MPI_VERSION >= 2
PEAK_MPI_IO_APIS_2(PEAK_ALIAS)
#undef PEAK_MPI_IO_APIS_2
#endif
#if MPI_VERSION > 3 || (MPI_VERSION == 3 && MPI_SUBVERSION >= 1)
PEAK_MPI_IO_APIS_31(PEAK_ALIAS)
#undef PEAK_MPI_IO_APIS_31
#endif
#if MPI_VERSION >= 4
PEAK_MPI_IO_APIS_4(PEAK_ALIAS)
#undef PEAK_MPI_IO_APIS_4
#endif
#if MPI_VERSION >= 3
PEAK_MPI_IO_APIS_3(PEAK_ALIAS)
#undef PEAK_MPI_IO_APIS_3
#endif
#undef PEAK_ALIAS
#undef PEAK_MPI_OPTIONAL_STATUS_API
#undef PEAK_MPI_APIS_3
#undef PEAK_MPI_APIS_4

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__))
ptrdiff_t peak_mpi_application_active_offset(void) { return peak_native_tls_offset(&application_active); }
#endif
