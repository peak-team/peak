#include "cuda_interceptor.h"

static GHashTable* cuda_kernel_dim_mapping;
static GumInterceptor* cuda_interceptor;
static gpointer* hook_cuda_launch;
static gpointer* hook_cu_launch;
static int device_count = 0;

typedef struct {
    dim3 gridDim;
    dim3 blockDim;
} KernelDimInfo;

static cudaError_t (*original_cuda_launch_kernel)(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream);

static CUresult (*original_cu_launch_kernel)(
    CUfunction func, 
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra);

static cudaError_t peak_cuda_launch_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    KernelDimInfo* dim_info = g_new(KernelDimInfo, 1);
    dim_info->gridDim = gridDim;
    dim_info->blockDim = blockDim;
    g_hash_table_insert(cuda_kernel_dim_mapping, (gpointer)func, dim_info);

    // g_printerr("Intercepted cudaLaunchKernel: func=%p, gridDim=(%d,%d,%d), blockDim=(%d,%d,%d), sharedMem=%zu\n",
    //            func, gridDim.x, gridDim.y, gridDim.z,
    //            blockDim.x, blockDim.y, blockDim.z, sharedMem);

    return original_cuda_launch_kernel(func, gridDim, blockDim, args, sharedMem, stream);
}

static CUresult peak_cu_launch_kernel(
    CUfunction func,
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra)
{
    KernelDimInfo* dim_info = g_new(KernelDimInfo, 1);
    dim_info->gridDim = (dim3){gridDimX, gridDimY, gridDimZ};
    dim_info->blockDim = (dim3){blockDimX, blockDimY, blockDimZ};
    g_hash_table_insert(cuda_kernel_dim_mapping, (gpointer)func, dim_info);

    // g_printerr("Intercepted cuLaunchKernel: func=%p, gridDim=(%d,%d,%d), blockDim=(%d,%d,%d), sharedMem=%u\n",
    //            func, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes);

    return original_cu_launch_kernel(func, gridDimX, gridDimY, gridDimZ,
                                     blockDimX, blockDimY, blockDimZ,
                                     sharedMemBytes, hStream, kernelParams, extra);
}

int cuda_interceptor_attach()
{
    cudaGetDeviceCount(&device_count);
    cuda_kernel_dim_mapping = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    GumReplaceReturn replace_check = -1;
    cuda_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(cuda_interceptor);
    hook_cuda_launch = gum_find_function("cudaLaunchKernel");
    hook_cu_launch = gum_find_function("cuLaunchKernel");
    if (hook_cuda_launch) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cuda_launch,
            (gpointer*)&peak_cuda_launch_kernel,
            (gpointer*)&original_cuda_launch_kernel);
    }
    if (hook_cu_launch) {
        replace_check = gum_interceptor_replace_fast(
            cuda_interceptor, hook_cu_launch,
            (gpointer*)&peak_cu_launch_kernel,
            (gpointer*)&original_cu_launch_kernel);
    }
    gum_interceptor_end_transaction(cuda_interceptor);

    return replace_check;
}

void cuda_interceptor_dettach()
{
    if (hook_cuda_launch) {
        gum_interceptor_revert(cuda_interceptor, hook_cuda_launch);
    }
    if (hook_cu_launch) {
        gum_interceptor_revert(cuda_interceptor, hook_cu_launch);
    }
    g_object_unref(cuda_interceptor);
    g_hash_table_destroy(cuda_kernel_dim_mapping);
}

void cuda_interceptor_print()
{
    GHashTableIter iter;
    gpointer key, value;

    guint row_width = 77;
    guint max_function_width = 20;
    guint max_col_width = 10;
    char* space_separator = malloc(row_width + 1);
    char* row_separator = malloc(row_width + 1);
    memset(space_separator, ' ', row_width);
    memset(row_separator, '-', row_width);
    space_separator[row_width] = '\0';
    row_separator[row_width] = '\0';

    g_printerr("\n%.*s\n", row_width, row_separator);
    g_printerr("%.*s GPU Statistics %.*s\n", (row_width - 16) / 2, space_separator, (row_width - 16) / 2, space_separator);
    g_printerr("%.*s\n", row_width, row_separator);
    g_printerr("Total GPU Devices Used: %d\n", device_count);
    g_printerr("\n%.*s kernel statistics (gpu) %.*s\n", (row_width - 25) / 2, row_separator, (row_width - 25) / 2, row_separator);
    g_printerr(" kernel dimensions (aggregates by kernel address)\n");
    g_printerr("%.*s\n", row_width, row_separator);
    g_printerr("|%18s|%14s|%14s|%15s|%10s|\n", "Kernel Addr (Host)", "Grid Dims", "Block Dims", "Total Threads", "per GPU");
    g_printerr("%.*s\n", row_width, row_separator);

    g_hash_table_iter_init(&iter, cuda_kernel_dim_mapping);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        KernelDimInfo* dim_info = (KernelDimInfo*)value;

        int total_threads = (dim_info->gridDim.x * dim_info->blockDim.x) *
                            (dim_info->gridDim.y * dim_info->blockDim.y) *
                            (dim_info->gridDim.z * dim_info->blockDim.z);
        int threads_per_gpu = total_threads / device_count;

        g_printerr("|%18p| (%3d,%3d,%3d)| (%3d,%3d,%3d)|%15d|%10d|\n",
                   key, 
                   dim_info->gridDim.x, dim_info->gridDim.y, dim_info->gridDim.z,
                   dim_info->blockDim.x, dim_info->blockDim.y, dim_info->blockDim.z,
                   total_threads,
                   threads_per_gpu);
    }

    g_printerr("%.*s\n", row_width, row_separator);
}
