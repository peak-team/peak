#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <thread>
#include <time.h>
#if defined(__linux__)
#include <sched.h>
#endif

#include "cuda_interceptor.h"
#include "internal/cuda_profiler_state.h"
#include "internal/general_listener/report_snapshot.h"
#include "internal/general_listener/socket_report_transport.h"
#include "internal/unsafe_gum_prologue.h"
#ifdef HAVE_MPI
#include "internal/general_listener/mpi_report_transport.h"
#endif
#include "logging.h"
#include "utils/env_parser.h"

#define PEAK_CUDA_WRAPPER_EXPORT extern "C" __attribute__((visibility("default")))

#if defined(PEAK_CUDA_COMPILE_RUNTIME_LAUNCH_EX) && \
    defined(CUDART_VERSION) && CUDART_VERSION >= 11080
#define PEAK_CUDA_RUNTIME_LAUNCH_EX 1
#endif
#if defined(PEAK_CUDA_COMPILE_DRIVER_LAUNCH_EX) && \
    defined(CUDA_VERSION) && CUDA_VERSION >= 11080
#define PEAK_CUDA_DRIVER_LAUNCH_EX 1
#endif

extern "C" gpointer peak_general_listener_find_function(const char* symbol);
extern "C" void peak_general_listener_exclude_current_thread(void);
extern "C" void peak_general_listener_fast_ignore_current_thread(void);
extern "C" void peak_general_listener_fast_unignore_current_thread(void);
extern "C" void pthread_listener_mark_next_created_thread_helper(void);

enum PeakCudaOutputAggregationMode {
    PEAK_CUDA_OUTPUT_AGGREGATION_LOCAL = 0,
    PEAK_CUDA_OUTPUT_AGGREGATION_MPI = 1,
    PEAK_CUDA_OUTPUT_AGGREGATION_SOCKET = 2,
};

static GHashTable* cuda_kernel_local_dim_mapping;
static GHashTable* cuda_graph_local_mapping;
static GMutex cuda_kernel_local_dim_mapping_mutex;
static GMutex cuda_graph_local_mapping_mutex;
static GumInterceptor* cuda_interceptor;
static gpointer* hook_cuda_launch;
static gpointer* hook_cuda_launch_cooperative;
static gpointer* hook_cuda_launch_cooperative_multiple_device;
static gpointer* hook_cuda_launch_exc;
static gpointer* hook_cu_launch;
static gpointer* hook_cu_launch_cooperative;
static gpointer* hook_cu_launch_cooperative_multiple_device;
static gpointer* hook_cu_launch_ex;
static gpointer* hook_cuda_graph_launch;
static gpointer* hook_cu_graph_launch;
enum PeakCudaCaptureHookIndex {
    PEAK_CUDA_HOOK_RUNTIME_BEGIN_CAPTURE = 0,
    PEAK_CUDA_HOOK_RUNTIME_BEGIN_CAPTURE_PTSZ,
    PEAK_CUDA_HOOK_RUNTIME_BEGIN_RECAPTURE_TO_GRAPH,
    PEAK_CUDA_HOOK_RUNTIME_BEGIN_RECAPTURE_TO_GRAPH_PTSZ,
    PEAK_CUDA_HOOK_RUNTIME_BEGIN_CAPTURE_TO_GRAPH,
    PEAK_CUDA_HOOK_RUNTIME_BEGIN_CAPTURE_TO_GRAPH_PTSZ,
    PEAK_CUDA_HOOK_RUNTIME_END_CAPTURE,
    PEAK_CUDA_HOOK_RUNTIME_END_CAPTURE_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_V1,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_V1_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_V2,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_V2_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_RECAPTURE_TO_GRAPH,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_RECAPTURE_TO_GRAPH_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_TO_GRAPH,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_TO_GRAPH_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_END_CAPTURE,
    PEAK_CUDA_HOOK_DRIVER_END_CAPTURE_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_TO_CIG,
    PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_TO_CIG_PTSZ,
    PEAK_CUDA_HOOK_DRIVER_END_CAPTURE_TO_CIG,
    PEAK_CUDA_HOOK_DRIVER_END_CAPTURE_TO_CIG_PTSZ,
    PEAK_CUDA_CAPTURE_HOOK_COUNT,
};
static gpointer* peak_cuda_capture_hooks[PEAK_CUDA_CAPTURE_HOOK_COUNT];
static GumInvocationListener*
    peak_cuda_capture_listeners[PEAK_CUDA_CAPTURE_HOOK_COUNT];
static gboolean peak_cuda_capture_hooks_ready;
extern size_t peak_gpu_hook_address_count;
extern char** peak_gpu_hook_strings;
extern gboolean peak_gpu_monitor_all;

static cudaError_t (*original_cuda_launch_kernel)(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream);

static cudaError_t (*original_cuda_launch_cooperative_kernel)(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream);

static cudaError_t (*original_cuda_launch_cooperative_kernel_multiple_device)(
    struct cudaLaunchParams* launchParamsList, unsigned int numDevices,
    unsigned int flags);

#if defined(PEAK_CUDA_RUNTIME_LAUNCH_EX)
static cudaError_t (*original_cuda_launch_kernel_exc)(
    const cudaLaunchConfig_t* config,
    const void* func, void** args);
#endif

static CUresult (*original_cu_launch_kernel)(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra);

static CUresult (*original_cu_launch_cooperative_kernel)(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams);

static CUresult (*original_cu_launch_cooperative_kernel_multiple_device)(
    CUDA_LAUNCH_PARAMS* launchParamsList,
    unsigned int numDevices, unsigned int flags);

#if defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
static CUresult (*original_cu_launch_kernel_ex)(
    const CUlaunchConfig* config, CUfunction func,
    void** kernelParams, void** extra);
#endif

static cudaError_t (*original_cuda_graph_launch)(
    cudaGraphExec_t graphExec, cudaStream_t stream);

static CUresult (*original_cu_graph_launch)(
    CUgraphExec hGraphExec, CUstream hStream);

/* cuFuncGetName was added after this project's minimum CUDA 11.2 support. */
typedef CUresult (*PeakCudaFuncGetNameFn)(const char** name,
                                          CUfunction function);
static PeakCudaFuncGetNameFn peak_cuda_func_get_name;
static PeakEnvWarningState peak_cuda_event_pool_capacity_warning_emitted;
static PeakEnvWarningState peak_cuda_finalization_timeout_warning_emitted;

typedef CUresult (*PeakCuCtxGetCurrentFn)(CUcontext* context);
typedef CUresult (*PeakCuCtxGetDeviceFn)(CUdevice* device);
typedef CUresult (*PeakCuCtxPushCurrentFn)(CUcontext context);
typedef CUresult (*PeakCuCtxPopCurrentFn)(CUcontext* context);
typedef CUresult (*PeakCuEventCreateFn)(CUevent* event, unsigned int flags);
typedef CUresult (*PeakCuEventDestroyFn)(CUevent event);
typedef CUresult (*PeakCuEventRecordFn)(CUevent event, CUstream stream);
typedef CUresult (*PeakCuEventQueryFn)(CUevent event);
typedef CUresult (*PeakCuEventElapsedTimeFn)(float* milliseconds,
                                             CUevent start,
                                             CUevent end);
typedef CUresult (*PeakCuStreamIsCapturingFn)(
    CUstream stream, CUstreamCaptureStatus* status);
typedef CUresult (*PeakCuThreadExchangeStreamCaptureModeFn)(
    CUstreamCaptureMode* mode);
typedef CUresult (*PeakCuGetProcAddressFn)(
    const char* symbol, void** function, int version,
    unsigned long long flags);
typedef CUresult (*PeakCuGetProcAddressV2Fn)(
    const char* symbol, void** function, int version,
    unsigned long long flags, int* symbol_status);

struct PeakCudaBackendApi {
    void* driver_handle;
    PeakCuCtxGetCurrentFn ctx_get_current;
    PeakCuCtxGetDeviceFn ctx_get_device;
    PeakCuCtxPushCurrentFn ctx_push_current;
    PeakCuCtxPopCurrentFn ctx_pop_current;
    PeakCuEventCreateFn event_create;
    PeakCuEventDestroyFn event_destroy;
    PeakCuEventRecordFn event_record;
    PeakCuEventQueryFn event_query;
    PeakCuEventElapsedTimeFn event_elapsed_time;
    PeakCuStreamIsCapturingFn driver_stream_is_capturing;
    PeakCuThreadExchangeStreamCaptureModeFn
        driver_thread_exchange_stream_capture_mode;
};

static PeakCudaBackendApi peak_cuda_backend_api;
static PeakCudaCapabilities peak_cuda_capabilities;
#ifdef PEAK_ENABLE_TEST_HOOKS
static std::atomic<std::uint64_t> peak_cuda_test_attach_calls{0};

extern "C" unsigned long long
peak_cuda_test_attach_call_count(void)
{
    return peak_cuda_test_attach_calls.load(std::memory_order_acquire);
}
#endif

extern "C" uint32_t
cuda_interceptor_compiled_api_mask(void)
{
    uint32_t mask =
        PEAK_CUDA_API_RUNTIME_LAUNCH |
        PEAK_CUDA_API_RUNTIME_COOPERATIVE |
        PEAK_CUDA_API_RUNTIME_MULTI_DEVICE |
        PEAK_CUDA_API_RUNTIME_GRAPH |
        PEAK_CUDA_API_DRIVER_LAUNCH |
        PEAK_CUDA_API_DRIVER_COOPERATIVE |
        PEAK_CUDA_API_DRIVER_MULTI_DEVICE |
        PEAK_CUDA_API_DRIVER_GRAPH |
        PEAK_CUDA_API_RUNTIME_CAPTURE |
        PEAK_CUDA_API_DRIVER_CAPTURE |
        PEAK_CUDA_API_DRIVER_TIMING;
#if defined(PEAK_CUDA_RUNTIME_LAUNCH_EX)
    mask |= PEAK_CUDA_API_RUNTIME_LAUNCH_EX;
#endif
#if defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
    mask |= PEAK_CUDA_API_DRIVER_LAUNCH_EX;
#endif
    return mask;
}

extern "C" PeakCudaCapabilities
cuda_interceptor_get_capabilities(void)
{
    return peak_cuda_capabilities;
}

enum PeakCudaCaptureApi {
    PEAK_CUDA_CAPTURE_API_RUNTIME = 0,
    PEAK_CUDA_CAPTURE_API_DRIVER,
};

enum PeakCudaCaptureOperation {
    PEAK_CUDA_CAPTURE_OPERATION_BEGIN = 0,
    PEAK_CUDA_CAPTURE_OPERATION_END,
};

struct PeakCudaCaptureHookDescriptor {
    const char* symbol;
    PeakCudaCaptureApi api;
    PeakCudaCaptureOperation operation;
};

static constexpr size_t kPeakCudaDirectCaptureHookCapacity = 42;
static gpointer peak_cuda_direct_capture_hooks[
    kPeakCudaDirectCaptureHookCapacity];
static GumInvocationListener* peak_cuda_direct_capture_listeners[
    kPeakCudaDirectCaptureHookCapacity];
static PeakCudaCaptureHookDescriptor peak_cuda_direct_capture_descriptors[
    kPeakCudaDirectCaptureHookCapacity];
static size_t peak_cuda_direct_capture_hook_count;

static const PeakCudaCaptureHookDescriptor peak_cuda_capture_descriptors[] = {
    {"cudaStreamBeginCapture", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cudaStreamBeginCapture_ptsz", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cudaStreamBeginRecaptureToGraph", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cudaStreamBeginRecaptureToGraph_ptsz", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cudaStreamBeginCaptureToGraph", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cudaStreamBeginCaptureToGraph_ptsz", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cudaStreamEndCapture", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_END},
    {"cudaStreamEndCapture_ptsz", PEAK_CUDA_CAPTURE_API_RUNTIME,
     PEAK_CUDA_CAPTURE_OPERATION_END},
    {"cuStreamBeginCapture", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginCapture_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginCapture_v2", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginCapture_v2_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginRecaptureToGraph", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginRecaptureToGraph_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginCaptureToGraph", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginCaptureToGraph_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamEndCapture", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_END},
    {"cuStreamEndCapture_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_END},
    {"cuStreamBeginCaptureToCig", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamBeginCaptureToCig_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_BEGIN},
    {"cuStreamEndCaptureToCig", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_END},
    {"cuStreamEndCaptureToCig_ptsz", PEAK_CUDA_CAPTURE_API_DRIVER,
     PEAK_CUDA_CAPTURE_OPERATION_END},
};

static_assert(sizeof(peak_cuda_capture_descriptors) /
                  sizeof(peak_cuda_capture_descriptors[0]) ==
              PEAK_CUDA_CAPTURE_HOOK_COUNT,
              "CUDA capture hook descriptor table is incomplete");

enum PeakCudaLaunchRecordKind {
    PEAK_CUDA_LAUNCH_RECORD_NONE = 0,
    PEAK_CUDA_LAUNCH_RECORD_KERNEL,
    PEAK_CUDA_LAUNCH_RECORD_GRAPH,
};

struct PeakCudaLaunchRecord {
    PeakCudaLaunchRecordKind kind;
    char kernel_name[256];
    std::uint64_t total_threads;
    std::uint64_t grid_size;
    std::uint64_t block_size;
    CUgraphExec graph;
    cudaError_t result;
};

struct PeakCudaEventSlot {
    cudaEvent_t start;
    cudaEvent_t end;
    gboolean initialized;
    CUcontext owner_context;
    CUdevice owner_device;
    PeakCudaSlotLease lease;
    PeakCudaLaunchRecord record;
};

struct PeakCudaActiveCapture {
    CUcontext context;
    CUstream stream;
    size_t count;
};

struct PeakCudaRetainedState {
    PeakCudaProfilerState profiler_state;
    PeakCudaSlotAllocator slot_allocator;
    PeakCudaPendingQueue pending_queue;
    std::vector<PeakCudaEventSlot> event_pool;
    std::vector<PeakCudaActiveCapture> active_captures;
};

/* A timed-out CUDA query may return after PEAK finalization. Keep every object
 * reachable by the harvester alive until process reclamation. */
static PeakCudaRetainedState& peak_cuda_retained_state =
    *new PeakCudaRetainedState;
static PeakCudaProfilerState& peak_cuda_profiler_state =
    peak_cuda_retained_state.profiler_state;
static PeakCudaSlotAllocator& peak_cuda_slot_allocator =
    peak_cuda_retained_state.slot_allocator;
static std::vector<PeakCudaEventSlot>& peak_cuda_event_pool =
    peak_cuda_retained_state.event_pool;
static PeakCudaPendingQueue& peak_cuda_pending_queue =
    peak_cuda_retained_state.pending_queue;
static std::vector<PeakCudaActiveCapture>& peak_cuda_active_captures =
    peak_cuda_retained_state.active_captures;
static pthread_mutex_t peak_cuda_harvester_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peak_cuda_harvester_cond = PTHREAD_COND_INITIALIZER;
static pthread_t peak_cuda_harvester_thread;
static std::atomic_bool peak_cuda_harvester_started{false};
static std::atomic_bool peak_cuda_harvester_running{false};
static std::atomic_bool peak_cuda_harvester_waiting{false};
static gboolean peak_cuda_harvester_initialization_requested;
static gboolean peak_cuda_harvester_initialization_allowed;
static gboolean peak_cuda_harvester_initialization_done;
static gboolean peak_cuda_harvester_capture_mode_ready;
static std::atomic_bool peak_cuda_harvester_initialization_inflight{false};
static std::atomic_bool peak_cuda_harvester_initialization_terminal{false};
static pthread_mutex_t peak_cuda_capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peak_cuda_capture_cond = PTHREAD_COND_INITIALIZER;
static std::mutex peak_cuda_capture_teardown_mutex;
static size_t peak_cuda_capture_transitions;
static std::atomic_bool peak_cuda_capture_blocked{false};
static std::atomic_bool peak_cuda_capture_tracking_terminal{false};
static constexpr size_t kPeakCudaTimingShardCount = 256;
static constexpr size_t kPeakCudaTimingExclusiveShardCount = 192;
struct alignas(64) PeakCudaTimingShard {
    std::atomic_uint active{0};
};
static PeakCudaTimingShard
    peak_cuda_timing_shards[kPeakCudaTimingShardCount];
static std::atomic_uint peak_cuda_next_timing_shard{0};
static thread_local size_t peak_cuda_timing_shard_index =
    kPeakCudaTimingShardCount;
static thread_local unsigned int peak_cuda_timing_depth;
static thread_local gboolean peak_cuda_timing_shard_exclusive;
static std::atomic_uint peak_cuda_next_slot_shard{0};
static thread_local size_t peak_cuda_slot_shard_index =
    PeakCudaSlotAllocator::kShardCount;
static constexpr size_t kPeakCudaSlotExclusiveShardCount = 192;
static thread_local gboolean peak_cuda_slot_shard_exclusive;
static thread_local unsigned int peak_cuda_capture_wrapper_depth;
static thread_local unsigned int peak_cuda_runtime_launch_wrapper_depth;
struct PeakCudaThreadIdentityCache {
    std::uint64_t epoch;
    std::uintptr_t identity;
    std::uintptr_t context;
    gboolean driver_function;
    gboolean valid;
    PeakCudaKernelIdentity value;
};
static std::atomic_ullong peak_cuda_identity_epoch{1};
static thread_local PeakCudaThreadIdentityCache peak_cuda_thread_identity;

class PeakCudaRuntimeLaunchGuard {
public:
    PeakCudaRuntimeLaunchGuard()
    {
        ++peak_cuda_runtime_launch_wrapper_depth;
    }

    ~PeakCudaRuntimeLaunchGuard()
    {
        --peak_cuda_runtime_launch_wrapper_depth;
    }

    PeakCudaRuntimeLaunchGuard(const PeakCudaRuntimeLaunchGuard&) = delete;
    PeakCudaRuntimeLaunchGuard& operator=(
        const PeakCudaRuntimeLaunchGuard&) = delete;
};

static constexpr size_t kPeakCudaLifecycleShardCount = 256;
static constexpr size_t kPeakCudaLifecycleExclusiveShardCount = 192;
struct alignas(64) PeakCudaLifecycleShard {
    std::atomic_uint active{0};
};
static PeakCudaLifecycleShard
    peak_cuda_lifecycle_shards[kPeakCudaLifecycleShardCount];
static std::atomic_uint peak_cuda_next_lifecycle_shard{0};
static thread_local size_t peak_cuda_lifecycle_shard_index =
    kPeakCudaLifecycleShardCount;
static thread_local unsigned int peak_cuda_lifecycle_depth;
static thread_local gboolean peak_cuda_lifecycle_shard_exclusive;
/* Odd epochs are closed; even epochs are open. */
static std::atomic_ullong peak_cuda_lifecycle_epoch{1};
static constexpr size_t kPeakCudaCaptureRegistryCapacity = 1024;
static size_t peak_cuda_event_pool_capacity = 256;
static size_t peak_cuda_graph_identity_capacity = 1024;
static unsigned long long peak_cuda_finalization_timeout_ms = 1000;
static gboolean peak_cuda_finalization_complete;
static gboolean peak_cuda_finalization_timed_out;
#ifdef PEAK_ENABLE_TEST_HOOKS
static std::atomic_bool peak_cuda_test_force_incomplete{false};
static std::atomic_bool peak_cuda_test_force_query_error{false};
static std::atomic_ullong peak_cuda_test_harvester_queries{0};
static std::atomic_bool peak_cuda_test_pause_capture_begin_flag{false};
static std::atomic_bool peak_cuda_test_capture_begin_waiting_flag{false};
static std::atomic_bool peak_cuda_test_pause_cuda_section_flag{false};
static std::atomic_uint peak_cuda_test_cuda_sections_waiting_count{0};
static std::atomic_bool
    peak_cuda_test_pause_lifecycle_before_increment_flag{false};
static std::atomic_uint
    peak_cuda_test_lifecycle_before_increment_waiting_count{0};
static std::atomic_bool
    peak_cuda_test_pause_lifecycle_after_admission_flag{false};
static std::atomic_uint
    peak_cuda_test_lifecycle_after_admission_waiting_count{0};
#endif

static constexpr size_t kPeakCudaHarvestRecordBudget = 64;
static constexpr std::uint64_t kPeakCudaHarvestTimeBudgetNs = 1000000;
static constexpr long kPeakCudaHarvestRetryNs = 1000000L;
static constexpr size_t kPeakCudaWakeShard = 0;

static void peak_cuda_request_harvester_stop_no_wait();

static gchar*
peak_cuda_driver_kernel_name(CUfunction function)
{
    const char* name = NULL;

    if (peak_cuda_func_get_name == NULL ||
        peak_cuda_func_get_name(&name, function) != CUDA_SUCCESS ||
        name == NULL || name[0] == '\0') {
        return NULL;
    }
    return g_strdup(name);
}

typedef struct {
    std::uint64_t total_gpu_threads;
    std::uint64_t max_gpu_threads;
    std::uint64_t min_gpu_threads;
    std::uint64_t total_kernel_call_cnt;
    std::uint64_t max_kernel_call_cnt;
    std::uint64_t min_kernel_call_cnt;
    std::uint64_t total_block_size;
    std::uint64_t max_block_size;
    std::uint64_t min_block_size;
    std::uint64_t total_grid_size;
    std::uint64_t max_grid_size;
    std::uint64_t min_grid_size;
    gdouble total_time;
    gdouble min_time;
    gdouble max_time;
} KernelDimInfo;

typedef struct {
    std::uint64_t total_graph_call_cnt;
    std::uint64_t max_graph_call_cnt;
    std::uint64_t min_graph_call_cnt;
    gdouble total_time;
    gdouble min_time;
    gdouble max_time;
    CUdevice device;
} GraphRecordInfo;

typedef struct {
    std::uintptr_t context;
    CUgraphExec graph;
} PeakCudaGraphKey;

static guint
peak_cuda_graph_key_hash(gconstpointer value)
{
    const PeakCudaGraphKey* key =
        static_cast<const PeakCudaGraphKey*>(value);
    return g_direct_hash(reinterpret_cast<gconstpointer>(key->context)) ^
           (g_direct_hash(key->graph) << 1);
}

static gboolean
peak_cuda_graph_key_equal(gconstpointer left, gconstpointer right)
{
    const PeakCudaGraphKey* a =
        static_cast<const PeakCudaGraphKey*>(left);
    const PeakCudaGraphKey* b =
        static_cast<const PeakCudaGraphKey*>(right);
    return a->context == b->context && a->graph == b->graph;
}
static std::mutex peak_cuda_lifecycle_mutex;
static std::atomic_bool peak_cuda_accepting_events{false};
static gboolean peak_cuda_hooks_reverted;

static PeakCudaLifecycleShard*
peak_cuda_lifecycle_try_enter()
{
    /* The epoch loads, reader publication, epoch transition, and cold-path
     * shard scans are all sequentially consistent. The first fixed set of
     * worker threads owns one shard each and publishes with stores instead of
     * contended RMWs. Threads beyond that bound retain the counter fallback.
     * An epoch change rejects a stale reader spanning detach and reattach. */
    unsigned long long epoch = peak_cuda_lifecycle_epoch.load(
        std::memory_order_seq_cst);
    if ((epoch & 1ULL) != 0) {
        return NULL;
    }
    if (peak_cuda_lifecycle_depth != 0) {
        ++peak_cuda_lifecycle_depth;
        return &peak_cuda_lifecycle_shards[
            peak_cuda_lifecycle_shard_index];
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_cuda_test_pause_lifecycle_before_increment_flag.load(
            std::memory_order_acquire)) {
        peak_cuda_test_lifecycle_before_increment_waiting_count.fetch_add(
            1, std::memory_order_release);
        while (peak_cuda_test_pause_lifecycle_before_increment_flag.load(
                   std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        peak_cuda_test_lifecycle_before_increment_waiting_count.fetch_sub(
            1, std::memory_order_release);
    }
#endif
    if (peak_cuda_lifecycle_shard_index == kPeakCudaLifecycleShardCount) {
        unsigned int ticket = peak_cuda_next_lifecycle_shard.fetch_add(
            1, std::memory_order_relaxed);
        peak_cuda_lifecycle_shard_exclusive =
            ticket < kPeakCudaLifecycleExclusiveShardCount;
        peak_cuda_lifecycle_shard_index =
            peak_cuda_lifecycle_shard_exclusive
                ? ticket
                : kPeakCudaLifecycleExclusiveShardCount +
                    ((ticket - kPeakCudaLifecycleExclusiveShardCount) %
                     (kPeakCudaLifecycleShardCount -
                      kPeakCudaLifecycleExclusiveShardCount));
    }
    PeakCudaLifecycleShard* shard =
        &peak_cuda_lifecycle_shards[peak_cuda_lifecycle_shard_index];
    if (peak_cuda_lifecycle_shard_exclusive) {
        shard->active.store(1, std::memory_order_seq_cst);
    } else {
        shard->active.fetch_add(1, std::memory_order_seq_cst);
    }
    if (peak_cuda_lifecycle_epoch.load(std::memory_order_seq_cst) != epoch) {
        if (peak_cuda_lifecycle_shard_exclusive) {
            shard->active.store(0, std::memory_order_seq_cst);
        } else {
            shard->active.fetch_sub(1, std::memory_order_seq_cst);
        }
        return NULL;
    }
    peak_cuda_lifecycle_depth = 1;
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_cuda_test_pause_lifecycle_after_admission_flag.load(
            std::memory_order_acquire)) {
        peak_cuda_test_lifecycle_after_admission_waiting_count.fetch_add(
            1, std::memory_order_release);
        while (peak_cuda_test_pause_lifecycle_after_admission_flag.load(
                   std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        peak_cuda_test_lifecycle_after_admission_waiting_count.fetch_sub(
            1, std::memory_order_release);
    }
#endif
    return shard;
}

static void
peak_cuda_lifecycle_leave(PeakCudaLifecycleShard* shard)
{
    if (shard != NULL) {
        if (--peak_cuda_lifecycle_depth != 0) {
            return;
        }
        if (peak_cuda_lifecycle_shard_exclusive) {
            shard->active.store(0, std::memory_order_seq_cst);
        } else {
            shard->active.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
}

static unsigned int
peak_cuda_active_wrapper_count()
{
    unsigned int total = 0;
    for (PeakCudaLifecycleShard& shard : peak_cuda_lifecycle_shards) {
        unsigned int active = shard.active.load(std::memory_order_seq_cst);
        if (active > UINT_MAX - total) {
            return UINT_MAX;
        }
        total += active;
    }
    return total;
}

static void
peak_cuda_lifecycle_close()
{
    unsigned long long epoch = peak_cuda_lifecycle_epoch.load(
        std::memory_order_seq_cst);
    if ((epoch & 1ULL) == 0) {
        peak_cuda_lifecycle_epoch.store(epoch + 1,
                                        std::memory_order_seq_cst);
    }
}

static void
peak_cuda_lifecycle_open()
{
    unsigned long long epoch = peak_cuda_lifecycle_epoch.load(
        std::memory_order_seq_cst);
    if ((epoch & 1ULL) != 0) {
        peak_cuda_lifecycle_epoch.store(epoch + 1,
                                        std::memory_order_seq_cst);
    }
}

class PeakCudaInflightGuard {
public:
    PeakCudaInflightGuard() : shard_(peak_cuda_lifecycle_try_enter()) {}

    ~PeakCudaInflightGuard()
    {
        peak_cuda_lifecycle_leave(shard_);
    }

    bool entered() const { return shard_ != NULL; }

private:
    PeakCudaLifecycleShard* shard_;
};

class PeakCudaCaptureTimingGuard {
public:
    PeakCudaCaptureTimingGuard() : shard_(NULL) {}

    bool try_enter()
    {
        if (shard_ != NULL) {
            return true;
        }
        if (peak_cuda_capture_blocked.load(std::memory_order_acquire)) {
            return false;
        }
        if (peak_cuda_timing_depth != 0) {
            shard_ = &peak_cuda_timing_shards[
                peak_cuda_timing_shard_index];
            ++peak_cuda_timing_depth;
            return true;
        }
        if (peak_cuda_timing_shard_index == kPeakCudaTimingShardCount) {
            unsigned int ticket = peak_cuda_next_timing_shard.fetch_add(
                1, std::memory_order_relaxed);
            peak_cuda_timing_shard_exclusive =
                ticket < kPeakCudaTimingExclusiveShardCount;
            peak_cuda_timing_shard_index =
                peak_cuda_timing_shard_exclusive
                    ? ticket
                    : kPeakCudaTimingExclusiveShardCount +
                        ((ticket - kPeakCudaTimingExclusiveShardCount) %
                         (kPeakCudaTimingShardCount -
                          kPeakCudaTimingExclusiveShardCount));
        }
        shard_ = &peak_cuda_timing_shards[peak_cuda_timing_shard_index];
        if (peak_cuda_timing_shard_exclusive) {
            shard_->active.store(1, std::memory_order_seq_cst);
        } else {
            shard_->active.fetch_add(1, std::memory_order_seq_cst);
        }
        peak_cuda_timing_depth = 1;
        if (peak_cuda_capture_blocked.load(std::memory_order_seq_cst)) {
            release_reader();
            return false;
        }
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (peak_cuda_test_pause_cuda_section_flag.load(
                std::memory_order_acquire)) {
            peak_cuda_test_cuda_sections_waiting_count.fetch_add(
                1, std::memory_order_release);
            while (peak_cuda_test_pause_cuda_section_flag.load(
                       std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            peak_cuda_test_cuda_sections_waiting_count.fetch_sub(
                1, std::memory_order_release);
        }
#endif
        return true;
    }

    void leave()
    {
        if (shard_ == NULL) {
            return;
        }
        release_reader();
    }

    ~PeakCudaCaptureTimingGuard()
    {
        leave();
    }

private:
    void release_reader()
    {
        PeakCudaTimingShard* shard = shard_;
        shard_ = NULL;
        if (--peak_cuda_timing_depth != 0) {
            return;
        }
        gboolean last_reader;
        if (peak_cuda_timing_shard_exclusive) {
            shard->active.store(0, std::memory_order_seq_cst);
            last_reader = TRUE;
        } else {
            last_reader = shard->active.fetch_sub(
                1, std::memory_order_seq_cst) == 1;
        }
        if (last_reader && peak_cuda_capture_blocked.load(
                               std::memory_order_acquire)) {
            pthread_mutex_lock(&peak_cuda_capture_mutex);
            pthread_cond_broadcast(&peak_cuda_capture_cond);
            pthread_mutex_unlock(&peak_cuda_capture_mutex);
        }
    }

    PeakCudaTimingShard* shard_;
};

static gboolean
peak_cuda_timing_sections_active()
{
    for (PeakCudaTimingShard& shard : peak_cuda_timing_shards) {
        if (shard.active.load(std::memory_order_seq_cst) != 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
peak_cuda_wait_for_capture_timing_section(
    PeakCudaCaptureTimingGuard* guard)
{
    while (guard != NULL &&
           peak_cuda_harvester_running.load(std::memory_order_acquire) &&
           !peak_cuda_capture_tracking_terminal.load(
               std::memory_order_acquire)) {
        if (guard->try_enter()) {
            return TRUE;
        }
        struct timespec retry = {0, kPeakCudaHarvestRetryNs};
        (void)nanosleep(&retry, NULL);
    }
    return FALSE;
}

static gboolean
peak_cuda_capture_register_locked(CUcontext context, CUstream stream)
{
    PeakCudaActiveCapture* free_capture = NULL;
    for (PeakCudaActiveCapture& capture : peak_cuda_active_captures) {
        if (capture.count != 0 && capture.context == context &&
            capture.stream == stream) {
            ++capture.count;
            return TRUE;
        }
        if (capture.count == 0 && free_capture == NULL) {
            free_capture = &capture;
        }
    }
    if (free_capture == NULL) {
        peak_cuda_capture_tracking_terminal.store(
            true, std::memory_order_release);
        peak_cuda_profiler_state.record_capture_query_failure();
        peak_log_warn(
            "[peak] CUDA capture registry capacity exceeded; CUDA timing remains disabled\n");
        return FALSE;
    }
    free_capture->context = context;
    free_capture->stream = stream;
    free_capture->count = 1;
    return TRUE;
}

static gboolean
peak_cuda_capture_remove_locked(CUcontext context, CUstream stream)
{
    for (PeakCudaActiveCapture& capture : peak_cuda_active_captures) {
        if (capture.count != 0 && capture.context == context &&
            capture.stream == stream) {
            --capture.count;
            if (capture.count == 0) {
                capture = {};
            }
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
peak_cuda_capture_any_active_locked()
{
    for (const PeakCudaActiveCapture& capture : peak_cuda_active_captures) {
        if (capture.count != 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
peak_cuda_capture_begin_transition()
{
    pthread_mutex_lock(&peak_cuda_capture_mutex);
    ++peak_cuda_capture_transitions;
    peak_cuda_capture_blocked.store(true, std::memory_order_seq_cst);
    while (peak_cuda_timing_sections_active()) {
        pthread_cond_wait(&peak_cuda_capture_cond, &peak_cuda_capture_mutex);
    }
    pthread_mutex_unlock(&peak_cuda_capture_mutex);
}

static void
peak_cuda_capture_end_transition_locked()
{
    if (peak_cuda_capture_transitions > 0) {
        --peak_cuda_capture_transitions;
    }
    if (peak_cuda_capture_transitions == 0 &&
        !peak_cuda_capture_any_active_locked() &&
        !peak_cuda_capture_tracking_terminal.load(
            std::memory_order_acquire)) {
        peak_cuda_capture_blocked.store(false, std::memory_order_seq_cst);
        pthread_cond_broadcast(&peak_cuda_capture_cond);
    }
}

static gboolean
peak_cuda_capture_status(CUstream stream, CUstreamCaptureStatus* status)
{
    if (peak_cuda_backend_api.driver_stream_is_capturing == NULL ||
        status == NULL) {
        peak_cuda_profiler_state.record_capture_query_unsupported();
        return FALSE;
    }
    CUresult result = peak_cuda_backend_api.driver_stream_is_capturing(
        stream, status);
    if (result == CUDA_ERROR_NOT_SUPPORTED) {
        peak_cuda_profiler_state.record_capture_query_unsupported();
        return FALSE;
    }
    if (result != CUDA_SUCCESS) {
        peak_cuda_profiler_state.record_capture_query_failure();
        return FALSE;
    }
    return TRUE;
}

struct PeakCudaCaptureInvocationData {
    CUstream stream;
    CUcontext context;
    CUstreamCaptureStatus before;
    gboolean before_known;
    gboolean outermost;
    PeakCudaLifecycleShard* lifecycle_shard;
    gboolean teardown_lock_held;
    gboolean fail_closed_transition;
};

static void
peak_cuda_capture_listener_on_enter(GumInvocationContext* context,
                                    gpointer user_data)
{
    const PeakCudaCaptureHookDescriptor* descriptor =
        static_cast<const PeakCudaCaptureHookDescriptor*>(user_data);
    PeakCudaCaptureInvocationData* invocation =
        GUM_IC_GET_INVOCATION_DATA(context,
                                   PeakCudaCaptureInvocationData);

    *invocation = {};
    invocation->stream = reinterpret_cast<CUstream>(
        gum_invocation_context_get_nth_argument(context, 0));
    invocation->outermost = peak_cuda_capture_wrapper_depth++ == 0;
    if (!invocation->outermost) {
        return;
    }

    invocation->lifecycle_shard = peak_cuda_lifecycle_try_enter();
    if (invocation->lifecycle_shard == NULL) {
        /* The application may begin capture after finalization has closed
         * lifecycle admission but before Gum has physically detached this
         * listener. Without a tracked edge, retaining CUDA-owned state is the
         * only safe teardown behavior. */
        peak_cuda_capture_teardown_mutex.lock();
        invocation->teardown_lock_held = TRUE;
        peak_cuda_capture_tracking_terminal.store(
            true, std::memory_order_release);
        invocation->fail_closed_transition = TRUE;
        peak_cuda_capture_begin_transition();
        return;
    }
    peak_cuda_capture_begin_transition();
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (descriptor->operation == PEAK_CUDA_CAPTURE_OPERATION_BEGIN &&
        peak_cuda_test_pause_capture_begin_flag.load(
            std::memory_order_acquire)) {
        peak_cuda_test_capture_begin_waiting_flag.store(
            true, std::memory_order_release);
        while (peak_cuda_test_pause_capture_begin_flag.load(
                   std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        peak_cuda_test_capture_begin_waiting_flag.store(
            false, std::memory_order_release);
    }
#endif
    invocation->before = CU_STREAM_CAPTURE_STATUS_NONE;
    invocation->before_known = peak_cuda_capture_status(
        invocation->stream, &invocation->before);
    if (peak_cuda_backend_api.ctx_get_current == NULL ||
        peak_cuda_backend_api.ctx_get_current(&invocation->context) !=
            CUDA_SUCCESS ||
        invocation->context == NULL) {
        invocation->context = NULL;
        peak_cuda_profiler_state.record_context_query_failure();
    }
}

static void
peak_cuda_capture_listener_on_leave(GumInvocationContext* context,
                                    gpointer user_data)
{
    const PeakCudaCaptureHookDescriptor* descriptor =
        static_cast<const PeakCudaCaptureHookDescriptor*>(user_data);
    PeakCudaCaptureInvocationData* invocation =
        GUM_IC_GET_INVOCATION_DATA(context,
                                   PeakCudaCaptureInvocationData);

    if (!invocation->outermost) {
        --peak_cuda_capture_wrapper_depth;
        return;
    }
    if (invocation->lifecycle_shard == NULL) {
        if (invocation->fail_closed_transition) {
            pthread_mutex_lock(&peak_cuda_capture_mutex);
            peak_cuda_capture_end_transition_locked();
            pthread_mutex_unlock(&peak_cuda_capture_mutex);
        }
        if (invocation->teardown_lock_held) {
            peak_cuda_capture_teardown_mutex.unlock();
        }
        --peak_cuda_capture_wrapper_depth;
        return;
    }

    CUstreamCaptureStatus after = CU_STREAM_CAPTURE_STATUS_NONE;
    gboolean after_known = peak_cuda_capture_status(
        invocation->stream, &after);
    int result = GPOINTER_TO_INT(
        gum_invocation_context_get_return_value(context));

    pthread_mutex_lock(&peak_cuda_capture_mutex);
    gboolean before_active =
        invocation->before != CU_STREAM_CAPTURE_STATUS_NONE;
    gboolean after_active =
        after != CU_STREAM_CAPTURE_STATUS_NONE;
    if (!invocation->before_known || !after_known) {
        peak_cuda_capture_tracking_terminal.store(
            true, std::memory_order_release);
    } else {
        if (!before_active && after_active) {
            if (invocation->context == NULL ||
                !peak_cuda_capture_register_locked(
                    invocation->context, invocation->stream)) {
                peak_cuda_capture_tracking_terminal.store(
                    true, std::memory_order_release);
            }
        } else if (before_active && !after_active) {
            if (invocation->context == NULL ||
                !peak_cuda_capture_remove_locked(invocation->context,
                                                 invocation->stream)) {
                peak_cuda_capture_tracking_terminal.store(
                    true, std::memory_order_release);
            }
        }

        gboolean expected_edge =
            descriptor->operation == PEAK_CUDA_CAPTURE_OPERATION_BEGIN
                ? (!before_active && after_active)
                : (before_active && !after_active);
        if (result == 0 && !expected_edge) {
            peak_cuda_capture_tracking_terminal.store(
                true, std::memory_order_release);
        }
    }
    peak_cuda_capture_end_transition_locked();
    pthread_mutex_unlock(&peak_cuda_capture_mutex);

    --peak_cuda_capture_wrapper_depth;
    peak_cuda_lifecycle_leave(invocation->lifecycle_shard);
}

static gboolean
peak_cuda_capture_entry_point_is_covered(
    gpointer entry, PeakCudaCaptureOperation operation)
{
    if (entry == NULL) {
        return FALSE;
    }
    for (size_t index = 0; index < PEAK_CUDA_CAPTURE_HOOK_COUNT;
         ++index) {
        if (peak_cuda_capture_descriptors[index].operation == operation &&
            reinterpret_cast<gpointer>(peak_cuda_capture_hooks[index]) ==
                entry) {
            return TRUE;
        }
    }
    for (size_t index = 0;
         index < peak_cuda_direct_capture_hook_count;
         ++index) {
        if (peak_cuda_direct_capture_descriptors[index].operation ==
                operation &&
            peak_cuda_direct_capture_hooks[index] == entry) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean str_equal_function(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

char* cu_demangle(char* mangled_name);

static const PeakCudaKernelIdentity*
peak_cuda_identify_kernel(gpointer identity, gboolean driver_function,
                          CUcontext context = NULL)
{
    const std::uint64_t epoch = peak_cuda_identity_epoch.load(
        std::memory_order_acquire);
    const std::uintptr_t identity_value =
        reinterpret_cast<std::uintptr_t>(identity);
    const std::uintptr_t context_value = driver_function
        ? reinterpret_cast<std::uintptr_t>(context)
        : 0;
    if (peak_cuda_thread_identity.valid &&
        peak_cuda_thread_identity.epoch == epoch &&
        peak_cuda_thread_identity.identity == identity_value &&
        peak_cuda_thread_identity.context == context_value &&
        peak_cuda_thread_identity.driver_function == driver_function) {
        return &peak_cuda_thread_identity.value;
    }
    gchar* resolved_name = NULL;
    char* demangled_name = NULL;
    char* target_name = NULL;

    if (peak_cuda_profiler_state.cached_identity(
            identity_value, driver_function,
            &peak_cuda_thread_identity.value, context_value)) {
        peak_cuda_thread_identity = {
            epoch, identity_value, context_value, driver_function, TRUE,
            peak_cuda_thread_identity.value,
        };
        return &peak_cuda_thread_identity.value;
    }
    if (driver_function) {
        resolved_name = peak_cuda_driver_kernel_name(
            reinterpret_cast<CUfunction>(identity));
    } else {
        resolved_name = gum_symbol_name_from_address(identity);
    }
    if (resolved_name != NULL) {
        demangled_name = cu_demangle(resolved_name);
        target_name = extract_function_name(
            demangled_name != NULL ? demangled_name : resolved_name);
    }
    peak_cuda_thread_identity.value = peak_cuda_profiler_state.identify(
        identity_value,
        driver_function,
        demangled_name != NULL ? demangled_name : resolved_name,
        target_name,
        context_value);
    g_free(resolved_name);
    free(demangled_name);
    free(target_name);
    peak_cuda_thread_identity.epoch = epoch;
    peak_cuda_thread_identity.identity = identity_value;
    peak_cuda_thread_identity.context = context_value;
    peak_cuda_thread_identity.driver_function = driver_function;
    peak_cuda_thread_identity.valid = TRUE;
    return &peak_cuda_thread_identity.value;
}

static const PeakCudaKernelIdentity*
peak_cuda_cached_driver_identity(CUfunction function, CUcontext* context)
{
    const std::uint64_t epoch = peak_cuda_identity_epoch.load(
        std::memory_order_acquire);
    /* CUfunction is a context-owned opaque handle. Reusing the same valid
     * handle does not change its owning context, so its thread-local identity
     * can bypass the globally serialized current-context query on repeated
     * launches. Lifecycle epoch changes invalidate this shortcut before hook
     * teardown or reattachment. */
    if (!peak_cuda_thread_identity.valid ||
        peak_cuda_thread_identity.epoch != epoch ||
        peak_cuda_thread_identity.identity !=
            reinterpret_cast<std::uintptr_t>(function) ||
        !peak_cuda_thread_identity.driver_function) {
        return NULL;
    }
    *context = reinterpret_cast<CUcontext>(
        peak_cuda_thread_identity.context);
    return &peak_cuda_thread_identity.value;
}

char* cu_demangle(char* mangled_name) {
    return mangled_name != NULL ? cxa_demangle(mangled_name) : NULL;
}

static PeakCudaLaunchDimensions
peak_cuda_launch_dimensions(unsigned int grid_x, unsigned int grid_y,
                            unsigned int grid_z, unsigned int block_x,
                            unsigned int block_y, unsigned int block_z)
{
    PeakCudaLaunchDimensions dimensions =
        peak_cuda_compute_launch_dimensions(
            grid_x, grid_y, grid_z, block_x, block_y, block_z);
    if (dimensions.overflow) {
        peak_cuda_profiler_state.record_dimension_overflow();
    }
    return dimensions;
}

static void update_kernel_map_info(const gchar* kernel_name,
                                   std::uint64_t total_threads,
                                   std::uint64_t grid_size,
                                   std::uint64_t block_size,
                                   gdouble elapsed_sec)
{
    KernelDimInfo* dim_info = (KernelDimInfo*) g_hash_table_lookup(
        cuda_kernel_local_dim_mapping, kernel_name);

    if (!dim_info) {
        gchar* key = g_strdup(kernel_name);
        if (key == NULL) {
            return;
        }
        dim_info = g_new(KernelDimInfo, 1);
        dim_info->total_gpu_threads = total_threads;
        dim_info->total_kernel_call_cnt = 1;
        dim_info->total_block_size = block_size;
        dim_info->total_grid_size = grid_size;
        dim_info->total_time = elapsed_sec;
        dim_info->max_gpu_threads = total_threads;
        dim_info->min_gpu_threads = total_threads;
        dim_info->max_block_size = block_size;
        dim_info->min_block_size = block_size;
        dim_info->max_grid_size = grid_size;
        dim_info->min_grid_size = grid_size;
        dim_info->max_time = elapsed_sec;
        dim_info->min_time = elapsed_sec;
        g_hash_table_insert(cuda_kernel_local_dim_mapping, key, dim_info);
    } else {
        bool overflow = false;
        dim_info->total_gpu_threads = peak_cuda_saturating_add_u64(
            dim_info->total_gpu_threads, total_threads, &overflow);
        dim_info->total_kernel_call_cnt = peak_cuda_saturating_add_u64(
            dim_info->total_kernel_call_cnt, 1, &overflow);
        dim_info->total_block_size = peak_cuda_saturating_add_u64(
            dim_info->total_block_size, block_size, &overflow);
        dim_info->total_grid_size = peak_cuda_saturating_add_u64(
            dim_info->total_grid_size, grid_size, &overflow);
        dim_info->total_time += elapsed_sec;
        dim_info->max_gpu_threads = std::max(dim_info->max_gpu_threads, total_threads);
        dim_info->min_gpu_threads = std::min(dim_info->min_gpu_threads, total_threads);
        dim_info->max_block_size = std::max(dim_info->max_block_size, block_size);
        dim_info->min_block_size = std::min(dim_info->min_block_size, block_size);
        dim_info->max_grid_size = std::max(dim_info->max_grid_size, grid_size);
        dim_info->min_grid_size = std::min(dim_info->min_grid_size, grid_size);
        dim_info->max_time = std::max(dim_info->max_time, elapsed_sec);
        dim_info->min_time = std::min(dim_info->min_time, elapsed_sec);
        if (overflow) {
            peak_cuda_profiler_state.record_dimension_overflow();
        }
    }
}

void insert_cuda_mapping_record(const gchar* kernel_name,
                                std::uint64_t total_threads,
                                std::uint64_t grid_size,
                                std::uint64_t block_size,
                                gdouble elapsed_sec)
{
    g_mutex_lock(&cuda_kernel_local_dim_mapping_mutex);
    update_kernel_map_info(kernel_name != NULL ? kernel_name : "<unknown>",
                           total_threads, grid_size, block_size, elapsed_sec);
    g_mutex_unlock(&cuda_kernel_local_dim_mapping_mutex);
}

void insert_cuda_graph_record(std::uintptr_t context, CUgraphExec graph,
                              CUdevice device, gdouble elapsed_sec)
{
    PeakCudaGraphKey lookup = {context, graph};
    g_mutex_lock(&cuda_graph_local_mapping_mutex);
    GraphRecordInfo* graph_info = (GraphRecordInfo*)
        g_hash_table_lookup(cuda_graph_local_mapping, &lookup);
    if (!graph_info) {
        if (g_hash_table_size(cuda_graph_local_mapping) >=
            peak_cuda_graph_identity_capacity) {
            peak_cuda_profiler_state.record_identity_full();
            g_mutex_unlock(&cuda_graph_local_mapping_mutex);
            return;
        }
        PeakCudaGraphKey* key = g_try_new(PeakCudaGraphKey, 1);
        graph_info = g_try_new(GraphRecordInfo, 1);
        if (key == NULL || graph_info == NULL) {
            g_free(key);
            g_free(graph_info);
            peak_cuda_profiler_state.record_identity_full();
            g_mutex_unlock(&cuda_graph_local_mapping_mutex);
            return;
        }
        *key = lookup;
        graph_info->total_graph_call_cnt = 1;
        graph_info->total_time = elapsed_sec;
        graph_info->max_time = elapsed_sec;
        graph_info->min_time = elapsed_sec;
        graph_info->device = device;
        g_hash_table_insert(cuda_graph_local_mapping, key, graph_info);
    } else {
        bool overflow = false;
        graph_info->total_graph_call_cnt = peak_cuda_saturating_add_u64(
            graph_info->total_graph_call_cnt, 1, &overflow);
        graph_info->total_time += elapsed_sec;
        graph_info->max_time = std::max(graph_info->max_time, elapsed_sec);
        graph_info->min_time = std::min(graph_info->min_time, elapsed_sec);
        if (overflow) {
            peak_cuda_profiler_state.record_dimension_overflow();
        }
    }
    g_mutex_unlock(&cuda_graph_local_mapping_mutex);
}

static size_t
peak_cuda_parse_event_pool_capacity()
{
    static const PeakEnvUnsignedSchema schema = {
        "PEAK_CUDA_EVENT_POOL_CAPACITY", "events", 256, 1, 65536, false,
        &peak_cuda_event_pool_capacity_warning_emitted, false,
    };

    return (size_t)peak_parse_env_unsigned(&schema);
}

static unsigned long long
peak_cuda_parse_finalization_timeout_ms()
{
    static const PeakEnvUnsignedSchema schema = {
        "PEAK_CUDA_FINALIZATION_TIMEOUT_MS", "milliseconds", 1000, 1,
        60000, false, &peak_cuda_finalization_timeout_warning_emitted, false,
    };

    return peak_parse_env_unsigned(&schema);
}

static std::uint64_t
peak_cuda_monotonic_ns()
{
    struct timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (std::uint64_t)now.tv_sec * 1000000000ULL +
           (std::uint64_t)now.tv_nsec;
}

static struct timespec
peak_cuda_realtime_after(long nanoseconds)
{
    struct timespec deadline = {};
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += nanoseconds;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    return deadline;
}

static void
peak_cuda_resolve_backend_api()
{
    if (peak_cuda_backend_api.driver_handle == NULL) {
        peak_cuda_backend_api.driver_handle =
            dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
    }
    void* driver = peak_cuda_backend_api.driver_handle;
    peak_cuda_backend_api.ctx_get_current = driver != NULL
        ? reinterpret_cast<PeakCuCtxGetCurrentFn>(
              dlsym(driver, "cuCtxGetCurrent"))
        : NULL;
    peak_cuda_backend_api.ctx_get_device = driver != NULL
        ? reinterpret_cast<PeakCuCtxGetDeviceFn>(
              dlsym(driver, "cuCtxGetDevice"))
        : NULL;
    peak_cuda_backend_api.ctx_push_current = driver != NULL
        ? reinterpret_cast<PeakCuCtxPushCurrentFn>(
              dlsym(driver, "cuCtxPushCurrent_v2"))
        : NULL;
    peak_cuda_backend_api.ctx_pop_current = driver != NULL
        ? reinterpret_cast<PeakCuCtxPopCurrentFn>(
              dlsym(driver, "cuCtxPopCurrent_v2"))
        : NULL;
    peak_cuda_backend_api.event_create = driver != NULL
        ? reinterpret_cast<PeakCuEventCreateFn>(
              dlsym(driver, "cuEventCreate"))
        : NULL;
    peak_cuda_backend_api.event_destroy = driver != NULL
        ? reinterpret_cast<PeakCuEventDestroyFn>(
              dlsym(driver, "cuEventDestroy_v2"))
        : NULL;
    peak_cuda_backend_api.event_record = driver != NULL
        ? reinterpret_cast<PeakCuEventRecordFn>(
              dlsym(driver, "cuEventRecord"))
        : NULL;
    peak_cuda_backend_api.event_query = driver != NULL
        ? reinterpret_cast<PeakCuEventQueryFn>(
              dlsym(driver, "cuEventQuery"))
        : NULL;
    peak_cuda_backend_api.event_elapsed_time = driver != NULL
        ? reinterpret_cast<PeakCuEventElapsedTimeFn>(
              dlsym(driver, "cuEventElapsedTime"))
        : NULL;
    peak_cuda_backend_api.driver_stream_is_capturing = driver != NULL
        ? reinterpret_cast<PeakCuStreamIsCapturingFn>(
              dlsym(driver, "cuStreamIsCapturing"))
        : NULL;
    peak_cuda_backend_api.driver_thread_exchange_stream_capture_mode =
        driver != NULL
            ? reinterpret_cast<PeakCuThreadExchangeStreamCaptureModeFn>(
                  dlsym(driver, "cuThreadExchangeStreamCaptureMode"))
            : NULL;
    peak_cuda_func_get_name = driver != NULL
        ? reinterpret_cast<PeakCudaFuncGetNameFn>(
              dlsym(driver, "cuFuncGetName"))
        : NULL;
}

static gboolean
peak_cuda_driver_timing_available()
{
    return peak_cuda_backend_api.event_create != NULL &&
        peak_cuda_backend_api.event_destroy != NULL &&
        peak_cuda_backend_api.event_record != NULL &&
        peak_cuda_backend_api.event_query != NULL &&
        peak_cuda_backend_api.event_elapsed_time != NULL;
}

struct PeakCudaContextActivation {
    CUcontext target;
    gboolean pushed;
};

static gboolean
peak_cuda_activate_context(CUcontext target,
                           PeakCudaContextActivation* activation)
{
    CUcontext current = NULL;
    if (activation == NULL || target == NULL ||
        peak_cuda_backend_api.ctx_get_current == NULL) {
        peak_cuda_profiler_state.record_context_switch_failure();
        return FALSE;
    }
    if (peak_cuda_backend_api.ctx_get_current(&current) != CUDA_SUCCESS) {
        peak_cuda_profiler_state.record_context_query_failure();
        return FALSE;
    }
    activation->target = target;
    activation->pushed = FALSE;
    if (current == target) {
        return TRUE;
    }
    if (peak_cuda_backend_api.ctx_push_current == NULL ||
        peak_cuda_backend_api.ctx_pop_current == NULL ||
        peak_cuda_backend_api.ctx_push_current(target) != CUDA_SUCCESS) {
        peak_cuda_profiler_state.record_context_switch_failure();
        return FALSE;
    }
    activation->pushed = TRUE;
    return TRUE;
}

static gboolean
peak_cuda_restore_context(PeakCudaContextActivation* activation)
{
    CUcontext popped = NULL;
    if (activation == NULL || !activation->pushed) {
        return TRUE;
    }
    activation->pushed = FALSE;
    if (peak_cuda_backend_api.ctx_pop_current == NULL ||
        peak_cuda_backend_api.ctx_pop_current(&popped) != CUDA_SUCCESS ||
        popped != activation->target) {
        peak_cuda_profiler_state.record_context_restore_failure();
        peak_cuda_accepting_events.store(false, std::memory_order_seq_cst);
        return FALSE;
    }
    return TRUE;
}

static gboolean
peak_cuda_destroy_slot_events_current(PeakCudaEventSlot* slot)
{
    gboolean destroyed = TRUE;
    if (slot == NULL) {
        return TRUE;
    }
    if (slot->start != NULL) {
        if (peak_cuda_backend_api.event_destroy != NULL &&
            peak_cuda_backend_api.event_destroy(
                reinterpret_cast<CUevent>(slot->start)) == CUDA_SUCCESS) {
            slot->start = NULL;
        } else {
            destroyed = FALSE;
        }
    }
    if (slot->end != NULL) {
        if (peak_cuda_backend_api.event_destroy != NULL &&
            peak_cuda_backend_api.event_destroy(
                reinterpret_cast<CUevent>(slot->end)) == CUDA_SUCCESS) {
            slot->end = NULL;
        } else {
            destroyed = FALSE;
        }
    }
    if (destroyed) {
        slot->initialized = FALSE;
    }
    return destroyed;
}

static gboolean
peak_cuda_destroy_event_pool()
{
    std::unique_lock<std::mutex> capture_teardown_lock(
        peak_cuda_capture_teardown_mutex, std::try_to_lock);
    if (!capture_teardown_lock.owns_lock()) {
        peak_log_warn(
            "[peak] retaining CUDA event state while a stream-capture call crosses teardown\n");
        return FALSE;
    }
    pthread_mutex_lock(&peak_cuda_capture_mutex);
    gboolean capture_unsafe = peak_cuda_capture_transitions != 0 ||
        peak_cuda_capture_any_active_locked() ||
        peak_cuda_capture_tracking_terminal.load(
            std::memory_order_acquire);
    pthread_mutex_unlock(&peak_cuda_capture_mutex);
    if (capture_unsafe) {
        peak_log_warn(
            "[peak] retaining CUDA event state because stream-capture quiescence is not proven\n");
        return FALSE;
    }

    gboolean retained = FALSE;
    for (PeakCudaEventSlot& slot : peak_cuda_event_pool) {
        if (slot.start == NULL && slot.end == NULL) {
            continue;
        }
        PeakCudaContextActivation activation = {};
        if (!peak_cuda_activate_context(slot.owner_context, &activation)) {
            retained = TRUE;
            continue;
        }
        if (!peak_cuda_destroy_slot_events_current(&slot)) {
            retained = TRUE;
        }
        if (!peak_cuda_restore_context(&activation)) {
            retained = TRUE;
        }
    }
    if (retained) {
        peak_log_warn("[peak] retaining CUDA event state that could not be destroyed in its owning context\n");
        return FALSE;
    }
    peak_cuda_event_pool.clear();
    peak_cuda_pending_queue.reset(0);
    peak_cuda_slot_allocator.reset(0);
    return TRUE;
}

static gboolean
peak_cuda_discard_lease_current(const PeakCudaSlotLease& lease)
{
    if (lease.index >= peak_cuda_event_pool.size()) {
        return FALSE;
    }
    PeakCudaEventSlot& slot = peak_cuda_event_pool[lease.index];
    if (!peak_cuda_destroy_slot_events_current(&slot)) {
        return FALSE;
    }
    slot.record.kind = PEAK_CUDA_LAUNCH_RECORD_NONE;
    return peak_cuda_slot_allocator.release(lease) ? TRUE : FALSE;
}

static gboolean
peak_cuda_pending_push(const PeakCudaSlotLease& lease)
{
    bool shard_was_empty = false;
    if (!peak_cuda_pending_queue.push(lease, &shard_was_empty)) {
        return FALSE;
    }
    /* The first producer shard provides low-latency wakeups; all other shards
     * rely on the same bounded 1-ms retry and never touch shared wake state.
     * If the helper is waiting, the mutex pairs the predicate update with
     * pthread_cond_timedwait's atomic unlock-and-wait, preventing a lost wake
     * without serializing launch workers. */
    bool helper_waiting = true;
    if (shard_was_empty && lease.shard == kPeakCudaWakeShard &&
        peak_cuda_harvester_waiting.compare_exchange_strong(
            helper_waiting, false, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        pthread_mutex_lock(&peak_cuda_harvester_mutex);
        (void)pthread_cond_signal(&peak_cuda_harvester_cond);
        pthread_mutex_unlock(&peak_cuda_harvester_mutex);
    }
    return TRUE;
}

static gboolean
peak_cuda_pending_pop(PeakCudaSlotLease* lease)
{
    return peak_cuda_pending_queue.pop(lease) ? TRUE : FALSE;
}

static void
peak_cuda_complete_record(PeakCudaEventSlot* slot, float milliseconds)
{
    if (slot->record.result == cudaSuccess) {
        if (slot->record.kind == PEAK_CUDA_LAUNCH_RECORD_KERNEL) {
            insert_cuda_mapping_record(slot->record.kernel_name,
                                       slot->record.total_threads,
                                       slot->record.grid_size,
                                       slot->record.block_size,
                                       milliseconds / 1000.0);
        } else if (slot->record.kind == PEAK_CUDA_LAUNCH_RECORD_GRAPH) {
            insert_cuda_graph_record(slot->lease.context,
                                     slot->record.graph,
                                     slot->owner_device,
                                     milliseconds / 1000.0);
        }
    }
    peak_cuda_profiler_state.record_launch_completed();
}

enum PeakCudaHarvestOutcome {
    PEAK_CUDA_HARVEST_RETAIN = 0,
    PEAK_CUDA_HARVEST_RETRY,
    PEAK_CUDA_HARVEST_RELEASE,
};

static PeakCudaHarvestOutcome
peak_cuda_harvest_one_current(const PeakCudaSlotLease& lease)
{
    if (lease.index >= peak_cuda_event_pool.size()) {
        return PEAK_CUDA_HARVEST_RETAIN;
    }
    if (!peak_cuda_harvester_running.load(std::memory_order_acquire)) {
        return PEAK_CUDA_HARVEST_RETAIN;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_cuda_test_force_incomplete.load(std::memory_order_relaxed)) {
        return PEAK_CUDA_HARVEST_RETRY;
    }
#endif
    PeakCudaCaptureTimingGuard capture_guard;
    if (!capture_guard.try_enter()) {
        return PEAK_CUDA_HARVEST_RETRY;
    }
    PeakCudaEventSlot& slot = peak_cuda_event_pool[lease.index];

    CUresult query;
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_cuda_test_force_query_error.exchange(
            false, std::memory_order_relaxed)) {
        query = CUDA_ERROR_INVALID_HANDLE;
    } else
#endif
    {
        query = peak_cuda_backend_api.event_query(
            reinterpret_cast<CUevent>(slot.end));
#ifdef PEAK_ENABLE_TEST_HOOKS
        peak_cuda_test_harvester_queries.fetch_add(
            1, std::memory_order_relaxed);
#endif
    }
    if (!peak_cuda_harvester_running.load(std::memory_order_acquire)) {
        return PEAK_CUDA_HARVEST_RETAIN;
    }
    if (query == CUDA_ERROR_NOT_READY) {
        return PEAK_CUDA_HARVEST_RETRY;
    }
    if (query != CUDA_SUCCESS) {
        peak_cuda_profiler_state.record_event_query_failure();
        gboolean destroyed = peak_cuda_destroy_slot_events_current(&slot);
        if (destroyed) {
            slot.record.kind = PEAK_CUDA_LAUNCH_RECORD_NONE;
            return PEAK_CUDA_HARVEST_RELEASE;
        }
        return PEAK_CUDA_HARVEST_RETAIN;
    }

    if (slot.start != NULL && slot.end != NULL) {
        float ms = 0.0f;
        if (peak_cuda_backend_api.event_elapsed_time(
                &ms, reinterpret_cast<CUevent>(slot.start),
                reinterpret_cast<CUevent>(slot.end)) != CUDA_SUCCESS) {
            peak_cuda_profiler_state.record_elapsed_time_failure();
        } else {
            peak_cuda_complete_record(&slot, ms);
        }
    }

    slot.record.kind = PEAK_CUDA_LAUNCH_RECORD_NONE;
    return PEAK_CUDA_HARVEST_RELEASE;
}

static gboolean
peak_cuda_harvest_pass()
{
    std::uint64_t started_ns = peak_cuda_monotonic_ns();
    PeakCudaContextActivation activation = {};
    CUcontext batch_context = NULL;
    PeakCudaSlotLease leases[kPeakCudaHarvestRecordBudget] = {};
    PeakCudaHarvestOutcome outcomes[kPeakCudaHarvestRecordBudget] = {};
    size_t outcome_count = 0;

    for (size_t checked = 0;
         checked < kPeakCudaHarvestRecordBudget; ++checked) {
        if (!peak_cuda_harvester_running.load(std::memory_order_acquire)) {
            break;
        }
        PeakCudaSlotLease lease = {};
        if (!peak_cuda_pending_pop(&lease)) {
            break;
        }
        CUcontext lease_context =
            reinterpret_cast<CUcontext>(lease.context);
        if (batch_context == NULL) {
            if (!peak_cuda_activate_context(lease_context, &activation)) {
                break;
            }
            batch_context = lease_context;
        } else if (lease_context != batch_context) {
            /* Keep one owning context current for the whole bounded pass.
             * A later pass will service a different context without adding
             * push/pop traffic to every completed launch. */
            (void)peak_cuda_pending_queue.requeue_local(lease);
            break;
        }
        leases[outcome_count] = lease;
        outcomes[outcome_count] = peak_cuda_harvest_one_current(lease);
        ++outcome_count;
        std::uint64_t now_ns = peak_cuda_monotonic_ns();
        if (started_ns != 0 && now_ns != 0 &&
            now_ns - started_ns >= kPeakCudaHarvestTimeBudgetNs) {
            break;
        }
    }

    gboolean restored = peak_cuda_restore_context(&activation);
    if (restored) {
        for (size_t index = 0; index < outcome_count; ++index) {
            if (outcomes[index] == PEAK_CUDA_HARVEST_RELEASE) {
                (void)peak_cuda_slot_allocator.release(leases[index]);
            } else if (outcomes[index] == PEAK_CUDA_HARVEST_RETRY &&
                       peak_cuda_harvester_running.load(
                           std::memory_order_acquire)) {
                /* Keep retries on the consumer-local list. The producer head
                 * remains empty, so a later application submission can wake
                 * the bounded retry wait with one shard transition. */
                (void)peak_cuda_pending_queue.requeue_local(leases[index]);
            }
        }
    }
    return TRUE;
}

static void
peak_cuda_pin_harvester_to_last_allowed_cpu()
{
#if defined(__linux__)
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return;
    }
    for (int cpu = CPU_SETSIZE - 1; cpu >= 0; --cpu) {
        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        (void)pthread_setaffinity_np(
            pthread_self(), sizeof(selected), &selected);
        return;
    }
#endif
}

static void*
peak_cuda_harvester_main(void*)
{
    /* Keep the helper off the low-numbered CPUs most launch-worker pools
     * select first. This changes only the helper's affinity and remains a
     * best-effort hint when the application uses the complete allowed set. */
    peak_cuda_pin_harvester_to_last_allowed_cpu();
    peak_general_listener_exclude_current_thread();
    peak_general_listener_fast_ignore_current_thread();

    pthread_mutex_lock(&peak_cuda_harvester_mutex);
    while (peak_cuda_harvester_running.load(std::memory_order_acquire) &&
           peak_cuda_harvester_initialization_allowed &&
           !peak_cuda_harvester_initialization_requested) {
        pthread_cond_wait(&peak_cuda_harvester_cond,
                          &peak_cuda_harvester_mutex);
    }
    gboolean initialize_capture_mode =
        peak_cuda_harvester_running.load(std::memory_order_acquire) &&
        peak_cuda_harvester_initialization_allowed;
    if (initialize_capture_mode) {
        peak_cuda_harvester_initialization_inflight.store(
            true, std::memory_order_release);
    }
    pthread_mutex_unlock(&peak_cuda_harvester_mutex);
    if (!initialize_capture_mode) {
        peak_general_listener_fast_unignore_current_thread();
        return NULL;
    }

    cudaError_t capture_mode_result = cudaSuccess;
    CUresult driver_capture_mode_result = CUDA_SUCCESS;
    PeakCudaCaptureTimingGuard capture_guard;
    gboolean capture_section_ready =
        peak_cuda_wait_for_capture_timing_section(&capture_guard);
#ifdef PEAK_ENABLE_TEST_HOOKS
    gboolean force_capture_mode_failure =
        g_strcmp0(g_getenv("PEAK_TEST_FAIL_CUDA_HARVESTER_CAPTURE_MODE"),
                  "1") == 0;
#endif
    if (!capture_section_ready) {
        capture_mode_result = cudaErrorUnknown;
        driver_capture_mode_result = CUDA_ERROR_UNKNOWN;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    else if (force_capture_mode_failure) {
        capture_mode_result = cudaErrorUnknown;
        driver_capture_mode_result = CUDA_ERROR_UNKNOWN;
    }
#endif
    else
    {
        cudaStreamCaptureMode runtime_mode = cudaStreamCaptureModeRelaxed;
        capture_mode_result =
            cudaThreadExchangeStreamCaptureMode(&runtime_mode);
        if (peak_cuda_backend_api
                .driver_thread_exchange_stream_capture_mode == NULL) {
            driver_capture_mode_result = CUDA_ERROR_NOT_SUPPORTED;
        } else {
            CUstreamCaptureMode driver_mode =
                CU_STREAM_CAPTURE_MODE_RELAXED;
            driver_capture_mode_result = peak_cuda_backend_api
                .driver_thread_exchange_stream_capture_mode(&driver_mode);
        }
    }
    capture_guard.leave();
    gboolean capture_api_ready =
        capture_mode_result == cudaSuccess &&
        driver_capture_mode_result == CUDA_SUCCESS;

    pthread_mutex_lock(&peak_cuda_harvester_mutex);
    peak_cuda_harvester_initialization_inflight.store(
        false, std::memory_order_release);
    gboolean capture_mode_ready =
        capture_api_ready &&
        peak_cuda_harvester_running.load(std::memory_order_acquire) &&
        peak_cuda_harvester_initialization_allowed;
    peak_cuda_harvester_capture_mode_ready = capture_mode_ready;
    peak_cuda_harvester_initialization_done = TRUE;
    if (!capture_api_ready) {
        peak_cuda_harvester_running.store(false, std::memory_order_release);
        peak_cuda_harvester_initialization_terminal.store(
            true, std::memory_order_release);
    }
    if (capture_mode_ready) {
        peak_cuda_accepting_events.store(true, std::memory_order_seq_cst);
    }
    pthread_cond_broadcast(&peak_cuda_harvester_cond);
    pthread_mutex_unlock(&peak_cuda_harvester_mutex);

    if (!capture_mode_ready) {
        if (!capture_api_ready) {
            peak_log_warn(
                "[peak] CUDA event harvester could not enter relaxed stream-capture mode (runtime status %d, Driver status %d); CUDA timing remains disabled\n",
                (int)capture_mode_result, (int)driver_capture_mode_result);
        }
        peak_general_listener_fast_unignore_current_thread();
        return NULL;
    }

    for (;;) {
        (void)peak_cuda_harvest_pass();
        if (!peak_cuda_harvester_running.load(std::memory_order_acquire)) {
            break;
        }

        struct timespec retry = peak_cuda_realtime_after(
            kPeakCudaHarvestRetryNs);
        pthread_mutex_lock(&peak_cuda_harvester_mutex);
        if (peak_cuda_harvester_running.load(std::memory_order_acquire)) {
            peak_cuda_harvester_waiting.store(true,
                                               std::memory_order_release);
            (void)pthread_cond_timedwait(&peak_cuda_harvester_cond,
                                         &peak_cuda_harvester_mutex,
                                         &retry);
            peak_cuda_harvester_waiting.store(false,
                                               std::memory_order_release);
        }
        pthread_mutex_unlock(&peak_cuda_harvester_mutex);
    }
    peak_general_listener_fast_unignore_current_thread();
    return NULL;
}

static gboolean
peak_cuda_start_harvester()
{
    pthread_mutex_lock(&peak_cuda_harvester_mutex);
    peak_cuda_harvester_running.store(true, std::memory_order_release);
    peak_cuda_harvester_waiting.store(false, std::memory_order_relaxed);
    peak_cuda_harvester_initialization_requested = FALSE;
    peak_cuda_harvester_initialization_allowed = TRUE;
    peak_cuda_harvester_initialization_done = FALSE;
    peak_cuda_harvester_capture_mode_ready = FALSE;
    peak_cuda_harvester_initialization_inflight.store(
        false, std::memory_order_release);
    peak_cuda_harvester_initialization_terminal.store(
        false, std::memory_order_release);
    pthread_mutex_unlock(&peak_cuda_harvester_mutex);
    pthread_listener_mark_next_created_thread_helper();
    if (pthread_create(&peak_cuda_harvester_thread, NULL,
                       peak_cuda_harvester_main, NULL) != 0) {
        pthread_mutex_lock(&peak_cuda_harvester_mutex);
        peak_cuda_harvester_running.store(false, std::memory_order_release);
        peak_cuda_harvester_initialization_terminal.store(
            true, std::memory_order_release);
        pthread_mutex_unlock(&peak_cuda_harvester_mutex);
        peak_log_warn("[peak] failed to start CUDA event harvester; CUDA timing remains disabled\n");
        return FALSE;
    }
    peak_cuda_harvester_started.store(true, std::memory_order_release);
    return TRUE;
}

static void
peak_cuda_request_harvester_initialization()
{
    if (peak_cuda_harvester_initialization_terminal.load(
            std::memory_order_acquire) ||
        !peak_cuda_harvester_started.load(std::memory_order_acquire)) {
        return;
    }
    if (pthread_mutex_trylock(&peak_cuda_harvester_mutex) != 0) {
        return;
    }
    if (peak_cuda_harvester_initialization_allowed &&
        !peak_cuda_harvester_initialization_done &&
        peak_cuda_harvester_running.load(std::memory_order_acquire)) {
        peak_cuda_harvester_initialization_requested = TRUE;
        pthread_cond_broadcast(&peak_cuda_harvester_cond);
    }
    pthread_mutex_unlock(&peak_cuda_harvester_mutex);
}

static void
peak_cuda_disable_harvester_initialization()
{
    pthread_mutex_lock(&peak_cuda_harvester_mutex);
    peak_cuda_harvester_initialization_allowed = FALSE;
    peak_cuda_harvester_initialization_terminal.store(
        true, std::memory_order_release);
    peak_cuda_accepting_events.store(false, std::memory_order_seq_cst);
    if (!peak_cuda_harvester_initialization_done) {
        peak_cuda_harvester_running.store(false, std::memory_order_release);
    }
    pthread_cond_broadcast(&peak_cuda_harvester_cond);
    pthread_mutex_unlock(&peak_cuda_harvester_mutex);
}

static void
peak_cuda_request_harvester_stop()
{
    if (!peak_cuda_harvester_started.load(std::memory_order_acquire)) {
        return;
    }
    pthread_mutex_lock(&peak_cuda_harvester_mutex);
    peak_cuda_harvester_running.store(false, std::memory_order_release);
    pthread_cond_broadcast(&peak_cuda_harvester_cond);
    pthread_mutex_unlock(&peak_cuda_harvester_mutex);
}

static void
peak_cuda_request_harvester_stop_no_wait()
{
    if (!peak_cuda_harvester_started.load(std::memory_order_acquire)) {
        return;
    }
    peak_cuda_harvester_running.store(false, std::memory_order_release);
    /* Timeout cleanup never joins this helper. A broadcast is safe without
     * acquiring the ring mutex; a lost wake only leaves process-retained state
     * for operating-system reclamation. */
    (void)pthread_cond_broadcast(&peak_cuda_harvester_cond);
}

static void
peak_cuda_stop_harvester()
{
    if (!peak_cuda_harvester_started.load(std::memory_order_acquire)) {
        return;
    }
    peak_cuda_request_harvester_stop();
    (void)pthread_join(peak_cuda_harvester_thread, NULL);
    peak_cuda_harvester_started.store(false, std::memory_order_release);
}

enum PeakCudaLaunchBackend {
    PEAK_CUDA_LAUNCH_BACKEND_RUNTIME = 0,
    PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
};

struct PeakCudaTimingLease {
    PeakCudaSlotLease lease;
    cudaEvent_t start;
    cudaEvent_t end;
};

static gboolean
peak_cuda_current_context(CUcontext* context)
{
    if (context == NULL || peak_cuda_backend_api.ctx_get_current == NULL ||
        peak_cuda_backend_api.ctx_get_current(context) != CUDA_SUCCESS ||
        *context == NULL) {
        peak_cuda_profiler_state.record_context_query_failure();
        return FALSE;
    }
    return TRUE;
}

static size_t
peak_cuda_current_slot_shard()
{
    if (peak_cuda_slot_shard_index == PeakCudaSlotAllocator::kShardCount) {
        const size_t ticket = peak_cuda_next_slot_shard.fetch_add(
            1, std::memory_order_relaxed);
        peak_cuda_slot_shard_exclusive =
            ticket < kPeakCudaSlotExclusiveShardCount;
        peak_cuda_slot_shard_index = peak_cuda_slot_shard_exclusive
            ? ticket
            : kPeakCudaSlotExclusiveShardCount +
                  ((ticket - kPeakCudaSlotExclusiveShardCount) %
                   (PeakCudaSlotAllocator::kShardCount -
                    kPeakCudaSlotExclusiveShardCount));
    }
    return peak_cuda_slot_shard_index;
}

static void
peak_cuda_record_launch_observed()
{
    peak_cuda_profiler_state.record_launch_observed(
        peak_cuda_current_slot_shard(), peak_cuda_slot_shard_exclusive);
}

static gboolean
peak_cuda_acquire_timing(PeakCudaLaunchBackend backend,
                         cudaStream_t stream,
                         CUcontext known_context,
                         PeakCudaCaptureTimingGuard* capture_guard,
                         PeakCudaTimingLease* timing)
{
    CUcontext context = known_context;
    if (capture_guard == NULL || timing == NULL) {
        return FALSE;
    }
    if (!peak_cuda_driver_timing_available()) {
        peak_cuda_profiler_state.record_event_create_failure();
        return FALSE;
    }
    if (!capture_guard->try_enter()) {
        if (peak_cuda_capture_tracking_terminal.load(
                std::memory_order_acquire)) {
            peak_cuda_profiler_state.record_capture_query_failure();
        } else {
            peak_cuda_profiler_state.record_stream_capture_skip();
        }
        return FALSE;
    }
    if (!peak_cuda_accepting_events.load(std::memory_order_acquire)) {
        peak_cuda_request_harvester_initialization();
        peak_cuda_profiler_state.record_harvester_unavailable();
        capture_guard->leave();
        return FALSE;
    }
    (void)backend;
    (void)stream;
    if (context == NULL && !peak_cuda_current_context(&context)) {
        capture_guard->leave();
        return FALSE;
    }

    PeakCudaSlotLease lease = {};
    bool admission_closed = false;
    if (!peak_cuda_slot_allocator.acquire_if_accepting(
            reinterpret_cast<std::uintptr_t>(context),
            peak_cuda_current_slot_shard(),
            peak_cuda_accepting_events, &lease, &admission_closed)) {
        if (!admission_closed) {
            peak_cuda_profiler_state.record_pool_full();
        }
        capture_guard->leave();
        return FALSE;
    }
    if (lease.index >= peak_cuda_event_pool.size()) {
        (void)peak_cuda_slot_allocator.release(lease);
        peak_cuda_profiler_state.record_pool_full();
        capture_guard->leave();
        return FALSE;
    }

    PeakCudaEventSlot& slot = peak_cuda_event_pool[lease.index];
    slot.lease = lease;
    if (!slot.initialized) {
        slot.owner_context = context;
        slot.owner_device = -1;
        if (peak_cuda_backend_api.ctx_get_device != NULL) {
            if (peak_cuda_backend_api.ctx_get_device(&slot.owner_device) !=
                CUDA_SUCCESS) {
                peak_cuda_profiler_state.record_context_query_failure();
            }
        }
        CUevent start = NULL;
        CUevent end = NULL;
        if (peak_cuda_backend_api.event_create(
                &start, CU_EVENT_DEFAULT) != CUDA_SUCCESS ||
            peak_cuda_backend_api.event_create(
                &end, CU_EVENT_DEFAULT) != CUDA_SUCCESS) {
            slot.start = reinterpret_cast<cudaEvent_t>(start);
            slot.end = reinterpret_cast<cudaEvent_t>(end);
            peak_cuda_profiler_state.record_event_create_failure();
            if (peak_cuda_destroy_slot_events_current(&slot)) {
                (void)peak_cuda_slot_allocator.release(lease);
            }
            capture_guard->leave();
            return FALSE;
        }
        slot.start = reinterpret_cast<cudaEvent_t>(start);
        slot.end = reinterpret_cast<cudaEvent_t>(end);
        slot.initialized = TRUE;
    } else if (slot.owner_context != context) {
        peak_cuda_profiler_state.record_context_query_failure();
        capture_guard->leave();
        return FALSE;
    }

    timing->lease = lease;
    timing->start = slot.start;
    timing->end = slot.end;
    return TRUE;
}

static gboolean
peak_cuda_record_event(PeakCudaLaunchBackend backend,
                       cudaEvent_t event, cudaStream_t stream)
{
    (void)backend;
    const gboolean recorded = event != NULL &&
        peak_cuda_backend_api.event_record != NULL &&
        peak_cuda_backend_api.event_record(
            reinterpret_cast<CUevent>(event),
            reinterpret_cast<CUstream>(stream)) == CUDA_SUCCESS;
    if (!recorded) {
        peak_cuda_profiler_state.record_timing_error();
        return FALSE;
    }
    return TRUE;
}

static gboolean
peak_cuda_publish_timing(const PeakCudaTimingLease& timing)
{
    if (!peak_cuda_pending_push(timing.lease)) {
        peak_cuda_profiler_state.record_pool_full();
        (void)peak_cuda_discard_lease_current(timing.lease);
        return FALSE;
    }
    peak_cuda_profiler_state.record_launch_accepted(
        timing.lease.shard,
        timing.lease.shard < kPeakCudaSlotExclusiveShardCount);
    return TRUE;
}

static gboolean
peak_cuda_commit_kernel_timing(
    const PeakCudaTimingLease& timing, const char* kernel_name,
    const PeakCudaLaunchDimensions& dimensions, cudaError_t result)
{
    if (timing.lease.index >= peak_cuda_event_pool.size()) {
        return FALSE;
    }
    PeakCudaEventSlot& slot = peak_cuda_event_pool[timing.lease.index];
    slot.record.kind = PEAK_CUDA_LAUNCH_RECORD_KERNEL;
    g_strlcpy(slot.record.kernel_name,
              kernel_name != NULL ? kernel_name : "<unknown-cuda-kernel>",
              sizeof(slot.record.kernel_name));
    slot.record.total_threads = dimensions.total_threads;
    slot.record.grid_size = dimensions.grid_size;
    slot.record.block_size = dimensions.block_size;
    slot.record.result = result;
    return peak_cuda_publish_timing(timing);
}

static gboolean
peak_cuda_commit_graph_timing(const PeakCudaTimingLease& timing,
                              CUgraphExec graph, cudaError_t result)
{
    if (timing.lease.index >= peak_cuda_event_pool.size()) {
        return FALSE;
    }
    PeakCudaEventSlot& slot = peak_cuda_event_pool[timing.lease.index];
    slot.record.kind = PEAK_CUDA_LAUNCH_RECORD_GRAPH;
    slot.record.graph = graph;
    slot.record.result = result;
    return peak_cuda_publish_timing(timing);
}

static void peak_cuda_clear_hook_pointers()
{
    hook_cuda_launch = NULL;
    hook_cuda_launch_cooperative = NULL;
    hook_cuda_launch_cooperative_multiple_device = NULL;
    hook_cuda_launch_exc = NULL;
    hook_cu_launch = NULL;
    hook_cu_launch_cooperative = NULL;
    hook_cu_launch_cooperative_multiple_device = NULL;
    hook_cu_launch_ex = NULL;
    hook_cuda_graph_launch = NULL;
    hook_cu_graph_launch = NULL;
    for (size_t index = 0; index < PEAK_CUDA_CAPTURE_HOOK_COUNT; ++index) {
        peak_cuda_capture_hooks[index] = NULL;
        if (peak_cuda_capture_listeners[index] != NULL) {
            g_object_unref(peak_cuda_capture_listeners[index]);
            peak_cuda_capture_listeners[index] = NULL;
        }
    }
    for (size_t index = 0;
         index < peak_cuda_direct_capture_hook_count;
         ++index) {
        peak_cuda_direct_capture_hooks[index] = NULL;
        if (peak_cuda_direct_capture_listeners[index] != NULL) {
            g_object_unref(peak_cuda_direct_capture_listeners[index]);
            peak_cuda_direct_capture_listeners[index] = NULL;
        }
        peak_cuda_direct_capture_descriptors[index] = {};
    }
    peak_cuda_direct_capture_hook_count = 0;
    peak_cuda_capture_hooks_ready = FALSE;
}

static gboolean
peak_cuda_timing_requested()
{
    return peak_gpu_monitor_all || peak_gpu_hook_address_count > 0;
}

static gboolean
peak_cuda_has_timed_hook()
{
    return hook_cuda_launch != NULL ||
           hook_cuda_launch_cooperative != NULL ||
           hook_cuda_launch_exc != NULL ||
           hook_cu_launch != NULL ||
           hook_cu_launch_cooperative != NULL ||
           hook_cu_launch_ex != NULL ||
           hook_cuda_graph_launch != NULL ||
           hook_cu_graph_launch != NULL;
}

static void
peak_cuda_log_incomplete_records(unsigned int active_wrappers)
{
    static constexpr size_t kDiagnosticLimit = 16;

    if (active_wrappers != 0) {
        peak_log_warn(
            "[peak] CUDA finalization retained %u active launch wrapper(s); their contexts are still application-owned\n",
            active_wrappers);
        return;
    }

    PeakCudaSlotLease leases[kDiagnosticLimit] = {};
    size_t affected = 0;
    if (!peak_cuda_slot_allocator.try_snapshot_active_leases(
            leases, kDiagnosticLimit, &affected)) {
        peak_log_warn(
            "[peak] CUDA incomplete event diagnostics unavailable because slot ownership is busy\n");
        return;
    }
    size_t logged = std::min(affected, kDiagnosticLimit);
    for (size_t index = 0; index < logged; ++index) {
        if (leases[index].index >= peak_cuda_event_pool.size()) {
            continue;
        }
        const PeakCudaEventSlot& slot =
            peak_cuda_event_pool[leases[index].index];
        peak_log_warn(
            "[peak] CUDA incomplete event context=%p device=%d\n",
            reinterpret_cast<void*>(leases[index].context),
            slot.owner_device);
    }
    if (affected > logged) {
        peak_log_warn(
            "[peak] CUDA incomplete event diagnostics omitted=%zu\n",
            affected - logged);
    }
}

static void
peak_cuda_finalize_pending()
{
    if (peak_cuda_finalization_complete || peak_cuda_finalization_timed_out) {
        return;
    }

    peak_cuda_lifecycle_close();
    std::uint64_t started_ns = peak_cuda_monotonic_ns();
    peak_cuda_disable_harvester_initialization();
    std::uint64_t timeout_ns = peak_cuda_finalization_timeout_ms * 1000000ULL;
    std::uint64_t deadline_ns = started_ns >
        std::numeric_limits<std::uint64_t>::max() - timeout_ns
            ? std::numeric_limits<std::uint64_t>::max()
            : started_ns + timeout_ns;
    struct timespec pause = {0, kPeakCudaHarvestRetryNs};
    size_t deadline_incomplete = 0;
    unsigned int deadline_wrappers = 0;
    gboolean deadline_initialization_inflight = FALSE;

    for (;;) {
        unsigned int wrappers = peak_cuda_active_wrapper_count();
        size_t active_slots = peak_cuda_slot_allocator.active_count();
        gboolean initialization_inflight =
            peak_cuda_harvester_initialization_inflight.load(
                std::memory_order_acquire);
        if (active_slots == 0 && wrappers == 0 &&
            !initialization_inflight) {
            peak_cuda_finalization_complete = TRUE;
            break;
        }
        std::uint64_t now_ns = peak_cuda_monotonic_ns();
        if (now_ns == 0 || now_ns >= deadline_ns) {
            break;
        }
        (void)nanosleep(&pause, NULL);
    }

    if (!peak_cuda_finalization_complete) {
        /* Close the helper before taking the cutoff snapshot. It may have
         * released the last lease at the deadline boundary; in that case the
         * no-CUDA-after-release invariant makes a normal join safe. */
        peak_cuda_request_harvester_stop_no_wait();
        deadline_wrappers = peak_cuda_active_wrapper_count();
        deadline_incomplete = peak_cuda_slot_allocator.active_count();
        deadline_initialization_inflight =
            peak_cuda_harvester_initialization_inflight.load(
                std::memory_order_acquire);
        if (deadline_incomplete == 0 && deadline_wrappers == 0 &&
            !deadline_initialization_inflight) {
            peak_cuda_finalization_complete = TRUE;
        }
    }

    if (peak_cuda_finalization_complete) {
        peak_cuda_stop_harvester();
    } else {
        /* A CUDA query may itself be stuck; never extend the deadline by
         * joining the helper. Retain its bounded state for process cleanup. */
        peak_cuda_profiler_state.record_finalization_timeout(
            deadline_incomplete);
        peak_cuda_finalization_timed_out = TRUE;
        peak_log_warn(
            "[peak] CUDA finalization_timeout=1 finalization_incomplete=%zu helper_initialization_inflight=%d; retaining incomplete event state\n",
            deadline_incomplete,
            deadline_initialization_inflight ? 1 : 0);
        peak_cuda_log_incomplete_records(deadline_wrappers);
    }
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    if (func == NULL) {
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    const PeakCudaKernelIdentity* identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity->target_match) {
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    PeakCudaRuntimeLaunchGuard runtime_launch;
    auto call_original = [&]() {
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    };
    const gchar* kernel_label = identity->name.data();
    peak_cuda_record_launch_observed();
    PeakCudaLaunchDimensions dimensions = peak_cuda_launch_dimensions(
        gridDim.x, gridDim.y, gridDim.z,
        blockDim.x, blockDim.y, blockDim.z);

    PeakCudaCaptureTimingGuard capture_guard;
    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                  stream, NULL, &capture_guard, &timing)) {
        return call_original();
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.start, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return call_original();
    }
    cudaError_t result = call_original();
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.end, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_kernel_timing(
        timing, kernel_label, dimensions, result);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_cooperative_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    if (func == NULL) {
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    const PeakCudaKernelIdentity* identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity->target_match) {
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    PeakCudaRuntimeLaunchGuard runtime_launch;
    auto call_original = [&]() {
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    };
    const gchar* kernel_label = identity->name.data();
    peak_cuda_record_launch_observed();
    PeakCudaLaunchDimensions dimensions = peak_cuda_launch_dimensions(
        gridDim.x, gridDim.y, gridDim.z,
        blockDim.x, blockDim.y, blockDim.z);

    PeakCudaCaptureTimingGuard capture_guard;
    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                  stream, NULL, &capture_guard, &timing)) {
        return call_original();
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.start, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return call_original();
    }
    cudaError_t result = call_original();
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.end, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_kernel_timing(
        timing, kernel_label, dimensions, result);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_cooperative_kernel_multiple_device(
    struct cudaLaunchParams* launchParamsList, unsigned int numDevices, unsigned int flags)
{
    if (launchParamsList == NULL || numDevices == 0) {
        return original_cuda_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cuda_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    PeakCudaRuntimeLaunchGuard runtime_launch;
    peak_cuda_record_launch_observed();
    peak_cuda_profiler_state.record_unsupported_multi_device();
    return original_cuda_launch_cooperative_kernel_multiple_device(
        launchParamsList, numDevices, flags);
}

#if defined(PEAK_CUDA_RUNTIME_LAUNCH_EX)
PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_kernel_exc(
    const cudaLaunchConfig_t* config,
    const void* func, void** args)
{
    if (config == NULL || func == NULL) {
        return original_cuda_launch_kernel_exc(config, func, args);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cuda_launch_kernel_exc(config, func, args);
    }
    dim3 gridDim = config->gridDim;
    dim3 blockDim = config->blockDim;
    cudaStream_t stream = config->stream;

    const PeakCudaKernelIdentity* identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity->target_match) {
        return original_cuda_launch_kernel_exc(config, func, args);
    }
    PeakCudaRuntimeLaunchGuard runtime_launch;
    auto call_original = [&]() {
        return original_cuda_launch_kernel_exc(config, func, args);
    };
    const gchar* kernel_label = identity->name.data();
    peak_cuda_record_launch_observed();
    PeakCudaLaunchDimensions dimensions = peak_cuda_launch_dimensions(
        gridDim.x, gridDim.y, gridDim.z,
        blockDim.x, blockDim.y, blockDim.z);

    PeakCudaCaptureTimingGuard capture_guard;
    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                  stream, NULL, &capture_guard, &timing)) {
        return call_original();
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.start, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return call_original();
    }
    cudaError_t result = call_original();
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.end, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_kernel_timing(
        timing, kernel_label, dimensions, result);
    return result;
}
#endif

static void
peak_cuda_record_blocked_driver_launch(gboolean known_target)
{
    /* Kernel identity filtering runs before the capture writer gate so the
     * non-target fast path does not enter that gate. Graph launches are
     * always profiling targets. */
    if (known_target || peak_gpu_monitor_all) {
        peak_cuda_record_launch_observed();
        peak_cuda_profiler_state.record_stream_capture_skip();
    }
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra)
{
    if (func == NULL) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams, extra);
    }
    if (peak_cuda_runtime_launch_wrapper_depth != 0) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams, extra);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams, extra);
    }
    CUcontext context = NULL;
    const PeakCudaKernelIdentity* identity =
        peak_cuda_cached_driver_identity(func, &context);
    if (identity == NULL) {
        if (!peak_cuda_current_context(&context)) {
            return original_cu_launch_kernel(
                func, gridDimX, gridDimY, gridDimZ,
                blockDimX, blockDimY, blockDimZ,
                sharedMemBytes, hStream, kernelParams, extra);
        }
        identity = peak_cuda_identify_kernel((gpointer)func, TRUE, context);
    }
    if (!identity->target_match) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams, extra);
    }
    PeakCudaCaptureTimingGuard capture_guard;
    if (!capture_guard.try_enter()) {
        peak_cuda_record_blocked_driver_launch(TRUE);
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams, extra);
    }
    const gchar* kernel_label = identity->name.data();
    peak_cuda_record_launch_observed();
    PeakCudaLaunchDimensions dimensions = peak_cuda_launch_dimensions(
        gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ);

    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                  (cudaStream_t)hStream, context,
                                  &capture_guard, &timing)) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.start, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
    }
    CUresult result = original_cu_launch_kernel(func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams, extra);
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.end, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_kernel_timing(
        timing, kernel_label, dimensions, (cudaError_t)result);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_cooperative_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams)
{
    if (func == NULL) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams);
    }
    if (peak_cuda_runtime_launch_wrapper_depth != 0) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams);
    }
    CUcontext context = NULL;
    const PeakCudaKernelIdentity* identity =
        peak_cuda_cached_driver_identity(func, &context);
    if (identity == NULL) {
        if (!peak_cuda_current_context(&context)) {
            return original_cu_launch_cooperative_kernel(
                func, gridDimX, gridDimY, gridDimZ,
                blockDimX, blockDimY, blockDimZ,
                sharedMemBytes, hStream, kernelParams);
        }
        identity = peak_cuda_identify_kernel((gpointer)func, TRUE, context);
    }
    if (!identity->target_match) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams);
    }
    PeakCudaCaptureTimingGuard capture_guard;
    if (!capture_guard.try_enter()) {
        peak_cuda_record_blocked_driver_launch(TRUE);
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams);
    }
    const gchar* kernel_label = identity->name.data();
    peak_cuda_record_launch_observed();
    PeakCudaLaunchDimensions dimensions = peak_cuda_launch_dimensions(
        gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ);

    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                  (cudaStream_t)hStream, context,
                                  &capture_guard, &timing)) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams);
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.start, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams);
    }
    CUresult result = original_cu_launch_cooperative_kernel(
                                                func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams);

    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.end, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_kernel_timing(
        timing, kernel_label, dimensions, (cudaError_t)result);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_cooperative_kernel_multiple_device(
    CUDA_LAUNCH_PARAMS* launchParamsList,
    unsigned int numDevices, unsigned int flags)
{
    if (launchParamsList == NULL || numDevices == 0) {
        return original_cu_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    if (peak_cuda_runtime_launch_wrapper_depth != 0) {
        return original_cu_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cu_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    peak_cuda_record_launch_observed();
    peak_cuda_profiler_state.record_unsupported_multi_device();
    return original_cu_launch_cooperative_kernel_multiple_device(
        launchParamsList, numDevices, flags);
}

#if defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_kernel_ex(
    const CUlaunchConfig* config, CUfunction func,
    void** kernelParams, void** extra)
{
    if (config == NULL || func == NULL) {
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    if (peak_cuda_runtime_launch_wrapper_depth != 0) {
        return original_cu_launch_kernel_ex(
            config, func, kernelParams, extra);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cu_launch_kernel_ex(
            config, func, kernelParams, extra);
    }
    unsigned int gridDimX = config->gridDimX;
    unsigned int gridDimY = config->gridDimY;
    unsigned int gridDimZ = config->gridDimZ;
    unsigned int blockDimX = config->blockDimX;
    unsigned int blockDimY = config->blockDimY;
    unsigned int blockDimZ = config->blockDimZ;
    CUstream hStream = config->hStream;

    CUcontext context = NULL;
    const PeakCudaKernelIdentity* identity =
        peak_cuda_cached_driver_identity(func, &context);
    if (identity == NULL) {
        if (!peak_cuda_current_context(&context)) {
            return original_cu_launch_kernel_ex(config, func,
                                                kernelParams, extra);
        }
        identity = peak_cuda_identify_kernel((gpointer)func, TRUE, context);
    }
    if (!identity->target_match) {
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    PeakCudaCaptureTimingGuard capture_guard;
    if (!capture_guard.try_enter()) {
        peak_cuda_record_blocked_driver_launch(TRUE);
        return original_cu_launch_kernel_ex(config, func,
                                            kernelParams, extra);
    }
    const gchar* kernel_label = identity->name.data();
    peak_cuda_record_launch_observed();
    PeakCudaLaunchDimensions dimensions = peak_cuda_launch_dimensions(
        gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ);

    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                  (cudaStream_t)hStream, context,
                                  &capture_guard, &timing)) {
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.start, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    CUresult result = original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.end, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_kernel_timing(
        timing, kernel_label, dimensions, (cudaError_t)result);
    return result;
}
#endif

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_graph_launch(
    cudaGraphExec_t graphExec, cudaStream_t stream)
{
    if (graphExec == NULL) {
        return original_cuda_graph_launch(graphExec, stream);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cuda_graph_launch(graphExec, stream);
    }
    PeakCudaRuntimeLaunchGuard runtime_launch;
    auto call_original = [&]() {
        return original_cuda_graph_launch(graphExec, stream);
    };
    /* Graph executable handles are rank-local; profile them locally only. */
    peak_cuda_record_launch_observed();
    PeakCudaCaptureTimingGuard capture_guard;
    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                  stream, NULL, &capture_guard, &timing)) {
        return call_original();
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.start, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return call_original();
    }
    cudaError_t result = call_original();
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_RUNTIME,
                                timing.end, stream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_graph_timing(timing, graphExec, result);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_graph_launch(
    CUgraphExec hGraphExec, CUstream hStream)
{
    if (hGraphExec == NULL) {
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    if (peak_cuda_runtime_launch_wrapper_depth != 0) {
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    PeakCudaInflightGuard in_flight;
    if (!in_flight.entered()) {
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    /* Graph executable handles are rank-local; profile them locally only. */
    PeakCudaCaptureTimingGuard capture_guard;
    if (!capture_guard.try_enter()) {
        peak_cuda_record_blocked_driver_launch(TRUE);
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    CUcontext context = NULL;
    if (!peak_cuda_current_context(&context)) {
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    peak_cuda_record_launch_observed();
    PeakCudaTimingLease timing = {};
    if (!peak_cuda_acquire_timing(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                  (cudaStream_t)hStream, context,
                                  &capture_guard, &timing)) {
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.start, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    CUresult result = original_cu_graph_launch(hGraphExec, hStream);
    if (!peak_cuda_record_event(PEAK_CUDA_LAUNCH_BACKEND_DRIVER,
                                timing.end, (cudaStream_t)hStream)) {
        (void)peak_cuda_discard_lease_current(timing.lease);
        return result;
    }
    (void)peak_cuda_commit_graph_timing(
        timing, hGraphExec, (cudaError_t)result);
    return result;
}

#if defined(__linux__) && defined(__aarch64__)
static gboolean
peak_cuda_capture_entry_is_arm64_branch(gpointer address)
{
    guint8* copy = NULL;
    gsize bytes_read = 0;
    guint32 instruction = 0;

    if (address == NULL) {
        return FALSE;
    }
    copy = gum_memory_read(address, sizeof(instruction), &bytes_read);
    if (copy == NULL || bytes_read != sizeof(instruction)) {
        g_free(copy);
        return FALSE;
    }
    memcpy(&instruction, copy, sizeof(instruction));
    g_free(copy);
    return (instruction & UINT32_C(0xfc000000)) ==
           UINT32_C(0x14000000);
}
#endif

static gboolean
peak_cuda_driver_typed_replacements_are_isolated()
{
#if !defined(GUM_PEAK_REDIRECT_RESOLVER_API_VERSION)
    return FALSE;
#else
    struct PeakCudaResolvedEntry {
        const char* symbol;
        gpointer canonical;
    };
    static const char* timed_symbols[] = {
        "cuLaunchKernel",
        "cuLaunchCooperativeKernel",
        "cuLaunchCooperativeKernelMultiDevice",
#if defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
        "cuLaunchKernelEx",
#endif
        "cuGraphLaunch",
    };
    PeakCudaResolvedEntry timed[G_N_ELEMENTS(timed_symbols)] = {};
    size_t timed_count = 0;
    for (const char* symbol : timed_symbols) {
        gpointer address = peak_general_listener_find_function(symbol);
        if (address == NULL) {
            continue;
        }
        gpointer canonical = NULL;
        if (!gum_interceptor_peak_resolve_redirect_chain(
                cuda_interceptor, address, &canonical) ||
            canonical == NULL) {
            peak_log_warn(
                "[peak] could not resolve CUDA Driver entry %s; Driver launch timing remains disabled\n",
                symbol);
            return FALSE;
        }
        if (canonical != address) {
            peak_log_warn(
                "[peak] CUDA Driver entry %s redirects through a dispatcher; Driver launch timing remains disabled\n",
                symbol);
            return FALSE;
        }
        timed[timed_count++] = {symbol, canonical};
    }

    for (size_t left = 0; left < timed_count; ++left) {
        for (size_t right = left + 1; right < timed_count; ++right) {
            if (timed[left].canonical == timed[right].canonical) {
                peak_log_warn(
                    "[peak] CUDA Driver entries %s and %s share a dispatcher; Driver launch timing remains disabled\n",
                    timed[left].symbol, timed[right].symbol);
                return FALSE;
            }
        }
    }

    auto non_timed_entry_is_isolated =
        [&](const char* symbol) -> gboolean {
            gpointer address = peak_general_listener_find_function(symbol);
            if (address == NULL) {
                return TRUE;
            }
            gpointer canonical = NULL;
            if (!gum_interceptor_peak_resolve_redirect_chain(
                    cuda_interceptor, address, &canonical) ||
                canonical == NULL) {
                peak_log_warn(
                    "[peak] could not resolve CUDA Driver support entry %s; Driver launch timing remains disabled\n",
                    symbol);
                return FALSE;
            }
            for (size_t index = 0; index < timed_count; ++index) {
                if (address == timed[index].canonical ||
                    canonical == timed[index].canonical) {
                    peak_log_warn(
                        "[peak] CUDA Driver entries %s and %s share an implementation; Driver launch timing remains disabled\n",
                        timed[index].symbol, symbol);
                    return FALSE;
                }
            }
            return TRUE;
        };

    static const char* support_symbols[] = {
        "cuCtxGetCurrent",
        "cuCtxGetDevice",
        "cuCtxPushCurrent_v2",
        "cuCtxPopCurrent_v2",
        "cuStreamIsCapturing",
        "cuThreadExchangeStreamCaptureMode",
        "cuFuncGetName",
        "cuDriverGetVersion",
        "cuGetProcAddress",
        "cuGetProcAddress_v2",
    };
    for (const char* symbol : support_symbols) {
        if (!non_timed_entry_is_isolated(symbol)) {
            return FALSE;
        }
    }
    for (const PeakCudaCaptureHookDescriptor& descriptor :
         peak_cuda_capture_descriptors) {
        if (descriptor.api == PEAK_CUDA_CAPTURE_API_DRIVER &&
            !non_timed_entry_is_isolated(descriptor.symbol)) {
            return FALSE;
        }
    }
    return TRUE;
#endif
}

static GumAttachReturn
peak_cuda_attach_capture_entry(gpointer target,
                               GumInvocationListener* listener)
{
    PeakGumTargetAttachPlan plan = {};
    peak_gum_target_attach_plan(target, &plan);

#if defined(__linux__) && defined(__aarch64__)
    if (peak_cuda_capture_entry_is_arm64_branch(target)) {
#if defined(GUM_PEAK_EXACT_ATTACH_API_VERSION)
        /* NVIDIA Driver exports are one-instruction tail branches. Keep each
         * lifecycle listener at its ABI-specific public entry instead of
         * letting Gum merge the entries at their shared dispatcher. */
        plan = {};
        plan.mutation_address = target;
        plan.mutation_guard_size = sizeof(guint32);
        plan.attach_exact_entry = TRUE;
#else
        return GUM_ATTACH_WRONG_SIGNATURE;
#endif
    }
#endif

    return peak_gum_interceptor_attach_target(cuda_interceptor,
                                              target,
                                              listener,
                                              &plan);
}

static gboolean
peak_cuda_install_capture_hook(PeakCudaCaptureHookIndex index,
                               gboolean* present)
{
    const PeakCudaCaptureHookDescriptor* descriptor =
        &peak_cuda_capture_descriptors[index];
    gpointer* target = static_cast<gpointer*>(
        peak_general_listener_find_function(descriptor->symbol));
    *present = target != NULL;
    if (target == NULL) {
        return FALSE;
    }

    for (size_t previous = 0; previous < index; ++previous) {
        if (peak_cuda_capture_hooks[previous] != target) {
            continue;
        }
        if (peak_cuda_capture_descriptors[previous].operation !=
            descriptor->operation) {
            peak_log_warn(
                "[peak] CUDA capture lifecycle entries %s and %s share an address but have opposite operations; CUDA timing remains disabled\n",
                peak_cuda_capture_descriptors[previous].symbol,
                descriptor->symbol);
            return FALSE;
        }
        peak_cuda_capture_hooks[index] = target;
        return TRUE;
    }

    GumInvocationListener* listener = gum_make_call_listener(
        peak_cuda_capture_listener_on_enter,
        peak_cuda_capture_listener_on_leave,
        const_cast<PeakCudaCaptureHookDescriptor*>(descriptor),
        NULL);
    if (listener == NULL) {
        peak_log_warn(
            "[peak] could not allocate CUDA capture lifecycle listener %s\n",
            descriptor->symbol);
        return FALSE;
    }
    GumAttachReturn result = peak_cuda_attach_capture_entry(target, listener);
    if (result != GUM_ATTACH_OK) {
        peak_log_warn(
            "[peak] could not install CUDA capture lifecycle listener %s (status %d)\n",
            descriptor->symbol, (int)result);
        g_object_unref(listener);
        return FALSE;
    }
    peak_cuda_capture_hooks[index] = target;
    peak_cuda_capture_listeners[index] = listener;
    return TRUE;
}

static gboolean
peak_cuda_install_direct_capture_hook(
    gpointer entry, const char* symbol,
    PeakCudaCaptureOperation operation)
{
    for (size_t index = 0; index < PEAK_CUDA_CAPTURE_HOOK_COUNT;
         ++index) {
        if (reinterpret_cast<gpointer>(peak_cuda_capture_hooks[index]) !=
            entry) {
            continue;
        }
        return peak_cuda_capture_descriptors[index].operation == operation;
    }
    for (size_t index = 0;
         index < peak_cuda_direct_capture_hook_count;
         ++index) {
        if (peak_cuda_direct_capture_hooks[index] != entry) {
            continue;
        }
        return peak_cuda_direct_capture_descriptors[index].operation ==
            operation;
    }
    if (peak_cuda_direct_capture_hook_count >=
        kPeakCudaDirectCaptureHookCapacity) {
        return FALSE;
    }

    size_t index = peak_cuda_direct_capture_hook_count;
    PeakCudaCaptureHookDescriptor* descriptor =
        &peak_cuda_direct_capture_descriptors[index];
    *descriptor = {symbol, PEAK_CUDA_CAPTURE_API_DRIVER, operation};
    GumInvocationListener* listener = gum_make_call_listener(
        peak_cuda_capture_listener_on_enter,
        peak_cuda_capture_listener_on_leave,
        descriptor,
        NULL);
    if (listener == NULL) {
        *descriptor = {};
        return FALSE;
    }
    GumAttachReturn result = peak_cuda_attach_capture_entry(entry, listener);
    if (result != GUM_ATTACH_OK) {
        g_object_unref(listener);
        *descriptor = {};
        return FALSE;
    }
    peak_cuda_direct_capture_hooks[index] = entry;
    peak_cuda_direct_capture_listeners[index] = listener;
    ++peak_cuda_direct_capture_hook_count;
    return TRUE;
}

static gboolean
peak_cuda_capture_entry_point_census(gboolean* available_out)
{
    struct PeakCudaCaptureQuery {
        const char* symbol;
        int introduction_version;
        PeakCudaCaptureOperation operation;
        gboolean requires_v2;
    };
    static const PeakCudaCaptureQuery queries[] = {
        {"cuStreamBeginCapture", 10000,
         PEAK_CUDA_CAPTURE_OPERATION_BEGIN, FALSE},
        {"cuStreamBeginCapture", 10010,
         PEAK_CUDA_CAPTURE_OPERATION_BEGIN, FALSE},
        {"cuStreamEndCapture", 10000,
         PEAK_CUDA_CAPTURE_OPERATION_END, FALSE},
        {"cuStreamBeginCaptureToGraph", 12030,
         PEAK_CUDA_CAPTURE_OPERATION_BEGIN, TRUE},
        {"cuStreamBeginCaptureToCig", 13010,
         PEAK_CUDA_CAPTURE_OPERATION_BEGIN, TRUE},
        {"cuStreamEndCaptureToCig", 13010,
         PEAK_CUDA_CAPTURE_OPERATION_END, TRUE},
        {"cuStreamBeginRecaptureToGraph", 13030,
         PEAK_CUDA_CAPTURE_OPERATION_BEGIN, TRUE},
    };
    static const unsigned long long flags[] = {0, 1, 2};
    typedef CUresult (*PeakCuDriverGetVersionFn)(int* version);

    *available_out = FALSE;
    if (peak_cuda_backend_api.driver_handle == NULL) {
        return TRUE;
    }
    PeakCuGetProcAddressV2Fn get_v2 =
        reinterpret_cast<PeakCuGetProcAddressV2Fn>(dlsym(
            peak_cuda_backend_api.driver_handle,
            "cuGetProcAddress_v2"));
    PeakCuGetProcAddressFn get_v1 =
        reinterpret_cast<PeakCuGetProcAddressFn>(dlsym(
            peak_cuda_backend_api.driver_handle,
            "cuGetProcAddress"));
    if (get_v2 == NULL && get_v1 == NULL) {
        return TRUE;
    }
    *available_out = TRUE;
    PeakCuDriverGetVersionFn get_driver_version =
        reinterpret_cast<PeakCuDriverGetVersionFn>(dlsym(
            peak_cuda_backend_api.driver_handle,
            "cuDriverGetVersion"));
    int driver_version = 0;
    if (get_driver_version == NULL ||
        get_driver_version(&driver_version) != CUDA_SUCCESS ||
        driver_version <= 0) {
        peak_log_warn(
            "[peak] could not determine the CUDA Driver version for capture entry-point validation; CUDA timing remains disabled\n");
        return FALSE;
    }
    gboolean begin_covered = FALSE;
    gboolean end_covered = FALSE;

    for (const PeakCudaCaptureQuery& query : queries) {
        if (query.introduction_version > driver_version) {
            continue;
        }
        if (get_v2 == NULL && query.requires_v2) {
            continue;
        }
        int versions[] = {query.introduction_version, driver_version};
        for (size_t version_index = 0;
             version_index < G_N_ELEMENTS(versions);
             ++version_index) {
            if (version_index != 0 &&
                versions[version_index] == versions[0]) {
                continue;
            }
            for (unsigned long long flag : flags) {
                void* entry = NULL;
                CUresult result;
                int symbol_status = 0;
                if (get_v2 != NULL) {
                    result = get_v2(query.symbol, &entry,
                                    versions[version_index], flag,
                                    &symbol_status);
                } else {
                    result = get_v1(query.symbol, &entry,
                                    versions[version_index], flag);
                }
                if (result != CUDA_SUCCESS) {
                    peak_log_warn(
                        "[peak] CUDA capture entry-point query failed for %s version=%d flags=%llu status=%d; CUDA timing remains disabled\n",
                        query.symbol, versions[version_index], flag,
                        (int)result);
                    return FALSE;
                }
                if (entry == NULL) {
                    if (get_v2 != NULL && symbol_status != 0 &&
                        symbol_status != 1 && symbol_status != 2) {
                        peak_log_warn(
                            "[peak] CUDA capture entry-point query returned an unknown symbol status for %s; CUDA timing remains disabled\n",
                            query.symbol);
                        return FALSE;
                    }
                    if (get_v2 == NULL || symbol_status == 0) {
                        peak_log_warn(
                            "[peak] CUDA capture entry-point query returned no pointer for %s; CUDA timing remains disabled\n",
                            query.symbol);
                        return FALSE;
                    }
                    continue;
                }
                if ((get_v2 != NULL && symbol_status != 0) ||
                    (!peak_cuda_capture_entry_point_is_covered(
                         entry, query.operation) &&
                     !peak_cuda_install_direct_capture_hook(
                         entry, query.symbol, query.operation))) {
                    peak_log_warn(
                        "[peak] CUDA capture entry-point query returned an uninstrumented pointer for %s version=%d flags=%llu; CUDA timing remains disabled\n",
                        query.symbol, versions[version_index], flag);
                    return FALSE;
                }
                if (query.operation ==
                    PEAK_CUDA_CAPTURE_OPERATION_BEGIN) {
                    begin_covered = TRUE;
                } else {
                    end_covered = TRUE;
                }
            }
        }
    }
    if (!begin_covered || !end_covered) {
        peak_log_warn(
            "[peak] CUDA capture entry-point census did not cover both begin and end operations; CUDA timing remains disabled\n");
        return FALSE;
    }
    return TRUE;
}

static GumReplaceReturn
peak_cuda_replace_fast(gpointer address,
                       gpointer replacement,
                       gpointer* original,
                       const char* symbol,
                       uint32_t capability)
{
    GumReplaceReturn result;

    peak_cuda_capabilities.found_apis |= capability;
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* fail_symbol = getenv("PEAK_TEST_FAIL_CUDA_REPLACEMENT");
    if (fail_symbol != NULL && strcmp(fail_symbol, symbol) == 0) {
        result = GUM_REPLACE_WRONG_TYPE;
    } else
#else
    (void)symbol;
#endif
    {
        result = gum_interceptor_replace_fast(
            cuda_interceptor, address, replacement, original, NULL);
    }
    if (result == GUM_REPLACE_OK) {
        peak_cuda_capabilities.installed_apis |= capability;
    } else {
        peak_cuda_capabilities.failed_apis |= capability;
    }
    return result;
}

extern "C" int cuda_interceptor_attach()
{
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_cuda_test_attach_calls.fetch_add(1, std::memory_order_acq_rel);
#endif
    if (!peak_cuda_timing_requested()) {
        return GUM_REPLACE_OK;
    }
    if (cuda_kernel_local_dim_mapping != NULL ||
        cuda_graph_local_mapping != NULL ||
        peak_cuda_harvester_started.load(std::memory_order_acquire) ||
        peak_cuda_finalization_timed_out) {
        peak_log_warn(
            "[peak] refusing to reinitialize active or retained CUDA profiler state\n");
        return -1;
    }
    peak_cuda_lifecycle_close();
    peak_cuda_accepting_events.store(false, std::memory_order_seq_cst);
    peak_cuda_stop_harvester();
    if (!peak_cuda_destroy_event_pool()) {
        return -1;
    }
    cuda_kernel_local_dim_mapping = g_hash_table_new_full(
        g_str_hash, str_equal_function, g_free, g_free);
    g_mutex_init(&cuda_kernel_local_dim_mapping_mutex);
    cuda_graph_local_mapping = g_hash_table_new_full(
        peak_cuda_graph_key_hash, peak_cuda_graph_key_equal, g_free, g_free);
    g_mutex_init(&cuda_graph_local_mapping_mutex);

    GumReplaceReturn replace_check = GUM_REPLACE_OK;
    GumReplaceReturn hook_replace_check = GUM_REPLACE_OK;
    peak_cuda_capabilities = {};
    peak_cuda_capabilities.compiled_apis =
        cuda_interceptor_compiled_api_mask();
    gboolean capture_hook_present[PEAK_CUDA_CAPTURE_HOOK_COUNT] = {};
    gboolean runtime_capture_hook_install_failed = FALSE;
    gboolean driver_capture_hook_install_failed = FALSE;
    if (cuda_interceptor == NULL) {
        cuda_interceptor = gum_interceptor_obtain();
    }
    peak_cuda_hooks_reverted = FALSE;
    peak_cuda_clear_hook_pointers();
    peak_cuda_event_pool_capacity = peak_cuda_parse_event_pool_capacity();
    peak_cuda_graph_identity_capacity = peak_cuda_event_pool_capacity * 4;
    peak_cuda_finalization_timeout_ms =
        peak_cuda_parse_finalization_timeout_ms();
    std::vector<std::string> configured_targets;
    configured_targets.reserve(peak_gpu_hook_address_count);
    for (size_t index = 0; index < peak_gpu_hook_address_count; ++index) {
        if (peak_gpu_hook_strings[index] != NULL) {
            configured_targets.emplace_back(peak_gpu_hook_strings[index]);
        }
    }
    peak_cuda_profiler_state.reset(
        peak_cuda_event_pool_capacity,
        peak_cuda_event_pool_capacity * 4,
        peak_gpu_monitor_all,
        configured_targets);
    peak_cuda_identity_epoch.fetch_add(1, std::memory_order_acq_rel);
    peak_cuda_slot_allocator.reset(peak_cuda_event_pool_capacity);
    peak_cuda_pending_queue.reset(peak_cuda_event_pool_capacity);
    peak_cuda_event_pool.assign(peak_cuda_event_pool_capacity, {});
    pthread_mutex_lock(&peak_cuda_capture_mutex);
    peak_cuda_active_captures.assign(
        kPeakCudaCaptureRegistryCapacity, {});
    peak_cuda_capture_transitions = 0;
    peak_cuda_capture_tracking_terminal.store(
        false, std::memory_order_release);
    peak_cuda_capture_blocked.store(false, std::memory_order_seq_cst);
    for (PeakCudaTimingShard& shard : peak_cuda_timing_shards) {
        shard.active.store(0, std::memory_order_relaxed);
    }
    pthread_mutex_unlock(&peak_cuda_capture_mutex);
    peak_cuda_finalization_complete = FALSE;
    peak_cuda_finalization_timed_out = FALSE;
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_cuda_test_force_incomplete.store(false, std::memory_order_relaxed);
    peak_cuda_test_force_query_error.store(false,
                                           std::memory_order_relaxed);
    peak_cuda_test_harvester_queries.store(
        0, std::memory_order_relaxed);
    peak_cuda_test_pause_capture_begin_flag.store(
        false, std::memory_order_relaxed);
    peak_cuda_test_capture_begin_waiting_flag.store(
        false, std::memory_order_relaxed);
    peak_cuda_test_pause_cuda_section_flag.store(
        false, std::memory_order_relaxed);
    peak_cuda_test_cuda_sections_waiting_count.store(
        0, std::memory_order_relaxed);
    peak_cuda_test_pause_lifecycle_before_increment_flag.store(
        false, std::memory_order_relaxed);
    peak_cuda_test_lifecycle_before_increment_waiting_count.store(
        0, std::memory_order_relaxed);
    peak_cuda_test_pause_lifecycle_after_admission_flag.store(
        false, std::memory_order_relaxed);
    peak_cuda_test_lifecycle_after_admission_waiting_count.store(
        0, std::memory_order_relaxed);
#endif
    /* Publish immutable Driver entry points before any replacement wrapper can
     * run. GPU timing was explicitly requested, so this attach-time lookup is
     * never paid by CPU-only profiling. */
    peak_cuda_resolve_backend_api();
    gboolean driver_typed_replacements_are_isolated =
        peak_cuda_driver_typed_replacements_are_isolated();
    if (!driver_typed_replacements_are_isolated) {
        peak_log_warn(
            "[peak] CUDA Driver exports share an ABI-generic dispatcher; Driver launch timing remains disabled\n");
    }
    gum_interceptor_begin_transaction(cuda_interceptor);

    hook_cuda_launch =
        (gpointer*) peak_general_listener_find_function("cudaLaunchKernel");
    if (hook_cuda_launch) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cuda_launch,
            (gpointer)&peak_cuda_launch_kernel,
            (gpointer*)&original_cuda_launch_kernel,
            "cudaLaunchKernel", PEAK_CUDA_API_RUNTIME_LAUNCH);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_launch = NULL;
        }
    }

    hook_cuda_launch_cooperative =
        (gpointer*) peak_general_listener_find_function("cudaLaunchCooperativeKernel");
    if (hook_cuda_launch_cooperative) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cuda_launch_cooperative,
            (gpointer)&peak_cuda_launch_cooperative_kernel,
            (gpointer*)&original_cuda_launch_cooperative_kernel,
            "cudaLaunchCooperativeKernel",
            PEAK_CUDA_API_RUNTIME_COOPERATIVE);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_launch_cooperative = NULL;
        }
    }

    hook_cuda_launch_cooperative_multiple_device =
        (gpointer*) peak_general_listener_find_function("cudaLaunchCooperativeKernelMultiDevice");
    if (hook_cuda_launch_cooperative_multiple_device) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cuda_launch_cooperative_multiple_device,
            (gpointer)&peak_cuda_launch_cooperative_kernel_multiple_device,
            (gpointer*)&original_cuda_launch_cooperative_kernel_multiple_device,
            "cudaLaunchCooperativeKernelMultiDevice",
            PEAK_CUDA_API_RUNTIME_MULTI_DEVICE);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_launch_cooperative_multiple_device = NULL;
        }
    }

#if defined(PEAK_CUDA_RUNTIME_LAUNCH_EX)
    hook_cuda_launch_exc =
        (gpointer*) peak_general_listener_find_function("cudaLaunchKernelExC");
    if (hook_cuda_launch_exc) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cuda_launch_exc,
            (gpointer)&peak_cuda_launch_kernel_exc,
            (gpointer*)&original_cuda_launch_kernel_exc,
            "cudaLaunchKernelExC", PEAK_CUDA_API_RUNTIME_LAUNCH_EX);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_launch_exc = NULL;
        }
    }
#endif

    hook_cu_launch = driver_typed_replacements_are_isolated
        ? (gpointer*) peak_general_listener_find_function("cuLaunchKernel")
        : NULL;
    if (hook_cu_launch) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cu_launch,
            (gpointer)&peak_cu_launch_kernel,
            (gpointer*)&original_cu_launch_kernel,
            "cuLaunchKernel", PEAK_CUDA_API_DRIVER_LAUNCH);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch = NULL;
        }
    }

    hook_cu_launch_cooperative = driver_typed_replacements_are_isolated
        ? (gpointer*) peak_general_listener_find_function(
            "cuLaunchCooperativeKernel")
        : NULL;
    if (hook_cu_launch_cooperative) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cu_launch_cooperative,
            (gpointer)&peak_cu_launch_cooperative_kernel,
            (gpointer*)&original_cu_launch_cooperative_kernel,
            "cuLaunchCooperativeKernel",
            PEAK_CUDA_API_DRIVER_COOPERATIVE);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch_cooperative = NULL;
        }
    }

    hook_cu_launch_cooperative_multiple_device =
        driver_typed_replacements_are_isolated
            ? (gpointer*) peak_general_listener_find_function(
                "cuLaunchCooperativeKernelMultiDevice")
            : NULL;
    if (hook_cu_launch_cooperative_multiple_device) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cu_launch_cooperative_multiple_device,
            (gpointer)&peak_cu_launch_cooperative_kernel_multiple_device,
            (gpointer*)&original_cu_launch_cooperative_kernel_multiple_device,
            "cuLaunchCooperativeKernelMultiDevice",
            PEAK_CUDA_API_DRIVER_MULTI_DEVICE);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch_cooperative_multiple_device = NULL;
        }
    }

#if defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
    hook_cu_launch_ex = driver_typed_replacements_are_isolated
        ? (gpointer*) peak_general_listener_find_function("cuLaunchKernelEx")
        : NULL;
    if (hook_cu_launch_ex) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cu_launch_ex,
            (gpointer)&peak_cu_launch_kernel_ex,
            (gpointer*)&original_cu_launch_kernel_ex,
            "cuLaunchKernelEx", PEAK_CUDA_API_DRIVER_LAUNCH_EX);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch_ex = NULL;
        }
    }
#endif

    hook_cuda_graph_launch =
        (gpointer*) peak_general_listener_find_function("cudaGraphLaunch");
    if (hook_cuda_graph_launch) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cuda_graph_launch,
            (gpointer)&peak_cuda_graph_launch,
            (gpointer*)&original_cuda_graph_launch,
            "cudaGraphLaunch", PEAK_CUDA_API_RUNTIME_GRAPH);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_graph_launch = NULL;
        }
    }

    hook_cu_graph_launch = driver_typed_replacements_are_isolated
        ? (gpointer*) peak_general_listener_find_function("cuGraphLaunch")
        : NULL;
    if (hook_cu_graph_launch) {
        hook_replace_check = peak_cuda_replace_fast(
            hook_cu_graph_launch,
            (gpointer)&peak_cu_graph_launch,
            (gpointer*)&original_cu_graph_launch,
            "cuGraphLaunch", PEAK_CUDA_API_DRIVER_GRAPH);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_graph_launch = NULL;
        }
    }

    /* Each Runtime launch reaches its corresponding typed Driver entry point.
     * Keep one timing replacement for every pair that is independently
     * available, while retaining the Runtime replacement for any missing
     * Driver entry instead of making unrelated legacy APIs block the fast
     * path. */
    if (driver_typed_replacements_are_isolated) {
        if (hook_cuda_launch != NULL && hook_cu_launch != NULL) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_launch);
            hook_cuda_launch = NULL;
            peak_cuda_capabilities.installed_apis &=
                ~PEAK_CUDA_API_RUNTIME_LAUNCH;
        }
        if (hook_cuda_launch_cooperative != NULL &&
            hook_cu_launch_cooperative != NULL) {
            gum_interceptor_revert(cuda_interceptor,
                                   hook_cuda_launch_cooperative);
            hook_cuda_launch_cooperative = NULL;
            peak_cuda_capabilities.installed_apis &=
                ~PEAK_CUDA_API_RUNTIME_COOPERATIVE;
        }
        if (hook_cuda_launch_cooperative_multiple_device != NULL &&
            hook_cu_launch_cooperative_multiple_device != NULL) {
            gum_interceptor_revert(
                cuda_interceptor,
                hook_cuda_launch_cooperative_multiple_device);
            hook_cuda_launch_cooperative_multiple_device = NULL;
            peak_cuda_capabilities.installed_apis &=
                ~PEAK_CUDA_API_RUNTIME_MULTI_DEVICE;
        }
#if defined(PEAK_CUDA_RUNTIME_LAUNCH_EX) && \
    defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
        if (hook_cuda_launch_exc != NULL && hook_cu_launch_ex != NULL) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_launch_exc);
            hook_cuda_launch_exc = NULL;
            peak_cuda_capabilities.installed_apis &=
                ~PEAK_CUDA_API_RUNTIME_LAUNCH_EX;
        }
#endif
        if (hook_cuda_graph_launch != NULL &&
            hook_cu_graph_launch != NULL) {
            gum_interceptor_revert(cuda_interceptor,
                                   hook_cuda_graph_launch);
            hook_cuda_graph_launch = NULL;
            peak_cuda_capabilities.installed_apis &=
                ~PEAK_CUDA_API_RUNTIME_GRAPH;
        }
    }

    for (size_t hook_index = 0;
         hook_index < PEAK_CUDA_CAPTURE_HOOK_COUNT;
         ++hook_index) {
        PeakCudaCaptureHookIndex index =
            static_cast<PeakCudaCaptureHookIndex>(hook_index);
        if (!driver_typed_replacements_are_isolated &&
            peak_cuda_capture_descriptors[index].api ==
                PEAK_CUDA_CAPTURE_API_DRIVER) {
            continue;
        }
        gboolean present = FALSE;
        gboolean installed = peak_cuda_install_capture_hook(
            index, &present);
        capture_hook_present[index] = present;
        if (present && !installed) {
            if (peak_cuda_capture_descriptors[index].api ==
                PEAK_CUDA_CAPTURE_API_RUNTIME) {
                runtime_capture_hook_install_failed = TRUE;
            } else {
                driver_capture_hook_install_failed = TRUE;
            }
        }
    }

    gboolean runtime_timing_hook =
        hook_cuda_launch != NULL ||
        hook_cuda_launch_cooperative != NULL ||
        hook_cuda_launch_exc != NULL ||
        hook_cuda_graph_launch != NULL;
    gboolean driver_timing_hook =
        hook_cu_launch != NULL ||
        hook_cu_launch_cooperative != NULL ||
        hook_cu_launch_ex != NULL ||
        hook_cu_graph_launch != NULL;
    gboolean runtime_capture_required_ready =
        peak_cuda_capture_hooks[PEAK_CUDA_HOOK_RUNTIME_BEGIN_CAPTURE] != NULL &&
        peak_cuda_capture_hooks[
            PEAK_CUDA_HOOK_RUNTIME_BEGIN_CAPTURE_PTSZ] != NULL &&
        peak_cuda_capture_hooks[PEAK_CUDA_HOOK_RUNTIME_END_CAPTURE] != NULL &&
        peak_cuda_capture_hooks[
            PEAK_CUDA_HOOK_RUNTIME_END_CAPTURE_PTSZ] != NULL;
    gboolean driver_capture_required_ready =
        peak_cuda_capture_hooks[
            PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_V2] != NULL &&
        peak_cuda_capture_hooks[
            PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_V2_PTSZ] != NULL &&
        peak_cuda_capture_hooks[PEAK_CUDA_HOOK_DRIVER_END_CAPTURE] != NULL &&
        peak_cuda_capture_hooks[
            PEAK_CUDA_HOOK_DRIVER_END_CAPTURE_PTSZ] != NULL;
    gboolean cig_pairs_ready =
        capture_hook_present[
            PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_TO_CIG] ==
            capture_hook_present[
                PEAK_CUDA_HOOK_DRIVER_END_CAPTURE_TO_CIG] &&
        capture_hook_present[
            PEAK_CUDA_HOOK_DRIVER_BEGIN_CAPTURE_TO_CIG_PTSZ] ==
            capture_hook_present[
                PEAK_CUDA_HOOK_DRIVER_END_CAPTURE_TO_CIG_PTSZ];
    gboolean any_timing_hook = runtime_timing_hook || driver_timing_hook;
    gboolean capture_entry_point_census_available = FALSE;
    gboolean capture_entry_points_ready = !any_timing_hook ||
        peak_cuda_capture_entry_point_census(
            &capture_entry_point_census_available);
    if (capture_entry_point_census_available &&
        capture_entry_points_ready) {
        driver_capture_required_ready = TRUE;
    }
    if (!capture_entry_points_ready) {
        peak_cuda_profiler_state.record_capture_query_unsupported();
    }
    peak_cuda_capture_hooks_ready = !any_timing_hook ||
        (runtime_capture_required_ready &&
         driver_capture_required_ready && cig_pairs_ready &&
         capture_entry_points_ready &&
         !runtime_capture_hook_install_failed &&
         !driver_capture_hook_install_failed);

    gboolean driver_timing_available =
        peak_cuda_driver_timing_available();
    if (runtime_capture_required_ready ||
        runtime_capture_hook_install_failed) {
        peak_cuda_capabilities.found_apis |=
            PEAK_CUDA_API_RUNTIME_CAPTURE;
    }
    if (driver_capture_required_ready ||
        driver_capture_hook_install_failed ||
        capture_entry_point_census_available) {
        peak_cuda_capabilities.found_apis |=
            PEAK_CUDA_API_DRIVER_CAPTURE;
    }
    if (runtime_capture_required_ready &&
        !runtime_capture_hook_install_failed) {
        peak_cuda_capabilities.installed_apis |=
            PEAK_CUDA_API_RUNTIME_CAPTURE;
    } else if (runtime_timing_hook) {
        peak_cuda_capabilities.failed_apis |=
            PEAK_CUDA_API_RUNTIME_CAPTURE;
    }
    if (driver_capture_required_ready &&
        !driver_capture_hook_install_failed &&
        capture_entry_points_ready) {
        peak_cuda_capabilities.installed_apis |=
            PEAK_CUDA_API_DRIVER_CAPTURE;
    } else if (driver_timing_hook) {
        peak_cuda_capabilities.failed_apis |=
            PEAK_CUDA_API_DRIVER_CAPTURE;
    }
    if (driver_timing_available) {
        peak_cuda_capabilities.found_apis |=
            PEAK_CUDA_API_DRIVER_TIMING;
        peak_cuda_capabilities.installed_apis |=
            PEAK_CUDA_API_DRIVER_TIMING;
    }
    peak_cuda_capabilities.active =
        peak_cuda_has_timed_hook() && peak_cuda_capture_hooks_ready;
    peak_cuda_capabilities.partial =
        peak_cuda_capabilities.failed_apis != 0 ||
        (peak_cuda_has_timed_hook() && !peak_cuda_capture_hooks_ready);

    peak_cuda_lifecycle_open();
    gum_interceptor_end_transaction(cuda_interceptor);
    if (peak_cuda_has_timed_hook() && peak_cuda_capture_hooks_ready) {
        if (!peak_cuda_start_harvester()) {
            peak_cuda_capabilities.active = FALSE;
            peak_cuda_capabilities.partial = TRUE;
        }
    } else if (peak_cuda_has_timed_hook()) {
        peak_cuda_harvester_initialization_terminal.store(
            true, std::memory_order_release);
        peak_log_warn(
            "[peak] CUDA capture lifecycle interception is incomplete (runtime=%d driver=%d optional_pairs=%d entry_points=%d runtime_failure=%d driver_failure=%d); CUDA timing remains disabled\n",
            (int)runtime_capture_required_ready,
            (int)driver_capture_required_ready,
            (int)cig_pairs_ready,
            (int)capture_entry_points_ready,
            (int)runtime_capture_hook_install_failed,
            (int)driver_capture_hook_install_failed);
    }

    return replace_check;
}

extern "C" void cuda_interceptor_dettach()
{
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    if (cuda_interceptor == NULL) {
        return;
    }

    peak_cuda_lifecycle_close();
    peak_cuda_disable_harvester_initialization();
    if (!peak_cuda_hooks_reverted) {
        gum_interceptor_begin_transaction(cuda_interceptor);
        if (hook_cuda_launch) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_launch);
        }
        if (hook_cuda_launch_cooperative) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_launch_cooperative);
        }
        if (hook_cuda_launch_cooperative_multiple_device) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_launch_cooperative_multiple_device);
        }
        if (hook_cuda_launch_exc) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_launch_exc);
        }
        if (hook_cu_launch) {
            gum_interceptor_revert(cuda_interceptor, hook_cu_launch);
        }
        if (hook_cu_launch_cooperative) {
            gum_interceptor_revert(cuda_interceptor, hook_cu_launch_cooperative);
        }
        if (hook_cu_launch_cooperative_multiple_device) {
            gum_interceptor_revert(cuda_interceptor, hook_cu_launch_cooperative_multiple_device);
        }
        if (hook_cu_launch_ex) {
            gum_interceptor_revert(cuda_interceptor, hook_cu_launch_ex);
        }
        if (hook_cuda_graph_launch) {
            gum_interceptor_revert(cuda_interceptor, hook_cuda_graph_launch);
        }
        if (hook_cu_graph_launch) {
            gum_interceptor_revert(cuda_interceptor, hook_cu_graph_launch);
        }
        for (size_t index = 0; index < PEAK_CUDA_CAPTURE_HOOK_COUNT;
             ++index) {
            if (peak_cuda_capture_listeners[index] != NULL) {
                gum_interceptor_detach(
                    cuda_interceptor, peak_cuda_capture_listeners[index]);
            }
        }
        for (size_t index = 0;
             index < peak_cuda_direct_capture_hook_count;
             ++index) {
            if (peak_cuda_direct_capture_listeners[index] != NULL) {
                gum_interceptor_detach(
                    cuda_interceptor,
                    peak_cuda_direct_capture_listeners[index]);
            }
        }
        gum_interceptor_end_transaction(cuda_interceptor);

        if (!gum_interceptor_flush(cuda_interceptor)) {
            peak_log_warn("[peak] CUDA interceptor teardown did not flush; leaving CUDA interceptor state alive\n");
            return;
        }

        peak_cuda_hooks_reverted = TRUE;
        peak_cuda_clear_hook_pointers();
    }

    unsigned int active_cuda_wrappers = peak_cuda_active_wrapper_count();
    if (active_cuda_wrappers != 0) {
        peak_log_warn("[peak] CUDA interceptor teardown observed %u active wrapper(s); keeping all CUDA state alive for cleanup retry\n",
                   active_cuda_wrappers);
        /*
         * A wrapper may still hold addresses into peak_cuda_event_pool before
         * its in-flight guard became observable. Do not drain maps, destroy
         * events, or clear identity state until a later detach observes zero.
         */
        return;
    }

    peak_cuda_finalize_pending();
    if (peak_cuda_finalization_timed_out) {
        peak_log_warn("[peak] retaining CUDA interceptor state after bounded finalization timeout\n");
        return;
    }
    if (!peak_cuda_destroy_event_pool()) {
        return;
    }

    if (cuda_kernel_local_dim_mapping != NULL) {
        g_hash_table_destroy(cuda_kernel_local_dim_mapping);
        cuda_kernel_local_dim_mapping = NULL;
        g_mutex_clear(&cuda_kernel_local_dim_mapping_mutex);
    }
    if (cuda_graph_local_mapping != NULL) {
        g_hash_table_destroy(cuda_graph_local_mapping);
        cuda_graph_local_mapping = NULL;
        g_mutex_clear(&cuda_graph_local_mapping_mutex);
    }
    /*
     * Keep cuda_interceptor referenced after physical detach. A target thread may
     * already be at a wrapper entry before PeakCudaInflightGuard executes, and
     * that wrapper can still need Gum's original trampoline. State is retained
     * unchanged on an active-wrapper observation and can be cleaned on retry.
     */
}

static void cuda_interceptor_print_graph_result(GHashTable* hashTable)
{
    gboolean have_output = FALSE;
    GHashTableIter iter;
    gpointer key, value;

    g_mutex_lock(&cuda_graph_local_mapping_mutex);
    g_hash_table_iter_init(&iter, hashTable);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GraphRecordInfo* graph_info = (GraphRecordInfo*) value;
        if (graph_info->total_graph_call_cnt > 0) {
            have_output = TRUE;
            break;
        }
    }

    if (have_output) {
        const guint row_width = 100;
        const guint max_col_width = 9;

        char* space_separator = (char *) malloc(row_width + 1);
        char* row_separator = (char *)  malloc(row_width + 1);
        memset(space_separator, ' ', row_width);
        memset(row_separator, '-', row_width);
        space_separator[row_width] = '\0';
        row_separator[row_width] = '\0';

        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sGPU STATISTICS (Graph)%*s\n",
            (row_width - 15) / 2, "",
            (row_width - 15 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);

        // Section: Graph call count & time
        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sGRAPH STATISTICS (GPU)%*s\n",
            (row_width - 26) / 2, "",
            (row_width - 26 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);
        peak_log_report("| %*s | %*s | %*s | %*s | %*s | %*s | %*s |\n",
            max_col_width, "Context",
            max_col_width, "Device",
            max_col_width, "Graph",
            max_col_width, "Calls",
            max_col_width, "Total(s)",
            max_col_width, "Max(s)",
            max_col_width, "Min(s)");
        peak_log_report("%s\n", row_separator);

        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            PeakCudaGraphKey* graph_key = (PeakCudaGraphKey*) key;
            GraphRecordInfo* graph_info = (GraphRecordInfo*) value;
            peak_log_report("| %p | %*d | %p | %*" G_GUINT64_FORMAT
                            " | %*.6f | %*.6f | %*.6f |\n",
                reinterpret_cast<void*>(graph_key->context),
                max_col_width, graph_info->device,
                reinterpret_cast<void*>(graph_key->graph),
                max_col_width, (guint64)graph_info->total_graph_call_cnt,
                max_col_width, graph_info->total_time,
                max_col_width, graph_info->max_time,
                max_col_width, graph_info->min_time);
        }
        peak_log_report("%s\n", row_separator);

        free(space_separator);
        free(row_separator);
    }
    g_mutex_unlock(&cuda_graph_local_mapping_mutex);
}

static PeakReportSnapshot*
peak_cuda_build_report_snapshot()
{
    static constexpr size_t kCounterCount = 25;
    std::vector<std::pair<std::string, KernelDimInfo>> kernels;
    PeakReportSnapshot* snapshot;
    size_t index = 0;

    g_mutex_lock(&cuda_kernel_local_dim_mapping_mutex);
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&iter, cuda_kernel_local_dim_mapping);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        kernels.emplace_back((const gchar*)key, *(KernelDimInfo*)value);
    }
    g_mutex_unlock(&cuda_kernel_local_dim_mapping_mutex);
    std::sort(kernels.begin(), kernels.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });
    if (kernels.size() > (SIZE_MAX - kCounterCount) / 4) {
        return NULL;
    }
    snapshot = peak_report_snapshot_create(
        kernels.size() * 4 + kCounterCount);
    if (snapshot == NULL || !peak_report_snapshot_set_program(snapshot, "CUDA")) {
        peak_report_snapshot_destroy(snapshot);
        return NULL;
    }
    snapshot->overhead = peak_report_snapshot_get_transport_overhead();
    snapshot->rank_count = 1;
    for (const auto& entry : kernels) {
        const KernelDimInfo& info = entry.second;
        const gchar* suffixes[] = {" [time]", " [threads]", " [block]", " [grid]"};
        for (const gchar* suffix : suffixes) {
            gchar* name = g_strdup_printf("CUDA kernel: %s%s",
                                          entry.first.c_str(), suffix);
            gboolean named = name != NULL &&
                peak_report_snapshot_set_name(snapshot, index, name);
            g_free(name);
            if (!named) {
                peak_report_snapshot_destroy(snapshot);
                return NULL;
            }
            snapshot->instrumented[index++] = 1;
        }
        index -= 4;
        snapshot->num_calls[index] = (unsigned long)std::min<std::uint64_t>(
            info.total_kernel_call_cnt, ULONG_MAX);
        snapshot->total_time[index] = info.total_time;
        snapshot->max_total_time[index] = info.max_time;
        snapshot->min_total_time[index++] = info.min_time;
        snapshot->num_calls[index] = (unsigned long)std::min<std::uint64_t>(
            info.total_gpu_threads, ULONG_MAX);
        snapshot->max_total_time[index] = (double)info.max_gpu_threads;
        snapshot->min_total_time[index++] = (double)info.min_gpu_threads;
        snapshot->num_calls[index] = (unsigned long)std::min<std::uint64_t>(
            info.total_block_size, ULONG_MAX);
        snapshot->max_total_time[index] = (double)info.max_block_size;
        snapshot->min_total_time[index++] = (double)info.min_block_size;
        snapshot->num_calls[index] = (unsigned long)std::min<std::uint64_t>(
            info.total_grid_size, ULONG_MAX);
        snapshot->max_total_time[index] = (double)info.max_grid_size;
        snapshot->min_total_time[index++] = (double)info.min_grid_size;
    }
    PeakCudaProfilerCounters counters = peak_cuda_profiler_state.counters();
    PeakCudaSlotAllocatorCounters slots = peak_cuda_slot_allocator.counters();
    const gchar* counter_names[kCounterCount] = {
        "CUDA profiler counter [observed]",
        "CUDA profiler counter [accepted]",
        "CUDA profiler counter [completed]",
        "CUDA profiler counter [pool_high_water]",
        "CUDA profiler counter [pool_full]",
        "CUDA profiler counter [identity_full]",
        "CUDA profiler counter [positive_identity_admission_failed]",
        "CUDA profiler counter [negative_identity_overflow]",
        "CUDA profiler counter [monitor_all_identity_overflow]",
        "CUDA profiler counter [identity_overflow_suppressed]",
        "CUDA profiler counter [event_create_failed]",
        "CUDA profiler counter [timing_error]",
        "CUDA profiler counter [harvester_unavailable]",
        "CUDA profiler counter [stream_capture_skipped]",
        "CUDA profiler counter [capture_query_failed]",
        "CUDA profiler counter [capture_query_unsupported]",
        "CUDA profiler counter [unsupported_multi_device]",
        "CUDA profiler counter [event_query_failed]",
        "CUDA profiler counter [elapsed_failed]",
        "CUDA profiler counter [context_query_failed]",
        "CUDA profiler counter [context_switch_failed]",
        "CUDA profiler counter [context_restore_failed]",
        "CUDA profiler counter [finalization_timeout]",
        "CUDA profiler counter [finalization_incomplete]",
        "CUDA profiler counter [dimension_overflow]",
    };
    const std::uint64_t counter_values[kCounterCount] = {
        counters.observed_launches,
        counters.accepted_launches,
        counters.completed_launches,
        slots.high_water_slots,
        counters.dropped_pool_full,
        counters.dropped_identity_full,
        counters.positive_identity_admission_failures,
        counters.negative_identity_overflow,
        counters.monitor_all_identity_overflow,
        counters.repeated_identity_overflow_suppressed,
        counters.dropped_event_create,
        counters.dropped_timing_error,
        counters.dropped_harvester_unavailable,
        counters.dropped_stream_capture,
        counters.dropped_capture_query,
        counters.dropped_capture_query_unsupported,
        counters.dropped_unsupported_multi_device,
        counters.dropped_event_query,
        counters.dropped_elapsed_time,
        counters.dropped_context_query,
        counters.dropped_context_switch,
        counters.dropped_context_restore,
        counters.finalization_timeouts,
        counters.finalization_incomplete,
        counters.dropped_dimension_overflow,
    };
    for (size_t counter = 0; counter < kCounterCount; ++counter) {
        if (!peak_report_snapshot_set_name(
                snapshot, index, counter_names[counter])) {
            peak_report_snapshot_destroy(snapshot);
            return NULL;
        }
        snapshot->instrumented[index] = 1;
        snapshot->num_calls[index++] =
            (unsigned long)std::min<std::uint64_t>(
                counter_values[counter], ULONG_MAX);
    }
    return snapshot;
}

static gboolean
peak_cuda_render_report_snapshot(const PeakReportSnapshot* snapshot)
{
    const size_t prefix_length = strlen("CUDA kernel: ");
    const size_t suffix_length = strlen(" [time]");
    gboolean have_output = FALSE;
    enum PeakCudaReportCounter {
        PEAK_CUDA_COUNTER_OBSERVED = 0,
        PEAK_CUDA_COUNTER_ACCEPTED,
        PEAK_CUDA_COUNTER_COMPLETED,
        PEAK_CUDA_COUNTER_POOL_HIGH_WATER,
        PEAK_CUDA_COUNTER_POOL_FULL,
        PEAK_CUDA_COUNTER_IDENTITY_FULL,
        PEAK_CUDA_COUNTER_POSITIVE_IDENTITY_ADMISSION_FAILED,
        PEAK_CUDA_COUNTER_NEGATIVE_IDENTITY_OVERFLOW,
        PEAK_CUDA_COUNTER_MONITOR_ALL_IDENTITY_OVERFLOW,
        PEAK_CUDA_COUNTER_IDENTITY_OVERFLOW_SUPPRESSED,
        PEAK_CUDA_COUNTER_EVENT_CREATE_FAILED,
        PEAK_CUDA_COUNTER_TIMING_ERROR,
        PEAK_CUDA_COUNTER_HARVESTER_UNAVAILABLE,
        PEAK_CUDA_COUNTER_STREAM_CAPTURE_SKIPPED,
        PEAK_CUDA_COUNTER_CAPTURE_QUERY_FAILED,
        PEAK_CUDA_COUNTER_CAPTURE_QUERY_UNSUPPORTED,
        PEAK_CUDA_COUNTER_UNSUPPORTED_MULTI_DEVICE,
        PEAK_CUDA_COUNTER_EVENT_QUERY_FAILED,
        PEAK_CUDA_COUNTER_ELAPSED_FAILED,
        PEAK_CUDA_COUNTER_CONTEXT_QUERY_FAILED,
        PEAK_CUDA_COUNTER_CONTEXT_SWITCH_FAILED,
        PEAK_CUDA_COUNTER_CONTEXT_RESTORE_FAILED,
        PEAK_CUDA_COUNTER_FINALIZATION_TIMEOUT,
        PEAK_CUDA_COUNTER_FINALIZATION_INCOMPLETE,
        PEAK_CUDA_COUNTER_DIMENSION_OVERFLOW,
        PEAK_CUDA_COUNTER_COUNT,
    };
    const gchar* counter_suffixes[PEAK_CUDA_COUNTER_COUNT] = {
        "[observed]",
        "[accepted]",
        "[completed]",
        "[pool_high_water]",
        "[pool_full]",
        "[identity_full]",
        "[positive_identity_admission_failed]",
        "[negative_identity_overflow]",
        "[monitor_all_identity_overflow]",
        "[identity_overflow_suppressed]",
        "[event_create_failed]",
        "[timing_error]",
        "[harvester_unavailable]",
        "[stream_capture_skipped]",
        "[capture_query_failed]",
        "[capture_query_unsupported]",
        "[unsupported_multi_device]",
        "[event_query_failed]",
        "[elapsed_failed]",
        "[context_query_failed]",
        "[context_switch_failed]",
        "[context_restore_failed]",
        "[finalization_timeout]",
        "[finalization_incomplete]",
        "[dimension_overflow]",
    };
    guint64 counters[PEAK_CUDA_COUNTER_COUNT] = {};

    if (snapshot == NULL) {
        return FALSE;
    }
    for (size_t i = 0; i + 3 < snapshot->hook_count; ++i) {
        const gchar* name = snapshot->names[i];
        size_t length;
        if (name == NULL || !g_str_has_prefix(name, "CUDA kernel: ")) {
            continue;
        }
        length = strlen(name);
        if (length < prefix_length + suffix_length ||
            strcmp(name + length - suffix_length, " [time]") != 0) {
            continue;
        }
        if (!have_output) {
            peak_log_report("\nGPU STATISTICS (Kernel)\n");
            peak_log_report("| Kernel | Calls | Total(s) | Max(s) | Min(s) | AvgThr | MaxThr | MinThr | AvgBlk | MaxBlk | MinBlk | AvgGrid | MaxGrid | MinGrid |\n");
            have_output = TRUE;
        }
        std::string label(name + prefix_length, length - prefix_length - suffix_length);
        const double calls = (double)snapshot->num_calls[i];
        peak_log_report("| %s | %lu | %.6f | %.6f | %.6f | %.2f | %.0f | %.0f | %.2f | %.0f | %.0f | %.2f | %.0f | %.0f |\n",
                        label.c_str(), snapshot->num_calls[i], snapshot->total_time[i],
                        snapshot->max_total_time[i], snapshot->min_total_time[i],
                        calls != 0.0 ? (double)snapshot->num_calls[i + 1] / calls : 0.0,
                        snapshot->max_total_time[i + 1], snapshot->min_total_time[i + 1],
                        calls != 0.0 ? (double)snapshot->num_calls[i + 2] / calls : 0.0,
                        snapshot->max_total_time[i + 2], snapshot->min_total_time[i + 2],
                        calls != 0.0 ? (double)snapshot->num_calls[i + 3] / calls : 0.0,
                        snapshot->max_total_time[i + 3], snapshot->min_total_time[i + 3]);
    }
    for (size_t i = 0; i < snapshot->hook_count; ++i) {
        const gchar* name = snapshot->names[i];
        if (name == NULL) {
            continue;
        }
        for (size_t counter = 0; counter < PEAK_CUDA_COUNTER_COUNT;
             ++counter) {
            if (g_str_has_suffix(name, counter_suffixes[counter])) {
                counters[counter] = snapshot->num_calls[i];
                break;
            }
        }
    }
    peak_log_report(
        "[peak] CUDA profiler samples: observed=%" G_GUINT64_FORMAT
        " accepted=%" G_GUINT64_FORMAT " completed=%" G_GUINT64_FORMAT
        " pool_high_water=%" G_GUINT64_FORMAT "\n",
        counters[PEAK_CUDA_COUNTER_OBSERVED],
        counters[PEAK_CUDA_COUNTER_ACCEPTED],
        counters[PEAK_CUDA_COUNTER_COMPLETED],
        counters[PEAK_CUDA_COUNTER_POOL_HIGH_WATER]);
    peak_log_report(
        "[peak] CUDA profiler capabilities: compiled_runtime_base=1"
#if defined(PEAK_CUDA_RUNTIME_LAUNCH_EX)
        " compiled_runtime_ex=1"
#else
        " compiled_runtime_ex=0"
#endif
        " compiled_driver_base=1"
#if defined(PEAK_CUDA_DRIVER_LAUNCH_EX)
        " compiled_driver_ex=1"
#else
        " compiled_driver_ex=0"
#endif
        " installed_runtime_base=%d installed_runtime_ex=%d"
        " installed_driver_base=%d installed_driver_ex=%d"
        " driver_timing=%d\n",
        (peak_cuda_capabilities.installed_apis &
         (PEAK_CUDA_API_RUNTIME_LAUNCH |
          PEAK_CUDA_API_RUNTIME_COOPERATIVE |
          PEAK_CUDA_API_RUNTIME_GRAPH)) != 0,
        (peak_cuda_capabilities.installed_apis &
         PEAK_CUDA_API_RUNTIME_LAUNCH_EX) != 0,
        (peak_cuda_capabilities.installed_apis &
         (PEAK_CUDA_API_DRIVER_LAUNCH |
          PEAK_CUDA_API_DRIVER_COOPERATIVE |
          PEAK_CUDA_API_DRIVER_GRAPH)) != 0,
        (peak_cuda_capabilities.installed_apis &
         PEAK_CUDA_API_DRIVER_LAUNCH_EX) != 0,
        (peak_cuda_capabilities.installed_apis &
         PEAK_CUDA_API_DRIVER_TIMING) != 0);
    peak_log_report(
        "[peak] CUDA profiler drops: pool_full=%" G_GUINT64_FORMAT
        " identity_full=%" G_GUINT64_FORMAT
        " positive_identity_admission_failed=%" G_GUINT64_FORMAT
        " negative_identity_overflow=%" G_GUINT64_FORMAT
        " monitor_all_identity_overflow=%" G_GUINT64_FORMAT
        " identity_overflow_suppressed=%" G_GUINT64_FORMAT
        " event_create_failed=%" G_GUINT64_FORMAT
        " timing_error=%" G_GUINT64_FORMAT
        " harvester_unavailable=%" G_GUINT64_FORMAT
        " stream_capture_skipped=%" G_GUINT64_FORMAT
        " capture_query_failed=%" G_GUINT64_FORMAT
        " capture_query_unsupported=%" G_GUINT64_FORMAT
        " unsupported_multi_device=%" G_GUINT64_FORMAT
        " event_query_failed=%" G_GUINT64_FORMAT
        " elapsed_failed=%" G_GUINT64_FORMAT
        " context_query_failed=%" G_GUINT64_FORMAT
        " context_switch_failed=%" G_GUINT64_FORMAT
        " context_restore_failed=%" G_GUINT64_FORMAT
        " dimension_overflow=%" G_GUINT64_FORMAT "\n",
        counters[PEAK_CUDA_COUNTER_POOL_FULL],
        counters[PEAK_CUDA_COUNTER_IDENTITY_FULL],
        counters[PEAK_CUDA_COUNTER_POSITIVE_IDENTITY_ADMISSION_FAILED],
        counters[PEAK_CUDA_COUNTER_NEGATIVE_IDENTITY_OVERFLOW],
        counters[PEAK_CUDA_COUNTER_MONITOR_ALL_IDENTITY_OVERFLOW],
        counters[PEAK_CUDA_COUNTER_IDENTITY_OVERFLOW_SUPPRESSED],
        counters[PEAK_CUDA_COUNTER_EVENT_CREATE_FAILED],
        counters[PEAK_CUDA_COUNTER_TIMING_ERROR],
        counters[PEAK_CUDA_COUNTER_HARVESTER_UNAVAILABLE],
        counters[PEAK_CUDA_COUNTER_STREAM_CAPTURE_SKIPPED],
        counters[PEAK_CUDA_COUNTER_CAPTURE_QUERY_FAILED],
        counters[PEAK_CUDA_COUNTER_CAPTURE_QUERY_UNSUPPORTED],
        counters[PEAK_CUDA_COUNTER_UNSUPPORTED_MULTI_DEVICE],
        counters[PEAK_CUDA_COUNTER_EVENT_QUERY_FAILED],
        counters[PEAK_CUDA_COUNTER_ELAPSED_FAILED],
        counters[PEAK_CUDA_COUNTER_CONTEXT_QUERY_FAILED],
        counters[PEAK_CUDA_COUNTER_CONTEXT_SWITCH_FAILED],
        counters[PEAK_CUDA_COUNTER_CONTEXT_RESTORE_FAILED],
        counters[PEAK_CUDA_COUNTER_DIMENSION_OVERFLOW]);
    peak_log_report(
        "[peak] CUDA profiler finalization: finalization_timeout=%"
        G_GUINT64_FORMAT " finalization_incomplete=%" G_GUINT64_FORMAT "\n",
        counters[PEAK_CUDA_COUNTER_FINALIZATION_TIMEOUT],
        counters[PEAK_CUDA_COUNTER_FINALIZATION_INCOMPLETE]);
    return TRUE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_force_incomplete_events(int enabled)
{
    peak_cuda_test_force_incomplete.store(
        enabled != 0, std::memory_order_relaxed);
}

PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_force_query_error_once(int enabled)
{
    peak_cuda_test_force_query_error.store(
        enabled != 0, std::memory_order_relaxed);
}

PEAK_CUDA_WRAPPER_EXPORT unsigned long long
peak_cuda_test_harvester_query_count(void)
{
    return peak_cuda_test_harvester_queries.load(
        std::memory_order_relaxed);
}

PEAK_CUDA_WRAPPER_EXPORT int
peak_cuda_test_harvester_ready(void)
{
    return peak_cuda_accepting_events.load(std::memory_order_acquire) ? 1 : 0;
}

PEAK_CUDA_WRAPPER_EXPORT unsigned long long
peak_cuda_test_active_slot_count(void)
{
    return peak_cuda_slot_allocator.active_count();
}

PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_pause_capture_begin(int enabled)
{
    peak_cuda_test_pause_capture_begin_flag.store(
        enabled != 0, std::memory_order_release);
}

PEAK_CUDA_WRAPPER_EXPORT int
peak_cuda_test_capture_begin_waiting(void)
{
    return peak_cuda_test_capture_begin_waiting_flag.load(
               std::memory_order_acquire)
        ? 1
        : 0;
}

PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_pause_cuda_section(int enabled)
{
    peak_cuda_test_pause_cuda_section_flag.store(
        enabled != 0, std::memory_order_release);
}

PEAK_CUDA_WRAPPER_EXPORT unsigned int
peak_cuda_test_cuda_sections_waiting(void)
{
    return peak_cuda_test_cuda_sections_waiting_count.load(
        std::memory_order_acquire);
}

PEAK_CUDA_WRAPPER_EXPORT int
peak_cuda_test_capture_blocked(void)
{
    return peak_cuda_capture_blocked.load(
               std::memory_order_acquire)
        ? 1
        : 0;
}

PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_pause_lifecycle_before_increment(int enabled)
{
    peak_cuda_test_pause_lifecycle_before_increment_flag.store(
        enabled != 0, std::memory_order_release);
}

PEAK_CUDA_WRAPPER_EXPORT unsigned int
peak_cuda_test_lifecycle_before_increment_waiting(void)
{
    return peak_cuda_test_lifecycle_before_increment_waiting_count.load(
        std::memory_order_acquire);
}

PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_pause_lifecycle_after_admission(int enabled)
{
    peak_cuda_test_pause_lifecycle_after_admission_flag.store(
        enabled != 0, std::memory_order_release);
}

PEAK_CUDA_WRAPPER_EXPORT unsigned int
peak_cuda_test_lifecycle_after_admission_waiting(void)
{
    return peak_cuda_test_lifecycle_after_admission_waiting_count.load(
        std::memory_order_acquire);
}

PEAK_CUDA_WRAPPER_EXPORT int
peak_cuda_test_lifecycle_closing(void)
{
    return (peak_cuda_lifecycle_epoch.load(std::memory_order_seq_cst) & 1ULL)
        != 0 ? 1 : 0;
}

PEAK_CUDA_WRAPPER_EXPORT void
peak_cuda_test_finalize(void)
{
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    if (cuda_kernel_local_dim_mapping != NULL &&
        cuda_graph_local_mapping != NULL) {
        peak_cuda_finalize_pending();
    }
}
#endif

extern "C" void cuda_interceptor_print_with_mpi_job_policy(
    int aggregation_mode, int active_mpi_job) {
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    if (cuda_kernel_local_dim_mapping == NULL ||
        cuda_graph_local_mapping == NULL) {
        return;
    }
    peak_cuda_finalize_pending();
    PeakReportSnapshot* local = peak_cuda_build_report_snapshot();
    if (local != NULL) {
#ifdef HAVE_MPI
        if (aggregation_mode == PEAK_CUDA_OUTPUT_AGGREGATION_MPI) {
            PeakReportSnapshot* aggregate = NULL;
            PeakMpiReportTransportResult result =
                peak_mpi_report_transport_reduce(local, &aggregate);
            if (result == PEAK_MPI_REPORT_TRANSPORT_ROOT_READY) {
                (void)peak_cuda_render_report_snapshot(aggregate);
            } else if (result != PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE) {
                (void)peak_cuda_render_report_snapshot(local);
            }
            peak_report_snapshot_destroy(aggregate);
        } else
#endif
        if (aggregation_mode == PEAK_CUDA_OUTPUT_AGGREGATION_SOCKET) {
            PeakSocketReportSession* session = NULL;
            PeakReportSnapshot* aggregate = NULL;
            PeakSocketReportRankSource socket_rank_source = active_mpi_job
                ? PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED
                : PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV;
            PeakSocketReportStatus status = peak_socket_report_transport_begin_channel(
                local, socket_rank_source,
                PEAK_SOCKET_REPORT_CHANNEL_CUDA, &session, &aggregate);
            if (status == PEAK_SOCKET_REPORT_SINGLE_READY) {
                (void)peak_cuda_render_report_snapshot(aggregate);
            } else if (status == PEAK_SOCKET_REPORT_ROOT_PREPARED) {
                if (peak_cuda_render_report_snapshot(aggregate)) {
                    (void)peak_socket_report_transport_commit(session);
                } else {
                    peak_socket_report_transport_abort(session);
                    (void)peak_cuda_render_report_snapshot(local);
                }
            } else if (status != PEAK_SOCKET_REPORT_PEER_RELEASED) {
                peak_socket_report_transport_abort(session);
                (void)peak_cuda_render_report_snapshot(local);
            }
            peak_report_snapshot_destroy(aggregate);
        } else {
            (void)peak_cuda_render_report_snapshot(local);
        }
        peak_report_snapshot_destroy(local);
    }
    cuda_interceptor_print_graph_result(cuda_graph_local_mapping);
}

extern "C" void
cuda_interceptor_print(int is_MPI)
{
    cuda_interceptor_print_with_mpi_job_policy(
        is_MPI ? PEAK_CUDA_OUTPUT_AGGREGATION_MPI :
                 PEAK_CUDA_OUTPUT_AGGREGATION_LOCAL,
        FALSE);
}
