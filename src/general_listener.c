#define _GNU_SOURCE
#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "internal/general_listener_internal.h"
#include "internal/jit_provider.h"
#include "detach_controller.h"
#include "logging.h"
#include "pthread_listener.h"
#include "internal/unsafe_gum_prologue.h"
#include <errno.h>
#include <dirent.h>
#include <float.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

#define PEAK_HEARTBEAT_MIN_OBSERVATION_US 10000U
#define PEAK_GLOBAL_DETACH_MIN_CALLS 2U

#define PEAK_SIG_STOP (SIGRTMIN + 0)
#define PEAK_SIG_CONT (SIGRTMIN + 1)
#define PEAK_TEXT_OUTPUT_ENV "PEAK_TEXT_OUTPUT"
#define PEAK_OUTPUT_AGGREGATION_HOST_ENV "PEAK_OUTPUT_AGGREGATION_HOST"
#define PEAK_OUTPUT_AGGREGATION_PORT_ENV "PEAK_OUTPUT_AGGREGATION_PORT"
#define PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV "PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"
#define PEAK_OUTPUT_AGGREGATION_TOKEN_ENV "PEAK_OUTPUT_AGGREGATION_TOKEN"
#define PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV \
    "PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"
#define PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_ENV \
    "PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS"
#define PEAK_MPI_FINALIZE_REQUEST_TIMEOUT_MS_DEFAULT 250
#define PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV \
    "PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS"
#define PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT 5000
#define PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL"
#define PEAK_TEST_MPI_REDUCER_FAIL_LABEL_ENV \
    "PEAK_TEST_MPI_REDUCER_FAIL_LABEL"
#define PEAK_CONTROLLER_MAX_PENDING_AGE_MS_ENV "PEAK_CONTROLLER_MAX_PENDING_AGE_MS"
#define PEAK_CONTROLLER_MAX_RETRY_COUNT_ENV "PEAK_CONTROLLER_MAX_RETRY_COUNT"
#define PEAK_REATTACH_COOLDOWN_MS_ENV "PEAK_REATTACH_COOLDOWN_MS"

PEAK_API GumInterceptor* interceptor;
PEAK_API GumInvocationListener** array_listener;
static gboolean* array_listener_detached;
static gboolean* array_listener_reattached;
static gboolean* array_listener_revisited;
static gboolean* array_listener_gum_detached;
static gboolean* array_listener_gum_detach_flushed;
static double* peak_hook_last_detach_time;
static PeakHookState* peak_hook_states;
static double* peak_hook_next_retry_time;
static double* peak_hook_pending_observed_time;
typedef enum {
    PEAK_HOOK_REQUEST_SOURCE_NONE = 0,
    PEAK_HOOK_REQUEST_SOURCE_API,
    PEAK_HOOK_REQUEST_SOURCE_DETACH_COUNT,
    PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT,
    PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT
} PeakHookRequestSource;
static PeakHookRequestSource* peak_hook_pending_request_source;
static gulong* peak_hook_pending_request_calls;
static double* peak_hook_pending_request_ratio;
static double* peak_hook_pending_request_global_overhead;
static double* peak_hook_pending_request_total_time;
static double* peak_hook_pending_request_rate;
static double* peak_hook_detach_profile_seconds;
static gulong* peak_hook_reattach_request_calls;
static gboolean* peak_hook_reattach_request_calls_valid;
static gulong* peak_hook_cached_sample_calls;
static double* peak_hook_cached_sample_profile_seconds;
static gboolean* peak_hook_cached_sample_valid;
static unsigned int* peak_hook_retry_count;
static PeakDetachStatus* peak_hook_last_retry_status;
static const char*
peak_hook_request_source_string(PeakHookRequestSource source);
static gboolean
peak_general_listener_startup_attach_can_skip_stop(void);
static gboolean
peak_general_listener_refresh_hook_sample_cache_unlocked(
    size_t hook_id,
    gulong* calls_out,
    double* profile_seconds_out);
static gboolean
peak_general_listener_try_hook_sample_cache_unlocked(
    size_t hook_id,
    gulong* calls_out,
    double* profile_seconds_out);
static _Atomic unsigned long long peak_general_listener_runtime_start_ns = 0;
static _Atomic unsigned long long
    peak_general_listener_heartbeat_control_baseline_ns = 0;
static _Atomic unsigned long long
    peak_general_listener_heartbeat_control_baseline_count = 0;
static _Atomic unsigned long long
    peak_general_listener_heartbeat_control_baseline_failed_count = 0;
static _Atomic gboolean
    peak_general_listener_heartbeat_control_baseline_valid = FALSE;
static _Atomic unsigned long long
    peak_general_listener_heartbeat_management_cpu_ns = 0;
static double peak_general_listener_final_heartbeat_overhead_seconds = 0.0;
typedef struct {
    gboolean valid;
    gboolean accounting_valid;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    double ratio;
} PeakFinalReportSnapshot;
static PeakFinalReportSnapshot peak_general_listener_final_report_snapshot;
extern gboolean* peak_need_detach;
extern gboolean* peak_detached;
extern gdouble* heartbeat_overhead;
extern unsigned int check_interval;
PEAK_API gpointer* hook_address = NULL;
static double peak_general_overhead;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
char** peak_demangled_strings;
extern gulong peak_max_num_threads;
extern double peak_main_time;
extern float peak_detach_cost;
extern float target_profile_ratio;
extern float global_target_ratio;
extern float peak_global_reattach_factor;
extern float peak_global_detach_factor;
extern bool enable_per_target_heartbeat;
extern bool enable_global_heartbeat;
extern bool enable_reattach;
extern unsigned long long sig_cont_wait_interval;
extern unsigned long long sig_stop_ack_wait_interval;
extern unsigned int heartbeat_time;
static gulong peak_detach_count = G_MAXULONG;
static gboolean peak_detach_count_overridden = FALSE;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
_Atomic gboolean heartbeat_running = true;
static pthread_t general_controller_thread;
static pthread_mutex_t general_controller_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t general_controller_wake_cond = PTHREAD_COND_INITIALIZER;
static gboolean general_controller_running = FALSE;
static gboolean general_controller_thread_started = FALSE;
static pthread_t general_controller_owner_thread;
static _Atomic gboolean general_controller_owner_known = FALSE;
static gboolean gum_find_functions_matching_initialize = false;
static GHashTable* gum_symbol_demangled_mapping;
static GHashTable* gum_symbol_short_mapping;
static const double peak_general_overhead_floor = 1e-9;
static pthread_mutex_t detach_trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static const double peak_controller_retry_base_delay = 0.001;
static const double peak_controller_retry_max_delay = 0.050;
static const unsigned int peak_controller_default_max_retry_count = 300;
static const double peak_controller_default_max_pending_age_s = 30.0;
static const unsigned int peak_controller_shutdown_drain_ms = 1000;
static _Atomic gboolean peak_general_callbacks_suspended = FALSE;
static _Atomic gboolean peak_general_mpi_finalize_requested = FALSE;
static _Atomic gboolean peak_general_mpi_reducer_failed_closed = FALSE;
static const unsigned int peak_reattach_default_cooldown_ms = 20000;
static size_t peak_general_controller_batch_cursor = 0;
static unsigned int peak_general_controller_next_batch_id = 1;
static gsize peak_controller_retry_limits_initialized = 0;
static gsize peak_reattach_policy_initialized = 0;
static unsigned int peak_controller_max_retry_count = 300;
static double peak_controller_max_pending_age_s = 30.0;
static double peak_reattach_cooldown_s = 60.0;
static gsize peak_controller_trace_config_initialized = 0;
static gchar* peak_controller_trace_path = NULL;
static gboolean peak_controller_trace_enabled = FALSE;
static gsize peak_attach_policy_initialized = 0;
static gboolean peak_allow_unsafe_gum_prologue = FALSE;
static PeakUnsafeGumProloguePolicy peak_unsafe_gum_prologue_policy =
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
static gboolean peak_dynamic_attach_needed = FALSE;
#define PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES 64U

#include "general_listener/config.inc"

#include "general_listener/accounting.inc"

#include "general_listener/callbacks.inc"
#include "general_listener/controller_state.inc"
#include "general_listener/controller.inc"
#include "general_listener/heartbeat.inc"

#include "general_listener/callback_runtime.inc"

#include "general_listener/attach.inc"

#include "general_listener/output.inc"

#include "general_listener/shutdown.inc"
