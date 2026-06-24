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
#define PEAK_JIT_ENABLE_ENV                    "PEAK_JIT_ENABLE"
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

static gboolean
peak_mpi_collective_output_enabled(void)
{
    const char* value = getenv(PEAK_MPI_COLLECTIVE_OUTPUT_ENV);

    if (value != NULL && value[0] != '\0') {
        return peak_env_value_truthy(value);
    }

    return TRUE;
}

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
    if (peak_memory_profile) {
        malloc_interceptor_detach();
    }
    gboolean dlopen_shutdown_flushed = dlopen_interceptor_dettach();
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
     * Error exits often happen while only a subset of ranks is unwinding.  Do
     * not let PEAK introduce MPI collectives or delayed PMPI_Finalize there;
     * rank-local output is the only safe profiler behavior.
     */
    int mpi_collective_output_enabled =
        found_MPI && peak_mpi_collective_output_enabled();
    int mpi_runtime_can_collect =
        found_MPI && peak_mpi_runtime_allows_collectives();
    int local_requested_mpi_finalize =
        mpi_interceptor_finalize_was_requested();
    int all_ranks_requested_mpi_finalize = 0;
    if (mpi_collective_output_enabled && mpi_runtime_can_collect &&
        !abnormal_exit) {
        all_ranks_requested_mpi_finalize =
            peak_mpi_all_ranks_requested_finalize(
                local_requested_mpi_finalize ? 1 : 0);
    }
    int app_requested_mpi_finalize =
        mpi_collective_output_enabled && mpi_runtime_can_collect &&
        !abnormal_exit && all_ranks_requested_mpi_finalize;
    if (found_MPI && abnormal_exit && local_requested_mpi_finalize) {
        g_printerr("[peak] PMPI_Finalize was requested before nonzero exit status %d; writing rank-local output and skipping PEAK-driven MPI_Finalize\n",
                   exit_status);
    } else if (found_MPI && !mpi_collective_output_enabled) {
        g_printerr("[peak] MPI collective output is disabled for strict teardown; writing rank-local output and skipping PEAK-driven MPI_Finalize\n");
    } else if (found_MPI && !mpi_runtime_can_collect) {
        g_printerr("[peak] MPI runtime is not in a collective-safe state; writing rank-local output and skipping PEAK-driven MPI_Finalize\n");
    } else if (found_MPI && !all_ranks_requested_mpi_finalize) {
        g_printerr("[peak] PMPI_Finalize was not observed on every rank; writing rank-local output and skipping PEAK-driven MPI_Finalize\n");
    } else if (found_MPI && !app_requested_mpi_finalize) {
        g_printerr("[peak] PMPI_Finalize was not observed; writing rank-local output and skipping PEAK-driven MPI_Finalize\n");
    }
    peak_general_listener_print(app_requested_mpi_finalize);
    #ifdef HAVE_CUDA
        cuda_interceptor_print(app_requested_mpi_finalize);
        cuda_interceptor_dettach();
    #endif
    if (found_MPI)
        mpi_interceptor_dettach(app_requested_mpi_finalize);
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
peak_env_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static const char*
peak_command_base_name(const char* path)
{
    const char* base;

    if (path == NULL) {
        return NULL;
    }

    base = strrchr(path, '/');
    return base != NULL ? base + 1 : path;
}

static gboolean
peak_command_is_jit_runtime(const char* command)
{
    const char* base = peak_command_base_name(command);

    return base != NULL &&
           (strcmp(base, "node") == 0 ||
            strcmp(base, "nodejs") == 0);
}

static gboolean
peak_should_wrap_main(const char* command)
{
    if (command == NULL) {
        return FALSE;
    }

    if (!check_command(command)) {
        return TRUE;
    }

    return peak_env_truthy(getenv(PEAK_JIT_ENABLE_ENV)) &&
           peak_command_is_jit_runtime(command);
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

    // Decide whether to use the main wrapper based on argv[0].
    if (peak_should_wrap_main(argv[0])) {
        return real___libc_start_main(main_wrapper, argc, argv, init, fini, rtld_fini, stack_end);
    } else {
        return real___libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end);
    }
}
#else
#error Unsupported platform
#endif
