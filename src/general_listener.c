#define _GNU_SOURCE
#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "exec_interceptor.h"
#include "peak_general_listener_internal.h"
#include "peak_jit_provider.h"
#include "peak_detach_controller.h"
#include "peak_logging.h"
#include "pthread_listener.h"
#include "unsafe_gum_prologue.h"
#include <errno.h>
#include <dirent.h>
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
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
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
static unsigned int* peak_hook_retry_count;
static PeakDetachStatus* peak_hook_last_retry_status;
static const char*
peak_hook_request_source_string(PeakHookRequestSource source);
static gboolean
peak_general_listener_startup_attach_can_skip_stop(void);
extern gboolean* peak_need_detach;
extern gboolean* peak_detached;
extern gdouble* heartbeat_overhead;
extern gboolean** peak_target_thread_called;
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
static pthread_once_t general_listener_atfork_once = PTHREAD_ONCE_INIT;
static gboolean general_controller_running = FALSE;
static gboolean general_controller_thread_started = FALSE;
static pthread_t general_controller_owner_thread;
static _Atomic gboolean general_controller_owner_known = FALSE;
static _Atomic gboolean general_listener_fork_child_minimal_callbacks = FALSE;
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
static gsize peak_attach_policy_initialized = 0;
static gboolean peak_allow_unsafe_gum_prologue = FALSE;
static PeakUnsafeGumProloguePolicy peak_unsafe_gum_prologue_policy =
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
static gboolean peak_dynamic_attach_needed = FALSE;
static char peak_exec_checkpoint_base[PATH_MAX] = "./peak_statslog";
static gboolean peak_exec_checkpoint_base_overlong = FALSE;
#define PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES 64U

void
peak_general_listener_after_fork_child(void)
{
    /*
     * A fork child inherits only the calling thread. Reset controller locks and
     * thread bookkeeping so a failed exec can continue to profile and finalize
     * without joining or waiting on vanished parent threads.
     */
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&general_controller_wake_mutex, NULL);
    pthread_cond_init(&general_controller_wake_cond, NULL);
    general_controller_running = FALSE;
    general_controller_thread_started = FALSE;
    atomic_store(&general_listener_fork_child_minimal_callbacks, TRUE);
    atomic_store(&general_controller_owner_known, FALSE);
    atomic_store(&peak_general_callbacks_suspended, FALSE);
    atomic_store(&peak_general_mpi_finalize_requested, FALSE);
    pthread_listener_atfork_child();
}

void
peak_general_listener_after_raw_fork_child(void)
{
    /*
     * Raw SYS_fork/SYS_clone children bypass pthread_atfork() and may resume
     * from PEAK's syscall interposer while inherited pthread/GLib/Gum state is
     * not safe to reinitialize. Keep this path to atomics and plain stores.
     * The exec wrappers will use try-lock checkpointing, and callbacks will use
     * the minimal fork-child accounting path.
     */
    general_controller_running = FALSE;
    general_controller_thread_started = FALSE;
    atomic_store(&general_listener_fork_child_minimal_callbacks, TRUE);
    atomic_store(&general_controller_owner_known, FALSE);
    atomic_store(&peak_general_callbacks_suspended, FALSE);
    atomic_store(&peak_general_mpi_finalize_requested, FALSE);
}

static void
peak_general_listener_register_atfork(void)
{
    (void)pthread_atfork(NULL, NULL, peak_general_listener_after_fork_child);
}

static gboolean
peak_general_listener_parse_detach_count_override(gulong* count_out)
{
    const char* value = getenv("PEAK_DETACH_COUNT");

    if (value == NULL || value[0] == '\0') {
        return FALSE;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);

    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0) {
        peak_log_info("[peak] ignoring invalid PEAK_DETACH_COUNT=%s\n", value);
        return FALSE;
    }

    if (count_out != NULL) {
        *count_out = (gulong)parsed;
    }
    return TRUE;
}

static unsigned int
peak_general_listener_parse_uint_env_default(const char* name,
                                             unsigned int default_value)
{
    const char* value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed > G_MAXUINT) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (unsigned int)parsed;
}

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
peak_env_value_falsey(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "0") == 0 ||
            g_ascii_strcasecmp(value, "false") == 0 ||
            g_ascii_strcasecmp(value, "off") == 0 ||
            g_ascii_strcasecmp(value, "no") == 0);
}

static void
peak_general_listener_init_attach_policy_once(void)
{
    gboolean policy_valid = FALSE;

    peak_allow_unsafe_gum_prologue =
        peak_env_value_truthy(g_getenv(PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV));

    peak_unsafe_gum_prologue_policy =
        peak_unsafe_gum_prologue_policy_from_env(
            g_getenv(PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV),
            &policy_valid);
    if (!policy_valid) {
        peak_log_info("[peak] ignoring invalid %s=%s; using %s policy\n",
                   PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV,
                   g_getenv(PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV),
                   peak_unsafe_gum_prologue_policy_name(
                       PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT));
        peak_unsafe_gum_prologue_policy =
            PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
    }
}

static void
peak_general_listener_init_attach_policy(void)
{
    if (g_once_init_enter(&peak_attach_policy_initialized)) {
        peak_general_listener_init_attach_policy_once();
        g_once_init_leave(&peak_attach_policy_initialized, 1);
    }
}

gboolean
peak_general_listener_attach_target_is_supported(const char* symbol_name,
                                                 gpointer address)
{
    const char* reason = NULL;

    peak_general_listener_init_attach_policy();

    if (peak_allow_unsafe_gum_prologue) {
        return TRUE;
    }

    if (peak_gum_prologue_too_short_for_attach(address, &reason)) {
        g_printerr("[peak] skipping Gum attach for hook %s: target prologue is too small for Gum entry patch (reason=%s); target will remain unprofiled\n",
                   symbol_name != NULL ? symbol_name : "<unknown>",
                   reason != NULL ? reason : "unknown");
        return FALSE;
    }

    if (peak_unsafe_gum_prologue_check(address,
                                       peak_unsafe_gum_prologue_policy,
                                       &reason)) {
        g_printerr("[peak] skipping Gum attach for hook %s: target prologue is not safe for Gum relocation (reason=%s, policy=%s); set %s=1 to override\n",
                   symbol_name != NULL ? symbol_name : "<unknown>",
                   reason != NULL ? reason : "unknown",
                   peak_unsafe_gum_prologue_policy_name(
                       peak_unsafe_gum_prologue_policy),
                   PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV);
        return FALSE;
    }

    return TRUE;
}

gboolean
peak_general_listener_support_attach_target_is_supported(const char* symbol_name,
                                                         gpointer address)
{
    peak_general_listener_init_attach_policy();

    (void)symbol_name;
    (void)address;
    return TRUE;
}

static gboolean
peak_general_listener_startup_attach_can_skip_stop(void)
{
    DIR* dir = opendir("/proc/self/task");
    struct dirent* entry;
    unsigned int task_count = 0;

    if (dir == NULL) {
        return FALSE;
    }

    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;

        if (name[0] == '.') {
            continue;
        }
        task_count++;
        if (task_count > 1) {
            closedir(dir);
            return FALSE;
        }
    }

    closedir(dir);
    return task_count == 1;
}

gboolean
peak_general_listener_needs_dynamic_attach(void)
{
    return peak_dynamic_attach_needed;
}

static gboolean
peak_socket_reduce_fallback_enabled(void)
{
    const char* value =
        getenv(PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV);

    if (value == NULL || value[0] == '\0') {
        return TRUE;
    }
    if (peak_env_value_falsey(value)) {
        return FALSE;
    }
    if (peak_env_value_truthy(value)) {
        return TRUE;
    }

    peak_log_info("[peak] Ignoring invalid %s=%s; using fallback-to-local behavior\n",
               PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV,
               value);
    return TRUE;
}

static void
peak_general_listener_mark_mpi_reducer_failed_closed(void)
{
    atomic_store_explicit(&peak_general_mpi_reducer_failed_closed,
                          TRUE,
                          memory_order_release);
}

gboolean
peak_general_listener_mpi_reducer_failed_closed(void)
{
    return atomic_load_explicit(&peak_general_mpi_reducer_failed_closed,
                                memory_order_acquire);
}

static long
peak_parse_long_env(const char* name)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || parsed < 0) {
        return -1;
    }

    return parsed;
}

static long
peak_mpi_env_size(void)
{
    static const char* names[] = {
        "PMI_SIZE",
        "PMIX_SIZE",
        "OMPI_COMM_WORLD_SIZE",
        "SLURM_NTASKS",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        long value = peak_parse_long_env(*name);
        if (value > 0) {
            return value;
        }
    }

    return -1;
}

static long
peak_mpi_env_rank(void)
{
    static const char* names[] = {
        "PMI_RANK",
        "PMIX_RANK",
        "OMPI_COMM_WORLD_RANK",
        "MV2_COMM_WORLD_RANK",
        "SLURM_PROCID",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        long value = peak_parse_long_env(*name);
        if (value >= 0) {
            return value;
        }
    }

    return -1;
}

static gboolean
peak_general_listener_should_print_text(gboolean rank_local_mpi_output)
{
    const char* value = getenv(PEAK_TEXT_OUTPUT_ENV);

    if (value != NULL && value[0] != '\0') {
        return peak_env_value_truthy(value);
    }

    return !rank_local_mpi_output;
}

typedef struct {
    size_t hook_id;
    GumInvocationListener* listener;
    PeakDetachOperation operation;
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

    pthread_mutex_lock(&detach_trace_mutex);
    fp = fopen(path, "a");
    if (fp != NULL) {
        fprintf(fp,
                "%.9f,%lu,%s,%s,%s,%d,%s,%u,%.9f,%u,%.3f,%u,%s,%s,%ld,0x%llx,0x%llx,%s,%lu,%.9f,%.9f,%.9f,%.9f\n",
                peak_second(),
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
                request_rate);
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
                                                  peak_detach_controller_last_stop_window_us(),
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
static __thread int general_listener_fork_child_callback_depth;

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

    // Prepare sigset
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);   

    //Register signal handlers
    //We now use sigaction() instead of signal(), because it supports SA_RESTART
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
    //Prepare sigset
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    //UnBlock signals
    pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
}

void pthread_pause_disable()
{
    //This is important for when you want to do some signal unsafe stuff
    //Eg.: locking mutex, calling printf() which has internal mutex, etc...
    //After unlocking mutex, you can enable pause again.
    pthread_pause_init();

    //Block signals
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
    return peak_general_listener_request_reattach_with_context_unlocked(
        hook_id,
        PEAK_HOOK_REQUEST_SOURCE_API,
        0,
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

size_t peak_general_listener_request_detach_batch(const size_t* hook_ids,
                                                  size_t hook_count)
{
    size_t accepted = 0;

    if (hook_ids == NULL || hook_count == 0) {
        return 0;
    }

    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < hook_count; i++) {
        if (peak_general_listener_request_detach_unlocked(hook_ids[i])) {
            accepted++;
        }
    }
    pthread_mutex_unlock(&lock);

    if (accepted > 0) {
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

        PeakDetachRequest mutation_request = {
            .hook_id = i,
            .symbol_name = peak_hook_strings[i],
            .function_address = symbol_address,
            .interceptor = interceptor,
            .listener = new_listener,
            .operation = PEAK_DETACH_OPERATION_ATTACH
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
            gum_interceptor_attach(interceptor, symbol_address, new_listener, NULL);
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
            peak_detach_controller_last_stop_window_us(),
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

    {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_REATTACH,
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
            gum_interceptor_attach(interceptor, hook_address[hook_id], array_listener[hook_id], NULL);
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
    stop_window_us = peak_detach_controller_last_stop_window_us();

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
                    gum_interceptor_attach(interceptor,
                                           hook_address[hook_id],
                                           array_listener[hook_id],
                                           NULL);
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

static void
peak_general_controller_start(void)
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

typedef struct {
    size_t index;
    double ratio;  // hard-gate ratio
    double rate;   // diagnostic trend, and reattach tie-breaking
} OverheadEntry;

// ratio descending (global detach)
static int compare_ratio_de(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->ratio < y->ratio) return 1;
    if (x->ratio > y->ratio) return -1;

    // tie-break: faster-growing overhead first
    if (x->rate < y->rate) return 1;
    if (x->rate > y->rate) return -1;
    return 0;
}

// rate ascending (global reattach)
static int compare_rate_inc(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->rate < y->rate) return -1;
    if (x->rate > y->rate) return 1;

    // tie-break: smaller ratio first
    if (x->ratio < y->ratio) return -1;
    if (x->ratio > y->ratio) return 1;
    return 0;
}

static inline double clipd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
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
    unsigned int heartbeat_time = heartbeat_args->heartbeat_time;   // base sleep (us)
    unsigned int check_interval = heartbeat_args->check_interval;
    unsigned int hb_min_us = heartbeat_args->hb_min_us;
    unsigned int hb_max_us = heartbeat_args->hb_max_us;
    double hb_k_err = heartbeat_args->hb_k_err;
    double hb_k_rate = heartbeat_args->hb_k_rate;
    double hb_ema_a = heartbeat_args->hb_ema_a;
    unsigned int heartbeat_counter = 0;

    gum_interceptor_ignore_current_thread(interceptor);

    OverheadEntry* entries = g_new0(OverheadEntry, peak_hook_address_count);

    gulong* calls_snapshot = g_new0(gulong, peak_hook_address_count);
    double* ratio_snapshot = g_new0(double, peak_hook_address_count);
    double* rate_snapshot  = g_new0(double, peak_hook_address_count);
    double* prev_ratio     = g_new0(double, peak_hook_address_count);
    double* prev_time      = g_new0(double, peak_hook_address_count);

    double now0 = peak_second();
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        calls_snapshot[i] = 0;
        ratio_snapshot[i] = 0.0;
        rate_snapshot[i]  = 0.0;
        prev_ratio[i]     = 0.0;
        prev_time[i]      = now0;
    }

    // ------------------------------
    // Global dynamics state
    // ------------------------------
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
            ratio_snapshot = g_renew(double, ratio_snapshot, current_hook_count);
            rate_snapshot = g_renew(double, rate_snapshot, current_hook_count);
            prev_ratio = g_renew(double, prev_ratio, current_hook_count);
            prev_time = g_renew(double, prev_time, current_hook_count);
            for (size_t i = old_capacity; i < current_hook_count; i++) {
                calls_snapshot[i] = 0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                prev_ratio[i] = 0.0;
                prev_time[i] = now;
            }
            heartbeat_capacity = current_hook_count;
        }

        double total_execution_time = now - peak_main_time;
        if (total_execution_time <= 0.0) total_execution_time = 1e-12;
        gboolean detach_observation_ready =
            total_execution_time >= min_detach_observation_time;

        double global_overhead = 0.0;

        pthread_mutex_lock(&lock);
        for (size_t i = 0; i < heartbeat_capacity; i++) {
            if (!(hook_address[i] && array_listener[i])) {
                calls_snapshot[i] = 0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i]  = 0.0;
                prev_ratio[i]     = 0.0;
                prev_time[i]      = now;
                continue;
            }

            PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);

            gulong total_num_calls = 0;
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                total_num_calls += pg_listener->num_calls[j];
            }

            calls_snapshot[i] = total_num_calls;
            double ratio =
                (total_num_calls * peak_general_overhead + heartbeat_overhead[i]) / total_execution_time;

            ratio_snapshot[i] = ratio;
            // g_printerr ("ratio %ld: %ld\n", i, total_num_calls);

            double dt = now - prev_time[i];
            if (dt <= 1e-12) dt = 1e-12;
            rate_snapshot[i] = (ratio - prev_ratio[i]) / dt;

            prev_ratio[i] = ratio;
            prev_time[i]  = now;

            if (!peak_detached[i]) {
                global_overhead += ratio;
            }
        }
        pthread_mutex_unlock(&lock);

        // g_printerr ("global_overhead %.3e\n", global_overhead);

        // ------------------------------------------------------------
        // 1) Per-target DETACH
        // ------------------------------------------------------------
        if (detach_observation_ready && enable_per_target_heartbeat) {
            pthread_mutex_lock(&lock);
            for (size_t i = 0; i < heartbeat_capacity; i++) {
                if (!(hook_address[i] && array_listener[i])) continue;
                if (peak_detached[i]) continue;

                if (ratio_snapshot[i] > target_profile_ratio) {
                    wake_controller |=
                        peak_general_listener_request_detach_with_context_unlocked(
                            i,
                            PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT,
                            calls_snapshot[i],
                            ratio_snapshot[i],
                            global_overhead,
                            total_execution_time,
                            rate_snapshot[i]);
                }
            }
             pthread_mutex_unlock(&lock);
        }

        // ------------------------------------------------------------
        // 2) Global DETACH
        // ------------------------------------------------------------
        if (detach_observation_ready && enable_global_heartbeat) {
            if (global_overhead > global_target_ratio * peak_global_detach_factor) {
                size_t n_attached = 0;
                pthread_mutex_lock(&lock);
                for (size_t i = 0; i < heartbeat_capacity; i++) {
                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (peak_detached[i]) continue;
                    if (calls_snapshot[i] < PEAK_GLOBAL_DETACH_MIN_CALLS) continue;
                    if (ratio_snapshot[i] <= global_target_ratio) continue;

                    entries[n_attached].index = i;
                    entries[n_attached].ratio = ratio_snapshot[i];
                    entries[n_attached].rate  = rate_snapshot[i];
                    n_attached++;
                }

                if (n_attached > 1) {
                    qsort(entries,
                          n_attached,
                          sizeof(OverheadEntry),
                          compare_ratio_de);
                }

                double reduced = global_overhead;
                for (size_t k = 0; k < n_attached && reduced > global_target_ratio; k++) {
                    size_t idx = entries[k].index;

                    if (!(hook_address[idx] && array_listener[idx])) continue;
                    if (peak_detached[idx]) continue;
                    if (entries[k].ratio <= global_target_ratio) break;

                    reduced -= entries[k].ratio;
                    wake_controller |=
                        peak_general_listener_request_detach_with_context_unlocked(
                            idx,
                            PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT,
                            calls_snapshot[idx],
                            ratio_snapshot[idx],
                            global_overhead,
                            total_execution_time,
                            rate_snapshot[idx]);
                }
                pthread_mutex_unlock(&lock);
            }
        }

        // ------------------------------------------------------------
        // 3) Reattach
        // ------------------------------------------------------------
        if (detach_observation_ready &&
            enable_reattach &&
            check_interval != 0 &&
            (heartbeat_counter % check_interval) == 0) {
            // Per-target REATTACH
            if (enable_per_target_heartbeat) {
                for (size_t i = 0; i < heartbeat_capacity; i++) {
                    pthread_mutex_lock(&lock);
                    gboolean should_consider =
                        (hook_address[i] && array_listener[i] &&
                         peak_detached[i] && !peak_need_detach[i] &&
                         peak_general_listener_reattach_cooldown_ready_unlocked(i,
                                                                                now));
                    pthread_mutex_unlock(&lock);
                    if (!should_consider) continue;

                    if (ratio_snapshot[i] <= target_profile_ratio) {
                        pthread_mutex_lock(&lock);
                        wake_controller |=
                            peak_general_listener_request_reattach_with_context_unlocked(
                                i,
                                PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT,
                                calls_snapshot[i],
                                ratio_snapshot[i],
                                global_overhead,
                                total_execution_time,
                                rate_snapshot[i]);
                        pthread_mutex_unlock(&lock);
                    }
                }
            }
            // Global REATTACH
            if (enable_global_heartbeat) {
                if (global_overhead <= peak_global_reattach_factor * global_target_ratio) {
                    size_t detached_cnt = 0;
                    pthread_mutex_lock(&lock);
                    for (size_t i = 0; i < heartbeat_capacity; i++) {
                        if (!(hook_address[i] && array_listener[i])) continue;
                        if (!peak_detached[i]) continue;
                        if (peak_need_detach[i]) continue;
                        if (!peak_general_listener_reattach_cooldown_ready_unlocked(
                                i,
                                now)) {
                            continue;
                        }

                        entries[detached_cnt].index = i;
                        entries[detached_cnt].ratio = ratio_snapshot[i];
                        entries[detached_cnt].rate  = rate_snapshot[i];
                        detached_cnt++;
                    }
                    pthread_mutex_unlock(&lock);

                    if (detached_cnt > 1) {
                        qsort(entries, detached_cnt, sizeof(OverheadEntry), compare_rate_inc);
                    }

                    for (size_t k = 0; k < detached_cnt; k++) {
                        size_t i = entries[k].index;

                        pthread_mutex_lock(&lock);
                        gboolean still_detached =
                            hook_address[i] && array_listener[i] &&
                            peak_detached[i] && !peak_need_detach[i];
                        pthread_mutex_unlock(&lock);
                        if (!still_detached) continue;

                        if (global_overhead + entries[k].ratio > global_target_ratio) {
                            break;
                        }

                        pthread_mutex_lock(&lock);
                        wake_controller |=
                            peak_general_listener_request_reattach_with_context_unlocked(
                                i,
                                PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT,
                                calls_snapshot[i],
                                ratio_snapshot[i],
                                global_overhead,
                                total_execution_time,
                                rate_snapshot[i]);
                        pthread_mutex_unlock(&lock);
                        global_overhead += entries[k].ratio;

                        if (global_overhead > peak_global_reattach_factor * global_target_ratio) {
                            break;
                        }
                    }
                }
            }
        }

        if (wake_controller) {
            peak_general_listener_controller_wake();
        }
        
        // ------------------------------------------------------------
        // Adaptive heartbeat sleep
        // ------------------------------------------------------------
        double gdt = now - prev_global_time;
        if (gdt <= 1e-12) gdt = 1e-12;

        double global_rate = (global_overhead - prev_global_overhead) / gdt;
        ema_global_rate = hb_ema_a * global_rate + (1.0 - hb_ema_a) * ema_global_rate;

        prev_global_overhead = global_overhead;
        prev_global_time     = now;

        // error: how much we exceed global target (normalized)
        double err = (global_target_ratio > 0.0) ? (global_overhead / global_target_ratio - 1.0) : 0.0;
        if (err < 0.0) err = 0.0;

        // // only care positive growth (shrinking shouldn't speed up)
        // double pos_rate = (ema_global_rate > 0.0) ? ema_global_rate : 0.0;

        // scale factor: faster when err/rate bigger
        double scale = 1.0 / (1.0 + hb_k_err * err + hb_k_rate * ema_global_rate);

        long long sleep_us = (long long)(clipd((double)heartbeat_time * scale,
                                       (double)hb_min_us,
                                       (double)hb_max_us) + 0.5);

        if (!peak_heartbeat_wait_us((unsigned int)sleep_us)) {
            break;
        }
    }

cleanup:
    g_free(prev_time);
    g_free(prev_ratio);
    g_free(rate_snapshot);
    g_free(ratio_snapshot);
    g_free(calls_snapshot);
    g_free(entries);

    gum_interceptor_unignore_current_thread(interceptor);
    return NULL;
}

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
peak_general_listener_on_enter_fork_child(GumInvocationListener* listener,
                                          GumInvocationContext* ic)
{
    PeakInvocationData* priv =
        GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);

    priv->initialized = FALSE;
    priv->stack_level = 0;
    if (listener == NULL ||
        atomic_load_explicit(&peak_general_callbacks_suspended,
                             memory_order_acquire) ||
        general_listener_fork_child_callback_depth > 0) {
        return;
    }

    general_listener_fork_child_callback_depth++;
    PeakGeneralListener* self = (PeakGeneralListener*)listener;
    if (self->num_calls == NULL) {
        general_listener_fork_child_callback_depth--;
        return;
    }
    self->num_calls[0]++;
    priv->start_time = peak_second();
    priv->stack_level = 1;
    priv->initialized = TRUE;
}

static void
peak_general_listener_on_leave_fork_child(GumInvocationListener* listener,
                                          GumInvocationContext* ic)
{
    PeakInvocationData* priv =
        GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);

    if (listener == NULL || priv == NULL || !priv->initialized) {
        return;
    }

    double duration = peak_second() - priv->start_time;
    PeakGeneralListener* self = (PeakGeneralListener*)listener;
    if (self->total_time == NULL || self->exclusive_time == NULL ||
        self->max_time == NULL || self->min_time == NULL) {
        priv->initialized = FALSE;
        priv->stack_level = 0;
        if (general_listener_fork_child_callback_depth > 0) {
            general_listener_fork_child_callback_depth--;
        }
        return;
    }
    if (duration > self->max_time[0]) {
        self->max_time[0] = duration;
    }
    if (duration < self->min_time[0] || self->num_calls[0] == 1) {
        self->min_time[0] = duration;
    }
    self->total_time[0] += duration;
    self->exclusive_time[0] += duration;
    priv->initialized = FALSE;
    priv->stack_level = 0;
    if (general_listener_fork_child_callback_depth > 0) {
        general_listener_fork_child_callback_depth--;
    }
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (atomic_load_explicit(&general_listener_fork_child_minimal_callbacks,
                             memory_order_acquire)) {
        peak_general_listener_on_enter_fork_child(listener, ic);
        return;
    }

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
        // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
        // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
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
        self->num_calls[index]++;
        // g_printerr ("hook_id %lu time %f count %lu\n", hook_id, *current_time, self->num_calls[mapped_tid]);
    } else {
        // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
        // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
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
        self->num_calls[index]++;
        size_t hook_id = self->hook_id;
        if (self->target_thread_called != NULL) {
            self->target_thread_called[index] = TRUE;
        }
        gboolean detach_requested = FALSE;

        pthread_mutex_lock(&lock);
        if (self->num_calls[index] >= peak_detach_count) {
            gulong total_num_calls = 0;
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                total_num_calls += self->num_calls[j];
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
        // gum_interceptor_revert(interceptor, hook_address[hook_id]);
        // g_printerr ("revert hook_id %lu %p\n", hook_id, hook_address[hook_id]);

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
    if (atomic_load_explicit(&general_listener_fork_child_minimal_callbacks,
                             memory_order_acquire)) {
        peak_general_listener_on_leave_fork_child(listener, ic);
        return;
    }

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
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        // size_t hook_id = state->hook_id;
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
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] +=
            peak_general_listener_exclusive_duration(
                end_time,
                child_duration);
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            g_free(tmp_ptr);
        }
    } else {
        pthread_pause_enable();
        // g_printerr("pthread_pause_enable\n");
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
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        // size_t hook_id = state->hook_id;
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
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] +=
            peak_general_listener_exclusive_duration(
                end_time,
                child_duration);
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
    self->target_thread_called = g_new0(gboolean, total_count);
    // g_print ("total count %lu self->num_calls %lu\n", total_count, self->num_calls[0]);
}

void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->exclusive_time);
    g_free(self->max_time);
    g_free(self->min_time);
    g_free(self->target_thread_called);
    self->target_thread_called = NULL;
}

__attribute__((noinline)) static void peak_general_overhead_dummy_func()
{
    struct timespec ts = { 0, 1 }; // Sleep for 1 nanosecond
    nanosleep(&ts, NULL);
}

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

    // g_printerr("%10lu times  %10.3f s total  %10.3e s max  %10.3e s min \n",
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->num_calls[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->total_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->max_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->min_time[0]);
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

    // g_printerr("orig %.6e time %.6e\n", orig_time, time);
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
    (void)pthread_once(&general_listener_atfork_once,
                       peak_general_listener_register_atfork);
    const char* statslog_path = getenv("PEAK_STATSLOG_PATH");
    const char* checkpoint_base =
        (statslog_path != NULL && statslog_path[0] != '\0') ?
            statslog_path : "./peak_statslog";
    size_t checkpoint_base_len = strlen(checkpoint_base);
    peak_exec_checkpoint_base_overlong = checkpoint_base_len >=
        sizeof(peak_exec_checkpoint_base);
    if (checkpoint_base_len >= sizeof(peak_exec_checkpoint_base)) {
        checkpoint_base_len = sizeof(peak_exec_checkpoint_base) - 1;
    }
    memcpy(peak_exec_checkpoint_base, checkpoint_base, checkpoint_base_len);
    peak_exec_checkpoint_base[checkpoint_base_len] = '\0';

    peak_general_controller_init_trace_config();
    peak_general_listener_init_attach_policy();
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_reattached = g_new0(gboolean, peak_hook_address_count);
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
    peak_hook_retry_count = g_new0(unsigned int, peak_hook_address_count);
    peak_hook_last_retry_status = g_new0(PeakDetachStatus, peak_hook_address_count);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    peak_demangled_strings = g_new0(char*, peak_hook_address_count);
    gboolean startup_attach_can_skip_stop =
        peak_general_listener_startup_attach_can_skip_stop();
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        // replace certain function we are capturing already.
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
            // C++ API template versions, also use cudaLaunchKernelExC internal
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
            peak_log_info("[peak] skipping target dlopen: PEAK owns the dlopen wrapper used for dynamic attach\n");
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
                            // Candidate is demangled name and it will only match one symbol, so we can directly use it
                            hook_address[i] = g_ptr_array_index(candidates, 0);
                            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
                        } else {
                            // Candidates might be in short names mapping, try looking up in short mapping
                            // if multiple candidates exist, we will use the first one
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
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);
            gpointer resolved_hook_address = hook_address[i];
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
            PeakDetachRequest mutation_request = {
                .hook_id = i,
                .symbol_name = peak_hook_strings[i],
                .function_address = resolved_hook_address,
                .interceptor = interceptor,
                .listener = new_listener,
                .operation = PEAK_DETACH_OPERATION_ATTACH
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
                gum_interceptor_attach(interceptor,
                                       resolved_hook_address,
                                       new_listener,
                                       NULL);
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
    peak_general_controller_start();
}

static char*
peak_stats_csv_path_with_suffix(const char* suffix)
{
    const char* env_path = getenv("PEAK_STATSLOG_PATH");
    const char* base =
        (env_path != NULL && env_path[0] != '\0') ? env_path : "./peak_statslog";
    int pid = (int)getpid();

    return g_strdup_printf("%s-p%d%s.csv",
                           base,
                           pid,
                           suffix != NULL ? suffix : "");
}

static char*
peak_stats_csv_path(void)
{
    return peak_stats_csv_path_with_suffix("");
}

static int
peak_stats_exec_checkpoint_path(char* buffer,
                                size_t buffer_size,
                                unsigned long long checkpoint_index)
{
    if (buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (peak_exec_checkpoint_base_overlong) {
        errno = ENAMETOOLONG;
        return -1;
    }

    size_t out = 0;
    const char* base =
        peak_exec_checkpoint_base[0] != '\0' ?
            peak_exec_checkpoint_base : "./peak_statslog";
    while (base[out] != '\0' && out + 1 < buffer_size) {
        buffer[out] = base[out];
        out++;
    }
    if (base[out] != '\0') {
        errno = ENAMETOOLONG;
        return -1;
    }

#define PEAK_APPEND_LITERAL(lit) do { \
        const char* _s = (lit); \
        while (*_s != '\0') { \
            if (out + 1 >= buffer_size) { \
                errno = ENAMETOOLONG; \
                return -1; \
            } \
            buffer[out++] = *_s++; \
        } \
    } while (0)
#define PEAK_APPEND_U64(value_expr) do { \
        unsigned long long _value = (unsigned long long)(value_expr); \
        char _digits[32]; \
        size_t _n = 0; \
        do { \
            _digits[_n++] = (char)('0' + (_value % 10ULL)); \
            _value /= 10ULL; \
        } while (_value != 0ULL); \
        while (_n != 0) { \
            if (out + 1 >= buffer_size) { \
                errno = ENAMETOOLONG; \
                return -1; \
            } \
            buffer[out++] = _digits[--_n]; \
        } \
    } while (0)

    PEAK_APPEND_LITERAL("-p");
    PEAK_APPEND_U64((unsigned long long)getpid());
    PEAK_APPEND_LITERAL("-exec");
    PEAK_APPEND_U64(checkpoint_index);
    PEAK_APPEND_LITERAL(".csv");
    buffer[out] = '\0';

#undef PEAK_APPEND_U64
#undef PEAK_APPEND_LITERAL
    return 0;
}

static void
peak_stats_csv_unlink(void)
{
    char* out_csv = peak_stats_csv_path();
    (void)unlink(out_csv);
    g_free(out_csv);
}

static FILE* peak_stats_csv_open_path(const char* out_csv) {
    FILE* fp = fopen(out_csv, "w");
    if (!fp) {
        g_printerr("[peak] failed to open stats csv '%s': %s\n", out_csv, strerror(errno));
        return NULL;
    }

    fprintf(fp,
            "function,"
            "count,per_thread,per_rank,call_max_s,call_min_s,"
            "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n");
    return fp;
}

static FILE* peak_stats_csv_open(void) {
    char* out_csv = peak_stats_csv_path();
    FILE* fp = peak_stats_csv_open_path(out_csv);
    g_free(out_csv);
    return fp;
}

static gboolean peak_general_listener_snapshot_lock(gboolean try_only);

static gboolean
peak_general_listener_export_csv_result_to_path(
    const char* out_csv,
    const char* const* function_names,
    const gboolean* function_active,
    size_t function_count,
    gulong* sum_num_calls,
    gdouble* sum_total_time,
    gdouble* max_total_time,
    gdouble* min_total_time,
    gdouble* sum_exclusive_time,
    gfloat* sum_max_time,
    gfloat* sum_min_time,
    gulong* thread_count,
    const int rank_count,
    gboolean write_empty)
{
    gboolean have_output = FALSE;
    for (size_t i = 0; i < function_count; i++) {
        if (function_active[i] && sum_num_calls[i] != 0) {
            have_output = TRUE;
            break;
        }
    }

    if (!have_output && !write_empty) {
        return FALSE;
    }

    FILE* csv = out_csv != NULL ? peak_stats_csv_open_path(out_csv)
                                : peak_stats_csv_open();

    if (csv) {
        for (size_t i = 0; i < function_count; i++) {
            if (function_active[i] && sum_num_calls[i] != 0) {
                fprintf(csv, "\"%s\",%lu,%lu,%lu,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e\n",
                    function_names[i] != NULL ? function_names[i] : "",
                    (unsigned long)sum_num_calls[i],
                    (unsigned long)sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                    (unsigned long)sum_num_calls[i] / rank_count,
                    (double)sum_max_time[i],
                    (double)sum_min_time[i],
                    (double)sum_total_time[i],
                    (double)sum_exclusive_time[i],
                    (double)max_total_time[i],
                    (double)min_total_time[i],
                    (double)(sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0)) * peak_general_overhead);
            }
        }
        fclose(csv);
        return TRUE;
    }

    return FALSE;
}

static gboolean
peak_general_listener_export_csv_result(gulong* sum_num_calls,
    gdouble* sum_total_time,
    gdouble* max_total_time,
    gdouble* min_total_time,
    gdouble* sum_exclusive_time,
    gfloat* sum_max_time,
    gfloat* sum_min_time,
    gulong* thread_count,
    const int rank_count)
{
    gboolean* function_active = g_new0(gboolean, peak_hook_address_count);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        function_active[i] = hook_address[i] != NULL;
    }

    gboolean wrote =
        peak_general_listener_export_csv_result_to_path(NULL,
                                                        (const char* const*)peak_demangled_strings,
                                                        function_active,
                                                        peak_hook_address_count,
                                                        sum_num_calls,
                                                        sum_total_time,
                                                        max_total_time,
                                                        min_total_time,
                                                        sum_exclusive_time,
                                                        sum_max_time,
                                                        sum_min_time,
                                                        thread_count,
                                                        rank_count,
                                                        FALSE);
    g_free(function_active);
    return wrote;
}

static gboolean
peak_checkpoint_raw_syscall_supported(void)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return TRUE;
#else
    return FALSE;
#endif
}

static long
peak_checkpoint_raw_syscall6(long number,
                             long arg1,
                             long arg2,
                             long arg3,
                             long arg4,
                             long arg5,
                             long arg6)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return peak_exec_call_raw_syscall6(number,
                                       arg1,
                                       arg2,
                                       arg3,
                                       arg4,
                                       arg5,
                                       arg6);
#else
    (void)number;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)arg6;
    errno = ENOSYS;
    return -1;
#endif
}

static int
peak_checkpoint_open_exclusive(const char* path)
{
#if defined(__linux__) && defined(SYS_openat)
    if (peak_checkpoint_raw_syscall_supported()) {
        return (int)peak_checkpoint_raw_syscall6(SYS_openat,
                                                 AT_FDCWD,
                                                 (long)path,
                                                 O_WRONLY | O_CREAT |
                                                     O_EXCL | O_CLOEXEC,
                                                 0600,
                                                 0,
                                                 0);
    }
#endif
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
}

static gboolean
peak_checkpoint_close_fd(int fd)
{
#if defined(__linux__) && defined(SYS_close)
    if (peak_checkpoint_raw_syscall_supported()) {
        return peak_checkpoint_raw_syscall6(SYS_close,
                                            fd,
                                            0,
                                            0,
                                            0,
                                            0,
                                            0) == 0;
    }
#endif
    return close(fd) == 0;
}

static void
peak_checkpoint_unlink_path(const char* path)
{
#if defined(__linux__) && defined(SYS_unlinkat)
    if (peak_checkpoint_raw_syscall_supported()) {
        (void)peak_checkpoint_raw_syscall6(SYS_unlinkat,
                                           AT_FDCWD,
                                           (long)path,
                                           0,
                                           0,
                                           0,
                                           0);
        return;
    }
#elif defined(__linux__) && defined(SYS_unlink)
    if (peak_checkpoint_raw_syscall_supported()) {
        (void)peak_checkpoint_raw_syscall6(SYS_unlink,
                                           (long)path,
                                           0,
                                           0,
                                           0,
                                           0,
                                           0);
        return;
    }
#endif
    (void)unlink(path);
}

static gboolean
peak_checkpoint_write_all(int fd, const char* data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t rc;
#if defined(__linux__) && defined(SYS_write)
        if (peak_checkpoint_raw_syscall_supported()) {
            rc = (ssize_t)peak_checkpoint_raw_syscall6(SYS_write,
                                                       fd,
                                                       (long)(data + written),
                                                       (long)(length - written),
                                                       0,
                                                       0,
                                                       0);
        } else
#endif
        {
            rc = write(fd, data + written, length - written);
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (rc == 0) {
            errno = EIO;
            return FALSE;
        }
        written += (size_t)rc;
    }
    return TRUE;
}

static gboolean
peak_checkpoint_write_csv_name(int fd, const char* name)
{
    if (!peak_checkpoint_write_all(fd, "\"", 1)) {
        return FALSE;
    }
    if (name != NULL) {
        const char* start = name;
        const char* p = name;
        while (*p != '\0') {
            if (*p == '"') {
                if (p > start &&
                    !peak_checkpoint_write_all(fd, start, (size_t)(p - start))) {
                    return FALSE;
                }
                if (!peak_checkpoint_write_all(fd, "\"\"", 2)) {
                    return FALSE;
                }
                start = p + 1;
            }
            p++;
        }
        if (p > start &&
            !peak_checkpoint_write_all(fd, start, (size_t)(p - start))) {
            return FALSE;
        }
    }
    return peak_checkpoint_write_all(fd, "\"", 1);
}

static gboolean
peak_checkpoint_buffer_append_char(char* buffer,
                                   size_t buffer_size,
                                   size_t* out,
                                   char value)
{
    if (*out + 1 >= buffer_size) {
        errno = EOVERFLOW;
        return FALSE;
    }
    buffer[(*out)++] = value;
    buffer[*out] = '\0';
    return TRUE;
}

static gboolean
peak_checkpoint_buffer_append_literal(char* buffer,
                                      size_t buffer_size,
                                      size_t* out,
                                      const char* value)
{
    while (value != NULL && *value != '\0') {
        if (!peak_checkpoint_buffer_append_char(buffer,
                                                buffer_size,
                                                out,
                                                *value++)) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
peak_checkpoint_buffer_append_ulong(char* buffer,
                                    size_t buffer_size,
                                    size_t* out,
                                    unsigned long value)
{
    char digits[32];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value != 0UL);

    while (count != 0) {
        if (!peak_checkpoint_buffer_append_char(buffer,
                                                buffer_size,
                                                out,
                                                digits[--count])) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
peak_checkpoint_buffer_append_padded_ulong(char* buffer,
                                           size_t buffer_size,
                                           size_t* out,
                                           unsigned long value,
                                           unsigned int width)
{
    char digits[32];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value != 0UL);

    while (count < width) {
        digits[count++] = '0';
    }
    while (count != 0) {
        if (!peak_checkpoint_buffer_append_char(buffer,
                                                buffer_size,
                                                out,
                                                digits[--count])) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
peak_checkpoint_buffer_append_double(char* buffer,
                                     size_t buffer_size,
                                     size_t* out,
                                     double value)
{
    if (value != value) {
        return peak_checkpoint_buffer_append_literal(buffer,
                                                     buffer_size,
                                                     out,
                                                     "nan");
    }
    if (value < 0.0) {
        if (!peak_checkpoint_buffer_append_char(buffer,
                                                buffer_size,
                                                out,
                                                '-')) {
            return FALSE;
        }
        value = -value;
    }
    if (value == 0.0) {
        return peak_checkpoint_buffer_append_literal(buffer,
                                                     buffer_size,
                                                     out,
                                                     "0.000000000e+00");
    }

    int exponent = 0;
    while (value >= 10.0 && exponent < 308) {
        value /= 10.0;
        exponent++;
    }
    while (value < 1.0 && exponent > -308) {
        value *= 10.0;
        exponent--;
    }

    unsigned long long scaled =
        (unsigned long long)(value * 1000000000.0 + 0.5);
    if (scaled >= 10000000000ULL) {
        scaled /= 10ULL;
        exponent++;
    }
    unsigned long digit = (unsigned long)(scaled / 1000000000ULL);
    unsigned long frac = (unsigned long)(scaled % 1000000000ULL);
    if (!peak_checkpoint_buffer_append_ulong(buffer, buffer_size, out, digit) ||
        !peak_checkpoint_buffer_append_char(buffer, buffer_size, out, '.') ||
        !peak_checkpoint_buffer_append_padded_ulong(buffer,
                                                    buffer_size,
                                                    out,
                                                    frac,
                                                    9) ||
        !peak_checkpoint_buffer_append_char(buffer, buffer_size, out, 'e')) {
        return FALSE;
    }
    if (exponent < 0) {
        if (!peak_checkpoint_buffer_append_char(buffer,
                                                buffer_size,
                                                out,
                                                '-')) {
            return FALSE;
        }
        exponent = -exponent;
    } else if (!peak_checkpoint_buffer_append_char(buffer,
                                                   buffer_size,
                                                   out,
                                                   '+')) {
        return FALSE;
    }
    return peak_checkpoint_buffer_append_padded_ulong(buffer,
                                                      buffer_size,
                                                      out,
                                                      (unsigned long)exponent,
                                                      2);
}

static gboolean
peak_checkpoint_write_row(int fd,
                          const char* name,
                          gulong num_calls,
                          gulong thread_count,
                          gdouble total_time,
                          gdouble max_total_time,
                          gdouble min_total_time,
                          gdouble exclusive_time,
                          gfloat max_time,
                          gfloat min_time)
{
    char row[512];
    size_t out = 0;
    gulong safe_thread_count = thread_count != 0 ? thread_count : 1;
    gulong per_thread =
        num_calls / safe_thread_count +
        ((num_calls % safe_thread_count != 0) ? 1 : 0);

    if (exclusive_time < 0.0) {
        exclusive_time = 0.0;
    }
    if (total_time >= 0.0 && exclusive_time > total_time) {
        exclusive_time = total_time;
    }

    if (!peak_checkpoint_write_csv_name(fd, name)) {
        return FALSE;
    }

    if (!peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_ulong(row,
                                             sizeof(row),
                                             &out,
                                             (unsigned long)num_calls) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_ulong(row,
                                             sizeof(row),
                                             &out,
                                             (unsigned long)per_thread) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_ulong(row,
                                             sizeof(row),
                                             &out,
                                             (unsigned long)num_calls) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(row,
                                              sizeof(row),
                                              &out,
                                              (double)max_time) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(row,
                                              sizeof(row),
                                              &out,
                                              (double)min_time) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(row,
                                              sizeof(row),
                                              &out,
                                              (double)total_time) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(row,
                                              sizeof(row),
                                              &out,
                                              (double)exclusive_time) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(row,
                                              sizeof(row),
                                              &out,
                                              (double)max_total_time) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(row,
                                              sizeof(row),
                                              &out,
                                              (double)min_total_time) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, ',') ||
        !peak_checkpoint_buffer_append_double(
            row,
            sizeof(row),
            &out,
            (double)per_thread * peak_general_overhead) ||
        !peak_checkpoint_buffer_append_char(row, sizeof(row), &out, '\n')) {
        return FALSE;
    }
    return peak_checkpoint_write_all(fd, row, out);
}

static gboolean
peak_general_listener_stream_exec_checkpoint(unsigned long long checkpoint_index,
                                             gboolean try_only)
{
    char path[PATH_MAX];
    int fd = -1;
    gboolean wrote = FALSE;
    unsigned long long candidate_index = checkpoint_index;
    static const char header[] =
        "function,"
        "count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n";

    for (unsigned int attempts = 0; attempts < 1024; attempts++, candidate_index++) {
        if (peak_stats_exec_checkpoint_path(path,
                                            sizeof(path),
                                            candidate_index) != 0) {
            return FALSE;
        }

        fd = peak_checkpoint_open_exclusive(path);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return FALSE;
        }
    }
    if (fd < 0) {
        errno = EEXIST;
        return FALSE;
    }

    if (!peak_checkpoint_write_all(fd, header, sizeof(header) - 1)) {
        (void)peak_checkpoint_close_fd(fd);
        peak_checkpoint_unlink_path(path);
        return FALSE;
    }
    wrote = TRUE;

    if (!peak_general_listener_snapshot_lock(try_only)) {
        (void)peak_checkpoint_close_fd(fd);
        peak_checkpoint_unlink_path(path);
        return FALSE;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        gulong num_calls = 0;
        gdouble total_time = 0.0;
        gdouble exclusive_time = 0.0;
        gdouble max_total_time = 0.0;
        gdouble min_total_time = 0.0;
        gfloat max_time = 0.0f;
        gfloat min_time = 0.0f;
        gulong threads_seen = 0;

        if (hook_address == NULL ||
            array_listener == NULL ||
            hook_address[i] == NULL ||
            array_listener[i] == NULL) {
            continue;
        }

        PeakGeneralListener* pg_listener =
            PEAKGENERAL_LISTENER(array_listener[i]);
        for (size_t j = 0; j < peak_max_num_threads; j++) {
            gulong calls = pg_listener->num_calls[j];
            num_calls += calls;
            total_time += pg_listener->total_time[j];
            exclusive_time += pg_listener->exclusive_time[j];
            if (calls != 0) {
                threads_seen++;
                if (pg_listener->total_time[j] > max_total_time) {
                    max_total_time = pg_listener->total_time[j];
                }
                if (pg_listener->total_time[j] < min_total_time ||
                    threads_seen == 1) {
                    min_total_time = pg_listener->total_time[j];
                }
                if (pg_listener->max_time[j] > max_time) {
                    max_time = pg_listener->max_time[j];
                }
                if (pg_listener->min_time[j] < min_time || threads_seen == 1) {
                    min_time = pg_listener->min_time[j];
                }
            }
        }

        if (num_calls == 0) {
            continue;
        }

        const char* name =
            (peak_demangled_strings != NULL && peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        if (!peak_checkpoint_write_row(fd,
                                       name,
                                       num_calls,
                                       threads_seen != 0 ? threads_seen : 1,
                                       total_time,
                                       max_total_time,
                                       min_total_time,
                                       exclusive_time,
                                       max_time,
                                       min_time)) {
            pthread_mutex_unlock(&lock);
            (void)peak_checkpoint_close_fd(fd);
            peak_checkpoint_unlink_path(path);
            return FALSE;
        }
    }
    pthread_mutex_unlock(&lock);

    if (!peak_checkpoint_close_fd(fd)) {
        peak_checkpoint_unlink_path(path);
        return FALSE;
    }
    return wrote;
}

static void
peak_general_listener_print_text_result(gulong* sum_num_calls,
                                        gdouble* sum_total_time,
                                        gdouble* max_total_time,
                                        gdouble* min_total_time,
                                        gdouble* sum_exclusive_time,
                                        gfloat* sum_max_time,
                                        gfloat* sum_min_time,
                                        gulong* thread_count,
                                        const int rank_count)
{
    guint max_function_width = 20;
    guint max_col_width = 10;
    guint row_width = max_function_width + max_col_width * 5 + 7;
    char* row_separator = malloc(row_width + 1);
    memset(row_separator, '-', row_width);
    row_separator[row_width] = '\0';

    char* argv_o;
    get_argv0(&argv_o);
    double total_overhead = 0.0;
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            total_overhead += (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                              * peak_general_overhead;
            have_output = TRUE;
        }
    }
    if (have_output &&
        peak_general_listener_should_print_text(
            rank_count <= 1 && peak_mpi_env_size() > 1)) {
        peak_log_report("%.*s\n", row_width, row_separator);
        peak_log_report("%*s PEAK Library\n", (row_width - 12) / 2, "");
        peak_log_report("%.*s\n", row_width, row_separator);
        peak_log_report("Time: %f\n", peak_main_time);
        peak_log_report("PEAK done with: %s\n", argv_o);
        peak_log_report("Estimated overhead: %.3es per call and %.3es total\n", peak_general_overhead, total_overhead);

        peak_log_report("\n%.*s function statistics (call)  %.*s\n", (row_width - 28) / 2, row_separator, (row_width - 28) / 2, row_separator);
        peak_log_report(" individual call counts and time (in seconds)\n");
        peak_log_report("%.*s\n", row_width, row_separator);
        peak_log_report("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "count",
                   max_col_width, "per thread",
                   max_col_width, "per rank",
                   max_col_width, "max",
                   max_col_width, "min");
        peak_log_report("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                if (!array_listener_detached[i])
                    peak_log_report("|%*s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                               max_function_width, truncated_name,
                               max_col_width, sum_num_calls[i],
                               max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                               max_col_width, sum_num_calls[i] / rank_count,
                               max_col_width, sum_max_time[i],
                               max_col_width, sum_min_time[i]);
                else {
                    if (!array_listener_reattached[i])
                        peak_log_report("|%*s*|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                    else
                        peak_log_report("|%*s**|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                }
                free(truncated_name);
            }
        }
        peak_log_report("%.*s\n", row_width, row_separator);

        peak_log_report("\n%.*s function statistics (thread)  %.*s\n", (row_width - 30) / 2, row_separator, (row_width - 30) / 2, row_separator);
        peak_log_report(" thread aggregated time (in seconds)\n");
        peak_log_report("%.*s\n", row_width, row_separator);
        peak_log_report("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "total",
                   max_col_width, "exclusive",
                   max_col_width, "max",
                   max_col_width, "min",
                   max_col_width, "overhead");
        peak_log_report("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                if (!array_listener_detached[i]) {
                    peak_log_report("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                               max_function_width, truncated_name,
                               max_col_width, sum_total_time[i],
                               max_col_width, sum_exclusive_time[i],
                               max_col_width, max_total_time[i],
                               max_col_width, min_total_time[i],
                               max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                              * peak_general_overhead);
                } else {
                    if (!array_listener_reattached[i])
                        peak_log_report("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                    max_function_width, truncated_name,
                                    max_col_width, sum_total_time[i],
                                    max_col_width, sum_exclusive_time[i],
                                    max_col_width, max_total_time[i],
                                    max_col_width, min_total_time[i],
                                    max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                    * peak_general_overhead);
                    else
                        peak_log_report("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_total_time[i],
                                max_col_width, sum_exclusive_time[i],
                                max_col_width, max_total_time[i],
                                max_col_width, min_total_time[i],
                                max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                * peak_general_overhead);
                }
                free(truncated_name);
            }
        }
        peak_log_report("%.*s\n", row_width, row_separator);
    }
    free(argv_o);
    free(row_separator);
}

static void
peak_general_listener_free_demangled_strings(void)
{
    if (peak_demangled_strings == NULL) {
        return;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
        peak_demangled_strings[i] = NULL;
    }
    g_free(peak_demangled_strings);
    peak_demangled_strings = NULL;
}

static void
peak_general_listener_sanitize_output_times(gdouble* sum_total_time,
                                            gdouble* sum_exclusive_time,
                                            size_t function_count)
{
    for (size_t i = 0; i < function_count; i++) {
        if (sum_exclusive_time[i] < 0.0) {
            sum_exclusive_time[i] = 0.0;
        }
        if (sum_total_time[i] >= 0.0 &&
            sum_exclusive_time[i] > sum_total_time[i]) {
            sum_exclusive_time[i] = sum_total_time[i];
        }
    }
}

static void
peak_general_listener_print_result(gulong* sum_num_calls,
                                   gdouble* sum_total_time,
                                   gdouble* max_total_time,
                                   gdouble* min_total_time,
                                   gdouble* sum_exclusive_time,
                                   gfloat* sum_max_time,
                                   gfloat* sum_min_time,
                                   gulong* thread_count,
                                   const int rank_count)
{
    peak_general_listener_sanitize_output_times(sum_total_time,
                                                sum_exclusive_time,
                                                peak_hook_address_count);
    peak_general_listener_export_csv_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count,
                                            rank_count);
    peak_general_listener_print_text_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count,
                                            rank_count);
}

static gboolean
peak_general_listener_snapshot_lock(gboolean try_only)
{
    if (!try_only) {
        pthread_mutex_lock(&lock);
        return TRUE;
    }

    int rc = pthread_mutex_trylock(&lock);
    if (rc == 0) {
        return TRUE;
    }
    errno = (rc == EBUSY) ? EAGAIN : rc;
    return FALSE;
}

gboolean
peak_general_listener_checkpoint_for_exec(unsigned long long checkpoint_index,
                                          gboolean try_only)
{
    gboolean wrote;
    int checkpoint_errno;

    wrote = peak_general_listener_stream_exec_checkpoint(checkpoint_index,
                                                         try_only);
    checkpoint_errno = errno;

    if (!wrote) {
        errno = checkpoint_errno != 0 ? checkpoint_errno : EIO;
    }
    return wrote;
}

#ifdef HAVE_MPI
static uint64_t
peak_general_listener_slot_identity_hash(size_t hook_id)
{
    const unsigned char* text;
    uint64_t hash = 1469598103934665603ULL;

    if (hook_id >= peak_hook_address_count) {
        return 0;
    }

    text = (const unsigned char*)
        (peak_demangled_strings != NULL && peak_demangled_strings[hook_id] != NULL ?
             peak_demangled_strings[hook_id] :
             peak_hook_strings != NULL && peak_hook_strings[hook_id] != NULL ?
                 peak_hook_strings[hook_id] : "");

    while (*text != '\0') {
        hash ^= (uint64_t)*text++;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void
peak_socket_reduce_hash_text(uint64_t* hash,
                             const char* label,
                             const char* value)
{
    const unsigned char* text;

    if (hash == NULL || value == NULL || value[0] == '\0') {
        return;
    }

    for (text = (const unsigned char*)label; text != NULL && *text != '\0'; text++) {
        *hash ^= (uint64_t)*text;
        *hash *= 1099511628211ULL;
    }
    *hash ^= (uint64_t)'=';
    *hash *= 1099511628211ULL;
    for (text = (const unsigned char*)value; *text != '\0'; text++) {
        *hash ^= (uint64_t)*text;
        *hash *= 1099511628211ULL;
    }
    *hash ^= (uint64_t)';';
    *hash *= 1099511628211ULL;
}

static uint64_t
peak_socket_reduce_session_token(void)
{
    static const char* shared_env_names[] = {
        "SLURM_JOB_ID",
        "SLURM_STEP_ID",
        "SLURM_STEPID",
        "SLURM_JOB_UID",
        "SLURM_CLUSTER_NAME",
        "SLURM_NODELIST",
        "SLURM_JOB_NODELIST",
        "PMI_JOBID",
        "PMI_KVS",
        "PMI_NAMESPACE",
        "PMIX_NAMESPACE",
        "OMPI_COMM_WORLD_JOBID",
        NULL,
    };
    const char* override = getenv(PEAK_OUTPUT_AGGREGATION_TOKEN_ENV);
    uint64_t hash = 1469598103934665603ULL;
    gboolean saw_shared_value = FALSE;

    if (override != NULL && override[0] != '\0') {
        peak_socket_reduce_hash_text(&hash,
                                     PEAK_OUTPUT_AGGREGATION_TOKEN_ENV,
                                     override);
        return hash;
    }

    for (size_t i = 0; shared_env_names[i] != NULL; i++) {
        const char* value = getenv(shared_env_names[i]);
        if (value != NULL && value[0] != '\0') {
            peak_socket_reduce_hash_text(&hash, shared_env_names[i], value);
            saw_shared_value = TRUE;
        }
    }

    if (!saw_shared_value) {
        peak_socket_reduce_hash_text(&hash, "fallback", "single-launcher");
    }
    return hash;
}

static gboolean
peak_general_listener_has_duplicate_slot_names(void)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        const char* left =
            peak_demangled_strings != NULL && peak_demangled_strings[i] != NULL ?
                peak_demangled_strings[i] :
                peak_hook_strings != NULL && peak_hook_strings[i] != NULL ?
                    peak_hook_strings[i] : NULL;

        if (left == NULL) {
            continue;
        }

        for (size_t j = i + 1; j < peak_hook_address_count; j++) {
            const char* right =
                peak_demangled_strings != NULL && peak_demangled_strings[j] != NULL ?
                    peak_demangled_strings[j] :
                    peak_hook_strings != NULL && peak_hook_strings[j] != NULL ?
                        peak_hook_strings[j] : NULL;

            if (right != NULL && strcmp(left, right) == 0) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t rank;
    uint64_t hook_count;
    uint64_t session_token;
} PeakSocketReduceHeader;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t rank;
    uint64_t hook_count;
    uint64_t session_token;
    uint8_t ack;
    uint8_t reserved[7];
} PeakSocketReduceReleaseFrame;

typedef struct {
    uint64_t identity_hash;
    uint64_t num_calls;
    double total_time;
    double max_total_time;
    double min_total_time;
    double exclusive_time;
    float max_time;
    float min_time;
    uint64_t thread_count;
    int detached;
    int reattached;
} PeakSocketReduceRecord;

#define PEAK_SOCKET_REDUCE_MAGIC 0x5045414b52454431ULL
#define PEAK_SOCKET_REDUCE_VERSION 2U
#define PEAK_SOCKET_REDUCE_RELEASE_ACK 0x51U
#define PEAK_SOCKET_REDUCE_RELEASE_FALLBACK 0x52U
#define PEAK_SOCKET_REDUCE_DEFAULT_TIMEOUT_MS 60000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_BASE 42000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_SPAN 20000

static int
peak_socket_reduce_parse_positive_int_env(const char* name, int default_value)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 ||
        parsed > INT_MAX) {
        peak_log_info("[peak] Ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (int)parsed;
}

static int
peak_socket_reduce_default_port(void)
{
    const char* job_id = getenv("SLURM_JOB_ID");
    char* end = NULL;
    long parsed = 0;

    if (job_id != NULL && job_id[0] != '\0') {
        errno = 0;
        parsed = strtol(job_id, &end, 10);
        if (errno != 0 || end == job_id) {
            parsed = 0;
        }
    }

    if (parsed < 0) {
        parsed = -parsed;
    }

    return PEAK_SOCKET_REDUCE_DEFAULT_PORT_BASE +
           (int)(parsed % PEAK_SOCKET_REDUCE_DEFAULT_PORT_SPAN);
}

static int
peak_socket_reduce_port(void)
{
    int port = peak_socket_reduce_parse_positive_int_env(
        PEAK_OUTPUT_AGGREGATION_PORT_ENV,
        peak_socket_reduce_default_port());

    if (port <= 0 || port > 65535) {
        peak_log_info("[peak] Ignoring out-of-range %s=%d\n",
                   PEAK_OUTPUT_AGGREGATION_PORT_ENV,
                   port);
        return peak_socket_reduce_default_port();
    }

    return port;
}

static int64_t
peak_socket_reduce_deadline_us(int timeout_ms)
{
    return g_get_monotonic_time() + (int64_t)timeout_ms * 1000;
}

static int
peak_socket_reduce_remaining_ms(int64_t deadline_us)
{
    int64_t now = g_get_monotonic_time();
    int64_t remaining = deadline_us - now;

    if (remaining <= 0) {
        return 0;
    }

    remaining = (remaining + 999) / 1000;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static gboolean
peak_socket_reduce_poll_fd(int fd, short events, int64_t deadline_us)
{
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int result;

        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (result == 0) {
            return FALSE;
        }
        return (pfd.revents & events) != 0;
    }

    return FALSE;
}

static gboolean
peak_socket_reduce_send_all(int fd,
                            const void* data,
                            size_t size,
                            int64_t deadline_us)
{
    const char* cursor = (const char*)data;

    while (size > 0 && peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        if (!peak_socket_reduce_poll_fd(fd, POLLOUT, deadline_us)) {
            return FALSE;
        }
        ssize_t written = send(fd, cursor, size, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (written == 0) {
            return FALSE;
        }
        cursor += written;
        size -= (size_t)written;
    }

    return size == 0;
}

static gboolean
peak_socket_reduce_recv_all(int fd,
                            void* data,
                            size_t size,
                            int64_t deadline_us)
{
    char* cursor = (char*)data;

    while (size > 0 && peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        if (!peak_socket_reduce_poll_fd(fd, POLLIN, deadline_us)) {
            return FALSE;
        }
        ssize_t read_count = recv(fd, cursor, size, MSG_WAITALL);
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (read_count == 0) {
            return FALSE;
        }
        cursor += read_count;
        size -= (size_t)read_count;
    }

    return size == 0;
}

static void
peak_socket_reduce_set_timeout(int fd, int timeout_ms)
{
    struct timeval tv;

    if (timeout_ms <= 0) {
        timeout_ms = 1;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static gboolean
peak_socket_reduce_hostlist_token_is_ascending_range(const char* token,
                                                     size_t token_len,
                                                     size_t* range_prefix_len)
{
    const char* dash = memchr(token, '-', token_len);
    char* end = NULL;
    long first;
    long last;

    if (range_prefix_len != NULL) {
        *range_prefix_len = token_len;
    }
    if (dash == NULL || dash == token || dash + 1 >= token + token_len) {
        return FALSE;
    }
    for (const char* cursor = token; cursor < token + token_len; cursor++) {
        if (cursor == dash) {
            continue;
        }
        if (!g_ascii_isdigit(*cursor)) {
            return FALSE;
        }
    }

    errno = 0;
    first = strtol(token, &end, 10);
    if (errno != 0 || end != dash) {
        return FALSE;
    }
    errno = 0;
    last = strtol(dash + 1, &end, 10);
    if (errno != 0 || end != token + token_len || first > last) {
        return FALSE;
    }

    if (range_prefix_len != NULL) {
        *range_prefix_len = (size_t)(dash - token);
    }
    return TRUE;
}

static gboolean
peak_socket_reduce_first_host_from_slurm_nodelist(const char* nodelist,
                                                 char* out,
                                                 size_t out_size)
{
    const char* bracket;
    const char* comma;
    const char* dash;
    size_t prefix_len;
    size_t token_len;
    size_t host_token_len;

    if (out == NULL || out_size == 0) {
        return FALSE;
    }
    out[0] = '\0';

    if (nodelist == NULL || nodelist[0] == '\0') {
        return FALSE;
    }

    bracket = strchr(nodelist, '[');
    if (bracket == NULL) {
        comma = strchr(nodelist, ',');
        token_len = comma != NULL ? (size_t)(comma - nodelist) : strlen(nodelist);
        if (token_len == 0 || token_len >= out_size) {
            return FALSE;
        }
        memcpy(out, nodelist, token_len);
        out[token_len] = '\0';
        return TRUE;
    }

    prefix_len = (size_t)(bracket - nodelist);
    comma = strchr(bracket + 1, ',');
    dash = strchr(bracket + 1, ']');
    if (dash == NULL) {
        return FALSE;
    }
    if (comma != NULL && comma < dash) {
        dash = comma;
    }
    token_len = (size_t)(dash - (bracket + 1));
    host_token_len = token_len;
    (void)peak_socket_reduce_hostlist_token_is_ascending_range(bracket + 1,
                                                               token_len,
                                                               &host_token_len);
    if (prefix_len + host_token_len == 0 ||
        prefix_len + host_token_len >= out_size) {
        return FALSE;
    }
    memcpy(out, nodelist, prefix_len);
    memcpy(out + prefix_len, bracket + 1, host_token_len);
    out[prefix_len + host_token_len] = '\0';
    return TRUE;
}

static gboolean
peak_socket_reduce_first_slurm_host(char* out, size_t out_size)
{
    const char* nodelist = getenv("SLURM_NODELIST");

    if (nodelist == NULL || nodelist[0] == '\0') {
        nodelist = getenv("SLURM_JOB_NODELIST");
    }

    return peak_socket_reduce_first_host_from_slurm_nodelist(nodelist,
                                                            out,
                                                            out_size);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
gboolean
peak_general_listener_test_first_slurm_host(const char* nodelist,
                                            char* out,
                                            size_t out_size)
{
    return peak_socket_reduce_first_host_from_slurm_nodelist(nodelist,
                                                            out,
                                                            out_size);
}
#endif

static gboolean
peak_socket_reduce_root_host(char* out, size_t out_size)
{
    const char* override = getenv(PEAK_OUTPUT_AGGREGATION_HOST_ENV);

    if (override != NULL && override[0] != '\0') {
        g_strlcpy(out, override, out_size);
        return TRUE;
    }

    if (peak_socket_reduce_first_slurm_host(out, out_size)) {
        return TRUE;
    }

    g_strlcpy(out, "127.0.0.1", out_size);
    return TRUE;
}

static PeakSocketReduceRecord*
peak_socket_reduce_build_records(gulong* sum_num_calls,
                                 gdouble* sum_total_time,
                                 gdouble* max_total_time,
                                 gdouble* min_total_time,
                                 gdouble* sum_exclusive_time,
                                 gfloat* sum_max_time,
                                 gfloat* sum_min_time,
                                 gulong* thread_count)
{
    PeakSocketReduceRecord* records =
        g_new0(PeakSocketReduceRecord, peak_hook_address_count);

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        records[i].identity_hash =
            peak_general_listener_slot_identity_hash(i);
        records[i].num_calls = (uint64_t)sum_num_calls[i];
        records[i].total_time = (double)sum_total_time[i];
        records[i].max_total_time = (double)max_total_time[i];
        records[i].min_total_time = (double)min_total_time[i];
        records[i].exclusive_time = (double)sum_exclusive_time[i];
        records[i].max_time = (float)sum_max_time[i];
        records[i].min_time = (float)sum_min_time[i];
        records[i].thread_count = (uint64_t)thread_count[i];
        records[i].detached = array_listener_detached[i] ? 1 : 0;
        records[i].reattached = array_listener_reattached[i] ? 1 : 0;
    }

    return records;
}

static int
peak_socket_reduce_create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in address;

    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(fd, 4096) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static gboolean
peak_socket_reduce_wait_connected(int fd, int64_t deadline_us)
{
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int poll_result;
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (poll_result == 0) {
            return FALSE;
        }
        if (getsockopt(fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &socket_error_size) != 0 ||
            socket_error != 0) {
            return FALSE;
        }
        return TRUE;
    }

    return FALSE;
}

static int
peak_socket_reduce_connect(const char* host, int port, int64_t deadline_us)
{
    char port_text[16];
    char qualified_host[NI_MAXHOST];
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* entry;

    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    qualified_host[0] = '\0';
    if (host != NULL && strchr(host, '.') == NULL) {
        char local_host[NI_MAXHOST];
        if (gethostname(local_host, sizeof(local_host)) == 0) {
            local_host[sizeof(local_host) - 1] = '\0';
            char* domain = strchr(local_host, '.');
            if (domain != NULL &&
                strlen(host) + strlen(domain) < sizeof(qualified_host)) {
                snprintf(qualified_host,
                         sizeof(qualified_host),
                         "%s%s",
                         host,
                         domain);
            }
        }
    }

    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        const char* candidates[3] = {
            host,
            qualified_host[0] != '\0' ? qualified_host : NULL,
            NULL,
        };

        for (size_t i = 0; candidates[i] != NULL; i++) {
            int gai = getaddrinfo(candidates[i], port_text, &hints, &result);
            if (gai == 0) {
                for (entry = result; entry != NULL; entry = entry->ai_next) {
                    int fd = socket(entry->ai_family,
                                    entry->ai_socktype,
                                    entry->ai_protocol);
                    int flags;
                    if (fd < 0) {
                        continue;
                    }
                    flags = fcntl(fd, F_GETFL, 0);
                    if (flags < 0 ||
                        fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                        close(fd);
                        continue;
                    }
                    if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0 ||
                        (errno == EINPROGRESS &&
                         peak_socket_reduce_wait_connected(fd, deadline_us))) {
                        (void)fcntl(fd, F_SETFL, flags);
                        peak_socket_reduce_set_timeout(
                            fd,
                            peak_socket_reduce_remaining_ms(deadline_us));
                        freeaddrinfo(result);
                        return fd;
                    }
                    close(fd);
                }
                freeaddrinfo(result);
                result = NULL;
            }
        }
        usleep(10000);
    }

    return -1;
}

static int
peak_socket_reduce_release_port(int port)
{
    return port < 65535 ? port + 1 : port - 1;
}

static gboolean
peak_socket_reduce_wait_for_release(const char* host,
                                    int port,
                                    const PeakSocketReduceHeader* header,
                                    int timeout_ms)
{
    int64_t deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    int fd = peak_socket_reduce_connect(host, port, deadline_us);
    PeakSocketReduceReleaseFrame frame;
    gboolean released;

    if (fd < 0 || header == NULL) {
        if (fd >= 0) {
            close(fd);
        }
        return FALSE;
    }

    memset(&frame, 0, sizeof(frame));
    frame.magic = header->magic;
    frame.version = header->version;
    frame.rank = header->rank;
    frame.hook_count = header->hook_count;
    frame.session_token = header->session_token;
    released = peak_socket_reduce_send_all(fd,
                                           &frame,
                                           sizeof(frame),
                                           deadline_us) &&
               peak_socket_reduce_recv_all(fd,
                                           &frame,
                                           sizeof(frame),
                                           deadline_us) &&
               frame.magic == header->magic &&
               frame.version == header->version &&
               frame.rank == header->rank &&
               frame.hook_count == header->hook_count &&
               frame.session_token == header->session_token &&
               frame.ack == PEAK_SOCKET_REDUCE_RELEASE_ACK;
    close(fd);
    return released;
}

static gboolean
peak_socket_reduce_release_peers(int port,
                                 gboolean* release_targets,
                                 int size,
                                 uint64_t hook_count,
                                 uint64_t session_token,
                                 int timeout_ms,
                                 uint8_t ack)
{
    int listener;
    int64_t deadline_us;
    unsigned int peer_count = 0;
    unsigned int released = 0;

    if (release_targets == NULL || size <= 1) {
        return TRUE;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (ack == PEAK_SOCKET_REDUCE_RELEASE_ACK &&
        peak_env_value_truthy(
            getenv(PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL_ENV))) {
        g_printerr("[peak] Socket aggregation release failure requested by test hook; peer ranks may fall back to local output\n");
        return FALSE;
    }
#endif

    for (int i = 1; i < size; i++) {
        if (release_targets[i]) {
            peer_count++;
        }
    }
    if (peer_count == 0) {
        return TRUE;
    }

    listener = peak_socket_reduce_create_listener(port);
    if (listener < 0) {
        g_printerr("[peak] Socket aggregation could not listen on release port %d; peer ranks may exit after timeout\n",
                   port);
        return FALSE;
    }

    deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    while (released < peer_count &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int poll_result;
        int fd;
        PeakSocketReduceReleaseFrame frame;
        gboolean valid;

        pfd.fd = listener;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_result == 0) {
            break;
        }

        fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(deadline_us));
        memset(&frame, 0, sizeof(frame));
        valid = peak_socket_reduce_recv_all(fd,
                                            &frame,
                                            sizeof(frame),
                                            deadline_us) &&
                frame.magic == PEAK_SOCKET_REDUCE_MAGIC &&
                frame.version == PEAK_SOCKET_REDUCE_VERSION &&
                frame.session_token == session_token &&
                frame.hook_count == hook_count &&
                frame.rank < (uint32_t)size &&
                frame.rank > 0 &&
                release_targets[frame.rank];
        if (valid) {
            frame.ack = ack;
            valid = peak_socket_reduce_send_all(fd,
                                                &frame,
                                                sizeof(frame),
                                                deadline_us);
        }
        close(fd);
        if (valid) {
            release_targets[frame.rank] = FALSE;
            released++;
        }
    }

    close(listener);
    if (released != peer_count) {
        peak_log_info("[peak] Socket aggregation released %u/%u peer ranks before timeout\n",
                   released,
                   peer_count);
        return FALSE;
    }
    return TRUE;
}

static gboolean
peak_socket_reduce_merge_records(PeakSocketReduceRecord* aggregate,
                                 const PeakSocketReduceRecord* incoming)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (incoming[i].identity_hash != aggregate[i].identity_hash) {
            return FALSE;
        }

        aggregate[i].num_calls += incoming[i].num_calls;
        aggregate[i].total_time += incoming[i].total_time;
        if (incoming[i].max_total_time > aggregate[i].max_total_time) {
            aggregate[i].max_total_time = incoming[i].max_total_time;
        }
        if (incoming[i].min_total_time < aggregate[i].min_total_time) {
            aggregate[i].min_total_time = incoming[i].min_total_time;
        }
        aggregate[i].exclusive_time += incoming[i].exclusive_time;
        if (incoming[i].max_time > aggregate[i].max_time) {
            aggregate[i].max_time = incoming[i].max_time;
        }
        if (incoming[i].min_time < aggregate[i].min_time) {
            aggregate[i].min_time = incoming[i].min_time;
        }
        aggregate[i].thread_count += incoming[i].thread_count;
        aggregate[i].detached = aggregate[i].detached || incoming[i].detached;
        aggregate[i].reattached =
            aggregate[i].reattached || incoming[i].reattached;
    }

    return TRUE;
}

static void
peak_socket_reduce_records_to_arrays(PeakSocketReduceRecord* records,
                                     gulong* sum_num_calls,
                                     gdouble* sum_total_time,
                                     gdouble* max_total_time,
                                     gdouble* min_total_time,
                                     gdouble* sum_exclusive_time,
                                     gfloat* sum_max_time,
                                     gfloat* sum_min_time,
                                     gulong* thread_count,
                                     gboolean* detached,
                                     gboolean* reattached)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        sum_num_calls[i] = (gulong)records[i].num_calls;
        sum_total_time[i] = (gdouble)records[i].total_time;
        max_total_time[i] = (gdouble)records[i].max_total_time;
        min_total_time[i] = (gdouble)records[i].min_total_time;
        sum_exclusive_time[i] = (gdouble)records[i].exclusive_time;
        sum_max_time[i] = (gfloat)records[i].max_time;
        sum_min_time[i] = (gfloat)records[i].min_time;
        thread_count[i] = (gulong)records[i].thread_count;
        detached[i] = records[i].detached ? TRUE : FALSE;
        reattached[i] = records[i].reattached ? TRUE : FALSE;
    }
}

static gboolean
peak_socket_reduce_rank_size(int* rank_out, int* size_out, gboolean allow_mpi)
{
#ifdef HAVE_MPI
    int initialized = 0;
    int finalized = 0;

    if (allow_mpi) {
        MPI_Initialized(&initialized);
        MPI_Finalized(&finalized);
        if (initialized && !finalized) {
            MPI_Comm_rank(MPI_COMM_WORLD, rank_out);
            MPI_Comm_size(MPI_COMM_WORLD, size_out);
            return TRUE;
        }
    }
#endif

    long env_size = peak_mpi_env_size();
    long env_rank = peak_mpi_env_rank();
    if (env_size > 1) {
        if (env_rank >= 0 && env_rank < env_size) {
            *rank_out = (int)env_rank;
            *size_out = (int)env_size;
            return TRUE;
        }
        return FALSE;
    }

    if (env_rank > 0) {
        return FALSE;
    }

    *rank_out = 0;
    *size_out = 1;
    return TRUE;
}

static gboolean
peak_general_listener_socket_reduce_result_with_rank_source(
    gulong* sum_num_calls,
    gdouble* sum_total_time,
    gdouble* max_total_time,
    gdouble* min_total_time,
    gdouble* sum_exclusive_time,
    gfloat* sum_max_time,
    gfloat* sum_min_time,
    gulong* thread_count,
    gboolean allow_mpi_rank_query)
{
    int rank = 0;
    int size = 1;
    int port = peak_socket_reduce_port();
    int release_port = peak_socket_reduce_release_port(port);
    int timeout_ms = peak_socket_reduce_parse_positive_int_env(
        PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
        PEAK_SOCKET_REDUCE_DEFAULT_TIMEOUT_MS);
    int64_t deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    uint64_t session_token = peak_socket_reduce_session_token();
    PeakSocketReduceRecord* local_records = NULL;
    PeakSocketReduceHeader header;

    if (!peak_socket_reduce_rank_size(&rank, &size, allow_mpi_rank_query)) {
        g_printerr("[peak] Socket aggregation could not determine rank/size from %s; skipping aggregate output\n",
                   allow_mpi_rank_query ? "MPI or launcher metadata"
                                        : "launcher metadata");
        return FALSE;
    }

    if (peak_general_listener_has_duplicate_slot_names()) {
        if (rank == 0) {
            g_printerr("[peak] Socket aggregation contains duplicate hook names, likely from multiple JIT generations; skipping aggregate output\n");
        }
        return FALSE;
    }

    if (size <= 1) {
        peak_general_listener_print_result(sum_num_calls,
                                           sum_total_time,
                                           max_total_time,
                                           min_total_time,
                                           sum_exclusive_time,
                                           sum_max_time,
                                           sum_min_time,
                                           thread_count,
                                           1);
        return TRUE;
    }

    local_records = peak_socket_reduce_build_records(sum_num_calls,
                                                     sum_total_time,
                                                     max_total_time,
                                                     min_total_time,
                                                     sum_exclusive_time,
                                                     sum_max_time,
                                                     sum_min_time,
                                                     thread_count);
    header.magic = PEAK_SOCKET_REDUCE_MAGIC;
    header.version = PEAK_SOCKET_REDUCE_VERSION;
    header.rank = (uint32_t)rank;
    header.hook_count = (uint64_t)peak_hook_address_count;
    header.session_token = session_token;

    if (rank != 0) {
        char root_host[256];
        int fd;
        gboolean sent;
        gboolean released;

        if (!peak_socket_reduce_root_host(root_host, sizeof(root_host))) {
            g_printerr("[peak] Socket aggregation could not determine root host; skipping aggregate output\n");
            g_free(local_records);
            return FALSE;
        }

        if (rank > 0) {
            usleep((useconds_t)((rank % 1024) * 1000));
        }
        fd = peak_socket_reduce_connect(root_host, port, deadline_us);
        if (fd < 0) {
            g_printerr("[peak] Socket aggregation could not connect to %s:%d; skipping aggregate output\n",
                       root_host,
                       port);
            g_free(local_records);
            return FALSE;
        }

        sent = peak_socket_reduce_send_all(fd,
                                           &header,
                                           sizeof(header),
                                           deadline_us) &&
               peak_socket_reduce_send_all(fd,
                                           local_records,
                                           sizeof(PeakSocketReduceRecord) *
                                               peak_hook_address_count,
                                           deadline_us);
        close(fd);
        g_free(local_records);
        if (!sent) {
            g_printerr("[peak] Socket aggregation send failed; skipping aggregate output\n");
            return FALSE;
        }
        released = peak_socket_reduce_wait_for_release(root_host,
                                                       release_port,
                                                       &header,
                                                       timeout_ms);
        if (!released) {
            g_printerr("[peak] Socket aggregation release wait failed; skipping aggregate output\n");
            return FALSE;
        }
        return TRUE;
    }

    int listener = peak_socket_reduce_create_listener(port);
    if (listener < 0) {
        g_printerr("[peak] Socket aggregation could not listen on port %d; skipping aggregate output\n",
                   port);
        g_free(local_records);
        return FALSE;
    }

    gboolean* seen = g_new0(gboolean, size);
    gboolean* release_targets = g_new0(gboolean, size);
    PeakSocketReduceRecord* aggregate =
        g_memdup2(local_records,
                  sizeof(PeakSocketReduceRecord) * peak_hook_address_count);
    unsigned int received = 0;
    gboolean failed = FALSE;
    seen[0] = TRUE;

    while (received < (unsigned int)(size - 1) &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int poll_result;

        pfd.fd = listener;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = TRUE;
            break;
        }
        if (poll_result == 0) {
            break;
        }

        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = TRUE;
            break;
        }
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(deadline_us));

        PeakSocketReduceHeader incoming_header;
        PeakSocketReduceRecord* incoming =
            g_new0(PeakSocketReduceRecord, peak_hook_address_count);
        gboolean ok =
            peak_socket_reduce_recv_all(fd,
                                        &incoming_header,
                                        sizeof(incoming_header),
                                        deadline_us) &&
            incoming_header.magic == PEAK_SOCKET_REDUCE_MAGIC &&
            incoming_header.version == PEAK_SOCKET_REDUCE_VERSION &&
            incoming_header.session_token == session_token &&
            incoming_header.hook_count == (uint64_t)peak_hook_address_count &&
            incoming_header.rank < (uint32_t)size &&
            incoming_header.rank > 0 &&
            !seen[incoming_header.rank];
        if (ok) {
            release_targets[incoming_header.rank] = TRUE;
        }
        ok = ok &&
            peak_socket_reduce_recv_all(
                fd,
                incoming,
                sizeof(PeakSocketReduceRecord) * peak_hook_address_count,
                deadline_us) &&
            peak_socket_reduce_merge_records(aggregate, incoming);
        close(fd);
        g_free(incoming);

        if (!ok) {
            failed = TRUE;
            break;
        }

        seen[incoming_header.rank] = TRUE;
        received++;
    }

    close(listener);
    g_free(local_records);
    g_free(seen);

    if (failed || received != (unsigned int)(size - 1)) {
        g_printerr("[peak] Socket aggregation received %u/%d peer ranks; skipping aggregate output on root\n",
                   received,
                   size - 1);
        (void)peak_socket_reduce_release_peers(release_port,
                                               release_targets,
                                               size,
                                               (uint64_t)peak_hook_address_count,
                                               session_token,
                                               timeout_ms,
                                               PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
        g_free(release_targets);
        g_free(aggregate);
        return FALSE;
    }

    gulong* socket_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* socket_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* socket_max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* socket_min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* socket_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* socket_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* socket_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* socket_thread_count = g_new0(gulong, peak_hook_address_count);
    gboolean* socket_array_listener_detached =
        g_new0(gboolean, peak_hook_address_count);
    gboolean* socket_array_listener_reattached =
        g_new0(gboolean, peak_hook_address_count);
    peak_socket_reduce_records_to_arrays(aggregate,
                                         socket_sum_num_calls,
                                         socket_sum_total_time,
                                         socket_max_total_time,
                                         socket_min_total_time,
                                         socket_sum_exclusive_time,
                                         socket_sum_max_time,
                                         socket_sum_min_time,
                                         socket_thread_count,
                                         socket_array_listener_detached,
                                         socket_array_listener_reattached);
    g_free(aggregate);

    gboolean* previous_array_listener_detached = array_listener_detached;
    gboolean* previous_array_listener_reattached = array_listener_reattached;
    array_listener_detached = socket_array_listener_detached;
    array_listener_reattached = socket_array_listener_reattached;

    peak_general_listener_export_csv_result(socket_sum_num_calls,
                                            socket_sum_total_time,
                                            socket_max_total_time,
                                            socket_min_total_time,
                                            socket_sum_exclusive_time,
                                            socket_sum_max_time,
                                            socket_sum_min_time,
                                            socket_thread_count,
                                            size);
    if (!peak_socket_reduce_release_peers(release_port,
                                          release_targets,
                                          size,
                                          (uint64_t)peak_hook_address_count,
                                          session_token,
                                          timeout_ms,
                                          PEAK_SOCKET_REDUCE_RELEASE_ACK)) {
        peak_stats_csv_unlink();
        array_listener_detached = previous_array_listener_detached;
        array_listener_reattached = previous_array_listener_reattached;
        g_free(socket_array_listener_detached);
        g_free(socket_array_listener_reattached);
        g_free(socket_sum_num_calls);
        g_free(socket_sum_total_time);
        g_free(socket_max_total_time);
        g_free(socket_min_total_time);
        g_free(socket_sum_exclusive_time);
        g_free(socket_sum_max_time);
        g_free(socket_sum_min_time);
        g_free(socket_thread_count);
        g_free(release_targets);
        return FALSE;
    }
    peak_general_listener_print_text_result(socket_sum_num_calls,
                                            socket_sum_total_time,
                                            socket_max_total_time,
                                            socket_min_total_time,
                                            socket_sum_exclusive_time,
                                            socket_sum_max_time,
                                            socket_sum_min_time,
                                            socket_thread_count,
                                            size);
    g_free(previous_array_listener_detached);
    g_free(previous_array_listener_reattached);
    g_free(socket_sum_num_calls);
    g_free(socket_sum_total_time);
    g_free(socket_max_total_time);
    g_free(socket_min_total_time);
    g_free(socket_sum_exclusive_time);
    g_free(socket_sum_max_time);
    g_free(socket_sum_min_time);
    g_free(socket_thread_count);
    g_free(release_targets);
    return TRUE;
}

static gboolean
peak_general_listener_socket_reduce_result(gulong* sum_num_calls,
                                           gdouble* sum_total_time,
                                           gdouble* max_total_time,
                                           gdouble* min_total_time,
                                           gdouble* sum_exclusive_time,
                                           gfloat* sum_max_time,
                                           gfloat* sum_min_time,
                                           gulong* thread_count)
{
    return peak_general_listener_socket_reduce_result_with_rank_source(
        sum_num_calls,
        sum_total_time,
        max_total_time,
        min_total_time,
        sum_exclusive_time,
        sum_max_time,
        sum_min_time,
        thread_count,
        TRUE);
}

static unsigned int
peak_mpi_output_collective_timeout_ms(void)
{
    unsigned int timeout_ms = peak_general_listener_parse_uint_env_default(
        PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
        PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT);
    return timeout_ms == 0 ? PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT
                           : timeout_ms;
}

static gboolean
peak_mpi_reducer_forced_failure(const char* label)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* value = getenv(PEAK_TEST_MPI_REDUCER_FAIL_LABEL_ENV);

    if (value == NULL || value[0] == '\0') {
        return FALSE;
    }
    return strcmp(value, "*") == 0 || strcmp(value, label) == 0;
#else
    (void)label;
    return FALSE;
#endif
}

static gboolean
peak_mpi_wait_collective_request(MPI_Request* request, const char* label)
{
    MPI_Status status;
    int done = 0;
    unsigned int timeout_ms = peak_mpi_output_collective_timeout_ms();
    double deadline = peak_second() + (double)timeout_ms / 1000.0;

    while (1) {
        int mpi_result = MPI_Test(request, &done, &status);
        if (mpi_result != MPI_SUCCESS) {
            g_printerr("[peak] MPI_Test for %s failed; abandoning MPI reducer without touching MPI again\n",
                       label);
            peak_general_listener_mark_mpi_reducer_failed_closed();
            return FALSE;
        }
        if (done) {
            return TRUE;
        }
        if (peak_second() >= deadline) {
            /*
             * As with the finalize proof, active nonblocking collectives do
             * not have a portable cancel path. Leave the request owned by MPI,
             * abandon PEAK's MPI reducer, and do not issue later MPI calls from
             * this teardown path.
             */
            g_printerr("[peak] MPI %s timed out after %u ms; abandoning MPI reducer without touching MPI again\n",
                       label,
                       timeout_ms);
            peak_general_listener_mark_mpi_reducer_failed_closed();
            return FALSE;
        }
        sched_yield();
    }
}

static gboolean
peak_mpi_allreduce_checked(const void* sendbuf,
                           void* recvbuf,
                           int count,
                           MPI_Datatype datatype,
                           MPI_Op op,
                           const char* label)
{
    if (peak_mpi_reducer_forced_failure(label)) {
        g_printerr("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }

    MPI_Request request = MPI_REQUEST_NULL;
    int mpi_result = MPI_Iallreduce(sendbuf,
                                    recvbuf,
                                    count,
                                    datatype,
                                    op,
                                    MPI_COMM_WORLD,
                                    &request);
    if (mpi_result != MPI_SUCCESS) {
        g_printerr("[peak] MPI_Iallreduce for %s failed; abandoning MPI reducer\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }
    return peak_mpi_wait_collective_request(&request, label);
}

static gboolean
peak_mpi_reduce_checked(const void* sendbuf,
                        void* recvbuf,
                        int count,
                        MPI_Datatype datatype,
                        MPI_Op op,
                        int root,
                        const char* label)
{
    if (peak_mpi_reducer_forced_failure(label)) {
        g_printerr("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }

    MPI_Request request = MPI_REQUEST_NULL;
    int mpi_result = MPI_Ireduce(sendbuf,
                                 recvbuf,
                                 count,
                                 datatype,
                                 op,
                                 root,
                                 MPI_COMM_WORLD,
                                 &request);
    if (mpi_result != MPI_SUCCESS) {
        g_printerr("[peak] MPI_Ireduce for %s failed; abandoning MPI reducer\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }
    return peak_mpi_wait_collective_request(&request, label);
}

static void
peak_general_listener_print_rank_local_result(gulong* sum_num_calls,
                                              gdouble* sum_total_time,
                                              gdouble* max_total_time,
                                              gdouble* min_total_time,
                                              gdouble* sum_exclusive_time,
                                              gfloat* sum_max_time,
                                              gfloat* sum_min_time,
                                              gulong* thread_count)
{
    peak_general_listener_print_result(sum_num_calls,
                                       sum_total_time,
                                       max_total_time,
                                       min_total_time,
                                       sum_exclusive_time,
                                       sum_max_time,
                                       sum_min_time,
                                       thread_count,
                                       1);
}

static gboolean
peak_general_listener_reduce_result(gulong* sum_num_calls,
                                    gdouble* sum_total_time,
                                    gdouble* max_total_time,
                                    gdouble* min_total_time,
                                    gdouble* sum_exclusive_time,
                                    gfloat* sum_max_time,
                                    gfloat* sum_min_time,
                                    gulong* thread_count)
{
    /*
     * The MPI reducer only runs after peak.c has proven that every rank reached
     * the finalizer path. Its collectives use bounded nonblocking wrappers so a
     * failed reducer can fall back to local output, but MPI_Test is still an MPI
     * progress call. For failure-prone teardown or jobs where MPI progress may
     * be compromised, use PEAK_OUTPUT_AGGREGATION=socket.
     */
    int rank, size;
    int init_flag;
    gulong local_hook_count;
    gulong min_hook_count = 0;
    gulong max_hook_count = 0;
    int local_duplicate_names;
    int any_duplicate_names = 0;
    uint64_t* slot_hashes = NULL;
    uint64_t* min_slot_hashes = NULL;
    uint64_t* max_slot_hashes = NULL;
    gboolean slot_identity_mismatch = FALSE;
    gulong* mpi_sum_num_calls = NULL;
    gdouble* mpi_sum_total_time = NULL;
    gdouble* mpi_max_total_time = NULL;
    gdouble* mpi_min_total_time = NULL;
    gdouble* mpi_sum_exclusive_time = NULL;
    gfloat* mpi_sum_max_time = NULL;
    gfloat* mpi_sum_min_time = NULL;
    gulong* mpi_thread_count = NULL;
    gboolean* mpi_array_listener_detached = NULL;
    gboolean* mpi_array_listener_reattached = NULL;
    MPI_Initialized(&init_flag);
    if (!init_flag)
        MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    local_hook_count = (gulong)peak_hook_address_count;
    if (!peak_mpi_allreduce_checked(&local_hook_count,
                                    &min_hook_count,
                                    1,
                                    MPI_UNSIGNED_LONG,
                                    MPI_MIN,
                                    "hook-count-min")) {
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(&local_hook_count,
                                    &max_hook_count,
                                    1,
                                    MPI_UNSIGNED_LONG,
                                    MPI_MAX,
                                    "hook-count-max")) {
        goto rank_local;
    }
    local_duplicate_names =
        peak_general_listener_has_duplicate_slot_names() ? 1 : 0;
    if (!peak_mpi_allreduce_checked(&local_duplicate_names,
                                    &any_duplicate_names,
                                    1,
                                    MPI_INT,
                                    MPI_MAX,
                                    "duplicate-hook-name-check")) {
        goto rank_local;
    }
    if (any_duplicate_names) {
        if (rank == 0) {
            g_printerr("[peak] MPI output contains duplicate hook names, "
                       "likely from multiple JIT generations; writing "
                       "rank-local PEAK output\n");
        }
        peak_general_listener_print_rank_local_result(sum_num_calls,
                                                      sum_total_time,
                                                      max_total_time,
                                                      min_total_time,
                                                      sum_exclusive_time,
                                                      sum_max_time,
                                                      sum_min_time,
                                                      thread_count);
        return FALSE;
    }
    if (min_hook_count != max_hook_count) {
        if (rank == 0) {
            g_printerr("[peak] MPI ranks observed different JIT hook counts "
                       "(min=%lu max=%lu); writing rank-local PEAK output\n",
                       (unsigned long)min_hook_count,
                       (unsigned long)max_hook_count);
        }
        peak_general_listener_print_rank_local_result(sum_num_calls,
                                                      sum_total_time,
                                                      max_total_time,
                                                      min_total_time,
                                                      sum_exclusive_time,
                                                      sum_max_time,
                                                      sum_min_time,
                                                      thread_count);
        return FALSE;
    }
    slot_hashes = g_new0(uint64_t, peak_hook_address_count);
    min_slot_hashes = g_new0(uint64_t, peak_hook_address_count);
    max_slot_hashes = g_new0(uint64_t, peak_hook_address_count);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        slot_hashes[i] = peak_general_listener_slot_identity_hash(i);
    }
    if (!peak_mpi_allreduce_checked(slot_hashes,
                                    min_slot_hashes,
                                    peak_hook_address_count,
                                    MPI_UNSIGNED_LONG_LONG,
                                    MPI_MIN,
                                    "hook-slot-min-hash")) {
        g_free(max_slot_hashes);
        g_free(min_slot_hashes);
        g_free(slot_hashes);
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(slot_hashes,
                                    max_slot_hashes,
                                    peak_hook_address_count,
                                    MPI_UNSIGNED_LONG_LONG,
                                    MPI_MAX,
                                    "hook-slot-max-hash")) {
        g_free(max_slot_hashes);
        g_free(min_slot_hashes);
        g_free(slot_hashes);
        goto rank_local;
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (min_slot_hashes[i] != max_slot_hashes[i]) {
            slot_identity_mismatch = TRUE;
            break;
        }
    }
    g_free(max_slot_hashes);
    g_free(min_slot_hashes);
    g_free(slot_hashes);
    if (slot_identity_mismatch) {
        if (rank == 0) {
            g_printerr("[peak] MPI ranks observed different JIT hook slot "
                       "identities; writing rank-local PEAK output\n");
        }
        peak_general_listener_print_rank_local_result(sum_num_calls,
                                                      sum_total_time,
                                                      max_total_time,
                                                      min_total_time,
                                                      sum_exclusive_time,
                                                      sum_max_time,
                                                      sum_min_time,
                                                      thread_count);
        return FALSE;
    }
    mpi_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    mpi_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    mpi_max_total_time = g_new0(gdouble, peak_hook_address_count);
    mpi_min_total_time = g_new0(gdouble, peak_hook_address_count);
    mpi_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    mpi_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    mpi_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    mpi_thread_count = g_new0(gulong, peak_hook_address_count);
    mpi_array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    mpi_array_listener_reattached = g_new0(gboolean, peak_hook_address_count);
    if (!peak_mpi_reduce_checked(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, "sum-num-calls") ||
        !peak_mpi_reduce_checked(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, "sum-total-time") ||
        !peak_mpi_reduce_checked(max_total_time, mpi_max_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MAX, 0, "max-total-time") ||
        !peak_mpi_reduce_checked(min_total_time, mpi_min_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MIN, 0, "min-total-time") ||
        !peak_mpi_reduce_checked(sum_exclusive_time, mpi_sum_exclusive_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, "sum-exclusive-time") ||
        !peak_mpi_reduce_checked(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, "sum-max-time") ||
        !peak_mpi_reduce_checked(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, "sum-min-time") ||
        !peak_mpi_reduce_checked(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, "thread-count") ||
        !peak_mpi_reduce_checked(array_listener_detached, mpi_array_listener_detached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, "detached-marker") ||
        !peak_mpi_reduce_checked(array_listener_reattached, mpi_array_listener_reattached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, "reattached-marker")) {
        g_free(mpi_sum_num_calls);
        g_free(mpi_sum_total_time);
        g_free(mpi_max_total_time);
        g_free(mpi_min_total_time);
        g_free(mpi_sum_exclusive_time);
        g_free(mpi_sum_max_time);
        g_free(mpi_sum_min_time);
        g_free(mpi_thread_count);
        g_free(mpi_array_listener_detached);
        g_free(mpi_array_listener_reattached);
        goto rank_local;
    }
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
    array_listener_detached = mpi_array_listener_detached;
    array_listener_reattached = mpi_array_listener_reattached;
    if (rank == 0) {
        peak_general_listener_print_result(mpi_sum_num_calls,
                                           mpi_sum_total_time,
                                           mpi_max_total_time,
                                           mpi_min_total_time,
                                           mpi_sum_exclusive_time,
                                           mpi_sum_max_time,
                                           mpi_sum_min_time,
                                           mpi_thread_count, size);
    }
    g_free(mpi_sum_num_calls);
    g_free(mpi_sum_total_time);
    g_free(mpi_max_total_time);
    g_free(mpi_min_total_time);
    g_free(mpi_sum_exclusive_time);
    g_free(mpi_sum_max_time);
    g_free(mpi_sum_min_time);
    g_free(mpi_thread_count);
    return TRUE;

rank_local:
    if (peak_general_listener_mpi_reducer_failed_closed() &&
        peak_socket_reduce_fallback_enabled()) {
        g_printerr("[peak] MPI reducer failed; trying PEAK-owned socket aggregation fallback without further MPI calls\n");
        if (peak_general_listener_socket_reduce_result_with_rank_source(
                sum_num_calls,
                sum_total_time,
                max_total_time,
                min_total_time,
                sum_exclusive_time,
                sum_max_time,
                sum_min_time,
                thread_count,
                FALSE)) {
            return FALSE;
        }
        g_printerr("[peak] MPI reducer socket fallback failed; writing rank-local output\n");
    }
    peak_general_listener_print_rank_local_result(sum_num_calls,
                                                  sum_total_time,
                                                  max_total_time,
                                                  min_total_time,
                                                  sum_exclusive_time,
                                                  sum_max_time,
                                                  sum_min_time,
                                                  thread_count);
    return FALSE;
}
#endif

gboolean peak_general_listener_print(PeakOutputAggregationMode aggregation_mode)
{
    gboolean used_mpi_aggregation = FALSE;

    atomic_store_explicit(&peak_general_mpi_reducer_failed_closed,
                          FALSE,
                          memory_order_release);

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
            PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                size_t index = j;
                sum_num_calls[i] += pg_listener->num_calls[index];
                sum_total_time[i] += pg_listener->total_time[index];
                sum_exclusive_time[i] += pg_listener->exclusive_time[index];
                if (pg_listener->num_calls[index] != 0) {
                    thread_count[i]++;
                    if (pg_listener->total_time[index] > max_total_time[i])
                        max_total_time[i] = pg_listener->total_time[index];
                    if (pg_listener->total_time[index] < min_total_time[i] || thread_count[i] == 1)
                        min_total_time[i] = pg_listener->total_time[index];
                    if (pg_listener->max_time[index] > sum_max_time[i])
                        sum_max_time[i] = pg_listener->max_time[index];
                    if (pg_listener->min_time[index] < sum_min_time[i] || thread_count[i] == 1)
                        sum_min_time[i] = pg_listener->min_time[index];
                }
            }
            if (thread_count[i] == 0)
                thread_count[i] = 1;
        }
    }
#ifdef HAVE_MPI
    if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI) {
        used_mpi_aggregation =
            peak_general_listener_reduce_result(sum_num_calls,
                                                sum_total_time,
                                                max_total_time,
                                                min_total_time,
                                                sum_exclusive_time,
                                                sum_max_time,
                                                sum_min_time,
                                                thread_count);
    } else if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET) {
        if (!peak_general_listener_socket_reduce_result(sum_num_calls,
                                                       sum_total_time,
                                                       max_total_time,
                                                       min_total_time,
                                                       sum_exclusive_time,
                                                       sum_max_time,
                                                       sum_min_time,
                                                       thread_count) &&
            peak_socket_reduce_fallback_enabled()) {
            g_printerr("[peak] Socket aggregation failed; falling back to rank-local output\n");
            peak_general_listener_print_result(sum_num_calls,
                                              sum_total_time,
                                              max_total_time,
                                              min_total_time,
                                              sum_exclusive_time,
                                              sum_max_time,
                                              sum_min_time,
                                              thread_count,
                                              1);
        }
    } else {
        peak_general_listener_print_result(sum_num_calls,
                                           sum_total_time,
                                           max_total_time,
                                           min_total_time,
                                           sum_exclusive_time,
                                           sum_max_time,
                                           sum_min_time,
                                           thread_count, 1);
    }
#else
    peak_general_listener_print_result(sum_num_calls,
                                       sum_total_time,
                                       max_total_time,
                                       min_total_time,
                                       sum_exclusive_time,
                                       sum_max_time,
                                       sum_min_time,
                                       thread_count, 1);
#endif
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
    peak_general_listener_free_demangled_strings();
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
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
    g_free(peak_hook_retry_count);
    g_free(peak_hook_last_retry_status);
    g_free(array_listener);

    interceptor = NULL;
    hook_address = NULL;
    array_listener_detached = NULL;
    array_listener_reattached = NULL;
    array_listener_gum_detached = NULL;
    array_listener_gum_detach_flushed = NULL;
    peak_hook_last_detach_time = NULL;
    peak_hook_states = NULL;
    peak_hook_next_retry_time = NULL;
    peak_hook_pending_observed_time = NULL;
    peak_hook_pending_request_source = NULL;
    peak_hook_pending_request_calls = NULL;
    peak_hook_pending_request_ratio = NULL;
    peak_hook_pending_request_global_overhead = NULL;
    peak_hook_pending_request_total_time = NULL;
    peak_hook_pending_request_rate = NULL;
    peak_hook_retry_count = NULL;
    peak_hook_last_retry_status = NULL;
    array_listener = NULL;

    return TRUE;
}
