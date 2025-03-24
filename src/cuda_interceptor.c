#include "cuda_interceptor.h"

static GHashTable* cuda_kernel_local_dim_mapping;
static GMutex cuda_kernel_local_dim_mapping_mutex;
static GumInterceptor* cuda_interceptor;
static gpointer* hook_cuda_launch;
static gpointer* hook_cu_launch;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;

static cudaError_t (*original_cuda_launch_kernel)(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream);

static CUresult (*original_cu_launch_kernel)(
    CUfunction func, 
    unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
    unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
    unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra);

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
} KernelDimInfo;

gboolean str_equal_function(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

static void insert_cuda_mapping_record(gchar* kernel_name, gulong total_threads, gulong grid_size, gulong block_size)
{
    g_mutex_lock(&cuda_kernel_local_dim_mapping_mutex);

    KernelDimInfo* dim_info = g_hash_table_lookup(cuda_kernel_local_dim_mapping, g_strdup(kernel_name));
    if (!dim_info) {
        dim_info = g_new(KernelDimInfo, 1);
        dim_info->total_gpu_threads = total_threads;
        dim_info->total_kernel_call_cnt = 1; 
        dim_info->total_block_size = block_size;
        dim_info->total_grid_size = grid_size;
        dim_info->max_gpu_threads = total_threads;
        dim_info->min_gpu_threads = total_threads;
        dim_info->max_block_size = block_size;
        dim_info->min_block_size = block_size;
        dim_info->max_grid_size = grid_size;
        dim_info->min_grid_size = grid_size;
        g_hash_table_insert(cuda_kernel_local_dim_mapping, g_strdup(kernel_name), dim_info);
    } else {
        dim_info = g_hash_table_lookup(cuda_kernel_local_dim_mapping, g_strdup(kernel_name));
        dim_info->total_gpu_threads += total_threads;
        dim_info->total_kernel_call_cnt++;
        dim_info->total_block_size += block_size;
        dim_info->total_grid_size += grid_size;
        dim_info->max_gpu_threads = max(dim_info->max_gpu_threads, total_threads);
        dim_info->min_gpu_threads = min(dim_info->min_gpu_threads, total_threads);
        dim_info->max_block_size = max(dim_info->max_block_size, block_size);
        dim_info->min_block_size = min(dim_info->min_block_size, block_size);
        dim_info->max_grid_size = max(dim_info->max_grid_size, grid_size);
        dim_info->min_grid_size = min(dim_info->min_grid_size, grid_size);
    }

    g_mutex_unlock(&cuda_kernel_local_dim_mapping_mutex);
    g_free(kernel_name);
}

static cudaError_t peak_cuda_launch_kernel(
    const void* func, dim3 gridDim, dim3 blockDim,
    void** args, size_t sharedMem, cudaStream_t stream)
{
    gchar* kernel_name = gum_symbol_name_from_address((gpointer)func);

    // TODO: compare name
    // FIXME: should we use a hash table to do the compare rather than for loop look up each time?
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (g_strcmp0(peak_hook_strings[i], kernel_name) == 0) {
            gulong total_threads = (gridDim.x * blockDim.x) * (gridDim.y * blockDim.y) * (gridDim.z * blockDim.z);
            gulong grid_size = gridDim.x * gridDim.y * gridDim.z;
            gulong block_size = blockDim.x * blockDim.y * blockDim.z;
            insert_cuda_mapping_record(kernel_name, total_threads, grid_size, block_size);
        }
    }

    return original_cuda_launch_kernel(func, gridDim, blockDim, args, sharedMem, stream);
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
    
    // TODO: compare name
    // FIXME: should we use a hash table to do the compare rather than for loop look up each time?
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (g_strcmp0(peak_hook_strings[i], kernel_name) == 0) {
            gulong total_threads = (gridDimX * blockDimX) * (gridDimY * blockDimY) * (gridDimZ * blockDimZ);
            gulong grid_size = gridDimX * gridDimY * gridDimZ;
            gulong block_size = blockDimX * blockDimY * blockDimZ;
            insert_cuda_mapping_record(kernel_name, total_threads, grid_size, block_size);
        }
    }

    return original_cu_launch_kernel(func, gridDimX, gridDimY, gridDimZ,
                                     blockDimX, blockDimY, blockDimZ,
                                     sharedMemBytes, hStream, kernelParams, extra);
}

int cuda_interceptor_attach()
{
    cuda_kernel_local_dim_mapping = g_hash_table_new_full(g_str_hash, str_equal_function, NULL, g_free);
    g_mutex_init(&cuda_kernel_local_dim_mapping_mutex);

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
    g_hash_table_destroy(cuda_kernel_local_dim_mapping);
    g_object_unref(cuda_interceptor);
}

static void cuda_interceptor_print_result(GHashTable* hashTable)
{
    GHashTableIter iter;
    gpointer key, value;

    guint row_width = 88;
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
    g_printerr("\n%.*s kernel statistics (gpu) %.*s\n", (row_width - 25) / 2 + 1, row_separator, (row_width - 25) / 2, row_separator);
    
    // TODO: Do we need to calculate per pthread/per MPI rank gpu_thread data?
    g_printerr(" kernel call count\n");
    g_printerr("%.*s\n", row_width, row_separator);
    g_printerr("|%*s|%*s|\n",
        max_function_width, "kernel",
        max_col_width, "call count");
    g_printerr("%.*s\n", row_width, row_separator);
    g_hash_table_iter_init(&iter, hashTable);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        KernelDimInfo* dim_info = value;
        g_printerr("|%*s|%*lu|\n",
            max_function_width, key,
            max_col_width, dim_info->total_kernel_call_cnt);
    }
    g_printerr("%.*s\n\n", row_width, row_separator);

    g_printerr("%.*s\n", row_width, row_separator);
    g_printerr(" kernel block & thread size\n");
    g_printerr("%.*s\n", row_width, row_separator);
    g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|%*s|\n",
        max_function_width, "kernel",
        max_col_width, "ave block",
        max_col_width, "ave grid",
        max_col_width, "max block",
        max_col_width, "min block",
        max_col_width, "max grid",
        max_col_width, "min grid"
    );
    g_printerr("%.*s\n", row_width, row_separator);
    g_hash_table_iter_init(&iter, hashTable);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        KernelDimInfo* dim_info = value;
        g_printerr("|%*s|%*.2f|%*.2f|%*lu|%*lu|%*lu|%*lu|\n",
            max_function_width, key,
            max_col_width, (1.0 * dim_info->total_block_size / dim_info->total_kernel_call_cnt),
            max_col_width, (1.0 * dim_info->total_grid_size / dim_info->total_kernel_call_cnt),
            max_col_width, dim_info->max_block_size,
            max_col_width, dim_info->min_block_size,
            max_col_width, dim_info->max_grid_size,
            max_col_width, dim_info->min_grid_size
        );
    }
    g_printerr("%.*s\n\n", row_width, row_separator);

    free(space_separator);
    free(row_separator);
}

// TODO:
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
                existing->max_block_size = max(existing->max_grid_size, global_values_array[i].max_grid_size);
                existing->min_block_size = min(existing->min_grid_size, global_values_array[i].min_grid_size);
            } else {
                g_hash_table_insert(cuda_kernel_global_dim_mapping, g_strdup(global_keys_array[i]), g_memdup(&global_values_array[i], sizeof(KernelDimInfo)));
            }
        }
    }
    
    MPI_Barrier(MPI_COMM_WORLD);

    if (world_rank == 0) {
        cuda_interceptor_print_result(cuda_kernel_global_dim_mapping);
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
        g_free(global_keys_string);
        g_free(keys_offset_array);
        g_free(values_offset_array);
        for (guint i = 0; i < global_kernel_count; i++) {
            g_free(global_keys_array);
        }
        g_free(global_keys_array);
        g_free(global_values_array);
    }
}
#endif

void cuda_interceptor_print() {
    #ifdef HAVE_MPI
        cuda_interceptor_reduce_result();
    #else
        cuda_interceptor_print_result(cuda_kernel_local_dim_mapping);
    #endif
}
