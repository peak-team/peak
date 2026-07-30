#include <algorithm>

#include "cuda_interceptor.h"
#include "internal/cuda_profiler_state.h"
#include "internal/general_listener/report_snapshot.h"
#include "internal/general_listener/socket_report_transport.h"
#ifdef HAVE_MPI
#include "internal/general_listener/mpi_report_transport.h"
#endif
#include "logging.h"

#define PEAK_CUDA_WRAPPER_EXPORT extern "C" __attribute__((visibility("default")))

extern "C" gpointer peak_general_listener_find_function(const char* symbol);

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

static cudaError_t (*original_cuda_launch_kernel_exc)(
    const cudaLaunchConfig_t* config,
    const void* func, void** args);

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

static CUresult (*original_cu_launch_kernel_ex)(
    const CUlaunchConfig* config, CUfunction func,
    void** kernelParams, void** extra);

static cudaError_t (*original_cuda_graph_launch)(
    cudaGraphExec_t graphExec, cudaStream_t stream);

static CUresult (*original_cu_graph_launch)(
    CUgraphExec hGraphExec, CUstream hStream);

/* cuFuncGetName was added after this project's minimum CUDA 11.2 support. */
typedef CUresult (*PeakCudaFuncGetNameFn)(const char** name,
                                          CUfunction function);
static PeakCudaFuncGetNameFn peak_cuda_func_get_name;
static std::once_flag peak_cuda_func_get_name_once;
static PeakCudaProfilerState peak_cuda_profiler_state;

struct PeakCudaEventSlot {
    cudaEvent_t start;
    cudaEvent_t end;
    gboolean initialized;
    gboolean leased;
};

static std::vector<PeakCudaEventSlot> peak_cuda_event_pool;
static std::vector<size_t> peak_cuda_free_event_slots;
static std::mutex peak_cuda_event_pool_mutex;
static size_t peak_cuda_event_pool_capacity = 256;

static gchar*
peak_cuda_driver_kernel_name(CUfunction function)
{
    const char* name = NULL;

    std::call_once(peak_cuda_func_get_name_once, []() {
        peak_cuda_func_get_name = reinterpret_cast<PeakCudaFuncGetNameFn>(
            peak_general_listener_find_function("cuFuncGetName"));
    });
    if (peak_cuda_func_get_name == NULL ||
        peak_cuda_func_get_name(&name, function) != CUDA_SUCCESS ||
        name == NULL || name[0] == '\0') {
        return NULL;
    }
    return g_strdup(name);
}

typedef struct {
    gulong total_gpu_threads;
    gulong max_gpu_threads;
    gulong min_gpu_threads;
    gulong total_kernel_call_cnt;
    gulong max_kernel_call_cnt;
    gulong min_kernel_call_cnt;
    gulong total_block_size;
    gulong max_block_size;
    gulong min_block_size;
    gulong total_grid_size;
    gulong max_grid_size;
    gulong min_grid_size;
    gdouble total_time;
    gdouble min_time;
    gdouble max_time;
} KernelDimInfo;

typedef struct {
    gchar* kernel_name;
    gulong total_threads;
    gulong grid_size;
    gulong block_size;
    cudaEvent_t* start_event;
    cudaEvent_t* end_event;
    cudaError_t result;
} KernelLaunchInfo;

struct KernelLaunchSeries{
    std::vector<KernelLaunchInfo> launches;
    std::mutex mtx;
};

typedef struct {
    gulong total_graph_call_cnt;
    gulong max_graph_call_cnt;
    gulong min_graph_call_cnt;
    gdouble total_time;
    gdouble min_time;
    gdouble max_time;
} GraphRecordInfo;

typedef struct {
    CUgraphExec_st* graph;
    cudaEvent_t* start_event;
    cudaEvent_t* end_event;
    cudaError_t result;
} GraphLaunchInfo;

struct GraphLaunchSeries{
    std::vector<GraphLaunchInfo> launches;
    std::mutex mtx;
};

static std::unordered_map<std::string, KernelLaunchSeries> peak_kernel_event_map;
static std::unordered_map<CUgraphExec_st*, GraphLaunchSeries> peak_graph_event_map;
static std::mutex peak_kernel_event_map_mutex;
static std::mutex peak_graph_event_map_mutex;
static std::mutex peak_cuda_lifecycle_mutex;
static std::atomic_bool peak_cuda_accepting_events{false};
static std::atomic_uint peak_cuda_in_flight{0};
static gboolean peak_cuda_hooks_reverted;

class PeakCudaInflightGuard {
public:
    PeakCudaInflightGuard()
    {
        peak_cuda_in_flight.fetch_add(1, std::memory_order_acq_rel);
    }

    ~PeakCudaInflightGuard()
    {
        peak_cuda_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
};

gboolean str_equal_function(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

char* cu_demangle(char* mangled_name);

static PeakCudaKernelIdentity
peak_cuda_identify_kernel(gpointer identity, gboolean driver_function)
{
    PeakCudaKernelIdentity cached;
    std::vector<std::string> targets;
    gchar* resolved_name = NULL;
    char* demangled_name = NULL;
    char* target_name = NULL;

    if (peak_cuda_profiler_state.cached_identity(
            reinterpret_cast<std::uintptr_t>(identity), driver_function, &cached)) {
        return cached;
    }
    for (size_t index = 0; index < peak_gpu_hook_address_count; ++index) {
        if (peak_gpu_hook_strings[index] != NULL) {
            targets.emplace_back(peak_gpu_hook_strings[index]);
        }
    }
    if (driver_function) {
        resolved_name = peak_cuda_driver_kernel_name(
            reinterpret_cast<CUfunction>(identity));
    } else {
        resolved_name = gum_symbol_name_from_address(identity);
    }
    if (resolved_name != NULL) {
        demangled_name = cu_demangle(resolved_name);
        target_name = extract_function_name(demangled_name);
    }
    PeakCudaKernelIdentity result = peak_cuda_profiler_state.identify(
        reinterpret_cast<std::uintptr_t>(identity),
        driver_function,
        demangled_name,
        target_name,
        peak_gpu_monitor_all,
        targets);
    g_free(resolved_name);
    free(demangled_name);
    free(target_name);
    return result;
}

char* cu_demangle(char* mangled_name) {
    return mangled_name != NULL ? cxa_demangle(mangled_name) : NULL;
}

static void update_kernel_map_info(const gchar* kernel_name, gulong total_threads, gulong grid_size, gulong block_size, gdouble elapsed_sec)
{
    gchar* key = g_strdup(kernel_name);
    KernelDimInfo* dim_info = (KernelDimInfo*) g_hash_table_lookup(cuda_kernel_local_dim_mapping, key);

    if (!dim_info) {
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
        g_free(key);
        dim_info->total_gpu_threads += total_threads;
        dim_info->total_kernel_call_cnt++;
        dim_info->total_block_size += block_size;
        dim_info->total_grid_size += grid_size;
        dim_info->total_time += elapsed_sec;
        dim_info->max_gpu_threads = std::max(dim_info->max_gpu_threads, total_threads);
        dim_info->min_gpu_threads = std::min(dim_info->min_gpu_threads, total_threads);
        dim_info->max_block_size = std::max(dim_info->max_block_size, block_size);
        dim_info->min_block_size = std::min(dim_info->min_block_size, block_size);
        dim_info->max_grid_size = std::max(dim_info->max_grid_size, grid_size);
        dim_info->min_grid_size = std::min(dim_info->min_grid_size, grid_size);
        dim_info->max_time = std::max(dim_info->max_time, elapsed_sec);
        dim_info->min_time = std::min(dim_info->min_time, elapsed_sec);
    }
}

void insert_cuda_mapping_record(gchar* kernel_name, gulong total_threads, gulong grid_size, gulong block_size, gdouble elapsed_sec)
{
    g_mutex_lock(&cuda_kernel_local_dim_mapping_mutex);
    update_kernel_map_info(kernel_name != NULL ? kernel_name : "<unknown>",
                           total_threads, grid_size, block_size, elapsed_sec);
    g_mutex_unlock(&cuda_kernel_local_dim_mapping_mutex);
}

void insert_cuda_graph_record(CUgraphExec_st* graph, gdouble elapsed_sec)
{
    g_mutex_lock(&cuda_graph_local_mapping_mutex);
    GraphRecordInfo* graph_info = (GraphRecordInfo*) g_hash_table_lookup(cuda_graph_local_mapping, graph);
    if (!graph_info) {
        graph_info = g_new(GraphRecordInfo, 1);
        graph_info->total_graph_call_cnt = 1;
        graph_info->total_time = elapsed_sec;
        graph_info->max_time = elapsed_sec;
        graph_info->min_time = elapsed_sec;
        g_hash_table_insert(cuda_graph_local_mapping, graph, graph_info);
    } else {
        graph_info->total_graph_call_cnt++;
        graph_info->total_time += elapsed_sec;
        graph_info->max_time = std::max(graph_info->max_time, elapsed_sec);
        graph_info->min_time = std::min(graph_info->min_time, elapsed_sec);
    }
    g_mutex_unlock(&cuda_graph_local_mapping_mutex);
}

static size_t
peak_cuda_parse_event_pool_capacity()
{
    const gchar* value = g_getenv("PEAK_CUDA_EVENT_POOL_CAPACITY");
    gchar* end = NULL;
    guint64 parsed;

    if (value == NULL || value[0] == '\0') {
        return 256;
    }
    parsed = g_ascii_strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > 65536) {
        peak_log_warn("[peak] invalid PEAK_CUDA_EVENT_POOL_CAPACITY=%s; using 256\n",
                      value);
        return 256;
    }
    return (size_t)parsed;
}

static void
peak_cuda_destroy_event_pool()
{
    std::lock_guard<std::mutex> lock(peak_cuda_event_pool_mutex);
    for (PeakCudaEventSlot& slot : peak_cuda_event_pool) {
        if (slot.start != NULL) {
            cudaEventDestroy(slot.start);
        }
        if (slot.end != NULL) {
            cudaEventDestroy(slot.end);
        }
    }
    peak_cuda_event_pool.clear();
    peak_cuda_free_event_slots.clear();
}

static gboolean
peak_cuda_acquire_event_pair(cudaEvent_t** start_event, cudaEvent_t** end_event)
{
    if (start_event == NULL || end_event == NULL ||
        !peak_cuda_accepting_events.load(std::memory_order_acquire) ||
        !peak_cuda_profiler_state.acquire_slot()) {
        return FALSE;
    }

    std::lock_guard<std::mutex> lock(peak_cuda_event_pool_mutex);
    if (peak_cuda_free_event_slots.empty()) {
        peak_cuda_profiler_state.release_slot();
        return FALSE;
    }
    PeakCudaEventSlot& slot =
        peak_cuda_event_pool[peak_cuda_free_event_slots.back()];
    peak_cuda_free_event_slots.pop_back();
    if (!slot.initialized) {
        if (cudaEventCreate(&slot.start) != cudaSuccess ||
            cudaEventCreate(&slot.end) != cudaSuccess) {
            if (slot.start != NULL) {
                cudaEventDestroy(slot.start);
                slot.start = NULL;
            }
            if (slot.end != NULL) {
                cudaEventDestroy(slot.end);
                slot.end = NULL;
            }
            peak_cuda_free_event_slots.push_back(
                (size_t)(&slot - peak_cuda_event_pool.data()));
            peak_cuda_profiler_state.record_event_create_failure();
            peak_cuda_profiler_state.release_slot();
            return FALSE;
        }
        slot.initialized = TRUE;
    }
    slot.leased = TRUE;
    *start_event = &slot.start;
    *end_event = &slot.end;
    return TRUE;
}

static gboolean
peak_cuda_record_event(cudaEvent_t* event, cudaStream_t stream)
{
    if (event == NULL || *event == NULL ||
        cudaEventRecord(*event, stream) != cudaSuccess) {
        peak_cuda_profiler_state.record_timing_error();
        return FALSE;
    }
    return TRUE;
}

static void
peak_cuda_release_event_pair(cudaEvent_t* start_event, cudaEvent_t* end_event)
{
    (void)end_event;
    if (start_event == NULL) {
        return;
    }
    std::lock_guard<std::mutex> lock(peak_cuda_event_pool_mutex);
    for (size_t index = 0; index < peak_cuda_event_pool.size(); ++index) {
        PeakCudaEventSlot& slot = peak_cuda_event_pool[index];
        if (&slot.start == start_event && slot.leased) {
            slot.leased = FALSE;
            peak_cuda_free_event_slots.push_back(index);
            peak_cuda_profiler_state.release_slot();
            return;
        }
    }
}

static void peak_cuda_release_kernel_launch(KernelLaunchInfo* launch,
                                            gboolean record_elapsed)
{
    if (launch == NULL) {
        return;
    }

    if (record_elapsed &&
        launch->start_event != NULL &&
        launch->end_event != NULL &&
        *(launch->start_event) != NULL &&
        *(launch->end_event) != NULL) {
        float ms = 0.0f;
        if (cudaEventElapsedTime(&ms, *(launch->start_event),
                                 *(launch->end_event)) != cudaSuccess) {
            peak_cuda_profiler_state.record_timing_error();
        } else if (launch->result == cudaSuccess) {
            insert_cuda_mapping_record(
                launch->kernel_name,
                launch->total_threads,
                launch->grid_size,
                launch->block_size,
                ms / 1000.0
            );
        }
    }

    peak_cuda_release_event_pair(launch->start_event, launch->end_event);
    launch->start_event = NULL;
    launch->end_event = NULL;
    g_free(launch->kernel_name);
    launch->kernel_name = NULL;
}

static void peak_cuda_release_graph_launch(GraphLaunchInfo* launch,
                                           gboolean record_elapsed)
{
    if (launch == NULL) {
        return;
    }

    if (record_elapsed &&
        launch->start_event != NULL &&
        launch->end_event != NULL &&
        *(launch->start_event) != NULL &&
        *(launch->end_event) != NULL) {
        float ms = 0.0f;
        if (cudaEventElapsedTime(&ms, *(launch->start_event),
                                 *(launch->end_event)) != cudaSuccess) {
            peak_cuda_profiler_state.record_timing_error();
        } else if (launch->result == cudaSuccess) {
            insert_cuda_graph_record(launch->graph, ms / 1000.0);
        }
    }

    peak_cuda_release_event_pair(launch->start_event, launch->end_event);
    launch->start_event = NULL;
    launch->end_event = NULL;
}

static void peak_cuda_enqueue_kernel_launch(const gchar* kernel_name,
                                            KernelLaunchInfo* info)
{
    const gchar* key = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gboolean accepted = FALSE;

    {
        std::lock_guard<std::mutex> map_lock(peak_kernel_event_map_mutex);
        if (peak_cuda_accepting_events.load(std::memory_order_acquire)) {
            auto& series = peak_kernel_event_map[key];
            std::lock_guard<std::mutex> lock(series.mtx);
            series.launches.push_back(*info);
            accepted = TRUE;
        }
    }

    if (!accepted) {
        peak_cuda_release_kernel_launch(info, FALSE);
    }
}

static void peak_cuda_enqueue_graph_launch(CUgraphExec_st* graph,
                                           GraphLaunchInfo* info)
{
    gboolean accepted = FALSE;

    {
        std::lock_guard<std::mutex> map_lock(peak_graph_event_map_mutex);
        if (peak_cuda_accepting_events.load(std::memory_order_acquire)) {
            auto& series = peak_graph_event_map[graph];
            std::lock_guard<std::mutex> lock(series.mtx);
            series.launches.push_back(*info);
            accepted = TRUE;
        }
    }

    if (!accepted) {
        peak_cuda_release_graph_launch(info, FALSE);
    }
}

static void peak_cuda_drain_kernel_event_map(gboolean record_elapsed)
{
    std::lock_guard<std::mutex> map_lock(peak_kernel_event_map_mutex);
    for (auto& [_, series] : peak_kernel_event_map) {
        std::lock_guard<std::mutex> lock(series.mtx);
        for (auto& launch : series.launches) {
            peak_cuda_release_kernel_launch(&launch, record_elapsed);
        }
        series.launches.clear();
    }
    peak_kernel_event_map.clear();
}

static void peak_cuda_drain_graph_event_map(gboolean record_elapsed)
{
    std::lock_guard<std::mutex> map_lock(peak_graph_event_map_mutex);
    for (auto& [_, series] : peak_graph_event_map) {
        std::lock_guard<std::mutex> lock(series.mtx);
        for (auto& launch : series.launches) {
            peak_cuda_release_graph_launch(&launch, record_elapsed);
        }
        series.launches.clear();
    }
    peak_graph_event_map.clear();
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
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    PeakCudaInflightGuard in_flight;
    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity.target_match) {
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    if (!peak_cuda_record_event(start, stream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cuda_launch_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    cudaError_t result = original_cuda_launch_kernel(func, gridDim, blockDim, args, sharedMem, stream);
    if (!peak_cuda_record_event(end, stream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_cooperative_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    PeakCudaInflightGuard in_flight;
    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity.target_match) {
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    if (!peak_cuda_record_event(start, stream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cuda_launch_cooperative_kernel(
            func, gridDim, blockDim, args, sharedMem, stream);
    }
    cudaError_t result = original_cuda_launch_cooperative_kernel(func, gridDim, blockDim, args, sharedMem, stream);
    if (!peak_cuda_record_event(end, stream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_cooperative_kernel_multiple_device(
    struct cudaLaunchParams* launchParamsList, unsigned int numDevices, unsigned int flags)
{
    PeakCudaInflightGuard in_flight;
    const void* func = launchParamsList->func;
    dim3 gridDim = launchParamsList->gridDim;
    dim3 blockDim = launchParamsList->blockDim;
    cudaStream_t stream = launchParamsList->stream;

    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity.target_match) {
        return original_cuda_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cuda_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    if (!peak_cuda_record_event(start, stream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cuda_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    cudaError_t result = original_cuda_launch_cooperative_kernel_multiple_device(launchParamsList, numDevices, flags);
    if (!peak_cuda_record_event(end, stream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_kernel_exc(
    const cudaLaunchConfig_t* config,
    const void* func, void** args)
{
    PeakCudaInflightGuard in_flight;
    dim3 gridDim = config->gridDim;
    dim3 blockDim = config->blockDim;
    cudaStream_t stream = config->stream;

    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, FALSE);
    if (!identity.target_match) {
        return original_cuda_launch_kernel_exc(config, func, args);
    }
    const gchar* kernel_label = identity.name.c_str();


    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cuda_launch_kernel_exc(config, func, args);
    }
    if (!peak_cuda_record_event(start, stream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cuda_launch_kernel_exc(config, func, args);
    }
    cudaError_t result = original_cuda_launch_kernel_exc(config, func, args);
    if (!peak_cuda_record_event(end, stream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra)
{
    PeakCudaInflightGuard in_flight;
    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, TRUE);
    if (!identity.target_match) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams, extra);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
    }
    if (!peak_cuda_record_event(start, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cu_launch_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
    }
    CUresult result = original_cu_launch_kernel(func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams, extra);
    if (!peak_cuda_record_event(end, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_cooperative_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams)
{
    PeakCudaInflightGuard in_flight;
    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, TRUE);
    if (!identity.target_match) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ,
            blockDimX, blockDimY, blockDimZ,
            sharedMemBytes, hStream, kernelParams);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams);
    }
    if (!peak_cuda_record_event(start, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cu_launch_cooperative_kernel(
            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY,
            blockDimZ, sharedMemBytes, hStream, kernelParams);
    }
    CUresult result = original_cu_launch_cooperative_kernel(
                                                func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams);

    if (!peak_cuda_record_event(end, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_cooperative_kernel_multiple_device(
    CUDA_LAUNCH_PARAMS* launchParamsList,
    unsigned int numDevices, unsigned int flags)
{
    PeakCudaInflightGuard in_flight;
    CUfunction func = launchParamsList->function;
    unsigned int gridDimX = launchParamsList->gridDimX;
    unsigned int gridDimY = launchParamsList->gridDimY;
    unsigned int gridDimZ = launchParamsList->gridDimZ;
    unsigned int blockDimX = launchParamsList->blockDimX;
    unsigned int blockDimY = launchParamsList->blockDimY;
    unsigned int blockDimZ = launchParamsList->blockDimZ;
    CUstream hStream = launchParamsList->hStream;

    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, TRUE);
    if (!identity.target_match) {
        return original_cu_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cu_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    if (!peak_cuda_record_event(start, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cu_launch_cooperative_kernel_multiple_device(
            launchParamsList, numDevices, flags);
    }
    CUresult result = original_cu_launch_cooperative_kernel_multiple_device(
                                    launchParamsList, numDevices, flags);
    if (!peak_cuda_record_event(end, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_kernel_ex(
    const CUlaunchConfig* config, CUfunction func,
    void** kernelParams, void** extra)
{
    PeakCudaInflightGuard in_flight;
    unsigned int gridDimX = config->gridDimX;
    unsigned int gridDimY = config->gridDimY;
    unsigned int gridDimZ = config->gridDimZ;
    unsigned int blockDimX = config->blockDimX;
    unsigned int blockDimY = config->blockDimY;
    unsigned int blockDimZ = config->blockDimZ;
    CUstream hStream = config->hStream;

    PeakCudaKernelIdentity identity =
        peak_cuda_identify_kernel((gpointer)func, TRUE);
    if (!identity.target_match) {
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    const gchar* kernel_label = identity.name.c_str();
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    if (!peak_cuda_record_event(start, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    }
    CUresult result = original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    if (!peak_cuda_record_event(end, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    KernelLaunchInfo info = {
        .kernel_name = g_strdup(kernel_label),
        .total_threads = total_threads,
        .grid_size = grid_size,
        .block_size = block_size,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_kernel_launch(kernel_label, &info);
    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_graph_launch(
    cudaGraphExec_t graphExec, cudaStream_t stream)
{
    PeakCudaInflightGuard in_flight;
    /* Graph executable handles are rank-local; profile them locally only. */
    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cuda_graph_launch(graphExec, stream);
    }
    if (!peak_cuda_record_event(start, (cudaStream_t)stream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cuda_graph_launch(graphExec, stream);
    }
    cudaError_t result = original_cuda_graph_launch(graphExec, stream);
    if (!peak_cuda_record_event(end, (cudaStream_t)stream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    GraphLaunchInfo info = {
        .graph = graphExec,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_graph_launch(graphExec, &info);

    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_graph_launch(
    CUgraphExec hGraphExec, CUstream hStream)
{
    PeakCudaInflightGuard in_flight;
    /* Graph executable handles are rank-local; profile them locally only. */
    cudaEvent_t* start = NULL;
    cudaEvent_t* end = NULL;
    if (!peak_cuda_acquire_event_pair(&start, &end)) {
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    if (!peak_cuda_record_event(start, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return original_cu_graph_launch(hGraphExec, hStream);
    }
    CUresult result = original_cu_graph_launch(hGraphExec, hStream);
    if (!peak_cuda_record_event(end, (cudaStream_t)hStream)) {
        peak_cuda_release_event_pair(start, end);
        return result;
    }

    GraphLaunchInfo info = {
        .graph = hGraphExec,
        .start_event = start,
        .end_event = end,
        .result = (cudaError_t)result
    };
    peak_cuda_enqueue_graph_launch(hGraphExec, &info);

    return result;
}

extern "C" int cuda_interceptor_attach()
{
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    peak_cuda_accepting_events.store(false, std::memory_order_release);
    cuda_kernel_local_dim_mapping = g_hash_table_new_full(g_str_hash, str_equal_function, NULL, g_free);
    g_mutex_init(&cuda_kernel_local_dim_mapping_mutex);
    cuda_graph_local_mapping = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    g_mutex_init(&cuda_graph_local_mapping_mutex);

    GumReplaceReturn replace_check = GUM_REPLACE_OK;
    GumReplaceReturn hook_replace_check = GUM_REPLACE_OK;
    if (cuda_interceptor == NULL) {
        cuda_interceptor = gum_interceptor_obtain();
    }
    peak_cuda_hooks_reverted = FALSE;
    peak_cuda_clear_hook_pointers();

    gum_interceptor_begin_transaction(cuda_interceptor);

    peak_cuda_destroy_event_pool();
    peak_cuda_event_pool_capacity = peak_cuda_parse_event_pool_capacity();
    peak_cuda_profiler_state.reset(peak_cuda_event_pool_capacity,
                                   peak_cuda_event_pool_capacity * 4);
    {
        std::lock_guard<std::mutex> event_lock(peak_cuda_event_pool_mutex);
        peak_cuda_event_pool.reserve(peak_cuda_event_pool_capacity);
        peak_cuda_free_event_slots.reserve(peak_cuda_event_pool_capacity);
        for (size_t i = 0; i < peak_cuda_event_pool_capacity; ++i) {
            peak_cuda_event_pool.push_back({NULL, NULL, FALSE, FALSE});
            peak_cuda_free_event_slots.push_back(i);
        }
    }

    hook_cuda_launch =
        (gpointer*) peak_general_listener_find_function("cudaLaunchKernel");
    if (hook_cuda_launch) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch,
            (gpointer)&peak_cuda_launch_kernel,
            (gpointer*)&original_cuda_launch_kernel,
            NULL);
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
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch_cooperative,
            (gpointer)&peak_cuda_launch_cooperative_kernel,
            (gpointer*)&original_cuda_launch_cooperative_kernel,
            NULL);
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
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch_cooperative_multiple_device,
            (gpointer)&peak_cuda_launch_cooperative_kernel_multiple_device,
            (gpointer*)&original_cuda_launch_cooperative_kernel_multiple_device,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_launch_cooperative_multiple_device = NULL;
        }
    }

    hook_cuda_launch_exc =
        (gpointer*) peak_general_listener_find_function("cudaLaunchKernelExC");
    if (hook_cuda_launch_exc) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch_exc,
            (gpointer)&peak_cuda_launch_kernel_exc,
            (gpointer*)&original_cuda_launch_kernel_exc,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_launch_exc = NULL;
        }
    }

    hook_cu_launch =
        (gpointer*) peak_general_listener_find_function("cuLaunchKernel");
    if (hook_cu_launch) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch,
            (gpointer)&peak_cu_launch_kernel,
            (gpointer*)&original_cu_launch_kernel,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch = NULL;
        }
    }

    hook_cu_launch_cooperative =
        (gpointer*) peak_general_listener_find_function("cuLaunchCooperativeKernel");
    if (hook_cu_launch_cooperative) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch_cooperative,
            (gpointer)&peak_cu_launch_cooperative_kernel,
            (gpointer*)&original_cu_launch_cooperative_kernel,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch_cooperative = NULL;
        }
    }

    hook_cu_launch_cooperative_multiple_device =
        (gpointer*) peak_general_listener_find_function("cuLaunchCooperativeKernelMultiDevice");
    if (hook_cu_launch_cooperative_multiple_device) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch_cooperative_multiple_device,
            (gpointer)&peak_cu_launch_cooperative_kernel_multiple_device,
            (gpointer*)&original_cu_launch_cooperative_kernel_multiple_device,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch_cooperative_multiple_device = NULL;
        }
    }

    hook_cu_launch_ex =
        (gpointer*) peak_general_listener_find_function("cuLaunchKernelEx");
    if (hook_cu_launch_ex) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch_ex,
            (gpointer)&peak_cu_launch_kernel_ex,
            (gpointer*)&original_cu_launch_kernel_ex,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_launch_ex = NULL;
        }
    }

    hook_cuda_graph_launch =
        (gpointer*) peak_general_listener_find_function("cudaGraphLaunch");
    if (hook_cuda_graph_launch) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_graph_launch,
            (gpointer)&peak_cuda_graph_launch,
            (gpointer*)&original_cuda_graph_launch,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cuda_graph_launch = NULL;
        }
    }

    hook_cu_graph_launch =
        (gpointer*) peak_general_listener_find_function("cuGraphLaunch");
    if (hook_cu_graph_launch) {
        hook_replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_graph_launch,
            (gpointer)&peak_cu_graph_launch,
            (gpointer*)&original_cu_graph_launch,
            NULL);
        if (hook_replace_check != GUM_REPLACE_OK) {
            if (replace_check == GUM_REPLACE_OK) {
                replace_check = hook_replace_check;
            }
            hook_cu_graph_launch = NULL;
        }
    }

    gum_interceptor_end_transaction(cuda_interceptor);
    peak_cuda_accepting_events.store(true, std::memory_order_release);

    return replace_check;
}

extern "C" void cuda_interceptor_dettach()
{
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    if (cuda_interceptor == NULL) {
        return;
    }

    peak_cuda_accepting_events.store(false, std::memory_order_release);
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
        gum_interceptor_end_transaction(cuda_interceptor);

        if (!gum_interceptor_flush(cuda_interceptor)) {
            peak_log_warn("[peak] CUDA interceptor teardown did not flush; leaving CUDA interceptor state alive\n");
            return;
        }

        peak_cuda_hooks_reverted = TRUE;
        peak_cuda_clear_hook_pointers();
    }

    unsigned int active_cuda_wrappers = peak_cuda_in_flight.load(std::memory_order_acquire);
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

    peak_cuda_drain_kernel_event_map(FALSE);
    peak_cuda_drain_graph_event_map(FALSE);

    peak_cuda_destroy_event_pool();

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
        peak_log_report("| %*s | %*s | %*s | %*s | %*s |\n",
            max_col_width, "Graph",
            max_col_width, "Calls",
            max_col_width, "Total(s)",
            max_col_width, "Max(s)",
            max_col_width, "Min(s)");
        peak_log_report("%s\n", row_separator);

        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            GraphRecordInfo* graph_info = (GraphRecordInfo*) value;
            peak_log_report("| %p | %*lu | %*.6f | %*.6f | %*.6f |\n",
                key,
                max_col_width, graph_info->total_graph_call_cnt,
                max_col_width, graph_info->total_time,
                max_col_width, graph_info->max_time,
                max_col_width, graph_info->min_time);
        }
        peak_log_report("%s\n", row_separator);

        free(space_separator);
        free(row_separator);
    }
}

static void cuda_sync_kernel_event() {
    cudaDeviceSynchronize();
    peak_cuda_drain_kernel_event_map(TRUE);
    peak_cuda_drain_graph_event_map(TRUE);
}

static PeakReportSnapshot*
peak_cuda_build_report_snapshot()
{
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
    if (kernels.size() > (SIZE_MAX - 4) / 4) {
        return NULL;
    }
    snapshot = peak_report_snapshot_create(kernels.size() * 4 + 4);
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
        snapshot->num_calls[index] = info.total_kernel_call_cnt;
        snapshot->total_time[index] = info.total_time;
        snapshot->max_total_time[index] = info.max_time;
        snapshot->min_total_time[index++] = info.min_time;
        snapshot->num_calls[index] = info.total_gpu_threads;
        snapshot->max_total_time[index] = (double)info.max_gpu_threads;
        snapshot->min_total_time[index++] = (double)info.min_gpu_threads;
        snapshot->num_calls[index] = info.total_block_size;
        snapshot->max_total_time[index] = (double)info.max_block_size;
        snapshot->min_total_time[index++] = (double)info.min_block_size;
        snapshot->num_calls[index] = info.total_grid_size;
        snapshot->max_total_time[index] = (double)info.max_grid_size;
        snapshot->min_total_time[index++] = (double)info.min_grid_size;
    }
    PeakCudaProfilerCounters counters = peak_cuda_profiler_state.counters();
    const gchar* drops[] = {"CUDA profiler drops [pool_full]",
                            "CUDA profiler drops [identity_full]",
                            "CUDA profiler drops [event_create]",
                            "CUDA profiler drops [timing_error]"};
    const guint64 values[] = {counters.dropped_pool_full,
                              counters.dropped_identity_full,
                              counters.dropped_event_create,
                              counters.dropped_timing_error};
    for (size_t drop = 0; drop < G_N_ELEMENTS(drops); ++drop) {
        if (!peak_report_snapshot_set_name(snapshot, index, drops[drop])) {
            peak_report_snapshot_destroy(snapshot);
            return NULL;
        }
        snapshot->instrumented[index] = 1;
        snapshot->num_calls[index++] = (gulong)values[drop];
    }
    return snapshot;
}

static gboolean
peak_cuda_render_report_snapshot(const PeakReportSnapshot* snapshot)
{
    const size_t prefix_length = strlen("CUDA kernel: ");
    const size_t suffix_length = strlen(" [time]");
    gboolean have_output = FALSE;
    guint64 drops[4] = {0, 0, 0, 0};

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
        if (name == NULL) continue;
        for (size_t drop = 0; drop < G_N_ELEMENTS(drops); ++drop) {
            const gchar* suffixes[] = {"[pool_full]", "[identity_full]",
                                       "[event_create]", "[timing_error]"};
            if (g_str_has_suffix(name, suffixes[drop])) {
                drops[drop] = snapshot->num_calls[i];
            }
        }
    }
    if (drops[0] != 0 || drops[1] != 0 || drops[2] != 0 || drops[3] != 0) {
        peak_log_report("[peak] CUDA profiler drops: pool_full=%" G_GUINT64_FORMAT
                        " identity_full=%" G_GUINT64_FORMAT " event_create=%"
                        G_GUINT64_FORMAT " timing_error=%" G_GUINT64_FORMAT "\n",
                        drops[0], drops[1], drops[2], drops[3]);
    }
    return TRUE;
}

extern "C" void cuda_interceptor_print_with_mpi_job_policy(
    int aggregation_mode, int active_mpi_job) {
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    if (cuda_kernel_local_dim_mapping == NULL ||
        cuda_graph_local_mapping == NULL) {
        return;
    }
    peak_cuda_accepting_events.store(false, std::memory_order_release);
    cuda_sync_kernel_event();
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
