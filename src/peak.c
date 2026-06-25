#define _GNU_SOURCE
#include <dlfcn.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_MPI
#include <mpi.h>
#include "mpi_interceptor.h"
#endif

#ifdef HAVE_CUDA
#include "cuda_interceptor.h"
#endif

#include "general_listener.h"
#include "peak_jit_provider.h"
#include "pthread_listener.h"
#include "syscall_interceptor.h"
#include "dlopen_interceptor.h"
#include "malloc_interceptor.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"
#include "utils/utils.h"

#define PEAK_TARGET_ENV                        "PEAK_TARGET"
#define PEAK_TARGET_FILE_ENV                   "PEAK_TARGET_FILE"
#define PEAK_TARGET_GROUP_ENV                  "PEAK_TARGET_GROUP"
#define PEAK_GPU_TARGET_ENV                    "PEAK_GPU_TARGET"
#define PEAK_GPU_TARGET_FILE_ENV               "PEAK_GPU_TARGET_FILE"
// #define PEAK_GPU_TARGET_GROUP_ENV           "PEAK_GPU_TARGET_GROUP"
#define PEAK_GPU_MONITOR_ALL                   "PEAK_GPU_MONITOR_ALL"
#define PEAK_NAME_TRUNCATE                     "PEAK_NAME_TRUNCATE"
#define PEAK_TARGET_DELIM                     ','
#define PEAK_COST_ENV                          "PEAK_COST"
#define PEAK_HEARTBEAT_INTERVAL_ENV            "PEAK_HEARTBEAT_INTERVAL"
#define PEAK_HIBERNATION_CYCLE_ENV             "PEAK_HIBERNATION_CYCLE"
#define PEAK_OVERHEAD_RATIO_ENV                "PEAK_OVERHEAD_RATIO"
#define PEAK_GLOBAL_OVERHEAD_RATIO_ENV         "PEAK_GLOBAL_OVERHEAD_RATIO"
#define PEAK_GLOBAL_DETACH_FACTOR_ENV          "PEAK_GLOBAL_DETACH_FACTOR"
#define PEAK_GLOBAL_REATTACH_FACTOR_ENV        "PEAK_GLOBAL_REATTACH_FACTOR"
#define PEAK_ENABLE_PER_TARGET_HEARTBEAT_ENV   "PEAK_ENABLE_PER_TARGET_HEARTBEAT"
#define PEAK_ENABLE_GLOBAL_HEARTBEAT_ENV       "PEAK_ENABLE_GLOBAL_HEARTBEAT"
#define PEAK_ENABLE_REATTACH_ENV               "PEAK_ENABLE_REATTACH"
#define PEAK_PAUSE_TIMEOUT_ENV                 "PEAK_PAUSE_TIMEOUT"
#define PEAK_SIG_CONT_TIMEOUT_ENV              "PEAK_SIG_CONT_TIMEOUT"
#define PEAK_HB_MIN_US_ENV                     "PEAK_HB_MIN_US"
#define PEAK_HB_MAX_US_ENV                     "PEAK_HB_MAX_US"
#define PEAK_HB_K_ERR_ENV                      "PEAK_HB_K_ERR"
#define PEAK_HB_K_RATE_ENV                     "PEAK_HB_K_RATE"
#define PEAK_HB_EMA_A_ENV                      "PEAK_HB_EMA_A"
#define PEAK_MAX_NUM_THREADS_ENV               "PEAK_MAX_NUM_THREADS"
#define PEAK_MEMORY_PROFILE                    "PEAK_MEMORY_PROFILE"
#define PEAK_MEMORY_TRACK_ALL                  "PEAK_MEMORY_TRACK_ALL"
#define PEAK_OUTPUT_AGGREGATION_ENV            "PEAK_OUTPUT_AGGREGATION"
#define PEAK_MPI_COLLECTIVE_OUTPUT_ENV         "PEAK_MPI_COLLECTIVE_OUTPUT"
#define PPID_FILE_NAME                         "/tmp/lock_peak_ppid_list"


gboolean* peak_need_detach;
gboolean* peak_detached;
gdouble* heartbeat_overhead;
gboolean** peak_target_thread_called;
static size_t peak_target_thread_called_count;
PeakHeartbeatArgs* args;
extern _Atomic gboolean heartbeat_running;
pthread_t heartbeat_thread;
PEAK_API size_t peak_hook_address_count;
unsigned int heartbeat_time;
unsigned int check_interval;
unsigned long long sig_stop_ack_wait_interval;
unsigned long long sig_cont_wait_interval;
float target_profile_ratio;
float global_target_ratio;
float peak_global_reattach_factor;
float peak_global_detach_factor;
bool enable_per_target_heartbeat;
bool enable_global_heartbeat;
bool enable_reattach;
unsigned int hb_min_us;
unsigned int hb_max_us;
double hb_k_err;
double hb_k_rate;
double hb_ema_a;
size_t peak_gpu_hook_address_count;
char** peak_hook_strings;
char** peak_gpu_hook_strings;
gulong peak_max_num_threads;
double peak_main_time;
float peak_detach_cost;
gboolean peak_gpu_monitor_all = false;
gboolean peak_truncate_function_name = false;
gboolean peak_memory_profile = false;
gboolean peak_memory_track_all = false;
#ifdef HAVE_MPI
static int found_MPI;
static int flag_clean_fppid = 0;
#endif

static _Atomic int peak_exit_status_known = 0;
static _Atomic int peak_exit_status_value = 0;
typedef enum {
    PEAK_FINI_NOT_STARTED = 0,
    PEAK_FINI_IN_PROGRESS = 1,
    PEAK_FINI_DONE = 2,
} PeakFiniState;

static _Atomic int peak_fini_state = PEAK_FINI_NOT_STARTED;

static gboolean
peak_env_value_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

#ifdef HAVE_MPI
static PeakOutputAggregationMode
peak_output_aggregation_mode_from_value(const char* name,
                                        const char* value,
                                        gboolean legacy_collective)
{
    if (value == NULL || value[0] == '\0') {
        return PEAK_OUTPUT_AGGREGATION_MPI;
    }

    if (legacy_collective && peak_env_value_truthy(value)) {
        return PEAK_OUTPUT_AGGREGATION_MPI;
    }

    if (g_ascii_strcasecmp(value, "mpi") == 0 ||
        g_ascii_strcasecmp(value, "collective") == 0 ||
        peak_env_value_truthy(value)) {
        return PEAK_OUTPUT_AGGREGATION_MPI;
    }

    if (g_ascii_strcasecmp(value, "socket") == 0 ||
        g_ascii_strcasecmp(value, "tcp") == 0 ||
        g_ascii_strcasecmp(value, "interconnect") == 0) {
        return PEAK_OUTPUT_AGGREGATION_SOCKET;
    }

    if (g_ascii_strcasecmp(value, "0") == 0 ||
        g_ascii_strcasecmp(value, "false") == 0 ||
        g_ascii_strcasecmp(value, "no") == 0 ||
        g_ascii_strcasecmp(value, "off") == 0 ||
        g_ascii_strcasecmp(value, "none") == 0 ||
        g_ascii_strcasecmp(value, "local") == 0 ||
        g_ascii_strcasecmp(value, "rank-local") == 0) {
        return PEAK_OUTPUT_AGGREGATION_LOCAL;
    }

    g_printerr("[peak] Unknown %s=%s; disabling aggregate output\n",
               name,
               value);
    return PEAK_OUTPUT_AGGREGATION_LOCAL;
}

static PeakOutputAggregationMode
peak_output_aggregation_mode(void)
{
    const char* aggregation = getenv(PEAK_OUTPUT_AGGREGATION_ENV);
    const char* value = getenv(PEAK_MPI_COLLECTIVE_OUTPUT_ENV);

    if (aggregation != NULL && aggregation[0] != '\0') {
        return peak_output_aggregation_mode_from_value(
            PEAK_OUTPUT_AGGREGATION_ENV,
            aggregation,
            FALSE);
    }

    if (value != NULL && value[0] != '\0') {
        return peak_output_aggregation_mode_from_value(
            PEAK_MPI_COLLECTIVE_OUTPUT_ENV,
            value,
            TRUE);
    }

    return PEAK_OUTPUT_AGGREGATION_MPI;
}

static gboolean
peak_output_aggregation_explicit(void)
{
    const char* aggregation = getenv(PEAK_OUTPUT_AGGREGATION_ENV);

    return aggregation != NULL && aggregation[0] != '\0';
}
#endif

void peak_init()
{

    gulong default_max_threads = (gulong)sysconf(_SC_NPROCESSORS_ONLN) * 2;
    peak_max_num_threads =
        parse_env_to_uint_default(PEAK_MAX_NUM_THREADS_ENV,
                                  (unsigned int)default_max_threads);
    peak_hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &peak_hook_strings);
    peak_hook_address_count += load_profiling_symbols(PEAK_TARGET_FILE_ENV, &peak_hook_strings, peak_hook_address_count);
    peak_hook_address_count += load_symbols_from_array(PEAK_TARGET_GROUP_ENV, &peak_hook_strings, peak_hook_address_count);
    peak_gpu_hook_address_count = parse_env_w_delim(PEAK_GPU_TARGET_ENV, PEAK_TARGET_DELIM, &peak_gpu_hook_strings);
    peak_gpu_hook_address_count += load_profiling_symbols(PEAK_GPU_TARGET_FILE_ENV, &peak_gpu_hook_strings, peak_gpu_hook_address_count);
    // TODO: add pre-defined kernels in the future: CUBLAS
    // peak_gpu_hook_address_count += load_symbols_from_array(PEAK_GPU_TARGET_GROUP, &peak_gpu_hook_strings, peak_gpu_hook_address_count);
    peak_detach_cost = parse_env_to_float(PEAK_COST_ENV);
    peak_gpu_monitor_all = parse_env_to_bool(PEAK_GPU_MONITOR_ALL);
    peak_truncate_function_name = parse_env_to_bool(PEAK_NAME_TRUNCATE);
    heartbeat_time = parse_env_to_time(PEAK_HEARTBEAT_INTERVAL_ENV);
    check_interval = parse_env_to_interval(PEAK_HIBERNATION_CYCLE_ENV);
    target_profile_ratio = parse_env_to_float_ratio(PEAK_OVERHEAD_RATIO_ENV);
    global_target_ratio = parse_env_to_float_ratio(PEAK_GLOBAL_OVERHEAD_RATIO_ENV);
    peak_global_detach_factor = parse_env_to_float_detach_factor(PEAK_GLOBAL_DETACH_FACTOR_ENV);
    peak_global_reattach_factor = parse_env_to_float_reattach_factor(PEAK_GLOBAL_REATTACH_FACTOR_ENV);
    enable_per_target_heartbeat = parse_env_to_bool(PEAK_ENABLE_PER_TARGET_HEARTBEAT_ENV);
    enable_global_heartbeat = parse_env_to_bool(PEAK_ENABLE_GLOBAL_HEARTBEAT_ENV);
    const char* enable_reattach_env = getenv(PEAK_ENABLE_REATTACH_ENV);
    enable_reattach =
        (enable_reattach_env == NULL) || parse_env_to_bool(PEAK_ENABLE_REATTACH_ENV);
    sig_stop_ack_wait_interval = parse_env_to_post_interval(PEAK_PAUSE_TIMEOUT_ENV);
    sig_cont_wait_interval = parse_env_to_post_interval(PEAK_SIG_CONT_TIMEOUT_ENV);
    hb_min_us = parse_env_to_uint_default(PEAK_HB_MIN_US_ENV, 10000);
    hb_max_us = parse_env_to_uint_default(PEAK_HB_MAX_US_ENV, 500000);
    hb_k_err = parse_env_to_double_default(PEAK_HB_K_ERR_ENV, 3.0);
    hb_k_rate = parse_env_to_double_default(PEAK_HB_K_RATE_ENV, 0.8);
    hb_ema_a = parse_env_to_double_default(PEAK_HB_EMA_A_ENV, 0.3);
    if (hb_max_us < hb_min_us) {
        hb_max_us = hb_min_us;
    }
    if (hb_ema_a <= 0.0 || hb_ema_a > 1.0) {
        hb_ema_a = 0.3;
    }
    peak_memory_profile = parse_env_to_bool(PEAK_MEMORY_PROFILE);
    peak_memory_track_all = parse_env_to_bool(PEAK_MEMORY_TRACK_ALL);
   
    //gum_init_embedded();

    pthread_listener_attach();
    syscall_interceptor_attach();
    dlopen_interceptor_attach();
#ifdef HAVE_MPI
    found_MPI = check_MPI();
    if (found_MPI) {
        int is_parent_MPI = check_parent_process(PPID_FILE_NAME, &flag_clean_fppid);
        if (is_parent_MPI > 0) {
            found_MPI = 0;
        } else if (mpi_interceptor_attach() != 0) {
            found_MPI = 0;
        }
    }
#endif
#ifdef HAVE_CUDA
    cuda_interceptor_attach();
#endif
    // general listener needs to be after pthread and mpi ones
    peak_target_thread_called = g_new0(gboolean*, peak_hook_address_count);
    peak_target_thread_called_count = peak_hook_address_count;
    for (gint i = 0; i < peak_hook_address_count; i++) {
        peak_target_thread_called[i] = g_new0(gboolean, peak_max_num_threads);
    }
    peak_need_detach = g_new0(gboolean, peak_hook_address_count);
    peak_detached = g_new0(gboolean, peak_hook_address_count);
    peak_jit_provider_enable();
    peak_general_listener_attach();
    dlopen_interceptor_enable_dynamic_attach();
    if (heartbeat_time != 0) {
        heartbeat_overhead = g_new0(gdouble, peak_hook_address_count);
        args = g_new0(PeakHeartbeatArgs, 1);
        args->heartbeat_time = heartbeat_time;
        args->check_interval = check_interval;
        args->hb_min_us = hb_min_us;
        args->hb_max_us = hb_max_us;
        args->hb_k_err = hb_k_err;
        args->hb_k_rate = hb_k_rate;
        args->hb_ema_a = hb_ema_a;
        pthread_mutex_lock(&heartbeat_mutex);
        atomic_store(&heartbeat_running, true);
        pthread_mutex_unlock(&heartbeat_mutex);
        // create heartbeat thread
        if (pthread_create(&heartbeat_thread, NULL, peak_heartbeat_monitor, args) != 0) {
            perror("Failed to create heartbeat thread");
            g_free(args);
            args = NULL;
            g_free(heartbeat_overhead);
            heartbeat_overhead = NULL;
            exit(EXIT_FAILURE);
        }
    }
    
    peak_main_time = peak_second();
    
    if (peak_memory_profile) {
        malloc_interceptor_attach();
    }
}

#ifdef HAVE_MPI
static int
peak_mpi_runtime_allows_collectives(void)
{
    int initialized = 0;
    int finalized = 0;

    MPI_Initialized(&initialized);
    MPI_Finalized(&finalized);
    return initialized && !finalized;
}

static int
peak_mpi_all_ranks_requested_finalize(int local_requested)
{
    int all_requested = 0;

    MPI_Allreduce(&local_requested,
                  &all_requested,
                  1,
                  MPI_INT,
                  MPI_MIN,
                  MPI_COMM_WORLD);
    return all_requested != 0;
}
#endif

static void
peak_fini_impl(void)
{
#ifdef HAVE_MPI
    int mpi_finalize_path =
        found_MPI && mpi_interceptor_finalize_path_active();
    if (mpi_finalize_path) {
        peak_general_listener_suspend_callbacks();
    }
#endif

    if (heartbeat_time != 0) {
        pthread_mutex_lock(&heartbeat_mutex);
        atomic_store(&heartbeat_running, false);
        pthread_cond_signal(&heartbeat_cond);
        pthread_mutex_unlock(&heartbeat_mutex);
        pthread_join(heartbeat_thread, NULL);
        if (heartbeat_overhead) {
            g_free(heartbeat_overhead);
            heartbeat_overhead = NULL;
        }
        if (args) {
            g_free(args);
            args = NULL;
        }
    }
    peak_main_time = peak_second() - peak_main_time;

    peak_general_listener_controller_stop();
    peak_jit_provider_disable();
    if (
#ifdef HAVE_MPI
        !mpi_finalize_path &&
#endif
        peak_memory_profile) {
        malloc_interceptor_detach();
    }
    gboolean dlopen_shutdown_flushed = TRUE;
#ifdef HAVE_MPI
    if (!mpi_finalize_path)
#endif
    {
        dlopen_shutdown_flushed = dlopen_interceptor_dettach();
    }
    if (!dlopen_shutdown_flushed) {
    #ifdef HAVE_MPI
        if (flag_clean_fppid) {
            remove_ppid_file(PPID_FILE_NAME);
        }
    #endif
        g_printerr("[peak] Skipping remaining PEAK teardown because dlopen replacement teardown was not proven safe\n");
        return;
    }
#ifdef HAVE_MPI
    if (flag_clean_fppid) {
        remove_ppid_file(PPID_FILE_NAME);
    }
    int exit_status_known =
        atomic_load_explicit(&peak_exit_status_known, memory_order_acquire);
    int exit_status =
        atomic_load_explicit(&peak_exit_status_value, memory_order_acquire);
    int abnormal_exit = exit_status_known == 2 && exit_status != 0;
    /*
     * Error exits often happen while only a subset of ranks is unwinding. Do
     * not let PEAK introduce MPI collectives there.
     */
    PeakOutputAggregationMode aggregation_mode =
        found_MPI ? peak_output_aggregation_mode()
                  : PEAK_OUTPUT_AGGREGATION_LOCAL;
    gboolean aggregation_explicit =
        found_MPI && peak_output_aggregation_explicit();
    int mpi_runtime_can_collect =
        found_MPI && peak_mpi_runtime_allows_collectives();
    int mpi_log_rank = 1;
    if (mpi_runtime_can_collect) {
        int mpi_rank = 0;
        MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
        mpi_log_rank = mpi_rank == 0;
    }
    int local_requested_mpi_finalize =
        mpi_interceptor_finalize_was_requested();
    int all_ranks_requested_mpi_finalize = 0;
    int need_mpi_finalize_proof =
        mpi_finalize_path ||
        aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI;
    if (need_mpi_finalize_proof &&
        mpi_runtime_can_collect &&
        !abnormal_exit) {
        all_ranks_requested_mpi_finalize =
            peak_mpi_all_ranks_requested_finalize(
                local_requested_mpi_finalize ? 1 : 0);
    }
    int use_mpi_collective_output =
        aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
        mpi_runtime_can_collect &&
        !abnormal_exit && all_ranks_requested_mpi_finalize;
    int use_socket_output =
        aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET &&
        mpi_runtime_can_collect &&
        !abnormal_exit &&
        (!mpi_finalize_path || aggregation_explicit);
    PeakOutputAggregationMode output_mode =
        use_mpi_collective_output ? PEAK_OUTPUT_AGGREGATION_MPI :
        use_socket_output ? PEAK_OUTPUT_AGGREGATION_SOCKET :
        PEAK_OUTPUT_AGGREGATION_LOCAL;
    int allow_real_mpi_finalize =
        mpi_finalize_path &&
        mpi_runtime_can_collect &&
        !abnormal_exit &&
        all_ranks_requested_mpi_finalize;
    if (found_MPI && abnormal_exit && local_requested_mpi_finalize && mpi_log_rank) {
        g_printerr("[peak] PMPI_Finalize was requested before nonzero exit status %d; skipping aggregate output\n",
                   exit_status);
    } else if (found_MPI && use_mpi_collective_output && mpi_log_rank) {
        g_printerr("[peak] PMPI_Finalize was observed on every rank; writing MPI-reduced output before MPI finalization or process exit\n");
    } else if (found_MPI && use_socket_output && mpi_log_rank) {
        g_printerr("[peak] Writing PEAK-owned socket-reduced output before MPI finalization or process exit\n");
    } else if (found_MPI && aggregation_mode == PEAK_OUTPUT_AGGREGATION_LOCAL && mpi_log_rank) {
        g_printerr("[peak] Aggregate output is disabled for strict teardown; writing rank-local output before MPI finalization or process exit\n");
    } else if (found_MPI && !mpi_runtime_can_collect && mpi_log_rank) {
        g_printerr("[peak] MPI runtime is not in an output-safe state; writing rank-local output before process exit\n");
    } else if (found_MPI &&
               aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
               !all_ranks_requested_mpi_finalize &&
               mpi_log_rank) {
        g_printerr("[peak] PMPI_Finalize was not observed on every rank; writing rank-local output before process exit\n");
    } else if (found_MPI && output_mode == PEAK_OUTPUT_AGGREGATION_LOCAL && mpi_log_rank) {
        g_printerr("[peak] Aggregate output was not proven safe; writing rank-local output before process exit\n");
    }
    if (found_MPI && mpi_finalize_path) {
        mpi_interceptor_set_real_finalize_allowed(allow_real_mpi_finalize);
        if (mpi_log_rank) {
            if (allow_real_mpi_finalize) {
                g_printerr("[peak] PEAK output is complete; returning to real PMPI_Finalize\n");
            } else {
                g_printerr("[peak] Real PMPI_Finalize is not proven all-rank safe; skipping real MPI finalizer\n");
            }
        }
    }
    peak_general_listener_print(output_mode);
    #ifdef HAVE_CUDA
        cuda_interceptor_print(use_mpi_collective_output);
        if (!mpi_finalize_path) {
            cuda_interceptor_dettach();
        }
    #endif
    if (found_MPI && !mpi_finalize_path) {
        mpi_interceptor_dettach(0);
    }
    if (found_MPI && mpi_finalize_path) {
        if (mpi_log_rank) {
            g_printerr("[peak] Leaving PEAK target hooks pinned and restoring support wrappers before application PMPI_Finalize\n");
        }
        syscall_interceptor_dettach();
        if (!pthread_listener_dettach()) {
            g_printerr("[peak] Leaving pthread listener bookkeeping allocated before application PMPI_Finalize\n");
        }
        return;
    }
#else
    peak_general_listener_print(0);
    #ifdef HAVE_CUDA
    cuda_interceptor_print(0);
    cuda_interceptor_dettach();
    #endif
#endif
    gboolean general_listener_shutdown_flushed = peak_general_listener_dettach();
    if (general_listener_shutdown_flushed) {
        dlopen_interceptor_release_retained_dynamic_handles();
    }
    syscall_interceptor_dettach();
    if (general_listener_shutdown_flushed) {
        if (!pthread_listener_dettach()) {
            g_printerr("[peak] Leaving pthread listener bookkeeping allocated for in-flight callbacks\n");
        }
    } else {
        g_printerr("[peak] Skipping pthread listener cleanup because general listener teardown is still reachable\n");
    }
    free_parsed_result(peak_hook_strings, peak_hook_address_count);
    if (general_listener_shutdown_flushed) {
        for (size_t i = 0; i < peak_target_thread_called_count; i++) {
            g_free(peak_target_thread_called[i]);
        }
        g_free(peak_target_thread_called);
        peak_target_thread_called = NULL;
        peak_target_thread_called_count = 0;
        g_free(peak_need_detach);
        g_free(peak_detached);
    } else {
        g_printerr("[peak] Leaving general listener bookkeeping allocated for in-flight callbacks\n");
    }
}

void peak_fini()
{
    int expected = PEAK_FINI_NOT_STARTED;

    if (atomic_compare_exchange_strong_explicit(
            &peak_fini_state,
            &expected,
            PEAK_FINI_IN_PROGRESS,
            memory_order_acq_rel,
            memory_order_acquire)) {
        peak_fini_impl();
        atomic_store_explicit(&peak_fini_state,
                              PEAK_FINI_DONE,
                              memory_order_release);
        return;
    }

    while (atomic_load_explicit(&peak_fini_state, memory_order_acquire) ==
           PEAK_FINI_IN_PROGRESS) {
        sched_yield();
    }
}

#if defined(__APPLE__)
__attribute__((used, section("__DATA,__mod_init_func"))) void* __init = peak_init;
__attribute__((used, section("__DATA,__mod_fini_func"))) void* __fini = peak_fini;
#elif defined(__ELF__)
//__attribute__((section(".init_array"))) void* __init = peak_init;
//__attribute__((section(".fini_array"))) void* __fini = peak_fini;
typedef int (*main_fn)(int, char**, char**);
typedef int (*libc_start_main_fn)(main_fn, int, char**, 
                                  int (*)(int, char**, char**),
                                  void (*)(void), void (*)(void), void*);

static main_fn real_main = NULL;
static libc_start_main_fn real___libc_start_main = NULL;

static gboolean
peak_should_wrap_main(int argc, char** argv)
{
    return peak_should_profile_command(argc, argv);
}

// Original function pointer for `exit`
static void (*original_exit)(int) = NULL;
static GumInterceptor* exit_interceptor = NULL;
static gpointer exit_address = NULL;
void exit_interceptor_detach();

static void
peak_exit(int status) {
    //g_printerr("Custom exit called with status: %d\n", status);

    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&peak_exit_status_known,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_store_explicit(&peak_exit_status_value,
                              status,
                              memory_order_release);
        atomic_store_explicit(&peak_exit_status_known,
                              2,
                              memory_order_release);
    } else {
        while (atomic_load_explicit(&peak_exit_status_known,
                                    memory_order_acquire) == 1) {
            sched_yield();
        }
    }
    peak_fini();
    atexit(exit_interceptor_detach);

    // Call the original `exit` function to terminate the process
    original_exit(status);
}

/**
 * @brief Attaches the interceptor to the `exit` function.
 *
 * This function uses the Gum API to intercept calls to the `exit` function, 
 * replacing it with a custom implementation (`peak_exit`).
 *
 * @return 0 on success, -1 on failure.
 */
int exit_interceptor_attach() {
    gum_init_embedded();
    GumReplaceReturn replace_check = -1;
    exit_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(exit_interceptor);
    exit_address = gum_find_function("exit");
    if (exit_address) {
        replace_check = gum_interceptor_replace_fast(exit_interceptor,
                                      exit_address, (gpointer)&peak_exit,
                                      (gpointer*)(&original_exit));
    }
    gum_interceptor_end_transaction(exit_interceptor);
    return replace_check;
}

/**
 * @brief Detaches the interceptor from the `exit` function.
 *
 * This function reverts the interception of the `exit` function, restoring its 
 * original behavior.
 */
void exit_interceptor_detach() {
    if (exit_interceptor == NULL || exit_address == NULL) {
        return;
    }

    gum_interceptor_begin_transaction(exit_interceptor);
    gum_interceptor_revert(exit_interceptor, exit_address);
    gum_interceptor_end_transaction(exit_interceptor);
    if (!gum_interceptor_flush(exit_interceptor)) {
        g_printerr("[peak] exit interceptor teardown did not flush; leaving exit interceptor state alive\n");
        return;
    }
    exit_address = NULL;
}

static int main_wrapper(int argc, char** argv, char** envp) {
    // Call peak_init before main
    // fprintf(stderr, "[LD_PRELOAD] main started. Running my code now.\n");
    if (!exit_interceptor_attach()) {
        peak_init();
    }

    int ret = real_main(argc, argv, envp);

    return ret;
}

__attribute__((visibility("default")))
int __libc_start_main(main_fn main, int argc, char** argv,
                      int (*init)(int, char**, char**),
                      void (*fini)(void), void (*rtld_fini)(void), void* stack_end) {
    // fprintf(stderr, "Running my code now.\n");
    if (!real___libc_start_main) {
        real___libc_start_main = (libc_start_main_fn)dlsym(RTLD_NEXT, "__libc_start_main");
        if (!real___libc_start_main) {
            fprintf(stderr, "Error: dlsym failed to find __libc_start_main\n");
            _exit(1);
        }
    }

    // Store the original main function pointer
    real_main = main;

    // Decide whether to use the main wrapper based on argv.
    gboolean should_wrap = peak_should_wrap_main(argc, argv);
    peak_set_process_profile_enabled(should_wrap);
    if (should_wrap) {
        return real___libc_start_main(main_wrapper, argc, argv, init, fini, rtld_fini, stack_end);
    } else {
        return real___libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end);
    }
}
#else
#error Unsupported platform
#endif
