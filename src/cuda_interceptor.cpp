#include "cuda_interceptor.h"
#include "logging.h"

#define PEAK_CUDA_WRAPPER_EXPORT extern "C" __attribute__((visibility("default")))

extern "C" gpointer peak_general_listener_find_function(const char* symbol);

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
static cudaEvent_t* peak_gpu_cuda_start_event_array;
static cudaEvent_t* peak_gpu_cuda_end_event_array;
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

    KernelLaunchSeries() {
        launches.reserve(100);  // Preallocate space for 100 launches to avoid lock performance
    }
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

    GraphLaunchSeries() {
        launches.reserve(10);
    }
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

char* cu_demangle(char* mangled_name) {
    int status;
    size_t size = sizeof(char) * 1000;
    // Fixme: size might be wrong
    char* demangled_name = (char*)malloc(size);

    __cu_demangle(mangled_name, demangled_name, &size, &status);
    if (status == 0) {
        return demangled_name;
    }

    free(demangled_name);
    return strdup(mangled_name);
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
        dim_info->max_gpu_threads = max(dim_info->max_gpu_threads, total_threads);
        dim_info->min_gpu_threads = min(dim_info->min_gpu_threads, total_threads);
        dim_info->max_block_size = max(dim_info->max_block_size, block_size);
        dim_info->min_block_size = min(dim_info->min_block_size, block_size);
        dim_info->max_grid_size = max(dim_info->max_grid_size, grid_size);
        dim_info->min_grid_size = min(dim_info->min_grid_size, grid_size);
        dim_info->max_time = max(dim_info->max_time, elapsed_sec);
        dim_info->min_time = min(dim_info->min_time, elapsed_sec);
    }
}

void insert_cuda_mapping_record(gchar* kernel_name, gulong total_threads, gulong grid_size, gulong block_size, gdouble elapsed_sec)
{
    if (peak_gpu_monitor_all) {
        gchar* demangled = cu_demangle(kernel_name);
        gchar* extract_kernel_name = extract_function_name(demangled);
        g_mutex_lock(&cuda_kernel_local_dim_mapping_mutex);
        update_kernel_map_info(demangled, total_threads, grid_size, block_size, elapsed_sec);
        g_mutex_unlock(&cuda_kernel_local_dim_mapping_mutex);
        free(demangled);
        free(extract_kernel_name);
    } else {
        // FIXME: should we use a hash table to do the compare rather than for loop look up each time?
        for (size_t i = 0; i < peak_gpu_hook_address_count; i++) {
            gchar* demangled = cu_demangle(kernel_name);
            gchar* extract_kernel_name = extract_function_name(demangled);
            if (g_strcmp0(peak_gpu_hook_strings[i], extract_kernel_name) == 0) {
                g_mutex_lock(&cuda_kernel_local_dim_mapping_mutex);
                update_kernel_map_info(demangled, total_threads, grid_size, block_size, elapsed_sec);
                g_mutex_unlock(&cuda_kernel_local_dim_mapping_mutex);
                break;
            }
            free(demangled);
            free(extract_kernel_name);
        }
    }
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
        graph_info->max_time = max(graph_info->max_time, elapsed_sec);
        graph_info->min_time = min(graph_info->min_time, elapsed_sec);
    }
    g_mutex_unlock(&cuda_graph_local_mapping_mutex);
}

static cudaEvent_t* peak_cuda_new_event_slot()
{
    cudaEvent_t* event = (cudaEvent_t*) calloc(1, sizeof(cudaEvent_t));
    if (event == NULL) {
        return NULL;
    }
    if (cudaEventCreate(event) != cudaSuccess) {
        free(event);
        return NULL;
    }
    return event;
}

static void peak_cuda_record_event(cudaEvent_t* event, cudaStream_t stream)
{
    if (event != NULL && *event != NULL) {
        cudaEventRecord(*event, stream);
    }
}

static void peak_cuda_release_event_pair(cudaEvent_t* start_event,
                                         cudaEvent_t* end_event)
{
    if (start_event != NULL) {
        if (*start_event != NULL) {
            cudaEventDestroy(*start_event);
        }
        free(start_event);
    }
    if (end_event != NULL) {
        if (*end_event != NULL) {
            cudaEventDestroy(*end_event);
        }
        free(end_event);
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
        cudaEventElapsedTime(&ms, *(launch->start_event), *(launch->end_event));
        if (launch->result == cudaSuccess) {
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
        cudaEventElapsedTime(&ms, *(launch->start_event), *(launch->end_event));
        if (launch->result == cudaSuccess) {
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
    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, stream);
    cudaError_t result = original_cuda_launch_kernel(func, gridDim, blockDim, args, sharedMem, stream);
    peak_cuda_record_event(end, stream);

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
    g_free(kernel_name);

    return result;
}

PEAK_CUDA_WRAPPER_EXPORT cudaError_t peak_cuda_launch_cooperative_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    PeakCudaInflightGuard in_flight;
    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, stream);
    cudaError_t result = original_cuda_launch_cooperative_kernel(func, gridDim, blockDim, args, sharedMem, stream);
    peak_cuda_record_event(end, stream);

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
    g_free(kernel_name);

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

    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, stream);
    cudaError_t result = original_cuda_launch_cooperative_kernel_multiple_device(launchParamsList, numDevices, flags);
    peak_cuda_record_event(end, stream);

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
    g_free(kernel_name);

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

    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";


    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, stream);
    cudaError_t result = original_cuda_launch_kernel_exc(config, func, args);
    peak_cuda_record_event(end, stream);

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
    g_free(kernel_name);

    return result;
}

PEAK_CUDA_WRAPPER_EXPORT CUresult peak_cu_launch_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra)
{
    PeakCudaInflightGuard in_flight;
    // Kernel Name Address Source
    // https://forums.developer.nvidia.com/t/how-to-get-a-kernel-functions-name-through-its-pointer/37427/2
    gchar* kernel_name = *(char**)((size_t)func + 8);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, (cudaStream_t)hStream);
    CUresult result = original_cu_launch_kernel(func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams, extra);
    peak_cuda_record_event(end, (cudaStream_t)hStream);

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
    gchar* kernel_name = *(char**)((size_t)func + 8);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, (cudaStream_t)hStream);
    CUresult result = original_cu_launch_cooperative_kernel(
                                                func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams);

    peak_cuda_record_event(end, (cudaStream_t)hStream);

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

    gchar* kernel_name = *(char**)((size_t)func + 8);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, (cudaStream_t)hStream);
    CUresult result = original_cu_launch_cooperative_kernel_multiple_device(
                                    launchParamsList, numDevices, flags);
    peak_cuda_record_event(end, (cudaStream_t)hStream);

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

    gchar* kernel_name = *(char**)((size_t)func + 8);
    const gchar* kernel_label = (kernel_name != NULL) ? kernel_name : "<unknown-cuda-kernel>";
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, (cudaStream_t)hStream);
    CUresult result = original_cu_launch_kernel_ex(config, func, kernelParams, extra);
    peak_cuda_record_event(end, (cudaStream_t)hStream);

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
    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, (cudaStream_t)stream);
    cudaError_t result = original_cuda_graph_launch(graphExec, stream);
    peak_cuda_record_event(end, (cudaStream_t)stream);

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
    cudaEvent_t* start = peak_cuda_new_event_slot();
    cudaEvent_t* end = peak_cuda_new_event_slot();
    peak_cuda_record_event(start, (cudaStream_t)hStream);
    CUresult result = original_cu_graph_launch(hGraphExec, hStream);
    peak_cuda_record_event(end, (cudaStream_t)hStream);

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

    // Initialize cudaEvent_t array
    peak_gpu_cuda_start_event_array = g_new0(cudaEvent_t, peak_gpu_hook_address_count);
    peak_gpu_cuda_end_event_array = g_new0(cudaEvent_t, peak_gpu_hook_address_count);
    for (size_t i = 0; i < peak_gpu_hook_address_count; i++) {
        if (cudaEventCreate(&peak_gpu_cuda_start_event_array[i]) != cudaSuccess) {
            peak_gpu_cuda_start_event_array[i] = NULL;
        }
        if (cudaEventCreate(&peak_gpu_cuda_end_event_array[i]) != cudaSuccess) {
            peak_gpu_cuda_end_event_array[i] = NULL;
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
        peak_log_warn("[peak] CUDA interceptor teardown observed %u active wrapper(s); keeping Gum trampoline state alive\n",
                   active_cuda_wrappers);
    }

    peak_cuda_drain_kernel_event_map(FALSE);
    peak_cuda_drain_graph_event_map(FALSE);

    for (size_t i = 0; i < peak_gpu_hook_address_count; i++) {
        if (peak_gpu_cuda_start_event_array != NULL &&
            peak_gpu_cuda_start_event_array[i] != NULL) {
            cudaEventDestroy(peak_gpu_cuda_start_event_array[i]);
        }
        if (peak_gpu_cuda_end_event_array != NULL &&
            peak_gpu_cuda_end_event_array[i] != NULL) {
            cudaEventDestroy(peak_gpu_cuda_end_event_array[i]);
        }
    }
    g_free(peak_gpu_cuda_start_event_array);
    g_free(peak_gpu_cuda_end_event_array);
    peak_gpu_cuda_start_event_array = NULL;
    peak_gpu_cuda_end_event_array = NULL;

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
     * that wrapper can still need Gum's original trampoline. Retaining this
     * bounded state avoids a teardown race without delaying physical detach.
     */
}

static void cuda_interceptor_print_kernel_result(GHashTable* hashTable)
{
    gboolean have_output = FALSE;
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, hashTable);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        KernelDimInfo* dim_info = (KernelDimInfo*) value;
        if (dim_info->total_kernel_call_cnt > 0) {
            have_output = TRUE;
            break;
        }
    }

    if (have_output) {
        const guint row_width = 100;
        const guint max_function_width = 40;
        const guint max_col_width = 9;

        char* space_separator = (char *) malloc(row_width + 1);
        char* row_separator = (char *)  malloc(row_width + 1);
        memset(space_separator, ' ', row_width);
        memset(row_separator, '-', row_width);
        space_separator[row_width] = '\0';
        row_separator[row_width] = '\0';

        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sGPU STATISTICS (Kernel)%*s\n",
            (row_width - 15) / 2, "",
            (row_width - 15 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);

        // Section: Kernel call count & time
        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sKERNEL STATISTICS (GPU)%*s\n",
            (row_width - 26) / 2, "",
            (row_width - 26 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);
        peak_log_report("| %-*s | %*s | %*s | %*s | %*s |\n",
            max_function_width, "Kernel",
            max_col_width, "Calls",
            max_col_width, "Total(s)",
            max_col_width, "Max(s)",
            max_col_width, "Min(s)");
        peak_log_report("%s\n", row_separator);

        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            KernelDimInfo* dim_info = (KernelDimInfo*) value;
            char* truncated_name = truncate_string((const char *)key, max_function_width);
            peak_log_report("| %-*s | %*lu | %*.6f | %*.6f | %*.6f |\n",
                max_function_width, truncated_name,
                max_col_width, dim_info->total_kernel_call_cnt,
                max_col_width, dim_info->total_time,
                max_col_width, dim_info->max_time,
                max_col_width, dim_info->min_time);
            free(truncated_name);
        }
        peak_log_report("%s\n", row_separator);

        // Section: Kernel block & thread size
        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sKERNEL BLOCK SIZE%*s\n",
            (row_width - 20) / 2, "",
            (row_width - 20 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);
        peak_log_report("| %-*s | %*s | %*s | %*s |\n",
            max_function_width, "Kernel",
            max_col_width, "AvgBlk",
            max_col_width, "MaxBlk",
            max_col_width, "MinBlk");
        peak_log_report("%s\n", row_separator);

        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            KernelDimInfo* dim_info = (KernelDimInfo*) value;
            char* truncated_name = truncate_string((const char *)key, max_function_width);
            peak_log_report("| %-*s | %*.2f | %*lu | %*lu |\n",
                max_function_width, truncated_name,
                max_col_width, (double)dim_info->total_block_size / dim_info->total_kernel_call_cnt,
                max_col_width, dim_info->max_block_size,
                max_col_width, dim_info->min_block_size);
            free(truncated_name);
        }
        peak_log_report("%s\n", row_separator);

        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sKERNEL GRID SIZE%*s\n",
            (row_width - 21) / 2, "",
            (row_width - 21 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);
        peak_log_report("| %-*s | %*s | %*s | %*s |\n",
            max_function_width, "Kernel",
            max_col_width, "AvgGrid",
            max_col_width, "MaxGrid",
            max_col_width, "MinGrid");
        peak_log_report("%s\n", row_separator);

        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            KernelDimInfo* dim_info = (KernelDimInfo*) value;
            char* truncated_name = truncate_string((const char *)key, max_function_width);
            peak_log_report("| %-*s | %*.2f | %*lu | %*lu |\n",
                max_function_width, truncated_name,
                max_col_width, (double)dim_info->total_grid_size / dim_info->total_kernel_call_cnt,
                max_col_width, dim_info->max_grid_size,
                max_col_width, dim_info->min_grid_size);
            free(truncated_name);
        }
        peak_log_report("%s\n", row_separator);

        free(space_separator);
        free(row_separator);
    }
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

static void cuda_interceptor_print_graph_mpi_result(const gint* ranks,
                                                    CUgraphExec_st** graphs,
                                                    const GraphRecordInfo* values,
                                                    gint count)
{
    gboolean have_output = FALSE;
    for (gint i = 0; i < count; i++) {
        if (values[i].total_graph_call_cnt > 0) {
            have_output = TRUE;
            break;
        }
    }

    if (have_output) {
        const guint row_width = 112;
        const guint max_col_width = 9;

        char* row_separator = (char *) malloc(row_width + 1);
        memset(row_separator, '-', row_width);
        row_separator[row_width] = '\0';

        peak_log_report("\n%s\n", row_separator);
        peak_log_report("%*sGRAPH STATISTICS (GPU, MPI)%*s\n",
            (row_width - 27) / 2, "",
            (row_width - 27 + 1) / 2, "");
        peak_log_report("%s\n", row_separator);
        peak_log_report("| %*s | %*s | %*s | %*s | %*s | %*s |\n",
            max_col_width, "Rank",
            18, "Graph",
            max_col_width, "Calls",
            max_col_width, "Total(s)",
            max_col_width, "Max(s)",
            max_col_width, "Min(s)");
        peak_log_report("%s\n", row_separator);

        for (gint i = 0; i < count; i++) {
            if (values[i].total_graph_call_cnt == 0) {
                continue;
            }
            peak_log_report("| %*d | %18p | %*lu | %*.6f | %*.6f | %*.6f |\n",
                max_col_width, ranks[i],
                graphs[i],
                max_col_width, values[i].total_graph_call_cnt,
                max_col_width, values[i].total_time,
                max_col_width, values[i].max_time,
                max_col_width, values[i].min_time);
        }
        peak_log_report("%s\n", row_separator);

        free(row_separator);
    }
}

#ifdef HAVE_MPI
static void
cuda_interceptor_reduce_kernel_result()
{
    int world_rank, world_size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if (!init_flag) {
        MPI_Init(NULL, NULL);
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    GHashTable* cuda_kernel_global_dim_mapping = NULL;
    if (world_rank == 0) {
        cuda_kernel_global_dim_mapping = g_hash_table_new_full(g_str_hash, str_equal_function, NULL, g_free);
    }

    gint local_kernel_count = g_hash_table_size(cuda_kernel_local_dim_mapping);
    gint local_values_size = local_kernel_count * sizeof(KernelDimInfo);
    gchar** local_keys = g_new(gchar*, local_kernel_count);
    KernelDimInfo* values = g_new(KernelDimInfo, local_kernel_count);

    GHashTableIter iter;
    gpointer key, value;
    guint index = 0;
    gint local_keys_buffer_size = 0;
    g_hash_table_iter_init(&iter, cuda_kernel_local_dim_mapping);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        local_keys_buffer_size += strlen((char*) key) + 1;
        local_keys[index] = g_strdup((char*) key);
        values[index] = *(KernelDimInfo*) value;
        index++;
    }

    gchar* local_keys_buffer = g_new(gchar, local_keys_buffer_size);
    guint offset = 0;
    for (guint i = 0; i < local_kernel_count; i++) {
        strcpy(&local_keys_buffer[offset], local_keys[i]);
        offset += strlen(local_keys[i]) + 1;
    }

    gulong global_kernel_count = 0;
    gulong global_keys_length_sum;
    gulong* kernel_count_array = NULL;
    gint* keys_buffer_array = NULL;
    gint* values_buffer_array = NULL;
    if (world_rank == 0) {
        global_kernel_count = 0;
        global_keys_length_sum = world_size; // initialize for each rank plus one for space or \0 after words
        kernel_count_array = g_new(gulong, world_size);
        keys_buffer_array = g_new(gint, world_size);
        values_buffer_array = g_new(gint, world_size);
    }
    MPI_Reduce(&local_kernel_count, &global_kernel_count, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_keys_buffer_size, &global_keys_length_sum, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Gather(&local_kernel_count, 1, MPI_INT, kernel_count_array, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Gather(&local_keys_buffer_size, 1, MPI_INT, keys_buffer_array, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Gather(&local_values_size, 1, MPI_INT, values_buffer_array, 1, MPI_INT, 0, MPI_COMM_WORLD);

    gchar *global_keys_string = NULL;
    gint *keys_offset_array = NULL;
    gint *values_offset_array = NULL;
    gchar **global_keys_array = NULL;
    KernelDimInfo* global_values_array = NULL;

    if (world_rank == 0) {
        // Key
        keys_offset_array = g_new(gint, world_size);
        keys_offset_array[0] = 0;
        for (guint i = 1; i < world_size; i++) {
            keys_offset_array[i] = keys_offset_array[i-1] + keys_buffer_array[i-1];
        }
        global_keys_string = g_new(gchar, global_keys_length_sum == 0 ? 1 : global_keys_length_sum);
        if (global_keys_length_sum > 0) {
            memset(global_keys_string, '\0', global_keys_length_sum - 1); // allocate string, pre-fill with \0
            global_keys_string[global_keys_length_sum - 1] = '\0';
        } else {
            global_keys_string[0] = '\0';
        }

        // Value
        global_values_array = (global_kernel_count > 0) ? g_new(KernelDimInfo, global_kernel_count) : NULL;
        values_offset_array = g_new(gint, world_size);
        values_offset_array[0] = 0;
        for (guint i = 1; i < world_size; i++) {
            values_offset_array[i] = values_offset_array[i-1] + values_buffer_array[i-1];
        }
    }

    MPI_Gatherv(local_keys_buffer, local_keys_buffer_size, MPI_CHAR, global_keys_string, keys_buffer_array, keys_offset_array, MPI_CHAR, 0, MPI_COMM_WORLD);
    MPI_Gatherv(values, local_kernel_count * sizeof(KernelDimInfo), MPI_BYTE, global_values_array, values_buffer_array, values_offset_array, MPI_BYTE, 0, MPI_COMM_WORLD);

    if (world_rank == 0) {
        if (global_kernel_count > 0) {
            // Spilt global key strings into global key array
            global_keys_array = g_new(gchar*, global_kernel_count);
            guint index = 0;
            for (guint i = 0; i < global_kernel_count; i++) {
                guint len = strlen(global_keys_string + index);
                global_keys_array[i] = g_new(gchar, len+1);
                strcpy(global_keys_array[i], global_keys_string + index);
                index += len + 1;
            }

            // aggregate value
            for (guint i = 0; i < global_kernel_count; i++) {
                KernelDimInfo* existing = (KernelDimInfo*) g_hash_table_lookup(cuda_kernel_global_dim_mapping, g_strdup(global_keys_array[i]));
                if (existing) {
                    existing->total_kernel_call_cnt += global_values_array[i].total_kernel_call_cnt;
                    existing->total_gpu_threads += global_values_array[i].total_gpu_threads;
                    existing->max_gpu_threads = max(existing->max_gpu_threads, global_values_array[i].max_gpu_threads);
                    existing->min_gpu_threads = min(existing->min_gpu_threads, global_values_array[i].min_gpu_threads);
                    existing->total_block_size += global_values_array[i].total_block_size;
                    existing->max_block_size = max(existing->max_block_size, global_values_array[i].max_block_size);
                    existing->min_block_size = min(existing->min_block_size, global_values_array[i].min_block_size);
                    existing->total_grid_size += global_values_array[i].total_grid_size;
                    existing->max_grid_size = max(existing->max_grid_size, global_values_array[i].max_grid_size);
                    existing->min_grid_size = min(existing->min_grid_size, global_values_array[i].min_grid_size);
                    existing->total_time += global_values_array[i].total_time;
                    existing->max_time = max(existing->max_time, global_values_array[i].max_time);
                    existing->min_time = min(existing->min_time, global_values_array[i].min_time);
                } else {
                    g_hash_table_insert(cuda_kernel_global_dim_mapping, g_strdup(global_keys_array[i]), g_memdup2(&global_values_array[i], sizeof(KernelDimInfo)));
                }
            }

            cuda_interceptor_print_kernel_result(cuda_kernel_global_dim_mapping);

            for (guint i = 0; i < global_kernel_count; i++) {
                g_free(global_keys_array[i]);
            }
        }

        g_free(global_keys_string);
        g_free(keys_offset_array);
        g_free(values_offset_array);
        g_free(global_keys_array);
        g_free(global_values_array);
    }

    for (guint i = 0; i < local_kernel_count; i++) {
        g_free(local_keys[i]);
    }
    g_free(local_keys);
    g_free(values);
    g_free(local_keys_buffer);
    if (world_rank == 0) {
        g_hash_table_destroy(cuda_kernel_global_dim_mapping);
        g_free(kernel_count_array);
        g_free(keys_buffer_array);
        g_free(values_buffer_array);
    }
}

static void
cuda_interceptor_reduce_graph_result()
{
    // FIXME: consider not using CUgraphExec_st* to determine unique graph, as it may not work across MPI ranks

    int world_rank, world_size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if (!init_flag) {
        MPI_Init(NULL, NULL);
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    gint local_graph_count = g_hash_table_size(cuda_graph_local_mapping);
    gint local_key_size = local_graph_count * sizeof(CUgraphExec_st*);
    gint local_values_size = local_graph_count * sizeof(GraphRecordInfo);
    CUgraphExec_st** keys = g_new(CUgraphExec_st*, local_graph_count);
    GraphRecordInfo* values = g_new(GraphRecordInfo, local_graph_count);

    GHashTableIter iter;
    gpointer key, value;
    guint index = 0;
    g_hash_table_iter_init(&iter, cuda_graph_local_mapping);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        keys[index] = (CUgraphExec_st*) key;
        values[index] = *(GraphRecordInfo*) value;
        index++;
    }

    gint global_graph_count = 0;
    gint* graph_count_array = NULL;
    gint* keys_buffer_array = NULL;
    gint* values_buffer_array = NULL;
    if (world_rank == 0) {
        global_graph_count = 0;
        graph_count_array = g_new(gint, world_size);
        keys_buffer_array = g_new(gint, world_size);
        values_buffer_array = g_new(gint, world_size);
    }
    MPI_Reduce(&local_graph_count, &global_graph_count, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Gather(&local_graph_count, 1, MPI_INT, graph_count_array, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Gather(&local_key_size, 1, MPI_INT, keys_buffer_array, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Gather(&local_values_size, 1, MPI_INT, values_buffer_array, 1, MPI_INT, 0, MPI_COMM_WORLD);

    gint *keys_offset_array = NULL;
    gint *values_offset_array = NULL;
    CUgraphExec_st** global_keys_array = NULL;
    GraphRecordInfo* global_values_array = NULL;
    gint* global_rank_array = NULL;

    if (world_rank == 0) {
        // Key
        global_keys_array = g_new(CUgraphExec_st*, global_graph_count);
        global_rank_array = g_new(gint, global_graph_count);
        keys_offset_array = g_new(gint, world_size);
        keys_offset_array[0] = 0;
        gint graph_offset = 0;
        for (guint i = 0; i < (guint) graph_count_array[0]; i++) {
            global_rank_array[graph_offset++] = 0;
        }
        for (guint i = 1; i < world_size; i++) {
            keys_offset_array[i] = keys_offset_array[i-1] + keys_buffer_array[i-1];
            for (guint j = 0; j < (guint) graph_count_array[i]; j++) {
                global_rank_array[graph_offset++] = i;
            }
        }

        // Value
        global_values_array = g_new(GraphRecordInfo, global_graph_count);
        values_offset_array = g_new(gint, world_size);
        values_offset_array[0] = 0;
        for (guint i = 1; i < world_size; i++) {
            values_offset_array[i] = values_offset_array[i-1] + values_buffer_array[i-1];
        }
    }
    MPI_Gatherv(keys, local_graph_count * sizeof(CUgraphExec_st*), MPI_BYTE, global_keys_array, keys_buffer_array, keys_offset_array, MPI_BYTE, 0, MPI_COMM_WORLD);
    MPI_Gatherv(values, local_graph_count * sizeof(GraphRecordInfo), MPI_BYTE, global_values_array, values_buffer_array, values_offset_array, MPI_BYTE, 0, MPI_COMM_WORLD);

    if (world_rank == 0) {
        if (global_graph_count > 0) {
            cuda_interceptor_print_graph_mpi_result(global_rank_array,
                                                    global_keys_array,
                                                    global_values_array,
                                                    global_graph_count);
        }

        g_free(keys_offset_array);
        g_free(values_offset_array);
        g_free(global_rank_array);
        g_free(global_keys_array);
        g_free(global_values_array);
    }

    g_free(keys);
    g_free(values);
    if (world_rank == 0) {
        g_free(graph_count_array);
        g_free(keys_buffer_array);
        g_free(values_buffer_array);
    }
}
#endif

static void cuda_sync_kernel_event() {
    cudaDeviceSynchronize();
    peak_cuda_drain_kernel_event_map(TRUE);
    peak_cuda_drain_graph_event_map(TRUE);
}

extern "C" void cuda_interceptor_print(int is_MPI) {
    std::lock_guard<std::mutex> lifecycle_lock(peak_cuda_lifecycle_mutex);
    if (cuda_kernel_local_dim_mapping == NULL ||
        cuda_graph_local_mapping == NULL) {
        return;
    }
    peak_cuda_accepting_events.store(false, std::memory_order_release);
    cuda_sync_kernel_event();
    #ifdef HAVE_MPI
        if (is_MPI) {
            cuda_interceptor_reduce_kernel_result();
            cuda_interceptor_reduce_graph_result();
        } else {
            cuda_interceptor_print_kernel_result(cuda_kernel_local_dim_mapping);
            cuda_interceptor_print_graph_result(cuda_graph_local_mapping);
        }
    #else
        cuda_interceptor_print_kernel_result(cuda_kernel_local_dim_mapping);
        cuda_interceptor_print_graph_result(cuda_graph_local_mapping);
    #endif
}
