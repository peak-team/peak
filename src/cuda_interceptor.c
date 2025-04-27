#include "cuda_interceptor.h"

static GHashTable* cuda_kernel_local_dim_mapping;
static GMutex cuda_kernel_local_dim_mapping_mutex;
static GumInterceptor* cuda_interceptor;
static gpointer* hook_cuda_launch;
static gpointer* hook_cuda_launch_cooperative;
static gpointer* hook_cuda_launch_cooperative_multiple_device;
static gpointer* hook_cuda_launch_exc;
static gpointer* hook_cu_launch;
static gpointer* hook_cu_launch_cooperative;
static gpointer* hook_cu_launch_cooperative_multiple_device;
static gpointer* hook_cu_launch_ex;
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
    struct timespec start_time;
    gulong total_threads;
    gulong grid_size;
    gulong block_size;
} KernelTimingPayload;

gboolean str_equal_function(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

char* cu_demangle(char* mangled_name) {
    int status;
    size_t size = sizeof(char) * 1000;
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
    KernelDimInfo* dim_info = g_hash_table_lookup(cuda_kernel_local_dim_mapping, key);

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

void CUDART_CB timing_callback(void* data) {
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    KernelTimingPayload* payload = (KernelTimingPayload*)data;
    gdouble elapsed_sec = (gdouble)(end_time.tv_sec - payload->start_time.tv_sec) +
                      (gdouble)(end_time.tv_nsec - payload->start_time.tv_nsec) / 1e9;

    insert_cuda_mapping_record(payload->kernel_name, payload->total_threads, payload->grid_size, payload->block_size, elapsed_sec);

    g_free(payload->kernel_name);
    g_free(payload);
}

static cudaError_t peak_cuda_launch_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    g_free(kernel_name);

    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    cudaError_t result = original_cuda_launch_kernel(func, gridDim, blockDim, args, sharedMem, stream);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc(stream, timing_callback, payload);

    return result;
}

static cudaError_t peak_cuda_launch_cooperative_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    g_free(kernel_name);

    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    cudaError_t result = original_cuda_launch_cooperative_kernel(func, gridDim, blockDim, args, sharedMem, stream);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc(stream, timing_callback, payload);

    return result;
}

static cudaError_t peak_cuda_launch_cooperative_kernel_multiple_device(
    struct cudaLaunchParams* launchParamsList, unsigned int numDevices, unsigned int flags)
{
    const void* func = launchParamsList->func;
    dim3 gridDim = launchParamsList->gridDim;
    dim3 blockDim = launchParamsList->blockDim;
    cudaStream_t stream = launchParamsList->stream;

    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    g_free(kernel_name);

    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    cudaError_t result = original_cuda_launch_cooperative_kernel_multiple_device(launchParamsList, numDevices, flags);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc(stream, timing_callback, payload);

    return result;
}

static cudaError_t peak_cuda_launch_kernel_exc(
    const cudaLaunchConfig_t* config,
    const void* func, void** args)
{
    dim3 gridDim = config->gridDim;
    dim3 blockDim = config->blockDim;
    cudaStream_t stream = config->stream;

    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    g_free(kernel_name);

    gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
    gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
    gulong block_size = blockDim.x * blockDim.y * blockDim.z;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    cudaError_t result = original_cuda_launch_kernel_exc(config, func, args);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc(stream, timing_callback, payload);

    return result;
}

static CUresult peak_cu_launch_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra)
{
    // Kernel Name Address Source
    // https://forums.developer.nvidia.com/t/how-to-get-a-kernel-functions-name-through-its-pointer/37427/2
    gchar* kernel_name = *(char**)((size_t)func + 8);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    CUresult result = original_cu_launch_kernel(func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams, extra);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc((cudaStream_t)hStream, timing_callback, payload);

    return result;
}

static CUresult peak_cu_launch_cooperative_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams)
{
    gchar* kernel_name = *(char**)((size_t)func + 8);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    CUresult result = original_cu_launch_cooperative_kernel(
                                                func, gridDimX, gridDimY, gridDimZ,
                                                blockDimX, blockDimY, blockDimZ,
                                                sharedMemBytes, hStream, kernelParams);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc((cudaStream_t)hStream, timing_callback, payload);

    return result;
}

static CUresult peak_cu_launch_cooperative_kernel_multiple_device(
    CUDA_LAUNCH_PARAMS* launchParamsList,
    unsigned int numDevices, unsigned int flags)
{
    CUfunction func = launchParamsList->function;
    unsigned int gridDimX = launchParamsList->gridDimX;
    unsigned int gridDimY = launchParamsList->gridDimY;
    unsigned int gridDimZ = launchParamsList->gridDimZ;
    unsigned int blockDimX = launchParamsList->blockDimX;
    unsigned int blockDimY = launchParamsList->blockDimY;
    unsigned int blockDimZ = launchParamsList->blockDimZ;
    CUstream hStream = launchParamsList->hStream;

    gchar* kernel_name = *(char**)((size_t)func + 8);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    CUresult result = original_cu_launch_cooperative_kernel_multiple_device(
                                    launchParamsList, numDevices, flags);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc((cudaStream_t)hStream, timing_callback, payload);

    return result;
}

static CUresult peak_cu_launch_kernel_ex(
    const CUlaunchConfig* config, CUfunction func, 
    void** kernelParams, void** extra)
{
    unsigned int gridDimX = config->gridDimX;
    unsigned int gridDimY = config->gridDimY;
    unsigned int gridDimZ = config->gridDimZ;
    unsigned int blockDimX = config->blockDimX;
    unsigned int blockDimY = config->blockDimY;
    unsigned int blockDimZ = config->blockDimZ;
    CUstream hStream = config->hStream;

    gchar* kernel_name = *(char**)((size_t)func + 8);
    KernelTimingPayload* payload = g_new(KernelTimingPayload, 1);
    payload->kernel_name = g_strdup(kernel_name);
    
    gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
    gulong grid_size = gridDimX * gridDimY * gridDimZ;
    gulong block_size = blockDimX * blockDimY * blockDimZ;

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    CUresult result = original_cu_launch_kernel_ex(config, func, kernelParams, extra);

    // Register Callback Stream
    payload->start_time = start_time;
    payload->total_threads = total_threads;
    payload->grid_size = grid_size;
    payload->block_size = block_size;
    cudaLaunchHostFunc((cudaStream_t)hStream, timing_callback, payload);

    return result;
}

int cuda_interceptor_attach()
{
    cuda_kernel_local_dim_mapping = g_hash_table_new_full(g_str_hash, str_equal_function, NULL, g_free);
    g_mutex_init(&cuda_kernel_local_dim_mapping_mutex);

    GumReplaceReturn replace_check = -1;
    cuda_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(cuda_interceptor);

    hook_cuda_launch = gum_find_function("cudaLaunchKernel");
    if (hook_cuda_launch) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch,
            (gpointer)&peak_cuda_launch_kernel,
            (gpointer*)&original_cuda_launch_kernel);
    }
    
    hook_cuda_launch_cooperative = gum_find_function("cudaLaunchCooperativeKernel");
    if (hook_cuda_launch_cooperative) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch_cooperative,
            (gpointer)&peak_cuda_launch_cooperative_kernel,
            (gpointer*)&original_cuda_launch_cooperative_kernel);
    }

    hook_cuda_launch_cooperative_multiple_device = gum_find_function("cudaLaunchCooperativeKernelMultiDevice");
    if (hook_cuda_launch_cooperative_multiple_device) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch_cooperative_multiple_device,
            (gpointer)&peak_cuda_launch_cooperative_kernel_multiple_device,
            (gpointer*)&original_cuda_launch_cooperative_kernel_multiple_device);
    }

    hook_cuda_launch_exc = gum_find_function("cudaLaunchKernelExC");
    if (hook_cuda_launch_exc) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch_exc,
            (gpointer)&peak_cuda_launch_kernel_exc,
            (gpointer*)&original_cuda_launch_kernel_exc);
    }

    hook_cu_launch = gum_find_function("cuLaunchKernel");
    if (hook_cu_launch) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch,
            (gpointer)&peak_cu_launch_kernel,
            (gpointer*)&original_cu_launch_kernel);
    }

    hook_cu_launch_cooperative = gum_find_function("cuLaunchCooperativeKernel");
    if (hook_cu_launch_cooperative) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch_cooperative,
            (gpointer)&peak_cu_launch_cooperative_kernel,
            (gpointer*)&original_cu_launch_cooperative_kernel);
    }

    hook_cu_launch_cooperative_multiple_device = gum_find_function("cuLaunchCooperativeKernelMultiDevice");
    if (hook_cu_launch_cooperative_multiple_device) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch_cooperative_multiple_device,
            (gpointer)&peak_cu_launch_cooperative_kernel_multiple_device,
            (gpointer*)&original_cu_launch_cooperative_kernel_multiple_device);
    }

    hook_cu_launch_ex = gum_find_function("cuLaunchKernelEx");
    if (hook_cu_launch_ex) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch_ex,
            (gpointer)&peak_cu_launch_kernel_ex,
            (gpointer*)&original_cu_launch_kernel_ex);
    }
    
    gum_interceptor_end_transaction(cuda_interceptor);

    return replace_check;
}

void cuda_interceptor_dettach()
{
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
    g_hash_table_destroy(cuda_kernel_local_dim_mapping);
    g_object_unref(cuda_interceptor);
}

static void cuda_interceptor_print_result(GHashTable* hashTable)
{
    gboolean have_output = FALSE;
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, hashTable);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        KernelDimInfo* dim_info = value;
        if (dim_info->total_kernel_call_cnt > 0) {
            have_output = TRUE;
            break;
        }
    }

    if (have_output) {
        const guint row_width = 100;
        const guint max_function_width = 40;
        const guint max_col_width = 9;
    
        char* space_separator = malloc(row_width + 1);
        char* row_separator = malloc(row_width + 1);
        memset(space_separator, ' ', row_width);
        memset(row_separator, '-', row_width);
        space_separator[row_width] = '\0';
        row_separator[row_width] = '\0';
    
        g_printerr("\n%s\n", row_separator);
        g_printerr("%*sGPU STATISTICS%*s\n", 
            (row_width - 15) / 2, "", 
            (row_width - 15 + 1) / 2, "");
        g_printerr("%s\n", row_separator);
    
        // Section: Kernel call count & time
        g_printerr("\n%s\n", row_separator);
        g_printerr("%*sKERNEL STATISTICS (GPU)%*s\n", 
            (row_width - 26) / 2, "", 
            (row_width - 26 + 1) / 2, "");
        g_printerr("%s\n", row_separator);
        g_printerr("| %-*s | %*s | %*s | %*s | %*s |\n",
            max_function_width, "Kernel",
            max_col_width, "Calls",
            max_col_width, "Total(s)",
            max_col_width, "Max(s)",
            max_col_width, "Min(s)");
        g_printerr("%s\n", row_separator);
    
        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            KernelDimInfo* dim_info = value;
            g_printerr("| %-*s | %*lu | %*.6f | %*.6f | %*.6f |\n",
                max_function_width, truncate_string(key, max_function_width),
                max_col_width, dim_info->total_kernel_call_cnt,
                max_col_width, dim_info->total_time,
                max_col_width, dim_info->max_time,
                max_col_width, dim_info->min_time);
        }
        g_printerr("%s\n", row_separator);
    
        // Section: Kernel block & thread size
        g_printerr("\n%s\n", row_separator);
        g_printerr("%*sKERNEL BLOCK SIZE%*s\n", 
            (row_width - 20) / 2, "", 
            (row_width - 20 + 1) / 2, "");
        g_printerr("%s\n", row_separator);
        g_printerr("| %-*s | %*s | %*s | %*s |\n",
            max_function_width, "Kernel",
            max_col_width, "AvgBlk",
            max_col_width, "MaxBlk",
            max_col_width, "MinBlk");
        g_printerr("%s\n", row_separator);
    
        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            KernelDimInfo* dim_info = value;
            g_printerr("| %-*s | %*.2f | %*lu | %*lu |\n",
                max_function_width, truncate_string(key, max_function_width),
                max_col_width, (double)dim_info->total_block_size / dim_info->total_kernel_call_cnt,
                max_col_width, dim_info->max_block_size,
                max_col_width, dim_info->min_block_size);
        }
        g_printerr("%s\n", row_separator);

        g_printerr("\n%s\n", row_separator);
        g_printerr("%*sKERNEL THREAD SIZE%*s\n", 
            (row_width - 21) / 2, "", 
            (row_width - 21 + 1) / 2, "");
        g_printerr("%s\n", row_separator);
        g_printerr("| %-*s | %*s | %*s | %*s |\n",
            max_function_width, "Kernel",
            max_col_width, "AvgGrid",
            max_col_width, "MaxGrid",
            max_col_width, "MinGrid");
        g_printerr("%s\n", row_separator);
    
        g_hash_table_iter_init(&iter, hashTable);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            KernelDimInfo* dim_info = value;
            g_printerr("| %-*s | %*.2f | %*lu | %*lu |\n",
                max_function_width, truncate_string(key, max_function_width),
                max_col_width, (double)dim_info->total_grid_size / dim_info->total_kernel_call_cnt,
                max_col_width, dim_info->max_grid_size,
                max_col_width, dim_info->min_grid_size);
        }
        g_printerr("%s\n", row_separator);
    
        free(space_separator);
        free(row_separator);
    }
}

#ifdef HAVE_MPI
static void
cuda_interceptor_reduce_result()
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

    gulong global_kernel_count;
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
    
    if (global_kernel_count > 0) {
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
            global_keys_string = g_new(gchar, global_keys_length_sum);
            memset(global_keys_string, ' ', global_keys_length_sum - 1); // allocate string, pre-fill with spaces and null terminator
            global_keys_string[global_keys_length_sum - 1] = '\0'; 

            // Value
            global_values_array = g_new(KernelDimInfo, global_kernel_count);
            values_offset_array = g_new(gint, global_kernel_count);
            values_offset_array[0] = 0;
            for (guint i = 1; i < world_size; i++) {
                values_offset_array[i] = values_offset_array[i-1] + values_buffer_array[i-1];
            }
        }
        MPI_Gatherv(local_keys_buffer, local_keys_buffer_size, MPI_CHAR, global_keys_string, keys_buffer_array, keys_offset_array, MPI_CHAR, 0, MPI_COMM_WORLD);  
        MPI_Gatherv(values, local_kernel_count * sizeof(KernelDimInfo), MPI_BYTE, global_values_array, values_buffer_array, values_offset_array, MPI_BYTE, 0, MPI_COMM_WORLD);

        if (world_rank == 0) {
            // Spilt global key strings into global key array
            global_keys_array = g_new(gchar*, global_kernel_count);
            guint index = 0;
            for (guint i = 0; i < global_kernel_count; i++) {
                guint len = strlen(global_keys_string + index);
                global_keys_array[i] = g_new(gchar, len);
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
        }
        
        MPI_Barrier(MPI_COMM_WORLD);

        if (world_rank == 0) {
            cuda_interceptor_print_result(cuda_kernel_global_dim_mapping);
            for (guint i = 0; i < global_kernel_count; i++) {
                g_free(global_keys_array[i]);
            }
            g_free(global_keys_string);
            g_free(keys_offset_array);
            g_free(values_offset_array);
            g_free(global_keys_array);
            g_free(global_values_array);
        }
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
#endif

void cuda_interceptor_print(int is_MPI) {
    #ifdef HAVE_MPI
        if (is_MPI) {
            cuda_interceptor_reduce_result();
        } else {
            cuda_interceptor_print_result(cuda_kernel_local_dim_mapping);
        }
    #else
        cuda_interceptor_print_result(cuda_kernel_local_dim_mapping);
    #endif
}
