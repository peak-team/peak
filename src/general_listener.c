#define _GNU_SOURCE
#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "internal/exec_raw_syscall.h"
#include "internal/general_listener_internal.h"
#include "internal/general_listener/attach_policy.h"
#include "internal/general_listener/exec_checkpoint_writer.h"
#include "internal/general_listener/report_formatter.h"
#include "internal/general_listener/report_maxima.h"
#include "internal/general_listener/report_model.h"
#include "internal/general_listener/report_snapshot.h"
#include "internal/general_listener/runtime_config.h"
#include "internal/general_listener/socket_report_transport.h"
#ifdef HAVE_MPI
#include "internal/general_listener/mpi_report_transport.h"
#endif
#include "internal/jit_provider.h"
#include "internal/unsafe_gum_prologue.h"
#include "detach_controller.h"
#include "logging.h"
#include "pthread_listener.h"
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
extern int peak_exec_checkpoint_enabled_at_startup(void) __attribute__((weak));
#endif

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

#define PEAK_HEARTBEAT_MIN_OBSERVATION_US 10000U
#define PEAK_GLOBAL_DETACH_MIN_CALLS 2U

#define PEAK_SIG_STOP (SIGRTMIN + 0)
#define PEAK_SIG_CONT (SIGRTMIN + 1)
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
    unsigned int local_ranks;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double control_risk_seconds;
    double profile_control_risk_seconds;
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
extern gboolean peak_truncate_function_name;
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
static const unsigned int peak_reattach_default_cooldown_ms = 60000;
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
static gboolean peak_dynamic_attach_needed = FALSE;
#define PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES 64U

/*
 * The lifecycle implementation remains in this translation unit because its
 * private declarations, static state, macros, and ordering form one contract.
 * Policy, formatting, serialization, and transport code that does not require
 * this shared state lives in independently compiled modules. See
 * general_listener/README.md.
 */
/* Configuration and controller declarations. */
static void
peak_general_listener_init_reattach_policy_once(void)
{
    unsigned int cooldown_ms =
        peak_general_listener_parse_uint_env_default(
            PEAK_REATTACH_COOLDOWN_MS_ENV,
            peak_reattach_default_cooldown_ms);
    peak_reattach_cooldown_s = (double)cooldown_ms / 1000.0;
}

static void
peak_general_listener_init_reattach_policy(void)
{
    if (g_once_init_enter(&peak_reattach_policy_initialized)) {
        peak_general_listener_init_reattach_policy_once();
        g_once_init_leave(&peak_reattach_policy_initialized, 1);
    }
}

gboolean
peak_general_listener_needs_dynamic_attach(void)
{
    return peak_dynamic_attach_needed;
}

typedef struct {
    size_t hook_id;
    GumInvocationListener* listener;
    PeakDetachOperation operation;
    PeakGumTargetAttachPlan attach_plan;
    PeakHookState stable_state;
    unsigned int retry_count;
    PeakDetachStatus last_retry_status;
    double pending_age_s;
} PeakGeneralBatchCandidate;

typedef enum {
    PEAK_GENERAL_SHUTDOWN_FAILURE_NONE = 0,
    PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE,
    PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE,
    PEAK_GENERAL_SHUTDOWN_FAILURE_PAUSE_FAILED
} PeakGeneralShutdownFailureBucket;

typedef struct {
    PeakGeneralShutdownFailureBucket bucket;
    PeakDetachStatus status;
} PeakGeneralShutdownFailure;

static double peak_general_controller_pending_age_for_trace_unlocked(size_t hook_id,
                                                                     double now);

static unsigned int
peak_general_controller_shutdown_drain_ms(void)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* value = g_getenv("PEAK_TEST_CONTROLLER_SHUTDOWN_DRAIN_MS");

    if (value != NULL && value[0] != '\0') {
        char* end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);

        if (end != value && *end == '\0' && parsed <= G_MAXUINT) {
            return (unsigned int)parsed;
        }
    }
#endif

    return peak_controller_shutdown_drain_ms;
}

static void
peak_general_shutdown_failure_note(PeakGeneralShutdownFailure* failure,
                                   PeakGeneralShutdownFailureBucket bucket,
                                   PeakDetachStatus status)
{
    if (failure == NULL) {
        return;
    }

    failure->bucket = bucket;
    failure->status = status;
}

static const char*
peak_general_shutdown_failure_bucket_string(
    const PeakGeneralShutdownFailure* failure)
{
    if (failure == NULL) {
        return "unknown";
    }

    switch (failure->bucket) {
        case PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE:
            return "prepare-exhausted";
        case PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE:
            return "snapshot-unsafe";
        case PEAK_GENERAL_SHUTDOWN_FAILURE_PAUSE_FAILED:
            return "pause-failed";
        case PEAK_GENERAL_SHUTDOWN_FAILURE_NONE:
        default:
            return "unknown";
    }
}

void peak_general_listener_controller_lock(void)
{
    pthread_mutex_lock(&lock);
}

void peak_general_listener_controller_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

void peak_general_listener_controller_wake(void)
{
    pthread_mutex_lock(&general_controller_wake_mutex);
    pthread_cond_signal(&general_controller_wake_cond);
    pthread_mutex_unlock(&general_controller_wake_mutex);
}

/* Runtime and controller accounting. */
static gboolean str_equal_function_general(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

static gboolean
peak_env_truthy_general(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static unsigned int
peak_general_controller_parse_uint_env(const char* name,
                                       unsigned int default_value)
{
    const char* value = g_getenv(name);
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > G_MAXUINT) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (unsigned int)parsed;
}

static void
peak_general_controller_init_retry_limits_once(void)
{
    unsigned int max_pending_age_ms =
        peak_general_controller_parse_uint_env(
            PEAK_CONTROLLER_MAX_PENDING_AGE_MS_ENV,
            (unsigned int)(peak_controller_default_max_pending_age_s * 1000.0));

    peak_controller_max_pending_age_s =
        max_pending_age_ms == 0 ? 0.0 : (double)max_pending_age_ms / 1000.0;
    peak_controller_max_retry_count =
        peak_general_controller_parse_uint_env(
            PEAK_CONTROLLER_MAX_RETRY_COUNT_ENV,
            peak_controller_default_max_retry_count);
}

static void
peak_general_controller_init_retry_limits(void)
{
    if (g_once_init_enter(&peak_controller_retry_limits_initialized)) {
        peak_general_controller_init_retry_limits_once();
        g_once_init_leave(&peak_controller_retry_limits_initialized, 1);
    }
}

static void
peak_general_controller_init_trace_config_once(void)
{
    const char* path = g_getenv("PEAK_DETACH_TRACE_PATH");

    if (path != NULL && path[0] != '\0') {
        peak_controller_trace_path = g_strdup(path);
        peak_controller_trace_enabled = TRUE;
    }
    peak_detach_controller_configure_trace_diagnostics(
        peak_controller_trace_enabled);
}

static void
peak_general_controller_init_trace_config(void)
{
    if (g_once_init_enter(&peak_controller_trace_config_initialized)) {
        peak_general_controller_init_trace_config_once();
        g_once_init_leave(&peak_controller_trace_config_initialized, 1);
    }
}

static gboolean
peak_general_controller_trace_enabled(void)
{
    return peak_controller_trace_enabled;
}

static unsigned long long
peak_general_listener_seconds_to_ns(double seconds)
{
    if (seconds <= 0.0) {
        return 0;
    }
    if (seconds >= (double)ULLONG_MAX / 1e9) {
        return ULLONG_MAX;
    }
    return (unsigned long long)(seconds * 1e9 + 0.5);
}

static unsigned long long
peak_general_listener_timespec_to_ns_saturating(const struct timespec* value)
{
    unsigned long long seconds;
    unsigned long long nanoseconds;

    if (value == NULL || value->tv_sec < 0 || value->tv_nsec < 0) {
        return 0;
    }
    seconds = (unsigned long long)value->tv_sec;
    nanoseconds = (unsigned long long)value->tv_nsec;
    if (seconds > ULLONG_MAX / 1000000000ULL) {
        return ULLONG_MAX;
    }
    seconds *= 1000000000ULL;
    if (nanoseconds > ULLONG_MAX - seconds) {
        return ULLONG_MAX;
    }
    return seconds + nanoseconds;
}

static gboolean
peak_general_listener_thread_cpu_ns(unsigned long long* out)
{
    struct timespec value;

    if (out == NULL || clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        return FALSE;
    }
    *out = peak_general_listener_timespec_to_ns_saturating(&value);
    return TRUE;
}

static unsigned long long
peak_general_listener_monotonic_delta_ns(unsigned long long current,
                                         unsigned long long previous)
{
    return current >= previous ? current - previous : 0;
}

static void
peak_general_listener_management_cpu_add_saturating(
    unsigned long long delta_ns)
{
    unsigned long long observed = atomic_load_explicit(
        &peak_general_listener_heartbeat_management_cpu_ns,
        memory_order_relaxed);

    while (delta_ns != 0 && observed != ULLONG_MAX) {
        unsigned long long updated =
            delta_ns > ULLONG_MAX - observed
                ? ULLONG_MAX
                : observed + delta_ns;
        if (atomic_compare_exchange_weak_explicit(
                &peak_general_listener_heartbeat_management_cpu_ns,
                &observed,
                updated,
                memory_order_relaxed,
                memory_order_relaxed)) {
            break;
        }
    }
}

static void
peak_general_listener_management_cpu_checkpoint(
    unsigned long long* previous_ns,
    gboolean* previous_valid)
{
    unsigned long long current_ns;

    if (previous_ns == NULL || previous_valid == NULL ||
        !peak_general_listener_thread_cpu_ns(&current_ns)) {
        return;
    }
    if (*previous_valid) {
        peak_general_listener_management_cpu_add_saturating(
            peak_general_listener_monotonic_delta_ns(current_ns,
                                                     *previous_ns));
    }
    *previous_ns = current_ns;
    *previous_valid = TRUE;
}

static double
peak_general_listener_management_cpu_seconds(void)
{
    return (double)atomic_load_explicit(
               &peak_general_listener_heartbeat_management_cpu_ns,
               memory_order_relaxed) /
           1e9;
}

static double
peak_general_listener_elapsed_at(double now)
{
    unsigned long long start_ns =
        atomic_load_explicit(&peak_general_listener_runtime_start_ns,
                             memory_order_relaxed);
    double start_time = (double)start_ns / 1e9;

    if (start_time <= 0.0) {
        start_time = peak_main_time;
    }

    if (start_time > 0.0 && now > start_time) {
        return now - start_time;
    }
    if (peak_main_time > 0.0 && peak_main_time < 3600.0) {
        return peak_main_time;
    }
    return 1e-12;
}

static uint64_t
peak_general_listener_count_since_baseline(uint64_t count,
                                           const _Atomic unsigned long long* baseline)
{
    uint64_t baseline_count;

    if (baseline == NULL) {
        return 0;
    }
    baseline_count =
        atomic_load_explicit(baseline, memory_order_relaxed);
    if (baseline_count == 0) {
        return count;
    }
    return count > baseline_count ? count - baseline_count : 0;
}

static uint64_t
peak_general_listener_control_window_count_since_heartbeat(
    const PeakDetachAccountingSnapshot* accounting)
{
    if (accounting == NULL) {
        return 0;
    }
    return peak_general_listener_count_since_baseline(
        accounting->completed_stop_window_count,
        &peak_general_listener_heartbeat_control_baseline_count);
}

static uint64_t
peak_general_listener_failed_window_count_since_heartbeat(
    const PeakDetachAccountingSnapshot* accounting)
{
    if (accounting == NULL) {
        return 0;
    }
    return peak_general_listener_count_since_baseline(
        accounting->failed_stop_window_count,
        &peak_general_listener_heartbeat_control_baseline_failed_count);
}

static uint64_t
peak_general_listener_control_wall_ns_since_heartbeat(
    const PeakDetachAccountingSnapshot* accounting)
{
    if (accounting == NULL) {
        return 0;
    }
    return peak_general_listener_count_since_baseline(
        accounting->stop_window_wall_ns,
        &peak_general_listener_heartbeat_control_baseline_ns);
}

static gboolean
peak_general_listener_accounting_snapshot_is_finite(
    const PeakDetachAccountingSnapshot* accounting)
{
    return accounting != NULL &&
           accounting->completed_stop_window_count != UINT64_MAX &&
           accounting->failed_stop_window_count != UINT64_MAX &&
           accounting->stop_window_wall_ns != UINT64_MAX;
}

static PeakDetachAccountingSnapshot
peak_general_listener_zero_accounting_snapshot(void)
{
    PeakDetachAccountingSnapshot accounting = {
        .completed_stop_window_count = 0,
        .failed_stop_window_count = 0,
        .stop_window_wall_ns = 0
    };

    return accounting;
}

static gboolean
peak_general_listener_accounting_snapshot(PeakDetachAccountingSnapshot* out)
{
    PeakDetachAccountingSnapshot candidate =
        peak_general_listener_zero_accounting_snapshot();

    if (out == NULL) {
        return FALSE;
    }

    if (!peak_detach_controller_accounting_snapshot(&candidate) ||
        !peak_general_listener_accounting_snapshot_is_finite(&candidate)) {
        *out = peak_general_listener_zero_accounting_snapshot();
        return FALSE;
    }

    *out = candidate;
    return TRUE;
}

static gboolean
peak_general_listener_runtime_accounting_snapshot(
    PeakDetachAccountingSnapshot* out)
{
    gboolean current_valid =
        peak_general_listener_accounting_snapshot(out);
    gboolean baseline_valid = atomic_load_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        memory_order_acquire);

    return current_valid && baseline_valid;
}

static void
peak_general_listener_reset_final_report_snapshot(void)
{
    memset(&peak_general_listener_final_report_snapshot,
           0,
           sizeof(peak_general_listener_final_report_snapshot));
}

void
peak_general_listener_note_runtime_start(double start_time)
{
    PeakDetachAccountingSnapshot accounting_baseline =
        peak_general_listener_zero_accounting_snapshot();
    gboolean baseline_valid;

    peak_general_listener_reset_final_report_snapshot();
    baseline_valid =
        peak_general_listener_accounting_snapshot(&accounting_baseline);
    atomic_store_explicit(&peak_general_listener_runtime_start_ns,
                          peak_general_listener_seconds_to_ns(start_time),
                          memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_ns,
        accounting_baseline.stop_window_wall_ns,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_count,
        accounting_baseline.completed_stop_window_count,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_failed_count,
        accounting_baseline.failed_stop_window_count,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        baseline_valid,
        memory_order_release);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_management_cpu_ns,
        0,
        memory_order_relaxed);
}

static void
peak_general_controller_trace_mutation_detail(size_t hook_id,
                                              PeakDetachOperation operation,
                                              const char* result,
                                              gboolean physical,
                                              PeakDetachStatus status,
                                              unsigned int retry_count,
                                              double pending_age_s,
                                              unsigned int batch_size,
                                              double stop_window_us,
                                              unsigned int batch_id,
                                              PeakDetachStatus last_retry_status)
{
    const char* path = peak_controller_trace_path;
    const PeakDetachFailureDetail* failure_detail =
        peak_detach_controller_last_failure_detail();
    const char* failure_reason =
        failure_detail != NULL && failure_detail->reason != NULL
            ? failure_detail->reason
            : "none";
    PeakHookRequestSource request_source = PEAK_HOOK_REQUEST_SOURCE_NONE;
    gulong request_calls = 0;
    double request_ratio = 0.0;
    double request_global_overhead = 0.0;
    double request_total_time = 0.0;
    double request_rate = 0.0;
    double trace_now;
    double trace_elapsed_time;
    uint64_t accounting_wall_ns;
    double accounting_wall_s = 0.0;
    double accounting_ratio = 0.0;
    PeakDetachAccountingSnapshot detach_accounting;
    gboolean accounting_valid;
    FILE* fp;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    if (hook_id < peak_hook_address_count) {
        if (peak_hook_pending_request_source != NULL) {
            request_source = peak_hook_pending_request_source[hook_id];
        }
        if (peak_hook_pending_request_calls != NULL) {
            request_calls = peak_hook_pending_request_calls[hook_id];
        }
        if (peak_hook_pending_request_ratio != NULL) {
            request_ratio = peak_hook_pending_request_ratio[hook_id];
        }
        if (peak_hook_pending_request_global_overhead != NULL) {
            request_global_overhead =
                peak_hook_pending_request_global_overhead[hook_id];
        }
        if (peak_hook_pending_request_total_time != NULL) {
            request_total_time = peak_hook_pending_request_total_time[hook_id];
        }
        if (peak_hook_pending_request_rate != NULL) {
            request_rate = peak_hook_pending_request_rate[hook_id];
        }
    }

    trace_now = peak_second();
    trace_elapsed_time = peak_general_listener_elapsed_at(trace_now);
    if (trace_elapsed_time <= 0.0) {
        trace_elapsed_time = 1e-12;
    }
    accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&detach_accounting);
    accounting_wall_ns =
        peak_general_listener_control_wall_ns_since_heartbeat(
            &detach_accounting);
    accounting_wall_s = (double)accounting_wall_ns / 1e9;
    accounting_ratio = accounting_wall_s / trace_elapsed_time;

    pthread_mutex_lock(&detach_trace_mutex);
    fp = fopen(path, "a");
    if (fp != NULL) {
        fprintf(fp,
                "%.9f,%lu,%s,%s,%s,%d,%s,%u,%.9f,%u,%.3f,%u,%s,%s,%ld,0x%llx,0x%llx,%s,%lu,%.12f,%.9f,%.9f,%.9f,%llu,%.9f,%.9f,%d,%llu\n",
                trace_now,
                (unsigned long)hook_id,
                hook_id < peak_hook_address_count && peak_hook_strings != NULL &&
                        peak_hook_strings[hook_id] != NULL
                    ? peak_hook_strings[hook_id]
                    : "<unknown>",
                peak_detach_controller_operation_string(operation),
                result != NULL ? result : "<unknown>",
                physical ? 1 : 0,
                peak_detach_controller_status_string(status),
                retry_count,
                pending_age_s,
                batch_size,
                stop_window_us,
                batch_id,
                peak_detach_controller_status_string(last_retry_status),
                failure_reason,
                failure_detail != NULL ? failure_detail->tid : 0,
                (unsigned long long)(failure_detail != NULL ? failure_detail->pc : 0),
                (unsigned long long)(failure_detail != NULL ? failure_detail->aux : 0),
                peak_hook_request_source_string(request_source),
                request_calls,
                request_ratio,
                request_global_overhead,
                request_total_time,
                request_rate,
                (unsigned long long)
                    peak_general_listener_control_window_count_since_heartbeat(
                        &detach_accounting),
                accounting_wall_s,
                accounting_ratio,
                accounting_valid ? 1 : 0,
                (unsigned long long)
                    peak_general_listener_failed_window_count_since_heartbeat(
                        &detach_accounting));
        fclose(fp);
    }
    pthread_mutex_unlock(&detach_trace_mutex);
}

static void
peak_general_controller_trace_mutation(size_t hook_id,
                                       PeakDetachOperation operation,
                                       const char* result,
                                       gboolean physical,
                                       PeakDetachStatus status)
{
    if (!peak_general_controller_trace_enabled()) {
        return;
    }

    peak_general_controller_trace_mutation_detail(hook_id,
                                                  operation,
                                                  result,
                                                  physical,
                                                  status,
                                                  hook_id < peak_hook_address_count &&
                                                          peak_hook_retry_count != NULL
                                                      ? peak_hook_retry_count[hook_id]
                                                      : 0,
                                                  0.0,
                                                  1,
                                                  strcmp(result, "prepare-failed") == 0
                                                      ? 0.0
                                                      : peak_detach_controller_last_stop_window_us(),
                                                  0,
                                                  hook_id < peak_hook_address_count &&
                                                          peak_hook_last_retry_status != NULL
                                                      ? peak_hook_last_retry_status[hook_id]
                                                      : PEAK_DETACH_STATUS_SAFE);
}

static unsigned int
peak_general_controller_allocate_batch_id_unlocked(void)
{
    unsigned int batch_id = peak_general_controller_next_batch_id++;

    if (batch_id == 0) {
        batch_id = peak_general_controller_next_batch_id++;
    }
    if (peak_general_controller_next_batch_id == 0) {
        peak_general_controller_next_batch_id = 1;
    }
    return batch_id;
}

static gboolean
peak_symbol_should_use_cpp_map(const char* symbol)
{
    if (peak_env_truthy_general(g_getenv("PEAK_ENABLE_CXX_SYMBOL_SCAN"))) {
        return TRUE;
    }

    return symbol != NULL &&
           (strstr(symbol, "::") != NULL ||
            strchr(symbol, '(') != NULL ||
            strchr(symbol, '<') != NULL ||
            strstr(symbol, "operator") != NULL);
}

gpointer
peak_general_listener_find_function(const char* symbol)
{
    if (symbol == NULL) {
        return NULL;
    }

    return gum_find_function(symbol);
}

/* Invocation listener and thread-pause primitives. */
static void peak_general_listener_iface_init(gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_EXTENDED(PeakGeneralListener,
                       peak_general_listener,
                       G_TYPE_OBJECT,
                       0,
                       G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                                             peak_general_listener_iface_init))
typedef struct _PeakGeneralThreadState {
    gulong level;
    gulong capacity;
    gdouble* child_time;
    pthread_t* tid_keys;
    size_t* mapped_ids;
    int* pause_session_ids;
    int* pause_status;
    size_t self_mapped_id;
    gboolean self_mapped_known;
} PeakGeneralThreadState;

typedef struct _PeakInvocationData {
    gdouble start_time;
    gulong stack_level;
    gboolean initialized;
} PeakInvocationData;

static __thread PeakGeneralThreadState thread_data;

pthread_once_t pthread_pause_once_ctrl = PTHREAD_ONCE_INIT;
pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  heartbeat_cond  = PTHREAD_COND_INITIALIZER;
static __thread int last_cont_id = -1;
static _Atomic int global_session_counter = 0;
static int pthread_pause_ack_pipe[2] = { -1, -1 };

static gboolean peak_general_controller_flush_teardown(void);
static gboolean peak_general_listener_pop_invocation(PeakInvocationData* priv,
                                                     gdouble* child_duration_out);

static int pthread_pause_deadline_ms(const struct timespec* deadline)
{
    struct timespec now;
    long long remaining_ns;

    clock_gettime(CLOCK_REALTIME, &now);
    remaining_ns = ((long long)deadline->tv_sec - (long long)now.tv_sec) * 1000000000LL +
                   ((long long)deadline->tv_nsec - (long long)now.tv_nsec);
    if (remaining_ns <= 0) {
        return 0;
    }

    remaining_ns = (remaining_ns + 999999LL) / 1000000LL;
    return remaining_ns > G_MAXINT ? G_MAXINT : (int)remaining_ns;
}

static int pthread_pause_wait_for_ack(int session_id, const struct timespec* deadline)
{
    if (pthread_pause_ack_pipe[0] < 0) {
        return -1;
    }

    for (;;) {
        int ack_session_id = -1;
        ssize_t nread = read(pthread_pause_ack_pipe[0],
                             &ack_session_id,
                             sizeof(ack_session_id));

        if (nread == sizeof(ack_session_id)) {
            if (ack_session_id == session_id) {
                return 0;
            }
            continue;
        }
        if (nread == -1 && errno == EINTR) {
            continue;
        }
        if (nread == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            int timeout_ms = pthread_pause_deadline_ms(deadline);
            int poll_ret;

            if (timeout_ms <= 0) {
                return 1;
            }

            pfd.fd = pthread_pause_ack_pipe[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll_ret = poll(&pfd, 1, timeout_ms);
            if (poll_ret > 0) {
                continue;
            }
            if (poll_ret == 0) {
                return 1;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        return -1;
    }
}

void pthread_pause_handler(int signal, siginfo_t* info, void* context)
{
    (void)context;
    int session_id = 0;
    if (info != NULL) {
        session_id = info->si_value.sival_int;
    }

    if (signal == PEAK_SIG_STOP) {
        if (pthread_pause_ack_pipe[1] >= 0) {
            ssize_t nwritten = write(pthread_pause_ack_pipe[1],
                                     &session_id,
                                     sizeof(session_id));
            (void)nwritten;
        }

        if (last_cont_id >= session_id) {
            return;
        }

        sigset_t block_set, original_mask, wait_set;

        sigemptyset(&block_set);
        sigaddset(&block_set, PEAK_SIG_CONT);
        pthread_sigmask(SIG_BLOCK, &block_set, &original_mask);

        if (last_cont_id >= session_id) {
            pthread_sigmask(SIG_SETMASK, &original_mask, NULL);
            return;
        }

        sigemptyset(&wait_set);
        sigaddset(&wait_set, PEAK_SIG_CONT);

        for (;;) {
            siginfo_t cont_info;
            struct timespec timeout;
            int ret;

            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += sig_cont_wait_interval / 1000000000;
            timeout.tv_nsec += sig_cont_wait_interval % 1000000000;

            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec += 1;
                timeout.tv_nsec -= 1000000000;
            }

            ret = sigtimedwait(&wait_set, &cont_info, &timeout);
            if (ret == -1) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (ret == PEAK_SIG_CONT) {
                int cont_id = cont_info.si_value.sival_int;

                if (cont_id > last_cont_id) {
                    last_cont_id = cont_id;
                }

                if (cont_id >= session_id) {
                    break;
                }
            }
        }

        pthread_sigmask(SIG_SETMASK, &original_mask, NULL);
    } else if (signal == PEAK_SIG_CONT) {
        if (session_id > last_cont_id) {
            last_cont_id = session_id;
        }
    } else {
        return;
    }
}

void pthread_pause_once(void)
{
    if (pipe2(pthread_pause_ack_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        pthread_pause_ack_pipe[0] = -1;
        pthread_pause_ack_pipe[1] = -1;
    }

    /* Prepare the pause-signal set. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);

    /* Register restartable pause-signal handlers with sigaction(). */
    const struct sigaction pause_sa = {
        .sa_sigaction = pthread_pause_handler,
        .sa_mask = sigset,
        .sa_flags = SA_RESTART | SA_SIGINFO,
        .sa_restorer = NULL
    };
    sigaction(PEAK_SIG_STOP, &pause_sa, NULL);
    sigaction(PEAK_SIG_CONT, &pause_sa, NULL);
}

#define pthread_pause_init() (pthread_once(&pthread_pause_once_ctrl, &pthread_pause_once))

void pthread_pause_enable()
{
    pthread_pause_init();
    /* Prepare the pause-signal set. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    /* Unblock pause signals for the current thread. */
    pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
}

void pthread_pause_disable()
{
    /*
     * Block pause signals around signal-unsafe work such as mutex operations
     * or stdio, then re-enable them after the critical section.
     */
    pthread_pause_init();

    /* Block pause signals for the current thread. */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);
}

static int pthread_pause_mapped(pthread_t thread, size_t mapped_id, int* session_id_out)
{
    union sigval sv;
    int session_id = atomic_fetch_add(&global_session_counter, 1);

    if (session_id_out != NULL) {
        *session_id_out = -1;
    }

    if (mapped_id >= peak_max_num_threads) {
        return -1;
    }

    sv.sival_int = session_id;

    while (pthread_sigqueue(thread, PEAK_SIG_STOP, sv) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
        usleep(1000);
    }

    if (session_id_out != NULL) {
        *session_id_out = session_id;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += sig_stop_ack_wait_interval / 1000000000;
    ts.tv_nsec += sig_stop_ack_wait_interval % 1000000000;

    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    return pthread_pause_wait_for_ack(session_id, &ts);
}

int pthread_pause(pthread_t thread, int* session_id_out)
{
    gboolean mapped_found = FALSE;
    size_t mapped_id = pthread_listener_lookup_thread(thread, &mapped_found);

    if (!mapped_found || mapped_id >= peak_max_num_threads) {
        if (session_id_out != NULL) {
            *session_id_out = -1;
        }
        return -1;
    }

    return pthread_pause_mapped(thread, mapped_id, session_id_out);
}

int pthread_unpause(pthread_t thread, int session_id)
{
    union sigval sv;
    sv.sival_int = session_id;

    while (pthread_sigqueue(thread, PEAK_SIG_CONT, sv) == -1) {
        if (errno != EAGAIN) {
            return -1;
        }
        usleep(1000);
    }

    return 0;
}

static gboolean peak_general_hook_is_published_unlocked(size_t hook_id)
{
    return peak_hook_states != NULL &&
           hook_address != NULL &&
           array_listener != NULL &&
           hook_id < peak_hook_address_count &&
           hook_address[hook_id] != NULL &&
           array_listener[hook_id] != NULL;
}

static void peak_general_listener_publish_legacy_flags_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count ||
        peak_need_detach == NULL ||
        peak_detached == NULL) {
        return;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_DETACH_REQUESTED:
        case PEAK_HOOK_DETACHING:
            peak_need_detach[hook_id] = TRUE;
            peak_detached[hook_id] = FALSE;
            break;
        case PEAK_HOOK_DETACHED:
        case PEAK_HOOK_REATTACH_REQUESTED:
        case PEAK_HOOK_REATTACHING:
        case PEAK_HOOK_SHUTDOWN:
            peak_need_detach[hook_id] = FALSE;
            peak_detached[hook_id] = TRUE;
            break;
        case PEAK_HOOK_UNRESOLVED:
        case PEAK_HOOK_ATTACHED:
        default:
            peak_need_detach[hook_id] = FALSE;
            peak_detached[hook_id] = FALSE;
            break;
    }
}
/* Controller state transitions. */

static gboolean
peak_general_controller_status_is_retryable(PeakDetachStatus status)
{
    switch (status) {
        case PEAK_DETACH_STATUS_TIMEOUT:
        case PEAK_DETACH_STATUS_CLASSIFY_FAILED:
        case PEAK_DETACH_STATUS_ERROR:
            return TRUE;
        case PEAK_DETACH_STATUS_SAFE:
        case PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED:
        case PEAK_DETACH_STATUS_DISABLED:
        case PEAK_DETACH_STATUS_UNSUPPORTED:
        case PEAK_DETACH_STATUS_MISSING_GUM_API:
        case PEAK_DETACH_STATUS_PERMISSION_DENIED:
        default:
            return FALSE;
    }
}

static const char*
peak_hook_request_source_string(PeakHookRequestSource source)
{
    switch (source) {
        case PEAK_HOOK_REQUEST_SOURCE_API:
            return "api";
        case PEAK_HOOK_REQUEST_SOURCE_DETACH_COUNT:
            return "detach-count";
        case PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT:
            return "per-target-heartbeat";
        case PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT:
            return "global-heartbeat";
        case PEAK_HOOK_REQUEST_SOURCE_NONE:
        default:
            return "none";
    }
}

static void
peak_general_controller_clear_pending_request_context_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count) {
        return;
    }

    if (peak_hook_pending_request_source != NULL) {
        peak_hook_pending_request_source[hook_id] =
            PEAK_HOOK_REQUEST_SOURCE_NONE;
    }
    if (peak_hook_pending_request_calls != NULL) {
        peak_hook_pending_request_calls[hook_id] = 0;
    }
    if (peak_hook_pending_request_ratio != NULL) {
        peak_hook_pending_request_ratio[hook_id] = 0.0;
    }
    if (peak_hook_pending_request_global_overhead != NULL) {
        peak_hook_pending_request_global_overhead[hook_id] = 0.0;
    }
    if (peak_hook_pending_request_total_time != NULL) {
        peak_hook_pending_request_total_time[hook_id] = 0.0;
    }
    if (peak_hook_pending_request_rate != NULL) {
        peak_hook_pending_request_rate[hook_id] = 0.0;
    }
}

static void
peak_general_controller_set_pending_request_context_unlocked(
    size_t hook_id,
    PeakHookRequestSource source,
    gulong calls,
    double ratio,
    double global_overhead,
    double total_time,
    double rate)
{
    if (hook_id >= peak_hook_address_count) {
        return;
    }

    if (peak_hook_pending_request_source != NULL) {
        peak_hook_pending_request_source[hook_id] = source;
    }
    if (peak_hook_pending_request_calls != NULL) {
        peak_hook_pending_request_calls[hook_id] = calls;
    }
    if (peak_hook_pending_request_ratio != NULL) {
        peak_hook_pending_request_ratio[hook_id] = ratio;
    }
    if (peak_hook_pending_request_global_overhead != NULL) {
        peak_hook_pending_request_global_overhead[hook_id] = global_overhead;
    }
    if (peak_hook_pending_request_total_time != NULL) {
        peak_hook_pending_request_total_time[hook_id] = total_time;
    }
    if (peak_hook_pending_request_rate != NULL) {
        peak_hook_pending_request_rate[hook_id] = rate;
    }
}

static gboolean
peak_general_listener_positive_finite(double value)
{
    return value > 0.0 && value == value && value <= DBL_MAX;
}

static gboolean
peak_general_listener_nonnegative_finite(double value)
{
    return value >= 0.0 && value == value && value <= DBL_MAX;
}

static gboolean
peak_general_listener_add_nonnegative_finite(double lhs,
                                             double rhs,
                                             double* out)
{
    if (!peak_general_listener_nonnegative_finite(lhs) ||
        !peak_general_listener_nonnegative_finite(rhs) ||
        lhs > DBL_MAX - rhs) {
        return FALSE;
    }

    if (out != NULL) {
        *out = lhs + rhs;
    }
    return TRUE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static uint64_t
peak_general_listener_add_uint64_saturated(uint64_t lhs, uint64_t rhs)
{
    const uint64_t max_published = UINT64_MAX - 1;

    if (lhs >= max_published || rhs > max_published - lhs) {
        return max_published;
    }
    return lhs + rhs;
}

uint64_t
peak_general_listener_test_add_uint64_saturated(uint64_t lhs, uint64_t rhs)
{
    return peak_general_listener_add_uint64_saturated(lhs, rhs);
}
#endif

static gboolean
peak_general_listener_subtract_nonnegative_finite(double* value,
                                                  double amount)
{
    double result;

    if (value == NULL ||
        !peak_general_listener_nonnegative_finite(*value) ||
        !peak_general_listener_nonnegative_finite(amount) ||
        amount > *value) {
        return FALSE;
    }

    result = *value - amount;
    if (!peak_general_listener_nonnegative_finite(result)) {
        return FALSE;
    }
    *value = result;
    return TRUE;
}

static gboolean
peak_general_listener_multiply_nonnegative_finite(double lhs,
                                                  double rhs,
                                                  double* out)
{
    double result;

    if (!peak_general_listener_nonnegative_finite(lhs) ||
        !peak_general_listener_nonnegative_finite(rhs) ||
        (rhs > 0.0 && lhs > DBL_MAX / rhs)) {
        return FALSE;
    }

    result = lhs * rhs;
    if (!peak_general_listener_nonnegative_finite(result)) {
        return FALSE;
    }
    if (out != NULL) {
        *out = result;
    }
    return TRUE;
}

static double
peak_general_listener_control_risk_seconds(double raw_control_seconds)
{
    double risk_control_seconds = DBL_MAX;

    if (!peak_general_listener_multiply_nonnegative_finite(
            raw_control_seconds,
            (double)peak_general_listener_local_mpi_ranks(),
            &risk_control_seconds)) {
        return DBL_MAX;
    }
    return risk_control_seconds;
}

static gboolean
peak_general_listener_multiply_positive_finite(double lhs,
                                               double rhs,
                                               double* out)
{
    double result;

    if (!peak_general_listener_positive_finite(lhs) ||
        !peak_general_listener_positive_finite(rhs) ||
        lhs > DBL_MAX / rhs) {
        return FALSE;
    }

    result = lhs * rhs;
    if (!peak_general_listener_positive_finite(result)) {
        return FALSE;
    }
    if (out != NULL) {
        *out = result;
    }
    return TRUE;
}

static double
peak_general_listener_profile_seconds_floor(void)
{
    if (peak_general_listener_positive_finite(peak_general_overhead)) {
        return peak_general_overhead;
    }
    return 1e-12;
}

static double
peak_general_listener_saved_detach_profile_seconds_unlocked(
    size_t hook_id,
    double fallback_seconds)
{
    if (hook_id < peak_hook_address_count &&
        peak_hook_detach_profile_seconds != NULL &&
        peak_general_listener_positive_finite(
            peak_hook_detach_profile_seconds[hook_id])) {
        return peak_hook_detach_profile_seconds[hook_id];
    }
    if (peak_general_listener_positive_finite(fallback_seconds)) {
        return fallback_seconds;
    }
    return peak_general_listener_profile_seconds_floor();
}

static double
peak_general_listener_projected_detach_profile_seconds_unlocked(
    size_t hook_id,
    double current_profile_seconds)
{
    double saved_seconds = 0.0;
    gboolean have_saved = FALSE;
    gboolean have_current =
        peak_general_listener_positive_finite(current_profile_seconds);

    if (hook_id < peak_hook_address_count &&
        peak_hook_detach_profile_seconds != NULL &&
        peak_general_listener_positive_finite(
            peak_hook_detach_profile_seconds[hook_id])) {
        saved_seconds = peak_hook_detach_profile_seconds[hook_id];
        have_saved = TRUE;
    }

    if (have_saved && have_current) {
        return saved_seconds > current_profile_seconds
                   ? saved_seconds
                   : current_profile_seconds;
    }
    if (have_saved) {
        return saved_seconds;
    }
    if (have_current) {
        return current_profile_seconds;
    }
    return peak_general_listener_profile_seconds_floor();
}

static double
peak_general_listener_historical_profile_rate(double projected_seconds,
                                              double elapsed_seconds)
{
    double rate;

    if (!peak_general_listener_positive_finite(projected_seconds) ||
        !peak_general_listener_positive_finite(elapsed_seconds) ||
        projected_seconds == DBL_MAX) {
        return DBL_MAX;
    }

    rate = projected_seconds / elapsed_seconds;
    return peak_general_listener_positive_finite(rate) ? rate : DBL_MAX;
}

static double
peak_general_listener_next_observation_lease_seconds(
    double request_rate,
    double observation_horizon_seconds)
{
    double lease_seconds = 0.0;

    if (!peak_general_listener_positive_finite(request_rate) ||
        !peak_general_listener_positive_finite(observation_horizon_seconds) ||
        request_rate == DBL_MAX) {
        return DBL_MAX;
    }

    if (!peak_general_listener_multiply_positive_finite(
            request_rate,
            observation_horizon_seconds,
            &lease_seconds)) {
        return DBL_MAX;
    }

    return lease_seconds;
}

static gboolean
peak_general_listener_hook_is_attached_for_policy_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count) {
        return FALSE;
    }

    if (peak_hook_states != NULL) {
        return peak_hook_states[hook_id] == PEAK_HOOK_ATTACHED;
    }
    return peak_detached == NULL || !peak_detached[hook_id];
}

static double
peak_general_listener_heartbeat_overhead_seconds_unlocked(void)
{
    double seconds = 0.0;

    if (heartbeat_overhead == NULL) {
        return 0.0;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address != NULL &&
            array_listener != NULL &&
            hook_address[i] &&
            array_listener[i] &&
            peak_general_listener_positive_finite(heartbeat_overhead[i]) &&
            seconds <= DBL_MAX - heartbeat_overhead[i]) {
            seconds += heartbeat_overhead[i];
        }
    }

    return seconds;
}

static void
peak_general_listener_snapshot_heartbeat_overhead_seconds_unlocked(void)
{
    peak_general_listener_final_heartbeat_overhead_seconds =
        peak_general_listener_heartbeat_overhead_seconds_unlocked();
}

static double
peak_general_listener_report_heartbeat_overhead_seconds(void)
{
    double seconds;

    pthread_mutex_lock(&lock);
    seconds = heartbeat_overhead != NULL
                  ? peak_general_listener_heartbeat_overhead_seconds_unlocked()
                  : peak_general_listener_final_heartbeat_overhead_seconds;
    pthread_mutex_unlock(&lock);
    return seconds;
}

static void
peak_general_listener_note_detach_profile_seconds_unlocked(size_t hook_id)
{
    double profile_seconds = 0.0;
    double fallback_seconds = 0.0;
    double detached_profile_seconds = 0.0;
    gboolean have_detached_sample = FALSE;

    if (hook_id >= peak_hook_address_count ||
        peak_hook_detach_profile_seconds == NULL) {
        return;
    }

    have_detached_sample =
        peak_general_listener_refresh_hook_sample_cache_unlocked(
            hook_id,
            NULL,
            &detached_profile_seconds);
    if (have_detached_sample) {
        fallback_seconds = detached_profile_seconds;
    }

    if (peak_hook_pending_request_ratio != NULL &&
        peak_hook_pending_request_total_time != NULL) {
        (void)peak_general_listener_multiply_positive_finite(
            peak_hook_pending_request_ratio[hook_id],
            peak_hook_pending_request_total_time[hook_id],
            &profile_seconds);
    }

    if (!have_detached_sample &&
        peak_hook_pending_request_calls != NULL &&
        peak_general_listener_positive_finite(peak_general_overhead) &&
        (double)peak_hook_pending_request_calls[hook_id] <=
            DBL_MAX / peak_general_overhead) {
        fallback_seconds =
            (double)peak_hook_pending_request_calls[hook_id] *
            peak_general_overhead;
    }
    if (!have_detached_sample &&
        heartbeat_overhead != NULL &&
        peak_general_listener_positive_finite(heartbeat_overhead[hook_id]) &&
        fallback_seconds <= DBL_MAX - heartbeat_overhead[hook_id]) {
        fallback_seconds += heartbeat_overhead[hook_id];
    }
    if (!peak_general_listener_positive_finite(fallback_seconds) &&
        peak_general_listener_positive_finite(
            peak_hook_detach_profile_seconds[hook_id])) {
        fallback_seconds = peak_hook_detach_profile_seconds[hook_id];
    }

    peak_hook_detach_profile_seconds[hook_id] =
        peak_general_listener_positive_finite(profile_seconds)
            ? profile_seconds
            : peak_general_listener_saved_detach_profile_seconds_unlocked(
                  hook_id,
                  fallback_seconds);
}

static void
peak_general_controller_reset_retry_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_next_retry_time == NULL ||
        peak_hook_retry_count == NULL) {
        return;
    }

    peak_hook_next_retry_time[hook_id] = 0.0;
    peak_hook_retry_count[hook_id] = 0;
    if (peak_hook_last_retry_status != NULL) {
        peak_hook_last_retry_status[hook_id] = PEAK_DETACH_STATUS_SAFE;
    }
    if (peak_hook_pending_observed_time != NULL) {
        peak_hook_pending_observed_time[hook_id] = 0.0;
    }
    peak_general_controller_clear_pending_request_context_unlocked(hook_id);
}

static void
peak_general_controller_mark_pending_started_unlocked(size_t hook_id,
                                                      double now)
{
    if (peak_hook_pending_observed_time == NULL ||
        hook_id >= peak_hook_address_count ||
        peak_hook_pending_observed_time[hook_id] > 0.0) {
        return;
    }

    peak_hook_pending_observed_time[hook_id] = now;
}

static void
peak_general_listener_note_detach_success_unlocked(size_t hook_id,
                                                   double now)
{
    if (peak_hook_last_detach_time == NULL || hook_id >= peak_hook_address_count) {
        return;
    }

    peak_hook_last_detach_time[hook_id] = now;
}

static void
peak_general_listener_note_reattach_success_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_reattach_request_calls == NULL ||
        peak_hook_reattach_request_calls_valid == NULL ||
        peak_hook_pending_request_calls == NULL) {
        return;
    }

    if (peak_hook_reattach_request_calls_valid[hook_id] &&
        array_listener_revisited != NULL &&
        peak_hook_pending_request_calls[hook_id] >
            peak_hook_reattach_request_calls[hook_id]) {
        array_listener_revisited[hook_id] = TRUE;
    }
    peak_hook_reattach_request_calls[hook_id] =
        peak_hook_pending_request_calls[hook_id];
    peak_hook_reattach_request_calls_valid[hook_id] = TRUE;
}

static gboolean
peak_general_listener_reattach_cooldown_ready_unlocked(size_t hook_id,
                                                       double now)
{
    peak_general_listener_init_reattach_policy();

    if (peak_reattach_cooldown_s <= 0.0 ||
        peak_hook_last_detach_time == NULL ||
        hook_id >= peak_hook_address_count ||
        peak_hook_last_detach_time[hook_id] <= 0.0) {
        return TRUE;
    }

    return now - peak_hook_last_detach_time[hook_id] >=
           peak_reattach_cooldown_s;
}

static void
peak_general_controller_note_retry_unlocked(size_t hook_id,
                                            PeakDetachStatus status)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_next_retry_time == NULL ||
        peak_hook_retry_count == NULL ||
        !peak_general_controller_status_is_retryable(status)) {
        return;
    }

    unsigned int retry_count = peak_hook_retry_count[hook_id];
    unsigned int shift = retry_count < 6 ? retry_count : 6;
    double delay = peak_controller_retry_base_delay * (double)(1U << shift);

    if (delay > peak_controller_retry_max_delay) {
        delay = peak_controller_retry_max_delay;
    }
    if (retry_count < G_MAXUINT) {
        peak_hook_retry_count[hook_id] = retry_count + 1;
    }
    peak_hook_next_retry_time[hook_id] = peak_second() + delay;
    if (peak_hook_last_retry_status != NULL) {
        peak_hook_last_retry_status[hook_id] = status;
    }
}

static gboolean
peak_general_controller_retry_budget_exceeded_unlocked(size_t hook_id,
                                                       double now)
{
    double pending_age_s;
    unsigned int retry_count;

    if (hook_id >= peak_hook_address_count) {
        return FALSE;
    }

    peak_general_controller_init_retry_limits();

    retry_count = peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0;
    pending_age_s = peak_general_controller_pending_age_for_trace_unlocked(hook_id, now);

    if (peak_controller_max_retry_count > 0 &&
        retry_count >= peak_controller_max_retry_count) {
        return TRUE;
    }

    return peak_controller_max_pending_age_s > 0.0 &&
           pending_age_s >= peak_controller_max_pending_age_s;
}

static gboolean
peak_general_controller_retry_ready_unlocked(size_t hook_id, double now)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_next_retry_time == NULL) {
        return TRUE;
    }

    return peak_hook_next_retry_time[hook_id] <= 0.0 ||
           peak_hook_next_retry_time[hook_id] <= now;
}

static void peak_general_controller_set_state_unlocked(size_t hook_id, PeakHookState state)
{
    if (peak_hook_states == NULL || hook_id >= peak_hook_address_count) {
        return;
    }

    peak_hook_states[hook_id] = state;
    peak_general_listener_publish_legacy_flags_unlocked(hook_id);
}

static gboolean
peak_general_controller_mpi_finalize_requested(void)
{
    return atomic_load_explicit(&peak_general_mpi_finalize_requested,
                                memory_order_acquire);
}

void peak_general_listener_controller_mark_attached_unlocked(size_t hook_id)
{
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
}

static gboolean
peak_general_listener_request_detach_with_context_unlocked(
    size_t hook_id,
    PeakHookRequestSource source,
    gulong calls,
    double ratio,
    double global_overhead,
    double total_time,
    double rate)
{
    if (peak_general_controller_mpi_finalize_requested()) {
        return FALSE;
    }

    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_ATTACHED:
            peak_general_controller_reset_retry_unlocked(hook_id);
            peak_general_controller_mark_pending_started_unlocked(hook_id,
                                                                  peak_second());
            peak_general_controller_set_pending_request_context_unlocked(
                hook_id,
                source,
                calls,
                ratio,
                global_overhead,
                total_time,
                rate);
            peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACH_REQUESTED);
            return TRUE;
        case PEAK_HOOK_DETACH_REQUESTED:
        case PEAK_HOOK_DETACHING:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean
peak_general_listener_request_detach_unlocked(size_t hook_id)
{
    return peak_general_listener_request_detach_with_context_unlocked(
        hook_id,
        PEAK_HOOK_REQUEST_SOURCE_API,
        0,
        0.0,
        0.0,
        0.0,
        0.0);
}

static gboolean
peak_general_listener_request_reattach_with_context_unlocked(
    size_t hook_id,
    PeakHookRequestSource source,
    gulong calls,
    double ratio,
    double global_overhead,
    double total_time,
    double rate)
{
    if (peak_general_controller_mpi_finalize_requested()) {
        return FALSE;
    }

    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_DETACHED:
            if (array_listener_gum_detached != NULL &&
                array_listener_gum_detach_flushed != NULL &&
                array_listener_gum_detached[hook_id] &&
                !array_listener_gum_detach_flushed[hook_id]) {
                return FALSE;
            }
            peak_general_controller_reset_retry_unlocked(hook_id);
            peak_general_controller_mark_pending_started_unlocked(hook_id,
                                                                  peak_second());
            peak_general_controller_set_pending_request_context_unlocked(
                hook_id,
                source,
                calls,
                ratio,
                global_overhead,
                total_time,
                rate);
            peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_REATTACH_REQUESTED);
            return TRUE;
        case PEAK_HOOK_REATTACH_REQUESTED:
        case PEAK_HOOK_REATTACHING:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean
peak_general_listener_request_reattach_unlocked(size_t hook_id)
{
    gulong calls = 0;

    if (hook_id < peak_hook_address_count &&
        peak_hook_states != NULL &&
        peak_hook_states[hook_id] == PEAK_HOOK_DETACHED &&
        !peak_general_listener_refresh_hook_sample_cache_unlocked(
            hook_id,
            &calls,
            NULL) &&
        !peak_general_listener_try_hook_sample_cache_unlocked(
            hook_id,
            &calls,
            NULL)) {
        return FALSE;
    }

    return peak_general_listener_request_reattach_with_context_unlocked(
        hook_id,
        PEAK_HOOK_REQUEST_SOURCE_API,
        calls,
        0.0,
        0.0,
        0.0,
        0.0);
}

gboolean peak_general_listener_request_detach(size_t hook_id)
{
    gboolean accepted;

    pthread_mutex_lock(&lock);
    accepted = peak_general_listener_request_detach_unlocked(hook_id);
    pthread_mutex_unlock(&lock);

    if (accepted) {
        peak_general_listener_controller_wake();
    }

    return accepted;
}

gboolean peak_general_listener_request_reattach(size_t hook_id)
{
    gboolean accepted;

    pthread_mutex_lock(&lock);
    accepted = peak_general_listener_request_reattach_unlocked(hook_id);
    pthread_mutex_unlock(&lock);

    if (accepted) {
        peak_general_listener_controller_wake();
    }

    return accepted;
}

PeakHookState peak_general_listener_hook_state(size_t hook_id)
{
    PeakHookState state = PEAK_HOOK_UNRESOLVED;

    pthread_mutex_lock(&lock);
    if (peak_hook_states != NULL && hook_id < peak_hook_address_count) {
        state = peak_hook_states[hook_id];
    }
    pthread_mutex_unlock(&lock);

    return state;
}

static gboolean
peak_general_controller_is_current_thread(void)
{
    return atomic_load(&general_controller_owner_known) &&
           pthread_equal(pthread_self(), general_controller_owner_thread);
}

static gboolean
peak_general_listener_provider_is_perfmap(const char* provider_name)
{
    return provider_name != NULL &&
           (g_ascii_strcasecmp(provider_name, "perfmap") == 0 ||
            g_ascii_strcasecmp(provider_name, "perf-map") == 0);
}

static gboolean
peak_general_listener_v8_optimized_symbol_matches_target(const char* target_name,
                                                         const char* symbol_name)
{
    const char* cursor = NULL;
    size_t target_len;
    size_t logical_len = 0;

    if (target_name == NULL || symbol_name == NULL) {
        return FALSE;
    }

    if (g_str_has_prefix(symbol_name, "JS:")) {
        cursor = symbol_name + 3;
    } else if (g_str_has_prefix(symbol_name, "LazyCompile:")) {
        cursor = symbol_name + 12;
    } else {
        return FALSE;
    }

    if (*cursor != '*') {
        return FALSE;
    }
    cursor++;
    if (*cursor == '\0') {
        return FALSE;
    }

    while (cursor[logical_len] != '\0' &&
           !g_ascii_isspace((guchar)cursor[logical_len])) {
        logical_len++;
    }

    target_len = strlen(target_name);
    return target_len == logical_len &&
           strncmp(target_name, cursor, logical_len) == 0;
}

static gboolean
peak_general_listener_dynamic_symbol_matches_target(const char* target_name,
                                                    const char* symbol_name,
                                                    const char* provider_name)
{
    if (target_name == NULL || symbol_name == NULL) {
        return FALSE;
    }

    if (strcmp(target_name, symbol_name) == 0) {
        return TRUE;
    }

    if (!peak_general_listener_provider_is_perfmap(provider_name)) {
        return FALSE;
    }

    return peak_general_listener_v8_optimized_symbol_matches_target(target_name,
                                                                    symbol_name);
}

gboolean
peak_general_listener_dynamic_symbol_matches_any_target(const char* symbol_name,
                                                        const char* provider_name)
{
    gboolean matched = FALSE;

    if (symbol_name == NULL || symbol_name[0] == '\0') {
        return FALSE;
    }

    pthread_mutex_lock(&lock);
    if (peak_hook_strings != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (peak_hook_strings[i] != NULL &&
                peak_general_listener_dynamic_symbol_matches_target(
                    peak_hook_strings[i],
                    symbol_name,
                    provider_name)) {
                matched = TRUE;
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);

    return matched;
}

static gboolean
peak_general_listener_expand_dynamic_hook_tables_unlocked(
    const char* target_name,
    size_t* hook_id_out)
{
    size_t old_count = peak_hook_address_count;
    size_t new_count = old_count + 1;
    char* target_copy = NULL;
    char** new_hook_strings;

    if (target_name == NULL || target_name[0] == '\0') {
        return FALSE;
    }

    target_copy = strdup(target_name);
    if (target_copy == NULL) {
        return FALSE;
    }

    new_hook_strings = realloc(peak_hook_strings,
                               sizeof(char*) * new_count);
    if (new_hook_strings == NULL) {
        free(target_copy);
        return FALSE;
    }
    peak_hook_strings = new_hook_strings;

    array_listener = g_renew(GumInvocationListener*, array_listener, new_count);
    array_listener_detached = g_renew(gboolean, array_listener_detached, new_count);
    array_listener_reattached = g_renew(gboolean, array_listener_reattached, new_count);
    array_listener_revisited =
        g_renew(gboolean, array_listener_revisited, new_count);
    array_listener_gum_detached =
        g_renew(gboolean, array_listener_gum_detached, new_count);
    array_listener_gum_detach_flushed =
        g_renew(gboolean, array_listener_gum_detach_flushed, new_count);
    peak_hook_last_detach_time =
        g_renew(double, peak_hook_last_detach_time, new_count);
    peak_hook_states = g_renew(PeakHookState, peak_hook_states, new_count);
    peak_hook_next_retry_time = g_renew(double, peak_hook_next_retry_time, new_count);
    peak_hook_pending_observed_time =
        g_renew(double, peak_hook_pending_observed_time, new_count);
    peak_hook_pending_request_source =
        g_renew(PeakHookRequestSource,
                peak_hook_pending_request_source,
                new_count);
    peak_hook_pending_request_calls =
        g_renew(gulong, peak_hook_pending_request_calls, new_count);
    peak_hook_pending_request_ratio =
        g_renew(double, peak_hook_pending_request_ratio, new_count);
    peak_hook_pending_request_global_overhead =
        g_renew(double, peak_hook_pending_request_global_overhead, new_count);
    peak_hook_pending_request_total_time =
        g_renew(double, peak_hook_pending_request_total_time, new_count);
    peak_hook_pending_request_rate =
        g_renew(double, peak_hook_pending_request_rate, new_count);
    peak_hook_detach_profile_seconds =
        g_renew(double, peak_hook_detach_profile_seconds, new_count);
    peak_hook_reattach_request_calls =
        g_renew(gulong, peak_hook_reattach_request_calls, new_count);
    peak_hook_reattach_request_calls_valid =
        g_renew(gboolean,
                peak_hook_reattach_request_calls_valid,
                new_count);
    peak_hook_cached_sample_calls =
        g_renew(gulong, peak_hook_cached_sample_calls, new_count);
    peak_hook_cached_sample_profile_seconds =
        g_renew(double, peak_hook_cached_sample_profile_seconds, new_count);
    peak_hook_cached_sample_valid =
        g_renew(gboolean, peak_hook_cached_sample_valid, new_count);
    peak_hook_retry_count =
        g_renew(unsigned int, peak_hook_retry_count, new_count);
    peak_hook_last_retry_status =
        g_renew(PeakDetachStatus, peak_hook_last_retry_status, new_count);
    hook_address = g_renew(gpointer, hook_address, new_count);
    peak_demangled_strings = g_renew(char*, peak_demangled_strings, new_count);
    peak_need_detach = g_renew(gboolean, peak_need_detach, new_count);
    peak_detached = g_renew(gboolean, peak_detached, new_count);
    if (heartbeat_overhead != NULL) {
        heartbeat_overhead = g_renew(gdouble, heartbeat_overhead, new_count);
    }

    peak_hook_strings[old_count] = target_copy;
    array_listener[old_count] = NULL;
    array_listener_detached[old_count] = FALSE;
    array_listener_reattached[old_count] = FALSE;
    array_listener_revisited[old_count] = FALSE;
    array_listener_gum_detached[old_count] = FALSE;
    array_listener_gum_detach_flushed[old_count] = TRUE;
    peak_hook_last_detach_time[old_count] = 0.0;
    peak_hook_states[old_count] = PEAK_HOOK_UNRESOLVED;
    peak_hook_next_retry_time[old_count] = 0.0;
    peak_hook_pending_observed_time[old_count] = 0.0;
    peak_hook_pending_request_source[old_count] =
        PEAK_HOOK_REQUEST_SOURCE_NONE;
    peak_hook_pending_request_calls[old_count] = 0;
    peak_hook_pending_request_ratio[old_count] = 0.0;
    peak_hook_pending_request_global_overhead[old_count] = 0.0;
    peak_hook_pending_request_total_time[old_count] = 0.0;
    peak_hook_pending_request_rate[old_count] = 0.0;
    peak_hook_detach_profile_seconds[old_count] = 0.0;
    peak_hook_reattach_request_calls[old_count] = 0;
    peak_hook_reattach_request_calls_valid[old_count] = FALSE;
    peak_hook_cached_sample_calls[old_count] = 0;
    peak_hook_cached_sample_profile_seconds[old_count] = 0.0;
    peak_hook_cached_sample_valid[old_count] = FALSE;
    peak_hook_retry_count[old_count] = 0;
    peak_hook_last_retry_status[old_count] = PEAK_DETACH_STATUS_SAFE;
    hook_address[old_count] = NULL;
    peak_demangled_strings[old_count] = NULL;
    peak_need_detach[old_count] = FALSE;
    peak_detached[old_count] = FALSE;
    if (heartbeat_overhead != NULL) {
        heartbeat_overhead[old_count] = 0.0;
    }

    peak_hook_address_count = new_count;
    if (hook_id_out != NULL) {
        *hook_id_out = old_count;
    }
    return TRUE;
}

PeakDynamicAttachResult
peak_general_listener_dynamic_attach_symbol(const char* symbol_name,
                                            gpointer symbol_address,
                                            gsize symbol_size,
                                            const char* provider_name)
{
    (void)symbol_size;

    if (symbol_name == NULL || symbol_name[0] == '\0' ||
        symbol_address == NULL || peak_hook_address_count == 0) {
        return PEAK_DYNAMIC_ATTACH_NO_MATCH;
    }

    if (!peak_general_controller_is_current_thread()) {
        g_printerr("[peak] refusing JIT dynamic attach outside the general listener controller thread\n");
        return PEAK_DYNAMIC_ATTACH_FAILED;
    }

    PeakDynamicAttachResult result = PEAK_DYNAMIC_ATTACH_NO_MATCH;

    pthread_mutex_lock(&lock);
    if (peak_hook_strings == NULL ||
        hook_address == NULL ||
        array_listener == NULL ||
        peak_hook_states == NULL) {
        pthread_mutex_unlock(&lock);
        return PEAK_DYNAMIC_ATTACH_FAILED;
    }

    gboolean matched_target = FALSE;
    gboolean duplicate_address = FALSE;
    const char* target_for_new_generation = NULL;
    size_t selected_hook_id = (size_t)-1;

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (peak_hook_strings[i] == NULL) {
            continue;
        }
        if (!peak_general_listener_dynamic_symbol_matches_target(
                peak_hook_strings[i],
                symbol_name,
                provider_name)) {
            continue;
        }

        matched_target = TRUE;
        if (target_for_new_generation == NULL) {
            target_for_new_generation = peak_hook_strings[i];
        }

        if (hook_address[i] == symbol_address) {
            duplicate_address = TRUE;
            result = PEAK_DYNAMIC_ATTACH_NO_MATCH;
            break;
        }

        if (selected_hook_id == (size_t)-1 &&
            hook_address[i] == NULL &&
            array_listener[i] == NULL &&
            peak_hook_states[i] == PEAK_HOOK_UNRESOLVED) {
            selected_hook_id = i;
        }
    }

    if (result == PEAK_DYNAMIC_ATTACH_NO_MATCH &&
        matched_target &&
        !duplicate_address &&
        selected_hook_id == (size_t)-1) {
        if (!peak_general_listener_expand_dynamic_hook_tables_unlocked(
                target_for_new_generation,
                &selected_hook_id)) {
            pthread_mutex_unlock(&lock);
            return PEAK_DYNAMIC_ATTACH_FAILED;
        }
    }

    if (!duplicate_address && selected_hook_id != (size_t)-1) {
        size_t i = selected_hook_id;
        if (!peak_general_listener_attach_target_is_supported(symbol_name,
                                                              symbol_address)) {
            g_printerr("[peak] skipping JIT attach for hook %lu (%s) from %s: unsafe Gum prologue\n",
                       (unsigned long)i,
                       symbol_name,
                       provider_name != NULL ? provider_name : "<unknown>");
            pthread_mutex_unlock(&lock);
            return PEAK_DYNAMIC_ATTACH_FAILED;
        }
        GumInvocationListener* new_listener =
            g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
        PEAKGENERAL_LISTENER(new_listener)->hook_id = i;

        PeakGumTargetAttachPlan attach_plan;
        peak_gum_target_attach_plan(symbol_address, &attach_plan);
        PeakDetachRequest mutation_request = {
            .hook_id = i,
            .symbol_name = peak_hook_strings[i],
            .function_address = symbol_address,
            .interceptor = interceptor,
            .listener = new_listener,
            .operation = PEAK_DETACH_OPERATION_ATTACH,
            .blocked_pc_start = attach_plan.mutation_guard_size > 0
                ? attach_plan.mutation_address
                : NULL,
            .blocked_pc_size = attach_plan.mutation_guard_size
        };
        PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                          &detach_status)) {
            result = peak_general_controller_status_is_retryable(detach_status) ?
                         PEAK_DYNAMIC_ATTACH_RETRY :
                         PEAK_DYNAMIC_ATTACH_FAILED;
            peak_log_debug("[peak] %s JIT attach for hook %lu (%s) from %s: %s\n",
                       result == PEAK_DYNAMIC_ATTACH_RETRY ? "retrying" : "skipping",
                       (unsigned long)i,
                       symbol_name,
                       provider_name != NULL ? provider_name : "<unknown>",
                       peak_detach_controller_status_string(detach_status));
            peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
            g_object_unref(new_listener);
            pthread_mutex_unlock(&lock);
            return result;
        }

        gum_interceptor_begin_transaction(interceptor);
        GumAttachReturn attach_status =
            peak_gum_interceptor_attach_target(interceptor,
                                               symbol_address,
                                               new_listener,
                                               &attach_plan);
        gum_interceptor_end_transaction(interceptor);

        if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                         &detach_status)) {
            peak_detach_controller_abort_after_failed_finish("JIT attach finish",
                                                            detach_status);
        }

        if (attach_status == GUM_ATTACH_OK) {
            hook_address[i] = symbol_address;
            g_free(peak_demangled_strings[i]);
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
            array_listener[i] = new_listener;
            array_listener_gum_detached[i] = FALSE;
            array_listener_gum_detach_flushed[i] = TRUE;
            peak_general_controller_reset_retry_unlocked(i);
            peak_general_controller_set_state_unlocked(i, PEAK_HOOK_ATTACHED);
            result = PEAK_DYNAMIC_ATTACH_ATTACHED;
        } else {
            g_printerr("[peak] Gum JIT attach failed for hook %lu (%s) from %s, status=%d\n",
                       (unsigned long)i,
                       symbol_name,
                       provider_name != NULL ? provider_name : "<unknown>",
                       attach_status);
            peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
            g_object_unref(new_listener);
            result = PEAK_DYNAMIC_ATTACH_FAILED;
        }
    }

    pthread_mutex_unlock(&lock);
    return result;
}
/* Controller execution. */

static gboolean peak_general_controller_pause_called_threads(
    size_t hook_id,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count)
{
    gboolean all_paused = TRUE;

    for (size_t s = 0; s < snapshot_count; s++) {
        pthread_t peak_tid_key = tid_keys[s];
        size_t cur_mapped_tid = mapped_ids[s];

        if (cur_mapped_tid < peak_max_num_threads &&
            peak_tid_key != controller_tid) {
            pause_status[s] =
                pthread_pause_mapped(peak_tid_key, cur_mapped_tid, &pause_session_ids[s]);
            if (pause_status[s] != 0) {
                all_paused = FALSE;
            }
        }
    }

    return all_paused;
}

static void peak_general_controller_resume_called_threads(
    size_t hook_id,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count)
{
    for (size_t s = 0; s < snapshot_count; s++) {
        pthread_t peak_tid_key = tid_keys[s];
        size_t cur_mapped_tid = mapped_ids[s];

        if (cur_mapped_tid < peak_max_num_threads &&
            peak_tid_key != controller_tid &&
            (pause_status[s] == 0 || pause_status[s] == 1) &&
            pause_session_ids[s] >= 0) {
            pthread_unpause(peak_tid_key, pause_session_ids[s]);
        }
    }
}

static gboolean
peak_general_controller_snapshot_is_safe(size_t hook_id,
                                         const size_t* mapped_ids,
                                         size_t snapshot_count,
                                         gboolean snapshot_complete)
{
    static gboolean warned_incomplete_snapshot = FALSE;
    static gboolean warned_over_capacity_thread = FALSE;

    if (!snapshot_complete) {
        if (!warned_incomplete_snapshot) {
            warned_incomplete_snapshot = TRUE;
            g_printerr("[peak] skipping physical detach/reattach because tracked thread snapshot exceeded PEAK thread capacity\n");
        }
        return FALSE;
    }

    for (size_t i = 0; i < snapshot_count; i++) {
        if (mapped_ids[i] >= peak_max_num_threads) {
            if (!warned_over_capacity_thread) {
                warned_over_capacity_thread = TRUE;
                g_printerr("[peak] skipping physical detach/reattach because tracked thread id %lu exceeds PEAK thread capacity %lu\n",
                           (unsigned long)mapped_ids[i],
                           (unsigned long)peak_max_num_threads);
            }
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
peak_general_controller_prepare_hook_mutation(size_t hook_id,
                                              GumInvocationListener* listener,
                                              PeakDetachOperation operation,
                                              const PeakGumTargetAttachPlan* attach_plan,
                                              PeakDetachStatus* status_out)
{
    PeakDetachRequest request = {
        .hook_id = hook_id,
        .symbol_name = hook_id < peak_hook_address_count ? peak_hook_strings[hook_id] : NULL,
        .function_address = hook_id < peak_hook_address_count ? hook_address[hook_id] : NULL,
        .interceptor = interceptor,
        .listener = listener,
        .operation = operation
    };
    if (operation == PEAK_DETACH_OPERATION_REATTACH && attach_plan != NULL) {
        request.blocked_pc_start = attach_plan->mutation_guard_size > 0
            ? attach_plan->mutation_address
            : NULL;
        request.blocked_pc_size = attach_plan->mutation_guard_size;
    }
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    gboolean prepared = peak_detach_controller_prepare_hook_mutation(&request, &status);

    if (status_out != NULL) {
        *status_out = status;
    }

    if (!prepared) {
        peak_general_controller_trace_mutation(hook_id,
                                               operation,
                                               "prepare-failed",
                                               FALSE,
                                               status);
    }

    return prepared;
}

static gboolean
peak_general_controller_finish_hook_mutation(size_t hook_id,
                                             GumInvocationListener* listener,
                                             PeakDetachOperation operation,
                                             PeakDetachStatus* status_out)
{
    PeakDetachRequest request = {
        .hook_id = hook_id,
        .symbol_name = hook_id < peak_hook_address_count ? peak_hook_strings[hook_id] : NULL,
        .function_address = hook_id < peak_hook_address_count ? hook_address[hook_id] : NULL,
        .interceptor = interceptor,
        .listener = listener,
        .operation = operation
    };

    return peak_detach_controller_finish_hook_mutation(&request, status_out);
}

static gboolean
peak_general_controller_handle_prepare_failure_unlocked(size_t hook_id,
                                                        PeakHookState stable_state,
                                                        PeakDetachStatus status)
{
    if (peak_general_controller_status_is_retryable(status)) {
        PeakDetachStatus last_retry_status =
            peak_hook_last_retry_status != NULL && hook_id < peak_hook_address_count
                ? peak_hook_last_retry_status[hook_id]
                : PEAK_DETACH_STATUS_SAFE;

        peak_general_controller_note_retry_unlocked(hook_id, status);
        if (!peak_general_controller_retry_budget_exceeded_unlocked(
                hook_id,
                peak_second())) {
            return TRUE;
        }

        g_printerr("[peak] Abandoning %s for hook %lu (%s) after %u retries and %.3fs pending age; leaving hook %s\n",
                   stable_state == PEAK_HOOK_ATTACHED ? "detach" : "reattach",
                   (unsigned long)hook_id,
                   hook_id < peak_hook_address_count &&
                           peak_hook_strings != NULL &&
                           peak_hook_strings[hook_id] != NULL
                       ? peak_hook_strings[hook_id]
                       : "<unknown>",
                   hook_id < peak_hook_address_count && peak_hook_retry_count != NULL
                       ? peak_hook_retry_count[hook_id]
                       : 0,
                   peak_general_controller_pending_age_for_trace_unlocked(
                       hook_id,
                       peak_second()),
                   stable_state == PEAK_HOOK_ATTACHED ? "attached" : "detached");
        peak_general_controller_trace_mutation_detail(
            hook_id,
            stable_state == PEAK_HOOK_ATTACHED ? PEAK_DETACH_OPERATION_DETACH
                                               : PEAK_DETACH_OPERATION_REATTACH,
            "retry-abandoned",
            FALSE,
            status,
            hook_id < peak_hook_address_count && peak_hook_retry_count != NULL
                ? peak_hook_retry_count[hook_id]
                : 0,
            peak_general_controller_pending_age_for_trace_unlocked(
                hook_id,
                peak_second()),
            1,
            0.0,
            0,
            last_retry_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, stable_state);
        return FALSE;
    }

    peak_general_controller_reset_retry_unlocked(hook_id);
    peak_general_controller_set_state_unlocked(hook_id, stable_state);
    return FALSE;
}

static gboolean peak_general_controller_detach_if_requested_unlocked(
    size_t hook_id,
    GumInvocationListener* listener,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count,
    gboolean snapshot_complete)
{
    if (!peak_general_hook_is_published_unlocked(hook_id) ||
        listener != array_listener[hook_id] ||
        peak_hook_states[hook_id] != PEAK_HOOK_DETACH_REQUESTED) {
        return FALSE;
    }

    {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           listener,
                                                           PEAK_DETACH_OPERATION_DETACH,
                                                           NULL,
                                                           &prepare_status)) {
            peak_general_controller_handle_prepare_failure_unlocked(hook_id,
                                                                    PEAK_HOOK_ATTACHED,
                                                                    prepare_status);
            return FALSE;
        }
    }

    gboolean helper_holds_threads = peak_detach_controller_threads_are_held();
    gboolean helper_applied_physical_patch =
        peak_detach_controller_current_mutation_uses_physical_patch();

    if (!helper_holds_threads &&
        !peak_general_controller_snapshot_is_safe(hook_id,
                                                  mapped_ids,
                                                  snapshot_count,
                                                  snapshot_complete)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          listener,
                                                          PEAK_DETACH_OPERATION_DETACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("detach snapshot abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_DETACH,
                                               "snapshot-unsafe",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
        return FALSE;
    }

    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHING);
    if (!helper_holds_threads &&
        !peak_general_controller_pause_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          listener,
                                                          PEAK_DETACH_OPERATION_DETACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("detach pause abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_DETACH,
                                               "pause-failed",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
        return FALSE;
    }

    if (!helper_applied_physical_patch) {
        gum_interceptor_begin_transaction(interceptor);
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_end_transaction(interceptor);
        array_listener_gum_detached[hook_id] = TRUE;
        array_listener_gum_detach_flushed[hook_id] =
            peak_general_controller_flush_teardown();
        if (!array_listener_gum_detach_flushed[hook_id]) {
            g_printerr("[peak] Gum detach for hook %lu (%s) did not flush; reattach disabled for this hook\n",
                       (unsigned long)hook_id,
                       peak_hook_strings[hook_id] != NULL ?
                           peak_hook_strings[hook_id] : "<unknown>");
        }
    }
    PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                      listener,
                                                      PEAK_DETACH_OPERATION_DETACH,
                                                      &finish_status)) {
        peak_detach_controller_abort_after_failed_finish("detach finish",
                                                        finish_status);
    }

    if (!helper_holds_threads) {
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
    }

    array_listener_detached[hook_id] = TRUE;
    peak_general_listener_note_detach_success_unlocked(hook_id,
                                                       peak_second());
    peak_general_listener_note_detach_profile_seconds_unlocked(hook_id);
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
    if (peak_general_controller_trace_enabled()) {
        unsigned int retry_count =
            peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0;
        PeakDetachStatus last_retry_status =
            peak_hook_last_retry_status != NULL ? peak_hook_last_retry_status[hook_id]
                                                : PEAK_DETACH_STATUS_SAFE;

        peak_general_controller_trace_mutation_detail(
            hook_id,
            PEAK_DETACH_OPERATION_DETACH,
            "success",
            helper_applied_physical_patch,
            finish_status,
            retry_count,
            peak_general_controller_pending_age_for_trace_unlocked(hook_id,
                                                                   peak_second()),
            1,
            peak_detach_controller_last_stop_window_us(),
            0,
            last_retry_status);
    }
    peak_general_controller_reset_retry_unlocked(hook_id);
    return TRUE;
}

static gboolean peak_general_controller_reattach_if_requested_unlocked(
    size_t hook_id,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count,
    gboolean snapshot_complete)
{
    if (!peak_general_hook_is_published_unlocked(hook_id) ||
        peak_hook_states[hook_id] != PEAK_HOOK_REATTACH_REQUESTED) {
        return FALSE;
    }

    PeakGumTargetAttachPlan attach_plan;
    peak_gum_target_attach_plan(hook_address[hook_id], &attach_plan);
    {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_REATTACH,
                                                           &attach_plan,
                                                           &prepare_status)) {
            peak_general_controller_handle_prepare_failure_unlocked(hook_id,
                                                                    PEAK_HOOK_DETACHED,
                                                                    prepare_status);
            return FALSE;
        }
    }

    gboolean helper_holds_threads = peak_detach_controller_threads_are_held();
    gboolean helper_applied_physical_patch =
        peak_detach_controller_current_mutation_uses_physical_patch();

    if (!helper_holds_threads &&
        !peak_general_controller_snapshot_is_safe(hook_id,
                                                  mapped_ids,
                                                  snapshot_count,
                                                  snapshot_complete)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_REATTACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("reattach snapshot abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_REATTACH,
                                               "snapshot-unsafe",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
        return FALSE;
    }

    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_REATTACHING);
    if (!helper_holds_threads &&
        !peak_general_controller_pause_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_REATTACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("reattach pause abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_REATTACH,
                                               "pause-failed",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
        return FALSE;
    }

    GumAttachReturn attach_status = GUM_ATTACH_OK;
    if (!helper_applied_physical_patch) {
        gum_interceptor_begin_transaction(interceptor);
        attach_status =
            peak_gum_interceptor_attach_target(interceptor,
                                               hook_address[hook_id],
                                               array_listener[hook_id],
                                               &attach_plan);
        gum_interceptor_end_transaction(interceptor);
    }
    PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                      array_listener[hook_id],
                                                      PEAK_DETACH_OPERATION_REATTACH,
                                                      &finish_status)) {
        peak_detach_controller_abort_after_failed_finish("reattach finish",
                                                        finish_status);
    }

    if (!helper_holds_threads) {
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
    }

    if (attach_status != GUM_ATTACH_OK) {
        g_printerr("[peak] Gum reattach failed for hook %lu (%s), status=%d\n",
                   (unsigned long)hook_id,
                   peak_hook_strings[hook_id] != NULL ? peak_hook_strings[hook_id] : "<unknown>",
                   attach_status);
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_REATTACH,
                                               "gum-failed",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
        return FALSE;
    }

    array_listener_gum_detached[hook_id] = FALSE;
    array_listener_gum_detach_flushed[hook_id] = TRUE;
    array_listener_reattached[hook_id] = TRUE;
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
    peak_general_listener_note_reattach_success_unlocked(hook_id);
    if (peak_general_controller_trace_enabled()) {
        unsigned int retry_count =
            peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0;
        PeakDetachStatus last_retry_status =
            peak_hook_last_retry_status != NULL ? peak_hook_last_retry_status[hook_id]
                                                : PEAK_DETACH_STATUS_SAFE;

        peak_general_controller_trace_mutation_detail(
            hook_id,
            PEAK_DETACH_OPERATION_REATTACH,
            "success",
            helper_applied_physical_patch,
            finish_status,
            retry_count,
            peak_general_controller_pending_age_for_trace_unlocked(hook_id,
                                                                   peak_second()),
            1,
            peak_detach_controller_last_stop_window_us(),
            0,
            last_retry_status);
    }
    peak_general_controller_reset_retry_unlocked(hook_id);
    return TRUE;
}

static gboolean peak_general_controller_shutdown_hook_unlocked(size_t hook_id,
                                                               pthread_t controller_tid,
                                                               pthread_t* tid_keys,
                                                               size_t* mapped_ids,
                                                               int* pause_session_ids,
                                                               int* pause_status,
                                                               size_t snapshot_count,
                                                               gboolean snapshot_complete,
                                                               PeakGeneralShutdownFailure* failure)
{
    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_SHUTDOWN);
        return TRUE;
    }

    if ((peak_hook_states[hook_id] == PEAK_HOOK_DETACHED ||
         peak_hook_states[hook_id] == PEAK_HOOK_REATTACH_REQUESTED) &&
        !array_listener_gum_detached[hook_id]) {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_SHUTDOWN,
                                                           NULL,
                                                           &prepare_status)) {
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE,
                prepare_status);
            return FALSE;
        }
        gboolean helper_applied_physical_patch =
            peak_detach_controller_current_mutation_uses_physical_patch();
        if (!peak_detach_controller_threads_are_held() &&
            !peak_general_controller_snapshot_is_safe(hook_id,
                                                      mapped_ids,
                                                      snapshot_count,
                                                      snapshot_complete)) {
            PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
            if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                              array_listener[hook_id],
                                                              PEAK_DETACH_OPERATION_SHUTDOWN,
                                                              &finish_status)) {
                peak_detach_controller_abort_after_failed_finish("detached shutdown snapshot abort",
                                                                finish_status);
            }
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE,
                PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            peak_general_controller_trace_mutation(hook_id,
                                                   PEAK_DETACH_OPERATION_SHUTDOWN,
                                                   "snapshot-unsafe",
                                                   helper_applied_physical_patch,
                                                   finish_status);
            return FALSE;
        }
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        gum_interceptor_begin_transaction(interceptor);
        gum_interceptor_detach(interceptor, array_listener[hook_id]);
        gum_interceptor_end_transaction(interceptor);
        array_listener_gum_detached[hook_id] = TRUE;
        array_listener_gum_detach_flushed[hook_id] = FALSE;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_SHUTDOWN,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("detached shutdown finish",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_SHUTDOWN,
                                               "success",
                                               helper_applied_physical_patch,
                                               finish_status);
    } else if (peak_hook_states[hook_id] != PEAK_HOOK_DETACHED &&
               peak_hook_states[hook_id] != PEAK_HOOK_REATTACH_REQUESTED &&
               peak_hook_states[hook_id] != PEAK_HOOK_SHUTDOWN) {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_SHUTDOWN,
                                                           NULL,
                                                           &prepare_status)) {
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE,
                prepare_status);
            return FALSE;
        }
        gboolean helper_holds_threads = peak_detach_controller_threads_are_held();
        gboolean helper_applied_physical_patch =
            peak_detach_controller_current_mutation_uses_physical_patch();

        if (!helper_holds_threads &&
            !peak_general_controller_snapshot_is_safe(hook_id,
                                                      mapped_ids,
                                                      snapshot_count,
                                                      snapshot_complete)) {
            PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
            if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                              array_listener[hook_id],
                                                              PEAK_DETACH_OPERATION_SHUTDOWN,
                                                              &finish_status)) {
                peak_detach_controller_abort_after_failed_finish("shutdown snapshot abort",
                                                                finish_status);
            }
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE,
                PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            peak_general_controller_trace_mutation(hook_id,
                                                   PEAK_DETACH_OPERATION_SHUTDOWN,
                                                   "snapshot-unsafe",
                                                   helper_applied_physical_patch,
                                                   finish_status);
            return FALSE;
        }

        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHING);
        if (!helper_holds_threads &&
            !peak_general_controller_pause_called_threads(hook_id,
                                                          controller_tid,
                                                          tid_keys,
                                                          mapped_ids,
                                                          pause_session_ids,
                                                          pause_status,
                                                          snapshot_count)) {
            PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
            if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                              array_listener[hook_id],
                                                              PEAK_DETACH_OPERATION_SHUTDOWN,
                                                              &finish_status)) {
                peak_detach_controller_abort_after_failed_finish("shutdown pause abort",
                                                                finish_status);
            }
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_PAUSE_FAILED,
                PEAK_DETACH_STATUS_ERROR);
            peak_general_controller_trace_mutation(hook_id,
                                                   PEAK_DETACH_OPERATION_SHUTDOWN,
                                                   "pause-failed",
                                                   helper_applied_physical_patch,
                                                   finish_status);
            peak_general_controller_resume_called_threads(hook_id,
                                                          controller_tid,
                                                          tid_keys,
                                                          mapped_ids,
                                                          pause_session_ids,
                                                          pause_status,
                                                          snapshot_count);
            return FALSE;
        }

        if (!array_listener_gum_detached[hook_id]) {
            gum_interceptor_begin_transaction(interceptor);
            gum_interceptor_detach(interceptor, array_listener[hook_id]);
            gum_interceptor_end_transaction(interceptor);
            array_listener_gum_detached[hook_id] = TRUE;
            array_listener_gum_detach_flushed[hook_id] = FALSE;
        }

        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_SHUTDOWN,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("shutdown finish",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_SHUTDOWN,
                                               "success",
                                               helper_applied_physical_patch,
                                               finish_status);
        if (!helper_holds_threads) {
            peak_general_controller_resume_called_threads(hook_id,
                                                          controller_tid,
                                                          tid_keys,
                                                          mapped_ids,
                                                          pause_session_ids,
                                                          pause_status,
                                                          snapshot_count);
        }
    }

    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_SHUTDOWN);
    peak_general_controller_reset_retry_unlocked(hook_id);
    return TRUE;
}

static gboolean peak_general_controller_flush_teardown(void)
{
    const unsigned int max_attempts = 100;

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++) {
        if (gum_interceptor_flush(interceptor)) {
            return TRUE;
        }
        usleep(1000);
    }

    return gum_interceptor_flush(interceptor);
}

static gboolean
peak_general_controller_has_pending_unlocked(void)
{
    if (peak_hook_states == NULL) {
        return FALSE;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED ||
            peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED) {
            return TRUE;
        }
    }

    return FALSE;
}

static void
peak_general_controller_observe_pending_for_trace_unlocked(size_t hook_id,
                                                           double now)
{
    if (peak_hook_pending_observed_time == NULL ||
        hook_id >= peak_hook_address_count) {
        return;
    }

    peak_general_controller_mark_pending_started_unlocked(hook_id, now);
}

static double
peak_general_controller_pending_age_for_trace_unlocked(size_t hook_id,
                                                       double now)
{
    if (peak_hook_pending_observed_time == NULL ||
        hook_id >= peak_hook_address_count ||
        peak_hook_pending_observed_time[hook_id] <= 0.0) {
        return 0.0;
    }

    return now - peak_hook_pending_observed_time[hook_id];
}

static size_t
peak_general_controller_collect_batch_unlocked(
    double now,
    PeakGeneralBatchCandidate* candidates,
    PeakDetachRequest* requests,
    size_t max_candidates)
{
    size_t count = 0;
    size_t total = peak_hook_address_count;
    size_t start = total > 0 ? peak_general_controller_batch_cursor % total : 0;
    size_t last_selected = start;
    gboolean selected_any = FALSE;

    for (size_t offset = 0; offset < total && count < max_candidates; offset++) {
        size_t i = (start + offset) % total;
        PeakDetachOperation operation;
        PeakHookState stable_state;

        if (peak_hook_states == NULL ||
            (peak_hook_states[i] != PEAK_HOOK_DETACH_REQUESTED &&
             peak_hook_states[i] != PEAK_HOOK_REATTACH_REQUESTED)) {
            continue;
        }
        if (!peak_general_controller_retry_ready_unlocked(i, now)) {
            continue;
        }
        if (!peak_general_hook_is_published_unlocked(i)) {
            continue;
        }

        if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED) {
            operation = PEAK_DETACH_OPERATION_DETACH;
            stable_state = PEAK_HOOK_ATTACHED;
        } else {
            if (array_listener_gum_detached != NULL &&
                array_listener_gum_detach_flushed != NULL &&
                array_listener_gum_detached[i] &&
                !array_listener_gum_detach_flushed[i]) {
                continue;
            }
            operation = PEAK_DETACH_OPERATION_REATTACH;
            stable_state = PEAK_HOOK_DETACHED;
        }

        peak_general_controller_observe_pending_for_trace_unlocked(i, now);

        candidates[count] = (PeakGeneralBatchCandidate){
            .hook_id = i,
            .listener = array_listener[i],
            .operation = operation,
            .stable_state = stable_state,
            .retry_count = peak_hook_retry_count != NULL ? peak_hook_retry_count[i] : 0,
            .last_retry_status = peak_hook_last_retry_status != NULL
                                      ? peak_hook_last_retry_status[i]
                                      : PEAK_DETACH_STATUS_SAFE,
            .pending_age_s =
                peak_general_controller_pending_age_for_trace_unlocked(i, now)
        };
        requests[count] = (PeakDetachRequest){
            .hook_id = i,
            .symbol_name = peak_hook_strings != NULL ? peak_hook_strings[i] : NULL,
            .function_address = hook_address[i],
            .interceptor = interceptor,
            .listener = array_listener[i],
            .operation = operation
        };
        if (operation == PEAK_DETACH_OPERATION_REATTACH &&
            requests[count].function_address != NULL) {
            peak_gum_target_attach_plan(requests[count].function_address,
                                        &candidates[count].attach_plan);
            requests[count].blocked_pc_start =
                candidates[count].attach_plan.mutation_guard_size > 0
                    ? candidates[count].attach_plan.mutation_address
                    : NULL;
            requests[count].blocked_pc_size =
                candidates[count].attach_plan.mutation_guard_size;
        }
        last_selected = i;
        selected_any = TRUE;
        count++;
    }

    if (selected_any && total > 0) {
        peak_general_controller_batch_cursor = (last_selected + 1) % total;
    }

    return count;
}

static gboolean
peak_general_controller_process_pending_batch_unlocked(void)
{
    PeakGeneralBatchCandidate candidates[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    PeakDetachRequest requests[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    PeakDetachBatchResult results[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    size_t prepared_count = 0;
    double now = peak_second();
    double stop_window_us = 0.0;
    unsigned int batch_id = 0;
    PeakDetachStatus batch_status = PEAK_DETACH_STATUS_ERROR;
    gboolean mutation_ok[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    GumAttachReturn attach_status[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    size_t candidate_count =
        peak_general_controller_collect_batch_unlocked(
            now,
            candidates,
            requests,
            PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES);

    if (candidate_count == 0) {
        return FALSE;
    }
    batch_id = peak_general_controller_allocate_batch_id_unlocked();
    for (size_t i = 0; i < candidate_count; i++) {
        mutation_ok[i] = TRUE;
        attach_status[i] = GUM_ATTACH_OK;
    }

    (void)peak_detach_controller_prepare_hook_mutation_batch(requests,
                                                             candidate_count,
                                                             results,
                                                             &prepared_count,
                                                             &batch_status);
    for (size_t i = 0; i < candidate_count; i++) {
        size_t hook_id = candidates[i].hook_id;

        if (!results[i].prepared) {
            continue;
        }

        if (candidates[i].operation == PEAK_DETACH_OPERATION_DETACH) {
            peak_general_controller_set_state_unlocked(hook_id,
                                                       PEAK_HOOK_DETACHING);
            if (!results[i].uses_physical_patch) {
                gum_interceptor_begin_transaction(interceptor);
                gum_interceptor_detach(interceptor, candidates[i].listener);
                gum_interceptor_end_transaction(interceptor);
                array_listener_gum_detached[hook_id] = TRUE;
                array_listener_gum_detach_flushed[hook_id] =
                    peak_general_controller_flush_teardown();
                if (!array_listener_gum_detach_flushed[hook_id]) {
                    g_printerr("[peak] Gum detach for hook %lu (%s) did not flush; reattach disabled for this hook\n",
                               (unsigned long)hook_id,
                               peak_hook_strings[hook_id] != NULL ?
                                   peak_hook_strings[hook_id] : "<unknown>");
                }
            }
        } else {
            peak_general_controller_set_state_unlocked(hook_id,
                                                       PEAK_HOOK_REATTACHING);
            if (!results[i].uses_physical_patch) {
                gum_interceptor_begin_transaction(interceptor);
                attach_status[i] =
                    peak_gum_interceptor_attach_target(
                        interceptor,
                        hook_address[hook_id],
                        array_listener[hook_id],
                        &candidates[i].attach_plan);
                gum_interceptor_end_transaction(interceptor);
                mutation_ok[i] = attach_status[i] == GUM_ATTACH_OK;
            }
        }
    }

    if (prepared_count > 0) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_finish_hook_mutation_batch(&finish_status)) {
            peak_detach_controller_abort_after_failed_finish("batch finish",
                                                            finish_status);
        }
        stop_window_us = peak_detach_controller_last_stop_window_us();

        for (size_t i = 0; i < candidate_count; i++) {
            if (!results[i].prepared) {
                continue;
            }
            if (candidates[i].operation == PEAK_DETACH_OPERATION_DETACH) {
                array_listener_detached[candidates[i].hook_id] = TRUE;
                peak_general_listener_note_detach_success_unlocked(
                    candidates[i].hook_id,
                    peak_second());
                peak_general_listener_note_detach_profile_seconds_unlocked(
                    candidates[i].hook_id);
                peak_general_controller_set_state_unlocked(candidates[i].hook_id,
                                                           PEAK_HOOK_DETACHED);
                peak_general_controller_trace_mutation_detail(
                    candidates[i].hook_id,
                    candidates[i].operation,
                    "success",
                    results[i].uses_physical_patch,
                    finish_status,
                    candidates[i].retry_count,
                    candidates[i].pending_age_s,
                    (unsigned int)candidate_count,
                    stop_window_us,
                    batch_id,
                    candidates[i].last_retry_status);
                peak_general_controller_reset_retry_unlocked(candidates[i].hook_id);
                continue;
            }

            if (!mutation_ok[i]) {
                g_printerr("[peak] Gum reattach failed for hook %lu (%s), status=%d\n",
                           (unsigned long)candidates[i].hook_id,
                           peak_hook_strings[candidates[i].hook_id] != NULL ?
                               peak_hook_strings[candidates[i].hook_id] : "<unknown>",
                           attach_status[i]);
                peak_general_controller_trace_mutation_detail(
                    candidates[i].hook_id,
                    candidates[i].operation,
                    "gum-failed",
                    results[i].uses_physical_patch,
                    finish_status,
                    candidates[i].retry_count,
                    candidates[i].pending_age_s,
                    (unsigned int)candidate_count,
                    stop_window_us,
                    batch_id,
                    candidates[i].last_retry_status);
                peak_general_controller_reset_retry_unlocked(candidates[i].hook_id);
                peak_general_controller_set_state_unlocked(candidates[i].hook_id,
                                                           PEAK_HOOK_DETACHED);
                continue;
            }

            array_listener_gum_detached[candidates[i].hook_id] = FALSE;
            array_listener_gum_detach_flushed[candidates[i].hook_id] = TRUE;
            array_listener_reattached[candidates[i].hook_id] = TRUE;
            peak_general_controller_set_state_unlocked(candidates[i].hook_id,
                                                       PEAK_HOOK_ATTACHED);
            peak_general_listener_note_reattach_success_unlocked(
                candidates[i].hook_id);
            peak_general_controller_trace_mutation_detail(
                candidates[i].hook_id,
                candidates[i].operation,
                "success",
                results[i].uses_physical_patch,
                finish_status,
                candidates[i].retry_count,
                candidates[i].pending_age_s,
                (unsigned int)candidate_count,
                stop_window_us,
                batch_id,
                candidates[i].last_retry_status);
            peak_general_controller_reset_retry_unlocked(candidates[i].hook_id);
        }
    }

    for (size_t i = 0; i < candidate_count; i++) {
        size_t hook_id = candidates[i].hook_id;

        if (results[i].prepared) {
            continue;
        }

        peak_general_controller_handle_prepare_failure_unlocked(
            hook_id,
            candidates[i].stable_state,
            results[i].status);
        peak_general_controller_trace_mutation_detail(
            hook_id,
            candidates[i].operation,
            "prepare-failed",
            FALSE,
            results[i].status,
            peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0,
            candidates[i].pending_age_s,
            (unsigned int)candidate_count,
            stop_window_us,
            batch_id,
            peak_hook_last_retry_status != NULL
                ? peak_hook_last_retry_status[hook_id]
                : PEAK_DETACH_STATUS_SAFE);
    }

    return prepared_count > 0;
}

static gboolean
peak_general_controller_process_pending_unlocked(pthread_t controller_tid,
                                                 pthread_t* tid_keys,
                                                 size_t* mapped_ids,
                                                 int* pause_session_ids,
                                                 int* pause_status)
{
    gboolean did_work = FALSE;
    double now = peak_second();

    if (peak_general_controller_mpi_finalize_requested()) {
        return FALSE;
    }

    if (peak_detach_controller_strict_batch_supported()) {
        (void)controller_tid;
        (void)tid_keys;
        (void)mapped_ids;
        (void)pause_session_ids;
        (void)pause_status;
        return peak_general_controller_process_pending_batch_unlocked();
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        gboolean snapshot_complete = FALSE;
        size_t snapshot_count;

        if (peak_hook_states == NULL ||
            (peak_hook_states[i] != PEAK_HOOK_DETACH_REQUESTED &&
             peak_hook_states[i] != PEAK_HOOK_REATTACH_REQUESTED)) {
            continue;
        }
        if (!peak_general_controller_retry_ready_unlocked(i, now)) {
            continue;
        }

        snapshot_count = pthread_listener_snapshot_threads(tid_keys,
                                                           mapped_ids,
                                                           peak_max_num_threads,
                                                           &snapshot_complete);
        for (size_t s = 0; s < snapshot_count; s++) {
            pause_session_ids[s] = -1;
            pause_status[s] = -1;
        }

        if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED) {
            did_work |= peak_general_controller_detach_if_requested_unlocked(i,
                                                                            array_listener[i],
                                                                            controller_tid,
                                                                            tid_keys,
                                                                            mapped_ids,
                                                                            pause_session_ids,
                                                                            pause_status,
                                                                            snapshot_count,
                                                                            snapshot_complete);
        } else if (peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED) {
            did_work |= peak_general_controller_reattach_if_requested_unlocked(i,
                                                                              controller_tid,
                                                                              tid_keys,
                                                                              mapped_ids,
                                                                              pause_session_ids,
                                                                              pause_status,
                                                                              snapshot_count,
                                                                              snapshot_complete);
        }
    }

    return did_work;
}

gboolean
peak_general_listener_controller_drain(unsigned int timeout_ms)
{
    if (interceptor == NULL ||
        peak_hook_states == NULL ||
        peak_hook_address_count == 0) {
        return TRUE;
    }

    if (peak_general_controller_mpi_finalize_requested()) {
        return TRUE;
    }

    pthread_t controller_tid = pthread_self();
    pthread_t* tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* pause_session_ids = g_new0(int, peak_max_num_threads);
    int* pause_status = g_new0(int, peak_max_num_threads);
    double deadline = peak_second() + ((double)timeout_ms / 1000.0);
    gboolean drained = FALSE;

    gum_interceptor_ignore_current_thread(interceptor);

    for (;;) {
        gboolean pending;

        pthread_mutex_lock(&lock);
        (void)peak_general_controller_process_pending_unlocked(controller_tid,
                                                               tid_keys,
                                                               mapped_ids,
                                                               pause_session_ids,
                                                               pause_status);
        pending = peak_general_controller_has_pending_unlocked();
        pthread_mutex_unlock(&lock);

        if (!pending) {
            drained = TRUE;
            break;
        }
        if (peak_second() >= deadline) {
            break;
        }

        peak_general_listener_controller_wake();
        usleep(1000);
    }

    g_free(pause_status);
    g_free(pause_session_ids);
    g_free(mapped_ids);
    g_free(tid_keys);

    gum_interceptor_unignore_current_thread(interceptor);

    return drained;
}

static void*
peak_general_controller_thread_main(void* arg)
{
    (void)arg;

    pthread_t controller_tid = pthread_self();
    pthread_t* tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* pause_session_ids = g_new0(int, peak_max_num_threads);
    int* pause_status = g_new0(int, peak_max_num_threads);

    general_controller_owner_thread = pthread_self();
    atomic_store(&general_controller_owner_known, TRUE);

    gum_interceptor_ignore_current_thread(interceptor);

    for (;;) {
        gboolean should_run;

        pthread_mutex_lock(&general_controller_wake_mutex);
        should_run = general_controller_running;
        pthread_mutex_unlock(&general_controller_wake_mutex);
        if (!should_run) {
            double deadline =
                peak_second() +
                ((double)peak_general_controller_shutdown_drain_ms() / 1000.0);
            while (peak_jit_provider_drain_pending()) {
                if (peak_second() >= deadline) {
                    (void)peak_jit_provider_drain_pending_force_not_exec_timeout();
                    break;
                }
                usleep(1000);
            }
            break;
        }

        pthread_mutex_lock(&lock);
        (void)peak_general_controller_process_pending_unlocked(controller_tid,
                                                               tid_keys,
                                                               mapped_ids,
                                                               pause_session_ids,
                                                               pause_status);
        pthread_mutex_unlock(&lock);

        dlopen_interceptor_drain_dynamic_attach_queue();
        peak_jit_provider_drain_pending();

        pthread_mutex_lock(&general_controller_wake_mutex);
        if (general_controller_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&general_controller_wake_cond,
                                         &general_controller_wake_mutex,
                                         &ts);
        }
        pthread_mutex_unlock(&general_controller_wake_mutex);
    }

    gum_interceptor_unignore_current_thread(interceptor);
    atomic_store(&general_controller_owner_known, FALSE);

    g_free(pause_status);
    g_free(pause_session_ids);
    g_free(mapped_ids);
    g_free(tid_keys);

    return NULL;
}

void
peak_general_listener_controller_start(void)
{
    pthread_mutex_lock(&general_controller_wake_mutex);
    if (!general_controller_thread_started) {
        general_controller_running = TRUE;
        if (pthread_create(&general_controller_thread,
                           NULL,
                           peak_general_controller_thread_main,
                           NULL) == 0) {
            general_controller_thread_started = TRUE;
        } else {
            general_controller_running = FALSE;
            g_printerr("[peak] failed to start general detach controller thread\n");
        }
    }
    pthread_mutex_unlock(&general_controller_wake_mutex);
}

void
peak_general_listener_controller_stop(void)
{
    gboolean should_join;
    pthread_t thread;
    unsigned int shutdown_drain_ms =
        peak_general_controller_shutdown_drain_ms();

    if (!peak_general_listener_controller_drain(shutdown_drain_ms)) {
        g_printerr("[peak] timed out draining pending target hook detach/reattach requests before controller shutdown\n");
    }

    pthread_mutex_lock(&general_controller_wake_mutex);
    should_join = general_controller_thread_started;
    if (should_join) {
        general_controller_running = FALSE;
        thread = general_controller_thread;
        pthread_cond_broadcast(&general_controller_wake_cond);
    }
    pthread_mutex_unlock(&general_controller_wake_mutex);

    if (should_join) {
        pthread_join(thread, NULL);
        pthread_mutex_lock(&general_controller_wake_mutex);
        general_controller_thread_started = FALSE;
        pthread_mutex_unlock(&general_controller_wake_mutex);
    }
}
/* Heartbeat policy. */

typedef struct {
    size_t index;
    double ratio; /* Detach pressure or reattach historical seconds. */
    double rate;  /* Detach contribution or oldest-detach ordering key. */
    double lifetime;
} OverheadEntry;

/* Sort global detach candidates by descending ratio. */
static int compare_ratio_de(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->ratio < y->ratio) return 1;
    if (x->ratio > y->ratio) return -1;

    /* Prefer faster-growing overhead when ratios tie. */
    if (x->rate < y->rate) return 1;
    if (x->rate > y->rate) return -1;
    if (x->index < y->index) return -1;
    if (x->index > y->index) return 1;
    return 0;
}

/* Sort global reattach candidates by ascending rate. */
static int compare_rate_inc(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->rate < y->rate) return -1;
    if (x->rate > y->rate) return 1;

    /* Prefer the smaller historical projection when rates tie. */
    if (x->ratio < y->ratio) return -1;
    if (x->ratio > y->ratio) return 1;
    if (x->index < y->index) return -1;
    if (x->index > y->index) return 1;
    return 0;
}

static inline double clipd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline gulong
peak_general_listener_num_calls_load(const gulong* slot)
{
    return __atomic_load_n(slot, __ATOMIC_RELAXED);
}

static inline gulong
peak_general_listener_num_calls_increment(gulong* slot)
{
    return __atomic_add_fetch(slot, 1, __ATOMIC_RELAXED);
}

static double
peak_general_listener_profile_seconds_for_calls_unlocked(size_t hook_id,
                                                         gulong calls)
{
    double profile_seconds = (double)calls * peak_general_overhead;

    if (heartbeat_overhead != NULL && hook_id < peak_hook_address_count) {
        profile_seconds += heartbeat_overhead[hook_id];
    }
    return profile_seconds;
}

static double
peak_general_listener_profile_seconds_at_boundary(void)
{
    double profile_seconds = 0.0;

    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address == NULL ||
            array_listener == NULL ||
            !hook_address[i] ||
            array_listener[i] == NULL) {
            continue;
        }

        PeakGeneralListener* pg_listener =
            PEAKGENERAL_LISTENER(array_listener[i]);
        gulong total_calls = 0;
        for (size_t j = 0; j < peak_max_num_threads; j++) {
            total_calls +=
                peak_general_listener_num_calls_load(
                    &pg_listener->num_calls[j]);
        }
        if (total_calls != 0 &&
            profile_seconds <=
                DBL_MAX - ((double)total_calls * peak_general_overhead)) {
            profile_seconds += (double)total_calls * peak_general_overhead;
        }
    }
    pthread_mutex_unlock(&lock);

    double heartbeat_profile_seconds =
        peak_general_listener_report_heartbeat_overhead_seconds();
    if (peak_general_listener_positive_finite(heartbeat_profile_seconds) &&
        profile_seconds <= DBL_MAX - heartbeat_profile_seconds) {
        profile_seconds += heartbeat_profile_seconds;
    }

    return profile_seconds;
}

void
peak_general_listener_freeze_final_report_snapshot(void)
{
    PeakDetachAccountingSnapshot accounting;
    PeakFinalReportSnapshot snapshot = {0};
    double control_risk_seconds;
    double profile_control_risk_seconds = DBL_MAX;

    snapshot.accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&accounting);
    snapshot.valid = TRUE;
    snapshot.local_ranks = peak_general_listener_local_mpi_ranks();
    snapshot.stop_window_count =
        peak_general_listener_control_window_count_since_heartbeat(
            &accounting);
    snapshot.failed_stop_window_count =
        peak_general_listener_failed_window_count_since_heartbeat(
            &accounting);
    snapshot.elapsed_seconds = peak_main_time;
    snapshot.profile_seconds =
        peak_general_listener_profile_seconds_at_boundary();
    snapshot.control_seconds =
        (double)peak_general_listener_control_wall_ns_since_heartbeat(
            &accounting) / 1e9;
    snapshot.management_seconds =
        peak_general_listener_management_cpu_seconds();
    control_risk_seconds =
        peak_general_listener_control_risk_seconds(snapshot.control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        snapshot.profile_seconds,
        control_risk_seconds,
        &profile_control_risk_seconds);
    snapshot.control_risk_seconds = control_risk_seconds;
    snapshot.profile_control_risk_seconds = profile_control_risk_seconds;
    if (snapshot.elapsed_seconds > 0.0) {
        snapshot.profile_ratio =
            snapshot.profile_seconds / snapshot.elapsed_seconds;
        snapshot.control_ratio =
            snapshot.control_seconds / snapshot.elapsed_seconds;
        snapshot.profile_control_risk_ratio =
            profile_control_risk_seconds / snapshot.elapsed_seconds;
        snapshot.control_risk_ratio =
            control_risk_seconds / snapshot.elapsed_seconds;
        snapshot.ratio =
            (snapshot.profile_seconds + snapshot.control_seconds) /
            snapshot.elapsed_seconds;
        snapshot.management_ratio =
            snapshot.management_seconds / snapshot.elapsed_seconds;
    }

    peak_general_listener_final_report_snapshot = snapshot;
}

static void
peak_general_listener_cache_hook_sample_unlocked(size_t hook_id,
                                                 gulong calls,
                                                 double profile_seconds)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_cached_sample_calls == NULL ||
        peak_hook_cached_sample_profile_seconds == NULL ||
        peak_hook_cached_sample_valid == NULL) {
        return;
    }

    peak_hook_cached_sample_calls[hook_id] = calls;
    peak_hook_cached_sample_profile_seconds[hook_id] = profile_seconds;
    peak_hook_cached_sample_valid[hook_id] = TRUE;
}

static gboolean
peak_general_listener_try_hook_sample_cache_unlocked(
    size_t hook_id,
    gulong* calls_out,
    double* profile_seconds_out)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_cached_sample_calls == NULL ||
        peak_hook_cached_sample_profile_seconds == NULL ||
        peak_hook_cached_sample_valid == NULL ||
        !peak_hook_cached_sample_valid[hook_id]) {
        return FALSE;
    }

    if (calls_out != NULL) {
        *calls_out = peak_hook_cached_sample_calls[hook_id];
    }
    if (profile_seconds_out != NULL) {
        *profile_seconds_out =
            peak_hook_cached_sample_profile_seconds[hook_id];
    }
    return TRUE;
}

static gboolean
peak_general_listener_refresh_hook_sample_cache_unlocked(
    size_t hook_id,
    gulong* calls_out,
    double* profile_seconds_out)
{
    if (hook_id >= peak_hook_address_count ||
        hook_address == NULL ||
        array_listener == NULL ||
        hook_address[hook_id] == NULL ||
        array_listener[hook_id] == NULL) {
        return FALSE;
    }

    PeakGeneralListener* pg_listener =
        PEAKGENERAL_LISTENER(array_listener[hook_id]);
    gulong total_num_calls = 0;
    for (size_t j = 0; j < peak_max_num_threads; j++) {
        total_num_calls += peak_general_listener_num_calls_load(
            &pg_listener->num_calls[j]);
    }

    double profile_seconds =
        peak_general_listener_profile_seconds_for_calls_unlocked(
            hook_id,
            total_num_calls);
    peak_general_listener_cache_hook_sample_unlocked(hook_id,
                                                     total_num_calls,
                                                     profile_seconds);
    if (calls_out != NULL) {
        *calls_out = total_num_calls;
    }
    if (profile_seconds_out != NULL) {
        *profile_seconds_out = profile_seconds;
    }
    return TRUE;
}

static gboolean
peak_general_listener_heartbeat_sample_can_reuse_cache_unlocked(
    size_t hook_id)
{
    if (hook_id >= peak_hook_address_count || peak_hook_states == NULL) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_DETACHED:
        case PEAK_HOOK_REATTACH_REQUESTED:
            return TRUE;
        case PEAK_HOOK_REATTACHING:
            /*
             * The controller has entered the physical reattach path. Keep
             * scanning so the first attached sample is seeded from live slots.
             */
            return FALSE;
        default:
            return FALSE;
    }
}

static gboolean
peak_heartbeat_wait_us(unsigned int sleep_us)
{
    pthread_mutex_lock(&heartbeat_mutex);
    if (!atomic_load(&heartbeat_running)) {
        pthread_mutex_unlock(&heartbeat_mutex);
        return FALSE;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += sleep_us / 1000000U;
    ts.tv_nsec += (long)(sleep_us % 1000000U) * 1000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    (void)pthread_cond_timedwait(&heartbeat_cond, &heartbeat_mutex, &ts);
    gboolean keep_running = atomic_load(&heartbeat_running);
    pthread_mutex_unlock(&heartbeat_mutex);
    return keep_running;
}

void* peak_heartbeat_monitor(void* arg) {
    PeakHeartbeatArgs* heartbeat_args = (PeakHeartbeatArgs*)arg;
    unsigned int heartbeat_time = heartbeat_args->heartbeat_time;
    unsigned int check_interval = heartbeat_args->check_interval;
    unsigned int hb_min_us = heartbeat_args->hb_min_us;
    unsigned int hb_max_us = heartbeat_args->hb_max_us;
    double hb_k_err = heartbeat_args->hb_k_err;
    double hb_k_rate = heartbeat_args->hb_k_rate;
    double hb_ema_a = heartbeat_args->hb_ema_a;
    unsigned int heartbeat_counter = 0;
    const unsigned int local_mpi_ranks =
        peak_general_listener_local_mpi_ranks();

    gum_interceptor_ignore_current_thread(interceptor);

    OverheadEntry* entries = g_new0(OverheadEntry, peak_hook_address_count);

    gulong* calls_snapshot = g_new0(gulong, peak_hook_address_count);
    double* profile_seconds_snapshot = g_new0(double, peak_hook_address_count);
    double* lifetime_snapshot = g_new0(double, peak_hook_address_count);
    double* ratio_snapshot = g_new0(double, peak_hook_address_count);
    double* rate_snapshot = g_new0(double, peak_hook_address_count);
    gulong* previous_calls = g_new0(gulong, peak_hook_address_count);
    double* previous_time = g_new0(double, peak_hook_address_count);
    gboolean* recent_baseline_ready = g_new0(gboolean, peak_hook_address_count);

    double now0 = peak_second();
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        calls_snapshot[i] = 0;
        profile_seconds_snapshot[i] = 0.0;
        lifetime_snapshot[i] = 0.0;
        ratio_snapshot[i] = 0.0;
        rate_snapshot[i] = 0.0;
        previous_calls[i] = 0;
        previous_time[i] = now0;
        recent_baseline_ready[i] = TRUE;
    }

    /* Global heartbeat dynamics. */
    double prev_global_overhead = 0.0;
    double prev_global_time     = now0;
    double ema_global_rate      = 0.0;
    size_t heartbeat_capacity = peak_hook_address_count;
    double min_detach_observation_time =
        (double)MAX(MAX(heartbeat_time, hb_min_us),
                    PEAK_HEARTBEAT_MIN_OBSERVATION_US) / 1e6;

    if (heartbeat_time > 0) {
        unsigned int initial_sleep_us =
            (unsigned int)clipd((double)heartbeat_time,
                                (double)hb_min_us,
                                (double)hb_max_us);
        if (!peak_heartbeat_wait_us(initial_sleep_us)) {
            goto cleanup;
        }
    }

    while (atomic_load(&heartbeat_running)) {
        unsigned long long management_cpu_checkpoint_ns = 0;
        gboolean management_cpu_checkpoint_valid =
            peak_general_listener_thread_cpu_ns(
                &management_cpu_checkpoint_ns);
        gboolean wake_controller = FALSE;
        heartbeat_counter++;
        double now = peak_second();

        pthread_mutex_lock(&lock);
        size_t current_hook_count = peak_hook_address_count;
        pthread_mutex_unlock(&lock);
        if (current_hook_count > heartbeat_capacity) {
            size_t old_capacity = heartbeat_capacity;
            entries = g_renew(OverheadEntry, entries, current_hook_count);
            calls_snapshot = g_renew(gulong, calls_snapshot, current_hook_count);
            profile_seconds_snapshot =
                g_renew(double, profile_seconds_snapshot, current_hook_count);
            lifetime_snapshot =
                g_renew(double, lifetime_snapshot, current_hook_count);
            ratio_snapshot =
                g_renew(double, ratio_snapshot, current_hook_count);
            rate_snapshot = g_renew(double, rate_snapshot, current_hook_count);
            previous_calls = g_renew(gulong, previous_calls, current_hook_count);
            previous_time = g_renew(double, previous_time, current_hook_count);
            recent_baseline_ready =
                g_renew(gboolean, recent_baseline_ready, current_hook_count);
            for (size_t i = old_capacity; i < current_hook_count; i++) {
                calls_snapshot[i] = 0;
                profile_seconds_snapshot[i] = 0.0;
                lifetime_snapshot[i] = 0.0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                previous_calls[i] = 0;
                previous_time[i] = now;
                recent_baseline_ready[i] = FALSE;
            }
            heartbeat_capacity = current_hook_count;
        }

        double total_execution_time = now - peak_main_time;
        if (total_execution_time <= 0.0) total_execution_time = 1e-12;
        double heartbeat_observation_horizon_seconds =
            (double)hb_max_us / 1e6;
        gboolean detach_observation_ready =
            total_execution_time >= min_detach_observation_time;

        double profile_spent_seconds = 0.0;
        double control_spent_seconds = 0.0;
        double spent_ratio = 0.0;
        double attached_recent_sum = 0.0;
        double attached_lifetime_sum = 0.0;
        double attached_pressure = 0.0;
        double profile_global_overhead = 0.0;
        double projected_pending_reattach_seconds = 0.0;
        uint64_t control_pause_wall_ns = 0;
        PeakDetachAccountingSnapshot detach_accounting;

        pthread_mutex_lock(&lock);
        for (size_t i = 0; i < heartbeat_capacity; i++) {
            if (!(hook_address[i] && array_listener[i])) {
                calls_snapshot[i] = 0;
                profile_seconds_snapshot[i] = 0.0;
                lifetime_snapshot[i] = 0.0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                previous_calls[i] = 0;
                previous_time[i] = now;
                recent_baseline_ready[i] = FALSE;
                if (peak_hook_cached_sample_valid != NULL &&
                    i < peak_hook_address_count) {
                    peak_hook_cached_sample_valid[i] = FALSE;
                }
                continue;
            }

            gulong total_num_calls = 0;
            double hook_profile_spent_seconds = 0.0;
            gboolean reuse_cached_sample =
                peak_general_listener_heartbeat_sample_can_reuse_cache_unlocked(
                    i);
            gboolean have_sample =
                reuse_cached_sample &&
                peak_general_listener_try_hook_sample_cache_unlocked(
                    i,
                    &total_num_calls,
                    &hook_profile_spent_seconds);
            if (!have_sample) {
                have_sample =
                    peak_general_listener_refresh_hook_sample_cache_unlocked(
                        i,
                        &total_num_calls,
                        &hook_profile_spent_seconds);
            }
            if (!have_sample) {
                calls_snapshot[i] = 0;
                profile_seconds_snapshot[i] = 0.0;
                lifetime_snapshot[i] = 0.0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                previous_calls[i] = 0;
                previous_time[i] = now;
                recent_baseline_ready[i] = FALSE;
                continue;
            }

            calls_snapshot[i] = total_num_calls;
            profile_seconds_snapshot[i] = hook_profile_spent_seconds;
            double ratio = hook_profile_spent_seconds / total_execution_time;
            lifetime_snapshot[i] = ratio;

            double recent_rate = 0.0;
            if (!reuse_cached_sample && recent_baseline_ready[i]) {
                double dt = now - previous_time[i];
                if (dt <= 1e-12) dt = 1e-12;
                gulong previous_num_calls = previous_calls[i];
                gulong recent_calls =
                    total_num_calls >= previous_num_calls
                        ? total_num_calls - previous_num_calls
                        : total_num_calls;
                recent_rate =
                    ((double)recent_calls * peak_general_overhead) / dt;
            }
            rate_snapshot[i] = recent_rate;
            ratio_snapshot[i] = ratio;

            previous_calls[i] = total_num_calls;
            previous_time[i] = now;
            recent_baseline_ready[i] = TRUE;
            profile_spent_seconds += hook_profile_spent_seconds;

            if (peak_general_listener_hook_is_attached_for_policy_unlocked(i)) {
                attached_recent_sum += recent_rate;
                attached_lifetime_sum += ratio;
            }
        }
        pthread_mutex_unlock(&lock);

        peak_general_listener_runtime_accounting_snapshot(&detach_accounting);
        control_pause_wall_ns =
            peak_general_listener_control_wall_ns_since_heartbeat(
                &detach_accounting);
        control_spent_seconds = (double)control_pause_wall_ns / 1e9;
        peak_general_listener_management_cpu_checkpoint(
            &management_cpu_checkpoint_ns,
            &management_cpu_checkpoint_valid);
        spent_ratio =
            (profile_spent_seconds + control_spent_seconds) /
            total_execution_time;
        attached_pressure = MAX(attached_recent_sum, attached_lifetime_sum);
        profile_global_overhead = attached_pressure;
        double projected_attached_recent_sum = attached_recent_sum;
        double projected_attached_lifetime_sum = attached_lifetime_sum;

        /* 1) Per-target detach. */
        if (detach_observation_ready && enable_per_target_heartbeat) {
            pthread_mutex_lock(&lock);
            for (size_t i = 0; i < heartbeat_capacity; i++) {
                if (!(hook_address[i] && array_listener[i])) continue;
                if (!peak_general_listener_hook_is_attached_for_policy_unlocked(
                        i)) {
                    continue;
                }

                if (calls_snapshot[i] >= PEAK_GLOBAL_DETACH_MIN_CALLS &&
                    ratio_snapshot[i] > target_profile_ratio) {
                    gboolean accepted =
                        peak_general_listener_request_detach_with_context_unlocked(
                            i,
                            PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT,
                            calls_snapshot[i],
                            lifetime_snapshot[i],
                            profile_global_overhead,
                            total_execution_time,
                            rate_snapshot[i]);
                    if (accepted) {
                        wake_controller = TRUE;
                        projected_attached_recent_sum -= rate_snapshot[i];
                        if (projected_attached_recent_sum < 0.0) {
                            projected_attached_recent_sum = 0.0;
                        }
                        projected_attached_lifetime_sum -= ratio_snapshot[i];
                        if (projected_attached_lifetime_sum < 0.0) {
                            projected_attached_lifetime_sum = 0.0;
                        }
                    }
                }
            }
             pthread_mutex_unlock(&lock);
        }

        /* 2) Global detach. */
        if (detach_observation_ready && enable_global_heartbeat) {
            double projected_global_overhead =
                MAX(projected_attached_recent_sum,
                    projected_attached_lifetime_sum);
            if (projected_global_overhead >
                global_target_ratio * peak_global_detach_factor) {
                size_t n_attached = 0;
                pthread_mutex_lock(&lock);
                for (size_t i = 0; i < heartbeat_capacity; i++) {
                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (!peak_general_listener_hook_is_attached_for_policy_unlocked(
                            i)) {
                        continue;
                    }
                    if (calls_snapshot[i] < PEAK_GLOBAL_DETACH_MIN_CALLS) {
                        continue;
                    }
                    double contribution =
                        MAX(rate_snapshot[i], ratio_snapshot[i]);
                    if (contribution <= 0.0) continue;
                    entries[n_attached].index = i;
                    entries[n_attached].ratio = contribution;
                    entries[n_attached].rate = rate_snapshot[i];
                    entries[n_attached].lifetime = ratio_snapshot[i];
                    n_attached++;
                }

                if (n_attached > 1) {
                    qsort(entries,
                          n_attached,
                          sizeof(OverheadEntry),
                          compare_ratio_de);
                }

                double reduced_recent_sum = projected_attached_recent_sum;
                double reduced_lifetime_sum = projected_attached_lifetime_sum;
                double reduced_global_overhead = projected_global_overhead;
                for (size_t k = 0; k < n_attached; k++) {
                    size_t idx = entries[k].index;

                    if (reduced_global_overhead <= global_target_ratio) {
                        break;
                    }
                    if (entries[k].ratio <= 0.0) {
                        break;
                    }

                    if (!(hook_address[idx] && array_listener[idx])) continue;
                    if (!peak_general_listener_hook_is_attached_for_policy_unlocked(
                            idx)) {
                        continue;
                    }
                    gboolean accepted =
                        peak_general_listener_request_detach_with_context_unlocked(
                            idx,
                            PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT,
                            calls_snapshot[idx],
                            lifetime_snapshot[idx],
                            profile_global_overhead,
                            total_execution_time,
                            rate_snapshot[idx]);
                    if (accepted) {
                        wake_controller = TRUE;
                        reduced_recent_sum -= entries[k].rate;
                        if (reduced_recent_sum < 0.0) {
                            reduced_recent_sum = 0.0;
                        }
                        reduced_lifetime_sum -= entries[k].lifetime;
                        if (reduced_lifetime_sum < 0.0) {
                            reduced_lifetime_sum = 0.0;
                        }
                        reduced_global_overhead =
                            MAX(reduced_recent_sum, reduced_lifetime_sum);
                    }
                }
                projected_attached_recent_sum = reduced_recent_sum;
                projected_attached_lifetime_sum = reduced_lifetime_sum;
                pthread_mutex_unlock(&lock);
            }
        }

        /* 3) Reattach. */
        if (detach_observation_ready &&
            enable_reattach &&
            check_interval != 0 &&
            (heartbeat_counter % check_interval) == 0) {
            double reattach_gate_ratio = 0.0;
            double reattach_spent_seconds = 0.0;
            double reattach_risk_spent_ratio = 0.0;
            double headroom_seconds = 0.0;
            PeakDetachAccountingSnapshot reattach_accounting_before;
            PeakDetachAccountingSnapshot reattach_accounting_after;
            gboolean reattach_accounting_before_valid =
                peak_general_listener_runtime_accounting_snapshot(
                    &reattach_accounting_before);
            double last_stop_seconds =
                peak_detach_controller_last_stop_window_us() / 1e6;
            double predicted_batch_stop_seconds = DBL_MAX;
            gboolean reattach_accounting_after_valid =
                peak_general_listener_runtime_accounting_snapshot(
                    &reattach_accounting_after);
            gboolean reattach_accounting_coherent =
                reattach_accounting_before_valid &&
                reattach_accounting_after_valid &&
                reattach_accounting_before.completed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_before.failed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_before.stop_window_wall_ns <
                    (UINT64_MAX - 1) &&
                reattach_accounting_after.completed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_after.failed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_after.stop_window_wall_ns <
                    (UINT64_MAX - 1) &&
                reattach_accounting_before.completed_stop_window_count ==
                    reattach_accounting_after.completed_stop_window_count &&
                reattach_accounting_before.failed_stop_window_count ==
                    reattach_accounting_after.failed_stop_window_count &&
                reattach_accounting_before.stop_window_wall_ns ==
                    reattach_accounting_after.stop_window_wall_ns;
            double reattach_control_spent_seconds =
                (double)peak_general_listener_control_wall_ns_since_heartbeat(
                    &reattach_accounting_after) / 1e9;
            double reattach_control_risk_seconds = DBL_MAX;
            gboolean reattach_budget_ok =
                reattach_accounting_coherent &&
                (enable_per_target_heartbeat || enable_global_heartbeat) &&
                peak_general_listener_positive_finite(total_execution_time) &&
                peak_general_listener_nonnegative_finite(global_target_ratio) &&
                peak_general_listener_nonnegative_finite(
                    peak_global_reattach_factor) &&
                peak_general_listener_nonnegative_finite(
                    profile_spent_seconds) &&
                peak_general_listener_nonnegative_finite(
                    reattach_control_spent_seconds) &&
                peak_general_listener_nonnegative_finite(
                    last_stop_seconds) &&
                peak_general_listener_multiply_nonnegative_finite(
                    reattach_control_spent_seconds,
                    (double)local_mpi_ranks,
                    &reattach_control_risk_seconds) &&
                peak_general_listener_multiply_nonnegative_finite(
                    last_stop_seconds,
                    (double)local_mpi_ranks,
                    &predicted_batch_stop_seconds) &&
                peak_general_listener_add_nonnegative_finite(
                    profile_spent_seconds,
                    reattach_control_risk_seconds,
                    &reattach_spent_seconds) &&
                peak_general_listener_multiply_nonnegative_finite(
                    peak_global_reattach_factor,
                    global_target_ratio,
                    &reattach_gate_ratio) &&
                peak_general_listener_multiply_nonnegative_finite(
                    global_target_ratio,
                    total_execution_time,
                    &headroom_seconds) &&
                peak_general_listener_subtract_nonnegative_finite(
                    &headroom_seconds,
                    profile_spent_seconds) &&
                peak_general_listener_subtract_nonnegative_finite(
                    &headroom_seconds,
                    reattach_control_risk_seconds);
            if (reattach_budget_ok) {
                reattach_risk_spent_ratio =
                    reattach_spent_seconds / total_execution_time;
                reattach_budget_ok =
                    peak_general_listener_nonnegative_finite(
                        reattach_risk_spent_ratio) &&
                    reattach_risk_spent_ratio <= reattach_gate_ratio;
            }

            if (reattach_budget_ok) {
                size_t pending_mutation_count = 0;
                size_t detached_cnt = 0;
                size_t admitted_count = 0;

                projected_pending_reattach_seconds = 0.0;
                pthread_mutex_lock(&lock);
                for (size_t i = 0; i < heartbeat_capacity; i++) {
                    double projected_seconds;
                    double projected_ratio;
                    gboolean per_target_eligible;
                    gboolean global_eligible;

                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (peak_hook_states == NULL) continue;
                    if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED ||
                        peak_hook_states[i] == PEAK_HOOK_DETACHING ||
                        peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED ||
                        peak_hook_states[i] == PEAK_HOOK_REATTACHING) {
                        pending_mutation_count++;
                    }
                    if (peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED ||
                        peak_hook_states[i] == PEAK_HOOK_REATTACHING) {
                        double projected_seconds =
                            peak_general_listener_projected_detach_profile_seconds_unlocked(
                                i,
                                profile_seconds_snapshot[i]);
                        double historical_rate =
                            peak_general_listener_historical_profile_rate(
                                projected_seconds,
                                total_execution_time);
                        double pending_rate = historical_rate;
                        if (peak_hook_pending_request_rate != NULL &&
                            peak_general_listener_positive_finite(
                                peak_hook_pending_request_rate[i])) {
                            pending_rate = peak_hook_pending_request_rate[i];
                        }
                        double pending_lease_seconds =
                            peak_general_listener_next_observation_lease_seconds(
                                pending_rate,
                                heartbeat_observation_horizon_seconds);
                        if (pending_lease_seconds == DBL_MAX ||
                            !peak_general_listener_add_nonnegative_finite(
                                projected_pending_reattach_seconds,
                                pending_lease_seconds,
                                &projected_pending_reattach_seconds)) {
                            reattach_budget_ok = FALSE;
                        }
                    }
                    if (!peak_detached[i]) continue;
                    if (peak_need_detach[i]) continue;
                    if (peak_hook_states[i] != PEAK_HOOK_DETACHED) {
                        continue;
                    }
                    if (!peak_general_listener_reattach_cooldown_ready_unlocked(
                            i,
                            now)) {
                        continue;
                    }

                    projected_seconds =
                        peak_general_listener_projected_detach_profile_seconds_unlocked(
                            i,
                            profile_seconds_snapshot[i]);
                    projected_ratio =
                        projected_seconds / total_execution_time;
                    per_target_eligible =
                        enable_per_target_heartbeat &&
                        projected_ratio <= target_profile_ratio;
                    global_eligible = enable_global_heartbeat;
                    if (!per_target_eligible && !global_eligible) {
                        continue;
                    }

                    entries[detached_cnt].index = i;
                    entries[detached_cnt].ratio = projected_seconds;
                    entries[detached_cnt].rate =
                        peak_hook_last_detach_time != NULL
                            ? peak_hook_last_detach_time[i]
                            : 0.0;
                    entries[detached_cnt].lifetime = projected_ratio;
                    detached_cnt++;
                }

                size_t pending_batch_windows =
                    (pending_mutation_count +
                     PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES - 1) /
                    PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES;
                double pending_stop_seconds = 0.0;
                reattach_budget_ok =
                    reattach_budget_ok &&
                    projected_pending_reattach_seconds != DBL_MAX &&
                    peak_general_listener_subtract_nonnegative_finite(
                        &headroom_seconds,
                        projected_pending_reattach_seconds) &&
                    peak_general_listener_multiply_nonnegative_finite(
                        (double)pending_batch_windows,
                        predicted_batch_stop_seconds,
                        &pending_stop_seconds) &&
                    peak_general_listener_subtract_nonnegative_finite(
                        &headroom_seconds,
                        pending_stop_seconds) &&
                    headroom_seconds > 0.0;

                if (reattach_budget_ok && detached_cnt > 1) {
                    qsort(entries,
                          detached_cnt,
                          sizeof(OverheadEntry),
                          compare_rate_inc);
                }

                for (size_t k = 0;
                     reattach_budget_ok && headroom_seconds > 0.0 &&
                         k < detached_cnt;
                     k++) {
                    size_t i = entries[k].index;
                    double projected_seconds;
                    double projected_ratio;
                    double request_rate;
                    double incremental_lease_seconds;
                    gboolean per_target_eligible;
                    gboolean global_eligible;
                    PeakHookRequestSource source;
                    gboolean still_detached =
                        hook_address[i] && array_listener[i] &&
                        peak_detached[i] && !peak_need_detach[i] &&
                        peak_hook_states != NULL &&
                        peak_hook_states[i] == PEAK_HOOK_DETACHED;
                    if (!still_detached) continue;
                    if (!peak_general_listener_reattach_cooldown_ready_unlocked(
                            i,
                            now)) {
                        continue;
                    }

                    projected_seconds = entries[k].ratio;
                    projected_ratio = entries[k].lifetime;
                    per_target_eligible =
                        enable_per_target_heartbeat &&
                        projected_ratio <= target_profile_ratio;
                    global_eligible = enable_global_heartbeat;
                    if (!per_target_eligible && !global_eligible) {
                        continue;
                    }
                    source = per_target_eligible
                                 ? PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT
                                 : PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT;
                    request_rate =
                        peak_general_listener_historical_profile_rate(
                            projected_seconds,
                            total_execution_time);
                    incremental_lease_seconds =
                        peak_general_listener_next_observation_lease_seconds(
                            request_rate,
                            heartbeat_observation_horizon_seconds);

                    size_t before_count =
                        pending_mutation_count + admitted_count;
                    size_t after_count = before_count + 1;
                    size_t before_windows =
                        (before_count +
                         PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES - 1) /
                        PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES;
                    size_t after_windows =
                        (after_count +
                         PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES - 1) /
                        PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES;
                    double extra_stop_seconds = DBL_MAX;
                    (void)peak_general_listener_multiply_nonnegative_finite(
                        (double)(after_windows - before_windows),
                        predicted_batch_stop_seconds,
                        &extra_stop_seconds);
                    double candidate_seconds = DBL_MAX;
                    double remaining_headroom_seconds = headroom_seconds;
                    if (incremental_lease_seconds != DBL_MAX) {
                        (void)peak_general_listener_add_nonnegative_finite(
                            incremental_lease_seconds,
                            extra_stop_seconds,
                            &candidate_seconds);
                    }

                    if (candidate_seconds == DBL_MAX ||
                        !peak_general_listener_subtract_nonnegative_finite(
                            &remaining_headroom_seconds,
                            candidate_seconds)) {
                        continue;
                    }

                    gboolean accepted;
                    double request_ratio =
                        projected_seconds / total_execution_time;
                    accepted =
                        peak_general_listener_request_reattach_with_context_unlocked(
                            i,
                            source,
                            calls_snapshot[i],
                            request_ratio,
                            spent_ratio,
                            total_execution_time,
                            request_rate);
                    if (!accepted) {
                        continue;
                    }

                    wake_controller = TRUE;
                    headroom_seconds = remaining_headroom_seconds;
                    admitted_count++;
                }
                pthread_mutex_unlock(&lock);
            }
        }

        if (wake_controller) {
            peak_general_listener_controller_wake();
        }

        /* Adapt the next heartbeat sleep interval. */
        double gdt = now - prev_global_time;
        if (gdt <= 1e-12) gdt = 1e-12;

        double global_rate =
            (profile_global_overhead - prev_global_overhead) / gdt;
        ema_global_rate = hb_ema_a * global_rate + (1.0 - hb_ema_a) * ema_global_rate;

        prev_global_overhead = profile_global_overhead;
        prev_global_time     = now;

        /* Normalized amount by which overhead exceeds the global target. */
        double err = (global_target_ratio > 0.0)
                         ? (profile_global_overhead / global_target_ratio - 1.0)
                         : 0.0;
        if (err < 0.0) err = 0.0;

        /* Check more frequently when error or growth rate is larger. */
        double scale = 1.0 / (1.0 + hb_k_err * err + hb_k_rate * ema_global_rate);

        long long sleep_us = (long long)(clipd((double)heartbeat_time * scale,
                                       (double)hb_min_us,
                                       (double)hb_max_us) + 0.5);

        peak_general_listener_management_cpu_checkpoint(
            &management_cpu_checkpoint_ns,
            &management_cpu_checkpoint_valid);
        if (!peak_heartbeat_wait_us((unsigned int)sleep_us)) {
            break;
        }
    }

cleanup:
    pthread_mutex_lock(&lock);
    peak_general_listener_snapshot_heartbeat_overhead_seconds_unlocked();
    pthread_mutex_unlock(&lock);
    g_free(previous_time);
    g_free(previous_calls);
    g_free(recent_baseline_ready);
    g_free(rate_snapshot);
    g_free(ratio_snapshot);
    g_free(lifetime_snapshot);
    g_free(profile_seconds_snapshot);
    g_free(calls_snapshot);
    g_free(entries);

    gum_interceptor_unignore_current_thread(interceptor);
    return NULL;
}

/* Invocation callbacks. */
void
peak_general_listener_suspend_callbacks(void)
{
    atomic_store_explicit(&peak_general_callbacks_suspended,
                          TRUE,
                          memory_order_release);
}

void
peak_general_listener_note_mpi_finalize_requested(void)
{
    atomic_store_explicit(&peak_general_mpi_finalize_requested,
                          TRUE,
                          memory_order_release);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic int peak_checkpoint_test_mutation_contender_invalidated;
#endif

static void
peak_general_listener_checkpoint_shadow_update(
    PeakGeneralListenerCheckpointShadow* shadow,
    gdouble duration,
    gdouble exclusive_duration)
{
    if (atomic_load_explicit(&shadow->invalid, memory_order_relaxed)) {
        return;
    }
    gulong observed = atomic_load_explicit(&shadow->sequence,
                                           memory_order_acquire);

    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        if (atomic_load_explicit(&shadow->invalid, memory_order_relaxed)) {
            return;
        }
        if ((observed & 1UL) != 0) {
            break;
        }
        if (atomic_compare_exchange_strong_explicit(&shadow->sequence,
                                                    &observed,
                                                    observed + 1UL,
                                                    memory_order_acquire,
                                                    memory_order_relaxed)) {
            shadow->completed_calls++;
            shadow->total_time += duration;
            shadow->exclusive_time += exclusive_duration;
            if (shadow->completed_calls == 1) {
                shadow->max_time = (gfloat)duration;
                shadow->min_time = (gfloat)duration;
            } else {
                if (duration > shadow->max_time) {
                    shadow->max_time = (gfloat)duration;
                }
                if (duration < shadow->min_time) {
                    shadow->min_time = (gfloat)duration;
                }
            }
            atomic_store_explicit(&shadow->sequence,
                                  observed + 2UL,
                                  memory_order_release);
            return;
        }
        if ((observed & 1UL) != 0) {
            break;
        }
    }
    int expected_invalid = 0;
    if (!atomic_compare_exchange_strong_explicit(&shadow->invalid,
                                                 &expected_invalid,
                                                 1,
                                                 memory_order_release,
                                                 memory_order_relaxed)) {
        return;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_store_explicit(&peak_checkpoint_test_mutation_contender_invalidated,
                          1,
                          memory_order_release);
#endif
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static PeakGeneralListener* peak_checkpoint_test_mutation_listener;
static size_t peak_checkpoint_test_mutation_index;
static gulong peak_checkpoint_test_mutation_sequence;

PEAK_API int
peak_general_listener_test_checkpoint_mutation_begin(void)
{
    PeakGeneralListener* listener;
    PeakGeneralListenerCheckpointShadow* shadow;

    if (array_listener == NULL || peak_hook_address_count == 0 ||
        array_listener[0] == NULL || peak_max_num_threads == 0 ||
        peak_checkpoint_test_mutation_listener != NULL) {
        return EINVAL;
    }
    listener = PEAKGENERAL_LISTENER(array_listener[0]);
    for (size_t index = 0; index < peak_max_num_threads; index++) {
        if (peak_general_listener_num_calls_load(&listener->num_calls[index]) != 0) {
            peak_checkpoint_test_mutation_index = index;
            break;
        }
    }
    if (peak_general_listener_num_calls_load(
            &listener->num_calls[peak_checkpoint_test_mutation_index]) == 0) {
        return EINVAL;
    }
    if (listener->checkpoint_shadow == NULL) {
        return EINVAL;
    }
    for (size_t index = 0; index < peak_max_num_threads; index++) {
        PeakGeneralListenerCheckpointShadow* current =
            &listener->checkpoint_shadow[index];

        atomic_store_explicit(&current->sequence, 0, memory_order_relaxed);
        atomic_store_explicit(&current->invalid, 0, memory_order_relaxed);
        current->completed_calls = 0;
        current->total_time = 0.0;
        current->exclusive_time = 0.0;
        current->max_time = 0.0f;
        current->min_time = 0.0f;
    }
    shadow = &listener->checkpoint_shadow[peak_checkpoint_test_mutation_index];
    peak_checkpoint_test_mutation_sequence = 0;
    if (!atomic_compare_exchange_strong_explicit(&shadow->sequence,
                                                 &peak_checkpoint_test_mutation_sequence,
                                                 1,
                                                 memory_order_acquire,
                                                 memory_order_relaxed)) {
        return EAGAIN;
    }
    shadow->total_time = 170.0;
    shadow->exclusive_time = 85.0;
    shadow->max_time = 10.0f;
    shadow->min_time = 10.0f;
    shadow->completed_calls = 1;
    peak_checkpoint_test_mutation_listener = listener;
    atomic_store_explicit(&peak_checkpoint_test_mutation_contender_invalidated,
                          0,
                          memory_order_release);
    return 0;
}

PEAK_API void
peak_general_listener_test_checkpoint_mutation_release(void)
{
    if (peak_checkpoint_test_mutation_listener != NULL) {
        atomic_store_explicit(
            &peak_checkpoint_test_mutation_listener->checkpoint_shadow[
                peak_checkpoint_test_mutation_index].sequence,
            peak_checkpoint_test_mutation_sequence + 2UL,
            memory_order_release);
        peak_checkpoint_test_mutation_listener = NULL;
    }
}

PEAK_API int
peak_general_listener_test_checkpoint_mutation_contend(void)
{
    PeakGeneralListener* listener = peak_checkpoint_test_mutation_listener;
    size_t index = peak_checkpoint_test_mutation_index;
    if (listener == NULL) {
        return EINVAL;
    }
    peak_general_listener_checkpoint_shadow_update(
        &listener->checkpoint_shadow[index], 20.0, 10.0);
    return 0;
}

PEAK_API int
peak_general_listener_test_checkpoint_mutation_contender_invalidated(void)
{
    return atomic_load_explicit(&peak_checkpoint_test_mutation_contender_invalidated,
                                memory_order_acquire);
}
#endif

static void
peak_general_listener_abandon_current_invocation(PeakInvocationData* priv,
                                                 gboolean use_pause_guard)
{
    if (priv == NULL || !priv->initialized) {
        return;
    }

    if (!peak_general_listener_pop_invocation(priv, NULL)) {
        return;
    }
    if (thread_data.level == 0) {
        void* tmp_ptr = thread_data.child_time;
        thread_data.child_time = NULL;
        if (use_pause_guard) {
            pthread_pause_disable();
        }
        g_free(tmp_ptr);
        if (use_pause_guard) {
            pthread_pause_enable();
        }
    } else if (thread_data.child_time != NULL) {
        thread_data.child_time[thread_data.level] = 0.0;
    }
}

static gboolean
peak_general_listener_pop_invocation(PeakInvocationData* priv,
                                     gdouble* child_duration_out)
{
    if (child_duration_out != NULL) {
        *child_duration_out = 0.0;
    }

    if (priv == NULL || !priv->initialized ||
        thread_data.child_time == NULL ||
        thread_data.level == 0 ||
        priv->stack_level == 0 ||
        thread_data.level < priv->stack_level) {
        if (priv != NULL) {
            priv->initialized = FALSE;
            priv->stack_level = 0;
        }
        return FALSE;
    }

    while (thread_data.level > priv->stack_level) {
        thread_data.level--;
        thread_data.child_time[thread_data.level] = 0.0;
    }

    thread_data.level--;
    if (child_duration_out != NULL) {
        *child_duration_out = thread_data.child_time[thread_data.level];
    }
    thread_data.child_time[thread_data.level] = 0.0;
    priv->initialized = FALSE;
    priv->stack_level = 0;
    return TRUE;
}

static gdouble
peak_general_listener_exclusive_duration(gdouble total_duration,
                                         gdouble child_duration)
{
    if (child_duration >= total_duration) {
        return 0.0;
    }

    return total_duration - child_duration;
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (!listener || g_object_is_floating(listener)) {
            return;
    }
    gum_interceptor_ignore_current_thread(interceptor);
    PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
    priv->initialized = FALSE;
    priv->stack_level = 0;
    if (atomic_load_explicit(&peak_general_callbacks_suspended,
                             memory_order_acquire)) {
        gum_interceptor_unignore_current_thread(interceptor);
        return;
    }

    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    pthread_t my_tid = pthread_self();
    gboolean mapped_found = FALSE;
    size_t mapped_tid = pthread_listener_lookup_thread(my_tid, &mapped_found);
    if (!mapped_found || mapped_tid >= peak_max_num_threads) {
        mapped_tid = 0;
    }
    thread_data.self_mapped_id = mapped_tid;
    thread_data.self_mapped_known = mapped_found && mapped_tid < peak_max_num_threads;
    if (peak_detach_cost == 0 && heartbeat_time == 0 &&
        !peak_detach_count_overridden) {
        size_t index = mapped_tid;
        if (thread_data.child_time == NULL) {
            thread_data.level = 0;
            thread_data.capacity = 16;
            thread_data.child_time = g_new(gdouble, 16);
        }
        thread_data.child_time[thread_data.level] = 0.0;
        thread_data.level++;
        if (thread_data.level == thread_data.capacity) {
            thread_data.capacity *= 2;
            thread_data.child_time = g_renew(double, thread_data.child_time, thread_data.capacity);
        }
        (void)peak_general_listener_num_calls_increment(
            &self->num_calls[index]);
    } else {
        size_t index = mapped_tid;
        if (thread_data.child_time == NULL) {
            thread_data.level = 0;
            thread_data.capacity = 16;
            pthread_pause_disable();
            thread_data.child_time = g_new(gdouble, 16);
            pthread_pause_enable();
        }
        thread_data.child_time[thread_data.level] = 0.0;
        thread_data.level++;
        if (thread_data.level == thread_data.capacity) {
            thread_data.capacity *= 2;
            pthread_pause_disable();
            thread_data.child_time = g_renew(double, thread_data.child_time, thread_data.capacity);
            pthread_pause_enable();
        }
        gulong current_num_calls =
            peak_general_listener_num_calls_increment(
                &self->num_calls[index]);
        size_t hook_id = self->hook_id;
        gboolean detach_requested = FALSE;

        pthread_mutex_lock(&lock);
        /*
         * Auxiliary listeners, including the overhead-calibration listener,
         * share this callback implementation.  Only the listener currently
         * published for a target may drive that target's lifecycle.
        */
        gboolean listener_owns_hook =
            peak_general_hook_is_published_unlocked(hook_id) &&
            array_listener[hook_id] == listener;
        if (listener_owns_hook && current_num_calls >= peak_detach_count) {
            gulong total_num_calls = 0;
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                total_num_calls += peak_general_listener_num_calls_load(
                    &self->num_calls[j]);
            }
            detach_requested =
                peak_general_listener_request_detach_with_context_unlocked(
                    hook_id,
                    PEAK_HOOK_REQUEST_SOURCE_DETACH_COUNT,
                    total_num_calls,
                    0.0,
                    0.0,
                    peak_second() - peak_main_time,
                    0.0);
        }
        pthread_mutex_unlock(&lock);
        if (detach_requested) {
            peak_general_listener_controller_wake();
        }

        if (check_interval != 0) pthread_pause_enable();
        else pthread_pause_disable();
    }
    priv->start_time = peak_second();
    priv->stack_level = thread_data.level;
    priv->initialized = TRUE;
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    gum_interceptor_ignore_current_thread(interceptor);
    if (peak_detach_cost == 0 && heartbeat_time == 0 &&
        !peak_detach_count_overridden) {
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                g_free(tmp_ptr);
            }
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
        if (!priv->initialized) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (atomic_load_explicit(&peak_general_callbacks_suspended,
                                 memory_order_acquire)) {
            peak_general_listener_abandon_current_invocation(priv, FALSE);
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        gboolean mapped_found = FALSE;
        size_t mapped_tid = pthread_listener_lookup_thread(pthread_self(), &mapped_found);
        if (!mapped_found || mapped_tid >= peak_max_num_threads) {
            mapped_tid = 0;
        }
        end_time = end_time - priv->start_time;
        size_t index = mapped_tid;
        gdouble child_duration = 0.0;
        if (!peak_general_listener_pop_invocation(priv, &child_duration)) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] ||
            peak_general_listener_num_calls_load(&self->num_calls[index]) == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] +=
            peak_general_listener_exclusive_duration(
                end_time,
                child_duration);
        if (G_UNLIKELY(self->checkpoint_shadow != NULL)) {
            peak_general_listener_checkpoint_shadow_update(
                &self->checkpoint_shadow[index],
                end_time,
                peak_general_listener_exclusive_duration(end_time, child_duration));
        }
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            g_free(tmp_ptr);
        }
    } else {
        pthread_pause_enable();
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                pthread_pause_disable();
                g_free(tmp_ptr);
                pthread_pause_enable();
            }
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
        if (!priv->initialized) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (atomic_load_explicit(&peak_general_callbacks_suspended,
                                 memory_order_acquire)) {
            peak_general_listener_abandon_current_invocation(priv, TRUE);
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        gboolean mapped_found = FALSE;
        size_t mapped_tid = pthread_listener_lookup_thread(pthread_self(), &mapped_found);
        if (!mapped_found || mapped_tid >= peak_max_num_threads) {
            mapped_tid = 0;
        }
        end_time = end_time - priv->start_time;
        size_t index = mapped_tid;
        gdouble child_duration = 0.0;
        if (!peak_general_listener_pop_invocation(priv, &child_duration)) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] ||
            peak_general_listener_num_calls_load(&self->num_calls[index]) == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] +=
            peak_general_listener_exclusive_duration(
                end_time,
                child_duration);
        if (G_UNLIKELY(self->checkpoint_shadow != NULL)) {
            peak_general_listener_checkpoint_shadow_update(
                &self->checkpoint_shadow[index],
                end_time,
                peak_general_listener_exclusive_duration(end_time, child_duration));
        }
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            pthread_pause_disable();
            g_free(tmp_ptr);
            pthread_pause_enable();
        }
    }
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_class_init(PeakGeneralListenerClass* klass)
{
    (void)PEAKGENERAL_IS_LISTENER;
    (void)glib_autoptr_cleanup_PeakGeneralListener;
}

static void
peak_general_listener_iface_init(gpointer g_iface,
                                 gpointer iface_data)
{
    GumInvocationListenerInterface* iface = g_iface;

    iface->on_enter = peak_general_listener_on_enter;
    iface->on_leave = peak_general_listener_on_leave;
}

static void
peak_general_listener_init(PeakGeneralListener* self)
{
    size_t total_count = peak_max_num_threads;
    self->num_calls = g_new0(gulong, total_count);
    self->total_time = g_new0(gdouble, total_count);
    self->exclusive_time = g_new0(gdouble, total_count);
    self->max_time = g_new0(gfloat, total_count);
    self->min_time = g_new0(gfloat, total_count);
    if (peak_exec_checkpoint_enabled_at_startup != NULL &&
        peak_exec_checkpoint_enabled_at_startup()) {
        self->checkpoint_shadow = g_aligned_alloc0(
            total_count,
            sizeof(*self->checkpoint_shadow),
            64);
        for (size_t index = 0; index < total_count; index++) {
            atomic_init(&self->checkpoint_shadow[index].sequence, 0);
            atomic_init(&self->checkpoint_shadow[index].invalid, 0);
        }
    }
    self->target_thread_called = g_new0(gboolean, total_count);
}

void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->exclusive_time);
    g_free(self->max_time);
    g_free(self->min_time);
    g_aligned_free(self->checkpoint_shadow);
    g_free(self->target_thread_called);
    self->target_thread_called = NULL;
}

__attribute__((noinline)) static void peak_general_overhead_dummy_func()
{
    struct timespec ts = { 0, 1 }; /* Sleep for one nanosecond. */
    nanosleep(&ts, NULL);
}

/* Listener attachment. */
static void
peak_general_overhead_bootstrapping()
{
    GumInvocationListener* listener_bootstrapping =
        g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
    PEAKGENERAL_LISTENER(listener_bootstrapping)->hook_id = 0;

    PeakDetachRequest mutation_request = {
        .hook_id = 0,
        .symbol_name = "peak_general_overhead_dummy_func",
        .function_address = &peak_general_overhead_dummy_func,
        .interceptor = interceptor,
        .listener = listener_bootstrapping,
        .operation = PEAK_DETACH_OPERATION_ATTACH
    };
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        peak_log_debug("[peak] skipping overhead calibration Gum attach: %s\n",
                   peak_detach_controller_status_string(detach_status));
        peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
        g_object_unref(listener_bootstrapping);
        peak_general_overhead = 0.0;
        return;
    }

    gum_interceptor_begin_transaction(interceptor);
    GumAttachReturn attach_status =
        gum_interceptor_attach(interceptor,
                               &peak_general_overhead_dummy_func,
                               listener_bootstrapping,
                               NULL);
    gum_interceptor_end_transaction(interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("overhead attach finish",
                                                        detach_status);
    }

    if (attach_status != GUM_ATTACH_OK) {
        g_printerr("[peak] overhead calibration Gum attach failed, status=%d\n",
                   attach_status);
        peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
        g_object_unref(listener_bootstrapping);
        peak_general_overhead = 0.0;
        return;
    }

    guint n_tests = 2000;
    double* time = g_new(double, n_tests * 2);
    for (guint i = 0; i < n_tests; i++) {
        time[n_tests + i] = peak_second();
        peak_general_overhead_dummy_func();
        time[n_tests + i] = peak_second() - time[n_tests + i];
    }

    mutation_request.listener = listener_bootstrapping;
    mutation_request.operation = PEAK_DETACH_OPERATION_DETACH;
    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        g_printerr("[peak] fatal overhead calibration Gum detach prepare failed after attach: %s\n",
                   peak_detach_controller_status_string(detach_status));
        peak_general_overhead = 0.0;
        g_free(time);
        peak_detach_controller_abort_after_failed_finish("overhead detach prepare",
                                                        detach_status);
    }
    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_detach(interceptor, listener_bootstrapping);
    gum_interceptor_end_transaction(interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("overhead detach finish",
                                                        detach_status);
    }
    if (!peak_general_controller_flush_teardown()) {
        g_printerr("[peak] overhead calibration Gum teardown did not flush; leaving bootstrap listener alive\n");
        g_free(time);
        return;
    }
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
    g_object_unref(listener_bootstrapping);

    for (guint i = 0; i < n_tests; i++) {
        time[i] = peak_second();
        peak_general_overhead_dummy_func();
        time[i] = peak_second() - time[i];
    }

    peak_general_overhead = (median_double(&time[n_tests], n_tests) - median_double(&time[0], n_tests));
    if (peak_general_overhead <= 0.0) {
        peak_general_overhead = peak_general_overhead_floor;
    }
    g_free(time);
}

static void
peak_symbol_map_add_target(GHashTable* table, const char* symbol)
{
    if (symbol == NULL || g_hash_table_contains(table, symbol)) {
        return;
    }

    g_hash_table_insert(table, g_strdup(symbol), g_ptr_array_new());
}

static void peak_build_symbol_map_once(size_t first_target_index) {
    gum_find_functions_matching_initialize = true;
    gum_symbol_demangled_mapping = g_hash_table_new_full(g_str_hash,
                                                         str_equal_function_general,
                                                         g_free,
                                                         (GDestroyNotify) g_ptr_array_unref);
    gum_symbol_short_mapping = g_hash_table_new_full(g_str_hash,
                                                     str_equal_function_general,
                                                     g_free,
                                                     (GDestroyNotify) g_ptr_array_unref);

    for (size_t i = first_target_index; i < peak_hook_address_count; i++) {
        const char* symbol = peak_hook_strings[i];
        if (!peak_symbol_should_use_cpp_map(symbol) ||
            cxa_demangle_status(symbol) == 0) {
            continue;
        }

        peak_symbol_map_add_target(gum_symbol_demangled_mapping, symbol);
        peak_symbol_map_add_target(gum_symbol_short_mapping, symbol);
    }

    if (g_hash_table_size(gum_symbol_demangled_mapping) == 0 &&
        g_hash_table_size(gum_symbol_short_mapping) == 0) {
        return;
    }

    GArray* addresses = gum_find_functions_matching("_Z*");
    for (gsize j = 0; j < addresses->len; j++) {
        gpointer addr = g_array_index(addresses, gpointer, j);
        if (!addr) continue;
        gchar* mangled = gum_symbol_name_from_address(addr);
        if (!mangled) continue;

        char* demangled = cxa_demangle(mangled);
        g_free(mangled);
        if (!demangled) continue;

        GPtrArray* demangled_candidates =
            g_hash_table_lookup(gum_symbol_demangled_mapping, demangled);
        if (demangled_candidates) {
            g_ptr_array_add(demangled_candidates, addr);
        }

        char* function_name = extract_function_name(demangled);
        GPtrArray* short_candidates =
            g_hash_table_lookup(gum_symbol_short_mapping, function_name);
        if (short_candidates) {
            g_ptr_array_add(short_candidates, addr);
        }

        free(function_name);
        free(demangled);
    }

    g_array_free(addresses, TRUE);
}

void peak_general_listener_attach()
{
    peak_general_controller_init_trace_config();
    peak_general_listener_init_attach_policy();
    peak_general_listener_final_heartbeat_overhead_seconds = 0.0;
    peak_general_listener_reset_final_report_snapshot();
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        FALSE,
        memory_order_release);
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_reattached = g_new0(gboolean, peak_hook_address_count);
    array_listener_revisited = g_new0(gboolean, peak_hook_address_count);
    array_listener_gum_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_gum_detach_flushed = g_new0(gboolean, peak_hook_address_count);
    peak_hook_last_detach_time = g_new0(double, peak_hook_address_count);
    peak_hook_states = g_new0(PeakHookState, peak_hook_address_count);
    peak_hook_next_retry_time = g_new0(double, peak_hook_address_count);
    peak_hook_pending_observed_time = g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_source =
        g_new0(PeakHookRequestSource, peak_hook_address_count);
    peak_hook_pending_request_calls = g_new0(gulong, peak_hook_address_count);
    peak_hook_pending_request_ratio = g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_global_overhead =
        g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_total_time =
        g_new0(double, peak_hook_address_count);
    peak_hook_pending_request_rate = g_new0(double, peak_hook_address_count);
    peak_hook_detach_profile_seconds =
        g_new0(double, peak_hook_address_count);
    peak_hook_reattach_request_calls =
        g_new0(gulong, peak_hook_address_count);
    peak_hook_reattach_request_calls_valid =
        g_new0(gboolean, peak_hook_address_count);
    peak_hook_cached_sample_calls = g_new0(gulong, peak_hook_address_count);
    peak_hook_cached_sample_profile_seconds =
        g_new0(double, peak_hook_address_count);
    peak_hook_cached_sample_valid =
        g_new0(gboolean, peak_hook_address_count);
    peak_hook_retry_count = g_new0(unsigned int, peak_hook_address_count);
    peak_hook_last_retry_status = g_new0(PeakDetachStatus, peak_hook_address_count);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    peak_demangled_strings = g_new0(char*, peak_hook_address_count);
    gboolean startup_attach_can_skip_stop =
        peak_general_listener_startup_attach_can_skip_stop();
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        /* Redirect functions for which PEAK already owns support wrappers. */
        if (strcmp(peak_hook_strings[i], "MPI_Finalize") == 0 ||
            strcmp(peak_hook_strings[i], "PMPI_Finalize") == 0) {
            hook_address[i] = NULL;
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "close") == 0) {
            hook_address[i] = peak_general_listener_find_function("peak_close");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "exit") == 0) {
            hook_address[i] = peak_general_listener_find_function("peak_exit");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "main") == 0) {
            hook_address[i] = NULL;
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelEx") == 0) {
            /* C++ template variants also use cudaLaunchKernelExC internally. */
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelExC") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernel") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernelEx") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_launch_kernel_ex");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaGraphLaunch") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cuda_graph_launch");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuGraphLaunch") == 0) {
            hook_address[i] =
                peak_general_listener_find_function("peak_cu_graph_launch");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "dlopen") == 0) {
            peak_log_info("[peak] skipping target dlopen: PEAK owns the dlopen listener used for dynamic attach\n");
            hook_address[i] = NULL;
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else {
            gpointer ptr = peak_general_listener_find_function(peak_hook_strings[i]);
            if (ptr) {
                hook_address[i] = ptr;
                char* demangled = cxa_demangle(peak_hook_strings[i]);
                peak_demangled_strings[i] = g_strdup(demangled);
                free(demangled);
            } else {
                if (peak_symbol_should_use_cpp_map(peak_hook_strings[i])) {
                    if (!gum_find_functions_matching_initialize) {
                        peak_build_symbol_map_once(i);
                    }

                    if (cxa_demangle_status(peak_hook_strings[i]) != 0) {
                        GPtrArray* candidates =
                            g_hash_table_lookup(gum_symbol_demangled_mapping,
                                                peak_hook_strings[i]);
                        if (candidates && candidates->len > 0) {
                            /* A full demangled name resolves to one candidate. */
                            hook_address[i] = g_ptr_array_index(candidates, 0);
                            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                        } else {
                            /* Fall back to the first short-name candidate. */
                            candidates = g_hash_table_lookup(gum_symbol_short_mapping,
                                                             peak_hook_strings[i]);
                            if (candidates && candidates->len > 0) {
                                hook_address[i] = g_ptr_array_index(candidates, 0);
                                gchar* mangled = gum_symbol_name_from_address(hook_address[i]);

                                if (mangled != NULL) {
                                    char* demangled = cxa_demangle(mangled);
                                    g_free(mangled);
                                    if (demangled != NULL) {
                                        peak_demangled_strings[i] = g_strdup(demangled);
                                        free(demangled);
                                    } else {
                                        /* Failed to demangle; fall back to original hook string */
                                        peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                                    }
                                } else {
                                    /* Failed to get mangled name; fall back to original hook string */
                                    peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                                }
                            }
                        }
                    }
                }
                if (hook_address[i] == NULL) {
                    peak_dynamic_attach_needed = TRUE;
                }
            }
        }
        if (hook_address[i]) {
            gpointer resolved_hook_address = hook_address[i];
            gboolean duplicate_address = FALSE;
            for (size_t j = 0; j < i; j++) {
                if (hook_address[j] == resolved_hook_address &&
                    array_listener[j] != NULL) {
                    duplicate_address = TRUE;
                    break;
                }
            }
            if (duplicate_address) {
                hook_address[i] = NULL;
                if (peak_demangled_strings[i] == NULL) {
                    peak_demangled_strings[i] =
                        g_strdup(peak_hook_strings[i]);
                }
                peak_log_debug("[peak] skipping duplicate startup target %s at %p\n",
                               peak_hook_strings[i],
                               resolved_hook_address);
                continue;
            }
            if (!peak_general_listener_attach_target_is_supported(
                    peak_hook_strings[i],
                    resolved_hook_address)) {
                hook_address[i] = NULL;
                g_free(peak_demangled_strings[i]);
                peak_demangled_strings[i] = NULL;
                continue;
            }
            GumInvocationListener* new_listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
            PEAKGENERAL_LISTENER(new_listener)->hook_id = i;
            hook_address[i] = NULL;
            PeakGumTargetAttachPlan attach_plan;
            peak_gum_target_attach_plan(resolved_hook_address, &attach_plan);
            PeakDetachRequest mutation_request = {
                .hook_id = i,
                .symbol_name = peak_hook_strings[i],
                .function_address = resolved_hook_address,
                .interceptor = interceptor,
                .listener = new_listener,
                .operation = PEAK_DETACH_OPERATION_ATTACH,
                .blocked_pc_start = attach_plan.mutation_guard_size > 0
                    ? attach_plan.mutation_address
                    : NULL,
                .blocked_pc_size = attach_plan.mutation_guard_size
            };
            PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;
            if (!startup_attach_can_skip_stop &&
                !peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                              &detach_status)) {
                g_printerr("[peak] skipping initial Gum attach for hook %lu (%s): %s\n",
                           (unsigned long)i,
                           peak_hook_strings[i] != NULL ? peak_hook_strings[i] : "<unknown>",
                           peak_detach_controller_status_string(detach_status));
                g_free(peak_demangled_strings[i]);
                peak_demangled_strings[i] = NULL;
                peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
                g_object_unref(new_listener);
                continue;
            }
            gum_interceptor_begin_transaction(interceptor);
            GumAttachReturn attach_status =
                peak_gum_interceptor_attach_target(interceptor,
                                                   resolved_hook_address,
                                                   new_listener,
                                                   &attach_plan);
            gum_interceptor_end_transaction(interceptor);
            if (!startup_attach_can_skip_stop &&
                !peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                             &detach_status)) {
                peak_detach_controller_abort_after_failed_finish("initial attach finish",
                                                                detach_status);
            }
            if (attach_status == GUM_ATTACH_OK) {
                hook_address[i] = resolved_hook_address;
                array_listener[i] = new_listener;
                array_listener_gum_detached[i] = FALSE;
                array_listener_gum_detach_flushed[i] = TRUE;
                peak_general_controller_set_state_unlocked(i, PEAK_HOOK_ATTACHED);
            } else {
                g_printerr("[peak] Gum initial attach failed for hook %lu (%s), status=%d\n",
                           (unsigned long)i,
                           peak_hook_strings[i] != NULL ? peak_hook_strings[i] : "<unknown>",
                           attach_status);
                peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
                g_object_unref(new_listener);
                g_free(peak_demangled_strings[i]);
                peak_demangled_strings[i] = NULL;
            }
        }
    }
    if (gum_find_functions_matching_initialize) {
        g_hash_table_destroy(gum_symbol_demangled_mapping);
        g_hash_table_destroy(gum_symbol_short_mapping);
        gum_symbol_demangled_mapping = NULL;
        gum_symbol_short_mapping = NULL;
        gum_find_functions_matching_initialize = false;
    }
    if (peak_hook_address_count) {
        peak_detach_count_overridden =
            peak_general_listener_parse_detach_count_override(&peak_detach_count);
        gboolean need_overhead_calibration =
            peak_detach_cost > 0 ||
            heartbeat_time != 0 ||
            peak_detach_count_overridden;
        if (need_overhead_calibration) {
            peak_general_overhead_bootstrapping();
        } else {
            peak_general_overhead = 0.0;
        }
        if (!peak_detach_count_overridden && peak_detach_cost > 0) {
            if (peak_general_overhead > 0.0) {
                peak_detach_count =
                    (peak_detach_cost > peak_general_overhead) ?
                    peak_detach_cost / peak_general_overhead : 1;
            } else {
                peak_detach_count = G_MAXULONG;
            }
        }
        peak_need_detach[0] = false;
    }
}

/* Checkpoint and report snapshot capture. */
#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_API int
peak_general_listener_test_checkpoint_snapshot_lock_hold(void)
{
    return pthread_mutex_lock(&lock);
}

PEAK_API int
peak_general_listener_test_checkpoint_snapshot_lock_release(void)
{
    return pthread_mutex_unlock(&lock);
}
#endif

static gboolean
peak_general_listener_checkpoint_snapshot_lock(void)
{
    return pthread_mutex_trylock(&lock) == 0;
}

typedef struct {
    PeakGeneralListenerCheckpointShadow* shadows;
    gulong* sequences;
    int* invalid;
} PeakExecCheckpointThreadSnapshot;

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic int peak_checkpoint_test_busy_pause = 0;
static _Atomic int peak_checkpoint_test_busy_held = 0;
static _Atomic int peak_checkpoint_test_busy_released = 0;

PEAK_API void
peak_general_listener_test_checkpoint_busy_pause_enable(void)
{
    atomic_store_explicit(&peak_checkpoint_test_busy_released,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_checkpoint_test_busy_held,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_checkpoint_test_busy_pause,
                          1,
                          memory_order_release);
}

PEAK_API int
peak_general_listener_test_checkpoint_busy_is_held(void)
{
    return atomic_load_explicit(&peak_checkpoint_test_busy_held,
                                memory_order_acquire);
}

PEAK_API void
peak_general_listener_test_checkpoint_busy_release(void)
{
    atomic_store_explicit(&peak_checkpoint_test_busy_released,
                          1,
                          memory_order_release);
}

static void
peak_general_listener_test_checkpoint_busy_pause(void)
{
    if (atomic_load_explicit(&peak_checkpoint_test_busy_pause,
                             memory_order_acquire) == 0) {
        return;
    }
    atomic_store_explicit(&peak_checkpoint_test_busy_held,
                          1,
                          memory_order_release);
    while (atomic_load_explicit(&peak_checkpoint_test_busy_released,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
    atomic_store_explicit(&peak_checkpoint_test_busy_pause,
                          0,
                          memory_order_release);
}
#endif

static gboolean
peak_exec_checkpoint_copy_listener(
    const PeakGeneralListener* listener,
    PeakExecCheckpointThreadSnapshot* snapshot,
    size_t thread_count)
{
#if defined(__linux__) && defined(SYS_process_vm_readv)
    if (listener->checkpoint_shadow == NULL) {
        errno = EOPNOTSUPP;
        return FALSE;
    }
    struct iovec local = { snapshot->shadows,
                           thread_count * sizeof(*snapshot->shadows) };
    struct iovec remote = { listener->checkpoint_shadow,
                            thread_count * sizeof(*listener->checkpoint_shadow) };
    size_t expected = thread_count * sizeof(*snapshot->shadows);
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        gboolean busy = FALSE;

        for (size_t index = 0; index < thread_count; index++) {
            snapshot->sequences[index] = atomic_load_explicit(
                &listener->checkpoint_shadow[index].sequence, memory_order_acquire);
            snapshot->invalid[index] = atomic_load_explicit(
                &listener->checkpoint_shadow[index].invalid, memory_order_acquire);
            if ((snapshot->sequences[index] & 1UL) != 0 ||
                snapshot->invalid[index] != 0) {
                busy = TRUE;
                break;
            }
        }
        if (busy) {
#ifdef PEAK_ENABLE_TEST_HOOKS
            peak_general_listener_test_checkpoint_busy_pause();
#endif
            continue;
        }

        long copied = peak_exec_raw_syscall6(
            SYS_process_vm_readv,
            (long)getpid(),
            (long)&local,
            1,
            (long)&remote,
            1,
            0);
        if (copied < 0 || (size_t)copied != expected) {
            return FALSE;
        }
        for (size_t index = 0; index < thread_count; index++) {
            gulong current = atomic_load_explicit(
                &listener->checkpoint_shadow[index].sequence, memory_order_acquire);
            int invalid = atomic_load_explicit(
                &listener->checkpoint_shadow[index].invalid, memory_order_acquire);
            if (current != snapshot->sequences[index] ||
                invalid != snapshot->invalid[index] || invalid != 0 ||
                (current & 1UL) != 0) {
                busy = TRUE;
                break;
            }
        }
        if (!busy) {
            return TRUE;
        }
    }
    errno = EAGAIN;
    return FALSE;
#else
    (void)listener;
    (void)snapshot;
    (void)thread_count;
    errno = ENOSYS;
    return FALSE;
#endif
}

static void
peak_exec_checkpoint_free_snapshot(PeakExecCheckpointRow* records,
                                   size_t* name_lengths,
                                   char* names,
                                   void* thread_storage)
{
    free(thread_storage);
    free(names);
    free(name_lengths);
    free(records);
}

gboolean
peak_general_listener_checkpoint_for_exec(unsigned long long checkpoint_index)
{
    size_t hook_count;
    size_t thread_count;
    size_t name_bytes = 0;
    size_t thread_storage_bytes;
    PeakExecCheckpointRow* records = NULL;
    size_t* name_lengths = NULL;
    char* names = NULL;
    void* thread_storage = NULL;
    PeakExecCheckpointThreadSnapshot threads = {0};
    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        return FALSE;
    }
    hook_count = peak_hook_address_count;
    thread_count = peak_max_num_threads;
    pthread_mutex_unlock(&lock);

    if (hook_count > SIZE_MAX / sizeof(*records) ||
        hook_count > SIZE_MAX / sizeof(*name_lengths) ||
        thread_count > SIZE_MAX / sizeof(gdouble)) {
        errno = EOVERFLOW;
        return FALSE;
    }
    records = calloc(hook_count != 0 ? hook_count : 1, sizeof(*records));
    name_lengths = calloc(hook_count != 0 ? hook_count : 1,
                          sizeof(*name_lengths));
    if (records == NULL || name_lengths == NULL) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        return FALSE;
    }

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        return FALSE;
    }
    for (size_t i = 0; i < hook_count; i++) {
        const char* name =
            (peak_demangled_strings != NULL &&
             peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        size_t length = strlen(name) + 1;

        if (length > SIZE_MAX - name_bytes) {
            pthread_mutex_unlock(&lock);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               NULL,
                                               NULL);
            errno = EOVERFLOW;
            return FALSE;
        }
        name_lengths[i] = length;
        name_bytes += length;
    }
    pthread_mutex_unlock(&lock);

    if (thread_count != 0 &&
        thread_count > SIZE_MAX /
            (sizeof(PeakGeneralListenerCheckpointShadow) + sizeof(gulong) +
             sizeof(int))) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        errno = EOVERFLOW;
        return FALSE;
    }
    thread_storage_bytes = thread_count *
        (sizeof(PeakGeneralListenerCheckpointShadow) + sizeof(gulong) +
         sizeof(int));
    names = malloc(name_bytes != 0 ? name_bytes : 1);
    int thread_storage_error = posix_memalign(
        &thread_storage,
        64,
        thread_storage_bytes != 0 ? thread_storage_bytes : 1);
    if (thread_storage_error != 0) {
        thread_storage = NULL;
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        errno = thread_storage_error;
        return FALSE;
    }
    if (names == NULL) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    threads.shadows = thread_storage;
    threads.sequences = (gulong*)(threads.shadows + thread_count);
    threads.invalid = (int*)(threads.sequences + thread_count);

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    size_t name_offset = 0;
    for (size_t i = 0; i < hook_count; i++) {
        const char* name;
        char* record_name;
        size_t current_length;

        if (i >= peak_hook_address_count || hook_address == NULL ||
            array_listener == NULL || hook_address[i] == NULL ||
            array_listener[i] == NULL) {
            continue;
        }
        name =
            (peak_demangled_strings != NULL &&
             peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        current_length = strlen(name) + 1;
        if (current_length > name_lengths[i] ||
            !peak_exec_checkpoint_copy_listener(
                PEAKGENERAL_LISTENER(array_listener[i]),
                &threads,
                thread_count)) {
            pthread_mutex_unlock(&lock);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               names,
                                               thread_storage);
            return FALSE;
        }
        record_name = names + name_offset;
        memcpy(record_name, name, current_length);
        records[i].name = record_name;
        name_offset += name_lengths[i];

        for (size_t j = 0; j < thread_count; j++) {
            const PeakGeneralListenerCheckpointShadow* shadow =
                &threads.shadows[j];
            gulong calls = shadow->completed_calls;

            records[i].num_calls += calls;
            records[i].total_time += shadow->total_time;
            records[i].exclusive_time += shadow->exclusive_time;
            if (calls != 0) {
                records[i].threads_seen++;
                if (shadow->total_time > records[i].max_total_time) {
                    records[i].max_total_time = shadow->total_time;
                }
                if (shadow->total_time < records[i].min_total_time ||
                    records[i].threads_seen == 1) {
                    records[i].min_total_time = shadow->total_time;
                }
                if (shadow->max_time > records[i].max_time) {
                    records[i].max_time = shadow->max_time;
                }
                if (shadow->min_time < records[i].min_time ||
                    records[i].threads_seen == 1) {
                    records[i].min_time = shadow->min_time;
                }
            }
        }
    }
    pthread_mutex_unlock(&lock);

    gboolean written = peak_exec_checkpoint_write_rows(
        checkpoint_index,
        records,
        hook_count,
        peak_general_overhead);
    peak_exec_checkpoint_free_snapshot(records,
                                       name_lengths,
                                       names,
                                       thread_storage);
    return written;
}

static PeakReportSnapshot*
peak_general_listener_build_report_snapshot(
    const gulong* sum_num_calls,
    const gdouble* sum_total_time,
    const gdouble* max_total_time,
    const gdouble* min_total_time,
    const gdouble* sum_exclusive_time,
    const gfloat* sum_max_time,
    const gfloat* sum_min_time,
    const gulong* thread_count,
    int rank_count,
    const PeakReportOverhead* report_overhead)
{
    PeakReportSnapshot* snapshot =
        peak_report_snapshot_create(peak_hook_address_count);

    if (snapshot == NULL) {
        return NULL;
    }
    get_argv0(&snapshot->program);
    snapshot->overhead_per_call = peak_general_overhead;
    snapshot->rank_count = rank_count;
    if (report_overhead != NULL) {
        snapshot->overhead = *report_overhead;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        const char* name =
            peak_demangled_strings != NULL &&
                    peak_demangled_strings[i] != NULL ?
                peak_demangled_strings[i] :
            peak_hook_strings != NULL && peak_hook_strings[i] != NULL ?
                peak_hook_strings[i] :
                "";

        if (!peak_report_snapshot_set_name(snapshot, i, name)) {
            peak_report_snapshot_destroy(snapshot);
            return NULL;
        }
        snapshot->instrumented[i] = hook_address[i] != NULL;
        snapshot->detached[i] = array_listener_detached != NULL ?
                                    array_listener_detached[i] : FALSE;
        snapshot->reattached[i] = array_listener_reattached != NULL ?
                                      array_listener_reattached[i] : FALSE;
        snapshot->revisited[i] = array_listener_revisited != NULL ?
                                     array_listener_revisited[i] : FALSE;
        snapshot->num_calls[i] = sum_num_calls[i];
        snapshot->total_time[i] = sum_total_time[i];
        snapshot->max_total_time[i] = max_total_time[i];
        snapshot->min_total_time[i] = min_total_time[i];
        snapshot->exclusive_time[i] = sum_exclusive_time[i];
        snapshot->max_time[i] = sum_max_time[i];
        snapshot->min_time[i] = sum_min_time[i];
        snapshot->thread_count[i] = thread_count[i];
    }
    return snapshot;
}

static void
peak_general_listener_release_report_names(void)
{
    if (peak_demangled_strings == NULL) {
        return;
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
    }
    g_free(peak_demangled_strings);
    peak_demangled_strings = NULL;
}

static double
peak_general_listener_profile_seconds_from_calls(gulong* sum_num_calls)
{
    double profile_seconds = 0.0;

    if (sum_num_calls == NULL) {
        return 0.0;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            profile_seconds +=
                (double)sum_num_calls[i] * peak_general_overhead;
        }
    }
    double heartbeat_profile_seconds =
        peak_general_listener_report_heartbeat_overhead_seconds();
    if (peak_general_listener_positive_finite(heartbeat_profile_seconds) &&
        profile_seconds <= DBL_MAX - heartbeat_profile_seconds) {
        profile_seconds += heartbeat_profile_seconds;
    }

    return profile_seconds;
}

static void
peak_general_listener_populate_report_risk_fields(PeakReportOverhead* report)
{
    double profile_control_risk_seconds = DBL_MAX;

    if (report == NULL) {
        return;
    }
    report->local_ranks = peak_general_listener_local_mpi_ranks();
    report->control_risk_seconds =
        peak_general_listener_control_risk_seconds(report->control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        report->profile_seconds,
        report->control_risk_seconds,
        &profile_control_risk_seconds);
    report->profile_control_risk_seconds = profile_control_risk_seconds;
}

static PeakReportOverhead
peak_general_listener_local_report_overhead(gulong* sum_num_calls)
{
    PeakReportOverhead overhead = {0};
    PeakDetachAccountingSnapshot accounting;
    double elapsed_seconds = peak_main_time;
    double control_risk_seconds = 0.0;
    double profile_control_risk_seconds = DBL_MAX;

    if (peak_general_listener_final_report_snapshot.valid) {
        overhead = (PeakReportOverhead) {
            .valid = TRUE,
            .accounting_valid =
                peak_general_listener_final_report_snapshot.accounting_valid,
            .local_ranks =
                peak_general_listener_final_report_snapshot.local_ranks,
            .stop_window_count =
                peak_general_listener_final_report_snapshot.stop_window_count,
            .failed_stop_window_count =
                peak_general_listener_final_report_snapshot.failed_stop_window_count,
            .elapsed_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .elapsed_min_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .elapsed_max_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .profile_seconds =
                peak_general_listener_final_report_snapshot.profile_seconds,
            .control_seconds =
                peak_general_listener_final_report_snapshot.control_seconds,
            .management_seconds =
                peak_general_listener_final_report_snapshot.management_seconds,
            .control_risk_seconds =
                peak_general_listener_final_report_snapshot.control_risk_seconds,
            .profile_control_risk_seconds =
                peak_general_listener_final_report_snapshot.profile_control_risk_seconds,
            .profile_ratio =
                peak_general_listener_final_report_snapshot.profile_ratio,
            .control_ratio =
                peak_general_listener_final_report_snapshot.control_ratio,
            .profile_control_risk_ratio =
                peak_general_listener_final_report_snapshot.profile_control_risk_ratio,
            .control_risk_ratio =
                peak_general_listener_final_report_snapshot.control_risk_ratio,
            .ratio = peak_general_listener_final_report_snapshot.ratio,
            .management_ratio =
                peak_general_listener_final_report_snapshot.management_ratio,
        };
        return overhead;
    }

    overhead.profile_seconds =
        peak_general_listener_profile_seconds_from_calls(sum_num_calls);
    overhead.accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&accounting);
    overhead.stop_window_count =
        peak_general_listener_control_window_count_since_heartbeat(
            &accounting);
    overhead.failed_stop_window_count =
        peak_general_listener_failed_window_count_since_heartbeat(
            &accounting);
    overhead.elapsed_seconds = elapsed_seconds;
    overhead.elapsed_min_seconds = elapsed_seconds;
    overhead.elapsed_max_seconds = elapsed_seconds;
    overhead.control_seconds =
        (double)peak_general_listener_control_wall_ns_since_heartbeat(
            &accounting) / 1e9;
    overhead.management_seconds =
        peak_general_listener_management_cpu_seconds();
    control_risk_seconds =
        peak_general_listener_control_risk_seconds(overhead.control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        overhead.profile_seconds,
        control_risk_seconds,
        &profile_control_risk_seconds);
    if (elapsed_seconds > 0.0) {
        overhead.profile_ratio =
            overhead.profile_seconds / elapsed_seconds;
        overhead.control_ratio =
            overhead.control_seconds / elapsed_seconds;
        overhead.profile_control_risk_ratio =
            profile_control_risk_seconds / elapsed_seconds;
        overhead.control_risk_ratio =
            control_risk_seconds / elapsed_seconds;
        overhead.ratio =
            (overhead.profile_seconds + overhead.control_seconds) /
            elapsed_seconds;
        overhead.management_ratio =
            overhead.management_seconds / elapsed_seconds;
    }
    overhead.valid = TRUE;
    peak_general_listener_populate_report_risk_fields(&overhead);
    return overhead;
}

/* Final reporting and shutdown. */
static void
peak_general_listener_refresh_revisited_markers(
    const gulong* local_final_calls)
{
    if (array_listener_revisited == NULL) {
        return;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (!array_listener_revisited[i] &&
            local_final_calls != NULL &&
            array_listener_reattached != NULL &&
            array_listener_reattached[i] &&
            peak_hook_reattach_request_calls != NULL &&
            peak_hook_reattach_request_calls_valid != NULL &&
            peak_hook_reattach_request_calls_valid[i] &&
            local_final_calls[i] > peak_hook_reattach_request_calls[i]) {
            array_listener_revisited[i] = TRUE;
        }
    }
}

#ifdef HAVE_MPI
typedef struct {
    gboolean* previous_detached;
    gboolean* previous_reattached;
    gboolean* previous_revisited;
    gboolean* installed_detached;
    gboolean* installed_reattached;
    gboolean* installed_revisited;
} PeakReportMarkerSwap;
#endif

static PeakReportFormatOptions
peak_general_listener_report_format_options(int rank_count)
{
    const PeakReportFormatOptions options = {
        .print_text = peak_general_listener_should_print_text(
            rank_count <= 1 && peak_general_listener_mpi_env_size() > 1),
        .truncate_names = peak_truncate_function_name,
    };

    return options;
}

static void
peak_general_listener_write_report(PeakReportSnapshot* snapshot,
                                   gboolean sanitize)
{
    PeakReportFormatOptions options;

    if (snapshot == NULL) {
        return;
    }
    if (sanitize) {
        peak_report_snapshot_prepare_for_render(snapshot);
    }
    options = peak_general_listener_report_format_options(
        snapshot->rank_count);
    (void)peak_report_formatter_write_csv(snapshot);
    peak_report_formatter_write_text(snapshot, &options);
    peak_general_listener_release_report_names();
}

#ifdef HAVE_MPI
static PeakReportMarkerSwap
peak_general_listener_begin_report_marker_swap(
    const PeakReportSnapshot* source)
{
    PeakReportMarkerSwap swap = {
        .previous_detached = array_listener_detached,
        .previous_reattached = array_listener_reattached,
        .previous_revisited = array_listener_revisited,
        .installed_detached =
            g_new0(gboolean, peak_hook_address_count),
        .installed_reattached =
            g_new0(gboolean, peak_hook_address_count),
        .installed_revisited =
            g_new0(gboolean, peak_hook_address_count),
    };

    if (source != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            swap.installed_detached[i] = source->detached[i] != 0;
            swap.installed_reattached[i] = source->reattached[i] != 0;
            swap.installed_revisited[i] = source->revisited[i] != 0;
        }
    }
    array_listener_detached = swap.installed_detached;
    array_listener_reattached = swap.installed_reattached;
    array_listener_revisited = swap.installed_revisited;
    return swap;
}

static void
peak_general_listener_commit_report_marker_swap(
    PeakReportMarkerSwap* swap)
{
    if (swap == NULL) {
        return;
    }
    g_free(swap->previous_detached);
    g_free(swap->previous_reattached);
    g_free(swap->previous_revisited);
    memset(swap, 0, sizeof(*swap));
}

static void
peak_general_listener_rollback_report_marker_swap(
    PeakReportMarkerSwap* swap)
{
    if (swap == NULL) {
        return;
    }
    array_listener_detached = swap->previous_detached;
    array_listener_reattached = swap->previous_reattached;
    array_listener_revisited = swap->previous_revisited;
    g_free(swap->installed_detached);
    g_free(swap->installed_reattached);
    g_free(swap->installed_revisited);
    memset(swap, 0, sizeof(*swap));
}
#endif

#ifdef HAVE_MPI
static gboolean
peak_general_listener_run_socket_report(
    PeakReportSnapshot* local,
    PeakSocketReportRankSource rank_source)
{
    PeakSocketReportSession* session = NULL;
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportStatus status = peak_socket_report_transport_begin(
        local, rank_source, &session, &aggregate);

    if (status == PEAK_SOCKET_REPORT_SINGLE_READY) {
        peak_general_listener_write_report(aggregate, TRUE);
        peak_report_snapshot_destroy(aggregate);
        return TRUE;
    }
    if (status == PEAK_SOCKET_REPORT_PEER_RELEASED) {
        return TRUE;
    }
    if (status != PEAK_SOCKET_REPORT_ROOT_PREPARED ||
        session == NULL || aggregate == NULL) {
        peak_socket_report_transport_abort(session);
        peak_report_snapshot_destroy(aggregate);
        return FALSE;
    }

    PeakReportMarkerSwap marker_swap =
        peak_general_listener_begin_report_marker_swap(aggregate);
    PeakReportFormatOptions options =
        peak_general_listener_report_format_options(aggregate->rank_count);

    (void)peak_report_formatter_write_csv(aggregate);
    if (!peak_socket_report_transport_commit(session)) {
        (void)peak_report_formatter_remove_csv();
        peak_general_listener_rollback_report_marker_swap(&marker_swap);
        peak_report_snapshot_destroy(aggregate);
        return FALSE;
    }
    peak_report_formatter_write_text(aggregate, &options);
    peak_general_listener_release_report_names();
    peak_general_listener_commit_report_marker_swap(&marker_swap);
    peak_report_snapshot_destroy(aggregate);
    return TRUE;
}
#endif

gboolean
peak_general_listener_mpi_reducer_failed_closed(void)
{
#ifdef HAVE_MPI
    return peak_mpi_report_transport_failed_closed();
#else
    return FALSE;
#endif
}

gboolean peak_general_listener_print(PeakOutputAggregationMode aggregation_mode)
{
    gboolean used_mpi_aggregation = FALSE;
    PeakReportSnapshot* local_snapshot = NULL;

#ifdef HAVE_MPI
    peak_mpi_report_transport_reset_failed_closed();
#endif
    if (interceptor != NULL) {
        gum_interceptor_ignore_current_thread(interceptor);
    }

    gulong* sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* thread_count = g_new0(gulong, peak_hook_address_count);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            PeakGeneralListener* pg_listener =
                PEAKGENERAL_LISTENER(array_listener[i]);
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                gulong calls = peak_general_listener_num_calls_load(
                    &pg_listener->num_calls[j]);

                sum_num_calls[i] += calls;
                sum_total_time[i] += pg_listener->total_time[j];
                sum_exclusive_time[i] += pg_listener->exclusive_time[j];
                if (calls != 0) {
                    thread_count[i]++;
                    if (pg_listener->total_time[j] > max_total_time[i]) {
                        max_total_time[i] = pg_listener->total_time[j];
                    }
                    if (pg_listener->total_time[j] < min_total_time[i] ||
                        thread_count[i] == 1) {
                        min_total_time[i] = pg_listener->total_time[j];
                    }
                    if (pg_listener->max_time[j] > sum_max_time[i]) {
                        sum_max_time[i] = pg_listener->max_time[j];
                    }
                    if (pg_listener->min_time[j] < sum_min_time[i] ||
                        thread_count[i] == 1) {
                        sum_min_time[i] = pg_listener->min_time[j];
                    }
                }
            }
            if (thread_count[i] == 0) {
                min_total_time[i] = DBL_MAX;
                sum_min_time[i] = FLT_MAX;
            }
        }
    }
    peak_general_listener_refresh_revisited_markers(sum_num_calls);

    PeakReportOverhead local_report =
        peak_general_listener_local_report_overhead(sum_num_calls);
    local_snapshot = peak_general_listener_build_report_snapshot(
        sum_num_calls,
        sum_total_time,
        max_total_time,
        min_total_time,
        sum_exclusive_time,
        sum_max_time,
        sum_min_time,
        thread_count,
        1,
        &local_report);

    if (local_snapshot != NULL) {
#ifdef HAVE_MPI
        if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI) {
            PeakReportSnapshot* aggregate = NULL;
            PeakMpiReportTransportResult result =
                peak_mpi_report_transport_reduce(local_snapshot, &aggregate);

            if (result == PEAK_MPI_REPORT_TRANSPORT_ROOT_READY) {
                PeakReportMarkerSwap marker_swap =
                    peak_general_listener_begin_report_marker_swap(aggregate);

                peak_general_listener_commit_report_marker_swap(&marker_swap);
                peak_general_listener_write_report(aggregate, TRUE);
                peak_report_snapshot_destroy(aggregate);
                used_mpi_aggregation = TRUE;
            } else if (result == PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE) {
                PeakReportMarkerSwap marker_swap =
                    peak_general_listener_begin_report_marker_swap(NULL);

                peak_general_listener_commit_report_marker_swap(&marker_swap);
                used_mpi_aggregation = TRUE;
            } else if (result == PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED &&
                       peak_general_listener_socket_reduce_fallback_enabled()) {
                g_printerr("[peak] MPI reducer failed; trying PEAK-owned socket aggregation fallback without further MPI calls\n");
                if (!peak_general_listener_run_socket_report(
                        local_snapshot,
                        PEAK_SOCKET_REPORT_RANK_ENV_ONLY)) {
                    g_printerr("[peak] MPI reducer socket fallback failed; writing rank-local output\n");
                    peak_general_listener_write_report(local_snapshot, TRUE);
                }
            } else {
                peak_general_listener_write_report(local_snapshot, TRUE);
            }
        } else if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET) {
            if (!peak_general_listener_run_socket_report(
                    local_snapshot,
                    PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV) &&
                peak_general_listener_socket_reduce_fallback_enabled()) {
                g_printerr("[peak] Socket aggregation failed; falling back to rank-local output\n");
                peak_general_listener_write_report(local_snapshot, TRUE);
            }
        } else {
            peak_general_listener_write_report(local_snapshot, TRUE);
        }
#else
        (void)aggregation_mode;
        peak_general_listener_write_report(local_snapshot, TRUE);
#endif
    }

    peak_report_snapshot_destroy(local_snapshot);
    g_free(sum_num_calls);
    g_free(sum_total_time);
    g_free(max_total_time);
    g_free(min_total_time);
    g_free(sum_exclusive_time);
    g_free(sum_max_time);
    g_free(sum_min_time);
    g_free(thread_count);

    if (interceptor != NULL) {
        gum_interceptor_unignore_current_thread(interceptor);
    }
    return used_mpi_aggregation;
}

gboolean peak_general_listener_dettach()
{
    if (interceptor == NULL) {
        return TRUE;
    }

    peak_general_listener_controller_stop();

    pthread_t controller_tid = pthread_self();
    pthread_t* tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* pause_session_ids = g_new0(int, peak_max_num_threads);
    int* pause_status = g_new0(int, peak_max_num_threads);

    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            gboolean shutdown_ok = FALSE;
            unsigned int shutdown_attempts = 0;
            PeakGeneralShutdownFailure shutdown_failure = {
                .bucket = PEAK_GENERAL_SHUTDOWN_FAILURE_NONE,
                .status = PEAK_DETACH_STATUS_ERROR
            };
            double deadline = peak_second() +
                              ((double)peak_general_controller_shutdown_drain_ms() /
                               1000.0);

            do {
                gboolean snapshot_complete = FALSE;
                size_t snapshot_count =
                    pthread_listener_snapshot_threads(tid_keys,
                                                      mapped_ids,
                                                      peak_max_num_threads,
                                                      &snapshot_complete);
                for (size_t s = 0; s < snapshot_count; s++) {
                    pause_session_ids[s] = -1;
                    pause_status[s] = -1;
                }

                shutdown_attempts++;
                shutdown_ok = peak_general_controller_shutdown_hook_unlocked(
                    i,
                    controller_tid,
                    tid_keys,
                    mapped_ids,
                    pause_session_ids,
                    pause_status,
                    snapshot_count,
                    snapshot_complete,
                    &shutdown_failure);
                if (shutdown_ok) {
                    break;
                }
                if (peak_second() >= deadline) {
                    break;
                }
                usleep(1000);
            } while (TRUE);

            if (!shutdown_ok) {
                pthread_mutex_unlock(&lock);
                g_free(pause_status);
                g_free(pause_session_ids);
                g_free(mapped_ids);
                g_free(tid_keys);
                g_printerr("[peak] Gum shutdown detach was not proven safe; bucket=%s status=%s attempts=%u; leaving listener state alive\n",
                           peak_general_shutdown_failure_bucket_string(
                               &shutdown_failure),
                           peak_detach_controller_status_string(
                               shutdown_failure.status),
                           shutdown_attempts);
                return FALSE;
            }
        }
    }
    pthread_mutex_unlock(&lock);

    PeakDetachStatus helper_shutdown_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_detach_controller_shutdown_helper(&helper_shutdown_status)) {
        g_free(pause_status);
        g_free(pause_session_ids);
        g_free(mapped_ids);
        g_free(tid_keys);
        g_printerr("[peak] detach helper shutdown failed: %s; leaving listener state alive\n",
                   peak_detach_controller_status_string(helper_shutdown_status));
        return FALSE;
    }

    g_free(pause_status);
    g_free(pause_session_ids);
    g_free(mapped_ids);
    g_free(tid_keys);

    if (!peak_general_controller_flush_teardown()) {
        g_printerr("[peak] Gum detach teardown did not flush; leaving listener state alive\n");
        return FALSE;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && array_listener[i] != NULL) {
            peak_general_listener_free(PEAKGENERAL_LISTENER(array_listener[i]));
            g_object_unref(array_listener[i]);
        }
    }
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
    g_free(array_listener_revisited);
    g_free(array_listener_gum_detached);
    g_free(array_listener_gum_detach_flushed);
    g_free(peak_hook_last_detach_time);
    g_free(peak_hook_states);
    g_free(peak_hook_next_retry_time);
    g_free(peak_hook_pending_observed_time);
    g_free(peak_hook_pending_request_source);
    g_free(peak_hook_pending_request_calls);
    g_free(peak_hook_pending_request_ratio);
    g_free(peak_hook_pending_request_global_overhead);
    g_free(peak_hook_pending_request_total_time);
    g_free(peak_hook_pending_request_rate);
    g_free(peak_hook_detach_profile_seconds);
    g_free(peak_hook_reattach_request_calls);
    g_free(peak_hook_reattach_request_calls_valid);
    g_free(peak_hook_cached_sample_calls);
    g_free(peak_hook_cached_sample_profile_seconds);
    g_free(peak_hook_cached_sample_valid);
    g_free(peak_hook_retry_count);
    g_free(peak_hook_last_retry_status);
    g_free(array_listener);

    interceptor = NULL;
    hook_address = NULL;
    array_listener_detached = NULL;
    array_listener_reattached = NULL;
    array_listener_revisited = NULL;
    array_listener_gum_detached = NULL;
    array_listener_gum_detach_flushed = NULL;
    peak_hook_last_detach_time = NULL;
    peak_hook_states = NULL;
    peak_hook_next_retry_time = NULL;
    peak_hook_pending_observed_time = NULL;
    atomic_store_explicit(&peak_general_listener_runtime_start_ns,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&peak_general_listener_heartbeat_control_baseline_ns,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_count,
        0,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_failed_count,
        0,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        FALSE,
        memory_order_release);
    peak_hook_pending_request_source = NULL;
    peak_hook_pending_request_calls = NULL;
    peak_hook_pending_request_ratio = NULL;
    peak_hook_pending_request_global_overhead = NULL;
    peak_hook_pending_request_total_time = NULL;
    peak_hook_pending_request_rate = NULL;
    peak_hook_detach_profile_seconds = NULL;
    peak_hook_reattach_request_calls = NULL;
    peak_hook_reattach_request_calls_valid = NULL;
    peak_hook_cached_sample_calls = NULL;
    peak_hook_cached_sample_profile_seconds = NULL;
    peak_hook_cached_sample_valid = NULL;
    peak_hook_retry_count = NULL;
    peak_hook_last_retry_status = NULL;
    array_listener = NULL;

    return TRUE;
}
