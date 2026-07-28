#define _GNU_SOURCE
#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>
#endif

#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "detach_controller.h"
#include "logging.h"
#include "internal/general_listener/attach_policy.h"
#include "internal/gum_module_mutation.h"
#include "internal/unsafe_gum_prologue.h"
#include "utils/source_target.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef void (*fn_void)(void);

#define PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY 256U
#define PEAK_DLOPEN_DYNAMIC_ATTACH_DRAIN_BUDGET 64U
#define PEAK_DLOPEN_SHUTDOWN_DRAIN_TIMEOUT_MS 5000L
#define PEAK_DLOPEN_PREPARE_RETRY_ATTEMPTS 50U
#define PEAK_DLOPEN_PREPARE_RETRY_SLEEP_NS 1000000L

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* dlopen_interceptor;
static GumInvocationListener* dlopen_listener;
static gboolean* dlopen_fftw_scope_targets = NULL;
static size_t dlopen_fftw_scope_target_count = 0;
static _Atomic size_t dlopen_unresolved_fftw_count = 0;
static _Atomic gboolean dlopen_may_have_unresolved_non_fftw = FALSE;
static _Atomic pid_t dlopen_listener_owner_pid = 0;
extern GumInterceptor* interceptor;
extern GumInvocationListener** array_listener;
extern gpointer* hook_address;
static gpointer* dlopen_hook_address = NULL;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
extern char** peak_demangled_strings;

typedef enum {
    PEAK_DLOPEN_CONTROLLER_CLOSED = 0,
    PEAK_DLOPEN_CONTROLLER_OPEN,
    PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN
} PeakDlopenControllerState;

typedef enum {
    PEAK_DLOPEN_ATTACH_ALL = 0,
    PEAK_DLOPEN_ATTACH_FFTW_ONLY,
    PEAK_DLOPEN_ATTACH_NON_FFTW_ONLY
} PeakDlopenAttachScope;

typedef struct {
    void* handle;
    char* filename;
    int binding_flags;
    PeakDlopenAttachScope scope;
    gpointer module_token;
    gboolean handle_retained;
} PeakDlopenDynamicAttachRequest;

typedef struct {
    const char* name;
    gpointer address;
} PeakDlopenResolvedTarget;

typedef struct {
    GumInvocationListener* listener;
    char* demangled_name;
    PeakGumTargetAttachPlan attach_plan;
} PeakDlopenAttachCandidate;

#ifdef PEAK_ENABLE_TEST_HOOKS
static char peak_dlopen_test_retry_handle_marker;
static char peak_dlopen_test_retained_handle_marker;
#define PEAK_DLOPEN_TEST_RETRY_HANDLE \
    ((void*)&peak_dlopen_test_retry_handle_marker)
#define PEAK_DLOPEN_TEST_RETAINED_HANDLE \
    ((void*)&peak_dlopen_test_retained_handle_marker)
#endif

typedef enum {
    PEAK_DLOPEN_ATTACH_DONE = 0,
    PEAK_DLOPEN_ATTACH_RETRY
} PeakDlopenAttachResult;

static pthread_mutex_t dynamic_attach_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dynamic_attach_gate_cond = PTHREAD_COND_INITIALIZER;
static PeakDlopenControllerState dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
static unsigned int active_dynamic_attach_count = 0;
static unsigned int active_dlopen_callback_count = 0;
static gboolean dynamic_attach_drain_active = FALSE;
static PeakDlopenDynamicAttachRequest dynamic_attach_queue[PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY];
static size_t dynamic_attach_queue_head = 0;
static size_t dynamic_attach_queue_tail = 0;
static size_t dynamic_attach_queue_length = 0;
static gboolean dynamic_attach_queue_overflow_reported = FALSE;
static GPtrArray* dynamic_attach_retained_handles = NULL;
static GHashTable* dlopen_completed_fftw_modules = NULL;
static __thread gboolean dynamic_attach_drain_reentrant = FALSE;
static unsigned long long dynamic_attach_enqueue_count = 0;
static unsigned long long dynamic_attach_drain_count = 0;
static unsigned long long dynamic_attach_requeue_count = 0;
static unsigned long long dynamic_attach_drop_full_count = 0;
static unsigned long long dynamic_attach_drop_closed_count = 0;
static unsigned long long dynamic_attach_drop_noload_count = 0;
static unsigned long long dynamic_attach_drop_requeue_count = 0;
static unsigned long long dynamic_attach_partial_success_count = 0;
static unsigned long long dynamic_attach_retained_handle_count = 0;
static size_t dynamic_attach_queue_max_depth = 0;
static gsize dlopen_runtime_config_initialized = 0;
static gboolean configured_dlopen_debug_enabled = FALSE;
static gchar* configured_dlopen_trace_path = NULL;
#ifdef PEAK_ENABLE_TEST_HOOKS
static gboolean dynamic_attach_test_manual_drain = FALSE;
static __thread gboolean dynamic_attach_test_explicit_drain = FALSE;
static pthread_mutex_t dynamic_attach_test_callback_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dynamic_attach_test_callback_cond =
    PTHREAD_COND_INITIALIZER;
static gboolean dynamic_attach_test_hold_callback = FALSE;
static gboolean dynamic_attach_test_callback_is_waiting = FALSE;
static gboolean dynamic_attach_test_hold_drain = FALSE;
static gboolean dynamic_attach_test_drain_is_waiting = FALSE;
static gboolean dynamic_attach_test_callback_waiting_for_drain = FALSE;
#endif

static gboolean
dlopen_interceptor_parse_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static void
dlopen_interceptor_init_runtime_config_once(void)
{
    const char* trace_path = g_getenv("PEAK_DLOPEN_TRACE_PATH");

    configured_dlopen_debug_enabled =
        dlopen_interceptor_parse_truthy(g_getenv("PEAK_DLOPEN_DEBUG"));
    if (trace_path != NULL && trace_path[0] != '\0') {
        configured_dlopen_trace_path = g_strdup(trace_path);
    }
}

static void
dlopen_interceptor_init_runtime_config(void)
{
    if (g_once_init_enter(&dlopen_runtime_config_initialized)) {
        dlopen_interceptor_init_runtime_config_once();
        g_once_init_leave(&dlopen_runtime_config_initialized, 1);
    }
}

static gboolean
dlopen_interceptor_debug_enabled(void)
{
    return configured_dlopen_debug_enabled;
}

typedef struct {
    size_t fftw;
    size_t non_fftw;
} PeakDlopenUnresolvedCounts;

static gboolean
dlopen_interceptor_is_fftw_group_symbol(const char* name)
{
    if (name == NULL) {
        return FALSE;
    }

    for (size_t i = 0; i < source_count_FFTW; i++) {
        if (strcmp(name, source_target_array_FFTW[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
dlopen_interceptor_target_is_unresolved_unlocked(size_t index);

static void
dlopen_interceptor_reset_fftw_target_scope(void)
{
    g_free(dlopen_fftw_scope_targets);
    dlopen_fftw_scope_targets = NULL;
    dlopen_fftw_scope_target_count = 0;
    atomic_store_explicit(&dlopen_unresolved_fftw_count,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&dlopen_may_have_unresolved_non_fftw,
                          FALSE,
                          memory_order_relaxed);
}

static void
dlopen_interceptor_initialize_fftw_target_scope(void)
{
    dlopen_interceptor_reset_fftw_target_scope();

    dlopen_fftw_scope_target_count = peak_hook_address_count;
    dlopen_fftw_scope_targets =
        g_new0(gboolean, dlopen_fftw_scope_target_count);
    size_t unresolved_fftw = 0;
    size_t unresolved_non_fftw = 0;
    for (size_t i = 0; i < dlopen_fftw_scope_target_count; i++) {
        gboolean is_fftw =
            dlopen_interceptor_is_fftw_group_symbol(peak_hook_strings[i]);
        if (is_fftw) {
            dlopen_fftw_scope_targets[i] = TRUE;
        }
        if (dlopen_interceptor_target_is_unresolved_unlocked(i)) {
            if (is_fftw) {
                unresolved_fftw++;
            } else {
                unresolved_non_fftw++;
            }
        }
    }
    atomic_store_explicit(&dlopen_unresolved_fftw_count,
                          unresolved_fftw,
                          memory_order_relaxed);
    atomic_store_explicit(&dlopen_may_have_unresolved_non_fftw,
                          unresolved_non_fftw > 0,
                          memory_order_relaxed);
}

static gboolean
dlopen_interceptor_target_is_unresolved_unlocked(size_t index)
{
    const char* name;

    if (index >= peak_hook_address_count || hook_address[index] != NULL ||
        array_listener[index] != NULL || peak_hook_strings[index] == NULL ||
        peak_demangled_strings[index] != NULL) {
        return FALSE;
    }

    name = peak_hook_strings[index];
    return strcmp(name, "main") != 0 && strcmp(name, "dlopen") != 0;
}

static gboolean
dlopen_interceptor_target_matches_scope_unlocked(
    size_t index,
    PeakDlopenAttachScope scope)
{
    gboolean is_fftw =
        index < dlopen_fftw_scope_target_count &&
        dlopen_fftw_scope_targets != NULL &&
        dlopen_fftw_scope_targets[index];

    switch (scope) {
        case PEAK_DLOPEN_ATTACH_FFTW_ONLY:
            return is_fftw;
        case PEAK_DLOPEN_ATTACH_NON_FFTW_ONLY:
            return !is_fftw;
        case PEAK_DLOPEN_ATTACH_ALL:
        default:
            return TRUE;
    }
}

static PeakDlopenUnresolvedCounts
dlopen_interceptor_unresolved_counts(void)
{
    return (PeakDlopenUnresolvedCounts) {
        .fftw = atomic_load_explicit(&dlopen_unresolved_fftw_count,
                                     memory_order_relaxed),
        .non_fftw = atomic_load_explicit(
            &dlopen_may_have_unresolved_non_fftw,
            memory_order_relaxed) ? 1 : 0
    };
}

static gboolean
dlopen_interceptor_fftw_module_scan_completed(void* handle)
{
    gpointer module_token = NULL;
    gboolean completed = FALSE;

    if (handle == NULL) {
        return FALSE;
    }
#if defined(__linux__)
    struct link_map* module_map = NULL;

    peak_general_listener_fast_ignore_current_thread();
    if (dlopen_interceptor != NULL) {
        gum_interceptor_ignore_current_thread(dlopen_interceptor);
    }
    if (dlinfo(handle, RTLD_DI_LINKMAP, &module_map) == 0) {
        module_token = module_map;
    }
    if (dlopen_interceptor != NULL) {
        gum_interceptor_unignore_current_thread(dlopen_interceptor);
    }
    peak_general_listener_fast_unignore_current_thread();
#else
    module_token = handle;
#endif
    if (module_token == NULL) {
        return FALSE;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dlopen_completed_fftw_modules != NULL) {
        completed = g_hash_table_contains(dlopen_completed_fftw_modules,
                                          module_token);
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return completed;
}

static void
dlopen_interceptor_refresh_unresolved_non_fftw_unlocked(void)
{
    gboolean unresolved_non_fftw = FALSE;

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (dlopen_interceptor_target_is_unresolved_unlocked(i) &&
            dlopen_interceptor_target_matches_scope_unlocked(
                i,
                PEAK_DLOPEN_ATTACH_NON_FFTW_ONLY)) {
            unresolved_non_fftw = TRUE;
            break;
        }
    }
    atomic_store_explicit(&dlopen_may_have_unresolved_non_fftw,
                          unresolved_non_fftw,
                          memory_order_relaxed);
}

static void
dlopen_interceptor_mark_target_resolved_unlocked(size_t hook_id)
{
    if (hook_id >= dlopen_fftw_scope_target_count) {
        return;
    }

    if (dlopen_fftw_scope_targets == NULL ||
        !dlopen_fftw_scope_targets[hook_id]) {
        return;
    }

    if (atomic_load_explicit(&dlopen_unresolved_fftw_count,
                             memory_order_relaxed) > 0) {
        atomic_fetch_sub_explicit(&dlopen_unresolved_fftw_count,
                                  1,
                                  memory_order_relaxed);
    }
}

static void
dlopen_interceptor_snapshot_counters(unsigned long long* enqueued,
                                     unsigned long long* drained,
                                     unsigned long long* requeued,
                                     unsigned long long* dropped_full,
                                     unsigned long long* dropped_closed,
                                     unsigned long long* dropped_noload,
                                     unsigned long long* dropped_requeue,
                                     unsigned long long* partial_success,
                                     unsigned long long* retained_handles,
                                     size_t* max_depth,
                                     size_t* queue_length,
                                     unsigned int* capacity,
                                     unsigned int* drain_budget)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    *enqueued = dynamic_attach_enqueue_count;
    *drained = dynamic_attach_drain_count;
    *requeued = dynamic_attach_requeue_count;
    *dropped_full = dynamic_attach_drop_full_count;
    *dropped_closed = dynamic_attach_drop_closed_count;
    *dropped_noload = dynamic_attach_drop_noload_count;
    *dropped_requeue = dynamic_attach_drop_requeue_count;
    *partial_success = dynamic_attach_partial_success_count;
    *retained_handles = dynamic_attach_retained_handle_count;
    *max_depth = dynamic_attach_queue_max_depth;
    *queue_length = dynamic_attach_queue_length;
    *capacity = PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
    *drain_budget = PEAK_DLOPEN_DYNAMIC_ATTACH_DRAIN_BUDGET;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

void
dlopen_interceptor_get_dynamic_attach_diagnostics(
    PeakDlopenDynamicAttachDiagnostics* diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    diagnostics->enqueued = dynamic_attach_enqueue_count;
    diagnostics->drained = dynamic_attach_drain_count;
    diagnostics->requeued = dynamic_attach_requeue_count;
    diagnostics->dropped_full = dynamic_attach_drop_full_count;
    diagnostics->dropped_closed = dynamic_attach_drop_closed_count;
    diagnostics->dropped_noload = dynamic_attach_drop_noload_count;
    diagnostics->dropped_requeue = dynamic_attach_drop_requeue_count;
    diagnostics->partial_success = dynamic_attach_partial_success_count;
    diagnostics->retained_handles = dynamic_attach_retained_handle_count;
    diagnostics->max_depth = dynamic_attach_queue_max_depth;
    diagnostics->queue_length = dynamic_attach_queue_length;
    diagnostics->capacity = PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
    diagnostics->drain_budget = PEAK_DLOPEN_DYNAMIC_ATTACH_DRAIN_BUDGET;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static void
dlopen_interceptor_trace_counters(const char* event)
{
    const char* path = configured_dlopen_trace_path;
    gboolean debug = dlopen_interceptor_debug_enabled();
    unsigned long long enqueued;
    unsigned long long drained;
    unsigned long long requeued;
    unsigned long long dropped_full;
    unsigned long long dropped_closed;
    unsigned long long dropped_noload;
    unsigned long long dropped_requeue;
    unsigned long long partial_success;
    unsigned long long retained_handles;
    size_t max_depth;
    size_t queue_length;
    unsigned int capacity;
    unsigned int drain_budget;

    if ((path == NULL || path[0] == '\0') && !debug) {
        return;
    }

    dlopen_interceptor_snapshot_counters(&enqueued,
                                         &drained,
                                         &requeued,
                                         &dropped_full,
                                         &dropped_closed,
                                         &dropped_noload,
                                         &dropped_requeue,
                                         &partial_success,
                                         &retained_handles,
                                         &max_depth,
                                         &queue_length,
                                         &capacity,
                                         &drain_budget);

    if (path != NULL && path[0] != '\0') {
        FILE* fp = fopen(path, "a");
        if (fp != NULL) {
            fprintf(fp,
                    "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%u,%u\n",
                    event != NULL ? event : "snapshot",
                    enqueued,
                    drained,
                    requeued,
                    dropped_full,
                    dropped_closed,
                    dropped_noload,
                    dropped_requeue,
                    partial_success,
                    retained_handles,
                    (unsigned long)max_depth,
                    (unsigned long)queue_length,
                    capacity,
                    drain_budget);
            fclose(fp);
        }
    }

    if (debug) {
        peak_log_debug("[peak] dlopen dynamic attach diagnostics event=%s enqueued=%llu drained=%llu requeued=%llu dropped_full=%llu dropped_closed=%llu dropped_noload=%llu dropped_requeue=%llu partial_success=%llu retained_handles=%llu max_depth=%lu queue_length=%lu capacity=%u drain_budget=%u\n",
                   event != NULL ? event : "snapshot",
                   enqueued,
                   drained,
                   requeued,
                   dropped_full,
                   dropped_closed,
                   dropped_noload,
                   dropped_requeue,
                   partial_success,
                   retained_handles,
                   (unsigned long)max_depth,
                   (unsigned long)queue_length,
                   capacity,
                   drain_budget);
    }
}

static gboolean
dlopen_interceptor_dynamic_attach_prepare_is_retryable(PeakDetachStatus status)
{
    switch (status) {
        case PEAK_DETACH_STATUS_TIMEOUT:
        case PEAK_DETACH_STATUS_CLASSIFY_FAILED:
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

static gboolean
dlopen_interceptor_revert_prepare_is_retryable(PeakDetachStatus status)
{
    return dlopen_interceptor_dynamic_attach_prepare_is_retryable(status) ||
           status == PEAK_DETACH_STATUS_PERMISSION_DENIED ||
           status == PEAK_DETACH_STATUS_ERROR;
}

static gboolean
dlopen_interceptor_prepare_hook_mutation_with_retry(
    const PeakDetachRequest* request,
    PeakDetachStatus* status_out)
{
    struct timespec retry_sleep = {
        .tv_sec = 0,
        .tv_nsec = PEAK_DLOPEN_PREPARE_RETRY_SLEEP_NS
    };

    for (unsigned int attempt = 0;
         attempt < PEAK_DLOPEN_PREPARE_RETRY_ATTEMPTS;
         attempt++) {
        PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

        if (peak_detach_controller_prepare_hook_mutation(request, &status)) {
            if (status_out != NULL) {
                *status_out = status;
            }
            return TRUE;
        }
        if (status_out != NULL) {
            *status_out = status;
        }
        if (!dlopen_interceptor_revert_prepare_is_retryable(status)) {
            return FALSE;
        }

        nanosleep(&retry_sleep, NULL);
    }

    return FALSE;
}

static void
dlopen_interceptor_add_milliseconds(struct timespec* ts, long milliseconds)
{
    ts->tv_sec += milliseconds / 1000L;
    ts->tv_nsec += (milliseconds % 1000L) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void
dlopen_interceptor_close_retained_handle(gpointer handle)
{
    if (handle != NULL) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (handle == PEAK_DLOPEN_TEST_RETAINED_HANDLE) {
            return;
        }
#endif
        dlclose(handle);
    }
}

static gboolean
dlopen_interceptor_queue_can_accept_unlocked(void)
{
    return dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
           dynamic_attach_queue_length < PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
}

static PeakDlopenDynamicAttachRequest*
dlopen_interceptor_find_queued_request_unlocked(const char* filename,
                                                gpointer module_token)
{
    size_t index = dynamic_attach_queue_head;

    for (size_t offset = 0; offset < dynamic_attach_queue_length; offset++) {
        PeakDlopenDynamicAttachRequest* queued =
            &dynamic_attach_queue[index];

        if (module_token != NULL) {
            if (queued->module_token == module_token) {
                return queued;
            }
        } else if (queued->module_token == NULL &&
                   queued->filename != NULL &&
                   filename != NULL &&
                   strcmp(queued->filename, filename) == 0) {
            return queued;
        }
        index = (index + 1) % PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
    }
    return NULL;
}

static void
dlopen_interceptor_merge_queued_request_unlocked(
    PeakDlopenDynamicAttachRequest* queued,
    int binding_flags,
    PeakDlopenAttachScope scope)
{
    if (queued->scope != scope) {
        /*
         * ALL is the union of the two narrower scopes.  This also preserves
         * an already-broader request when a later callback needs less work.
         */
        queued->scope = PEAK_DLOPEN_ATTACH_ALL;
    }
    if ((binding_flags & RTLD_NOW) != 0) {
        queued->binding_flags &= ~RTLD_LAZY;
        queued->binding_flags |= RTLD_NOW;
    }
}

static gboolean
dlopen_interceptor_begin_callback(void)
{
    gboolean admitted;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    while (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
           atomic_load_explicit(&dlopen_listener_owner_pid,
                                memory_order_acquire) != 0 &&
           dynamic_attach_drain_active) {
        /*
         * The controller may perform symbol lookup only while callback
         * admission is closed. Waiting here completes the barrier in the
         * reverse direction: a drain cannot overlap a newly arriving
         * application dlopen callback.
         */
#ifdef PEAK_ENABLE_TEST_HOOKS
        dynamic_attach_test_callback_waiting_for_drain = TRUE;
#endif
        pthread_cond_wait(&dynamic_attach_gate_cond,
                          &dynamic_attach_gate_mutex);
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    dynamic_attach_test_callback_waiting_for_drain = FALSE;
#endif
    admitted = dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
        atomic_load_explicit(&dlopen_listener_owner_pid,
                             memory_order_acquire) != 0;
    if (admitted) {
        active_dlopen_callback_count++;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return admitted;
}

static gboolean
dlopen_interceptor_end_callback(void)
{
    gboolean should_wake;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (active_dlopen_callback_count > 0) {
        active_dlopen_callback_count--;
    }
    should_wake =
        dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
        active_dlopen_callback_count == 0 &&
        dynamic_attach_queue_length != 0;
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_OPEN) {
        pthread_cond_broadcast(&dynamic_attach_gate_cond);
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return should_wake;
}

static unsigned int
dlopen_interceptor_active_callback_count(void)
{
    unsigned int count;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    count = active_dlopen_callback_count;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    return count;
}

static gboolean
dlopen_interceptor_wait_for_dynamic_attach_idle(void)
{
    struct timespec deadline;
    gboolean idle = TRUE;

    clock_gettime(CLOCK_REALTIME, &deadline);
    dlopen_interceptor_add_milliseconds(&deadline,
                                        PEAK_DLOPEN_SHUTDOWN_DRAIN_TIMEOUT_MS);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    while (active_dynamic_attach_count > 0) {
        int wait_status =
            pthread_cond_timedwait(&dynamic_attach_gate_cond,
                                   &dynamic_attach_gate_mutex,
                                   &deadline);
        if (wait_status == ETIMEDOUT) {
            idle = FALSE;
            break;
        }
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    return idle;
}

static gboolean
dlopen_interceptor_wait_for_callbacks_idle(void)
{
    struct timespec deadline;
    gboolean idle = TRUE;

    clock_gettime(CLOCK_REALTIME, &deadline);
    dlopen_interceptor_add_milliseconds(&deadline,
                                        PEAK_DLOPEN_SHUTDOWN_DRAIN_TIMEOUT_MS);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    while (active_dlopen_callback_count > 0) {
        int wait_status =
            pthread_cond_timedwait(&dynamic_attach_gate_cond,
                                   &dynamic_attach_gate_mutex,
                                   &deadline);
        if (wait_status == ETIMEDOUT) {
            idle = FALSE;
            break;
        }
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    return idle;
}

static void
dlopen_interceptor_retain_dynamic_handle(
    PeakDlopenDynamicAttachRequest* request,
    gboolean completed_fftw_scan)
{
    if (request == NULL || request->handle == NULL) {
        return;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (!request->handle_retained) {
        if (dynamic_attach_retained_handles == NULL) {
            dynamic_attach_retained_handles =
                g_ptr_array_new_with_free_func(
                    dlopen_interceptor_close_retained_handle);
        }
        g_ptr_array_add(dynamic_attach_retained_handles, request->handle);
        request->handle_retained = TRUE;
        dynamic_attach_retained_handle_count++;
    }
    if (completed_fftw_scan && request->module_token != NULL) {
        if (dlopen_completed_fftw_modules == NULL) {
            dlopen_completed_fftw_modules =
                g_hash_table_new(g_direct_hash, g_direct_equal);
        }
        g_hash_table_add(dlopen_completed_fftw_modules,
                         request->module_token);
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static gboolean
dlopen_interceptor_loaded_module_identity(void* handle,
                                          const char* fallback_filename,
                                          char** filename_out,
                                          gpointer* module_token_out,
                                          long* namespace_id_out)
{
    const char* identity = fallback_filename;
    gpointer module_token = NULL;
    size_t identity_length;

    if (handle == NULL || filename_out == NULL ||
        module_token_out == NULL || namespace_id_out == NULL) {
        return FALSE;
    }

#if defined(__linux__)
    struct link_map* module_map = NULL;
    Lmid_t namespace_id;

    if (dlinfo(handle, RTLD_DI_LINKMAP, &module_map) != 0 ||
        module_map == NULL ||
        dlinfo(handle, RTLD_DI_LMID, &namespace_id) != 0) {
        return FALSE;
    }
    if (module_map->l_name != NULL && module_map->l_name[0] != '\0') {
        identity = module_map->l_name;
    }
    module_token = module_map;
    *namespace_id_out = (long)namespace_id;
#else
    module_token = handle;
    *namespace_id_out = 0;
#endif

    if (identity == NULL || identity[0] == '\0') {
        return FALSE;
    }
    identity_length = strlen(identity);
    if (identity_length == G_MAXSIZE) {
        return FALSE;
    }
    *filename_out = g_try_malloc(identity_length + 1);
    if (*filename_out == NULL) {
        return FALSE;
    }
    memcpy(*filename_out, identity, identity_length + 1);
    *module_token_out = module_token;
    return TRUE;
}

static void*
dlopen_interceptor_duplicate_dynamic_handle_reference(
    void* application_handle,
    const char* fallback_filename,
    int binding_flags,
    char** filename_out,
    gpointer* module_token_out)
{
#ifdef RTLD_NOLOAD
    void* retained_handle = NULL;
    char* module_filename = NULL;
    gpointer module_token = NULL;
    long namespace_id;

    peak_general_listener_fast_ignore_current_thread();
    if (dlopen_interceptor != NULL) {
        gum_interceptor_ignore_current_thread(dlopen_interceptor);
    }
    if (!dlopen_interceptor_loaded_module_identity(
            application_handle,
            fallback_filename,
            &module_filename,
            &module_token,
            &namespace_id)) {
        if (dlopen_interceptor != NULL) {
            gum_interceptor_unignore_current_thread(dlopen_interceptor);
        }
        peak_general_listener_fast_unignore_current_thread();
        return NULL;
    }

#if defined(__linux__)
    retained_handle = dlmopen((Lmid_t)namespace_id,
                              module_filename,
                              binding_flags | RTLD_NOLOAD);
#else
    retained_handle = dlopen(module_filename,
                             binding_flags | RTLD_NOLOAD);
#endif

    if (retained_handle != NULL) {
        gpointer retained_token = NULL;
#if defined(__linux__)
        struct link_map* retained_map = NULL;

        if (dlinfo(retained_handle, RTLD_DI_LINKMAP, &retained_map) == 0) {
            retained_token = retained_map;
        }
#else
        retained_token = retained_handle;
#endif
        if (retained_token != module_token) {
            dlclose(retained_handle);
            retained_handle = NULL;
        }
    }
    if (dlopen_interceptor != NULL) {
        gum_interceptor_unignore_current_thread(dlopen_interceptor);
    }
    peak_general_listener_fast_unignore_current_thread();
    if (retained_handle == NULL) {
        g_free(module_filename);
        return NULL;
    }

    *filename_out = module_filename;
    *module_token_out = module_token;
    return retained_handle;
#else
    (void)application_handle;
    (void)fallback_filename;
    (void)binding_flags;
    (void)filename_out;
    (void)module_token_out;
    return NULL;
#endif
}

static void*
dlopen_interceptor_release_dynamic_attach_request_metadata(
    PeakDlopenDynamicAttachRequest* request)
{
    void* handle_to_close = NULL;

    if (request->handle != NULL && !request->handle_retained) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (request->handle != PEAK_DLOPEN_TEST_RETRY_HANDLE)
#endif
        {
            handle_to_close = request->handle;
        }
    }

    request->handle = NULL;
    g_free(request->filename);
    request->filename = NULL;
    request->binding_flags = 0;
    request->scope = PEAK_DLOPEN_ATTACH_ALL;
    request->module_token = NULL;
    request->handle_retained = FALSE;
    return handle_to_close;
}

static void
dlopen_interceptor_release_dynamic_attach_request(
    PeakDlopenDynamicAttachRequest* request)
{
    void* handle_to_close =
        dlopen_interceptor_release_dynamic_attach_request_metadata(request);

    if (handle_to_close != NULL) {
        dlclose(handle_to_close);
    }
}

static gboolean
dlopen_interceptor_flush_teardown(void)
{
    const unsigned int max_attempts = 100;

    if (dlopen_interceptor == NULL) {
        return TRUE;
    }

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++) {
        if (gum_interceptor_flush(dlopen_interceptor)) {
            return TRUE;
        }
        usleep(1000);
    }

    return gum_interceptor_flush(dlopen_interceptor);
}

static gboolean
dlopen_interceptor_begin_dynamic_attach_drain(size_t* initial_queue_length)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_OPEN ||
        dynamic_attach_drain_active ||
        active_dlopen_callback_count != 0 ||
        dynamic_attach_queue_length == 0) {
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        return FALSE;
    }

    if (initial_queue_length != NULL) {
        *initial_queue_length = dynamic_attach_queue_length;
    }
    dynamic_attach_drain_active = TRUE;
    active_dynamic_attach_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return TRUE;
}

static void
dlopen_interceptor_end_dynamic_attach_drain(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_drain_active = FALSE;
    if (active_dynamic_attach_count > 0) {
        active_dynamic_attach_count--;
    }
    /*
     * Wake callbacks that arrived after the drain closed admission.  Shutdown
     * waiters use the same condition and safely recheck their own predicate.
     */
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static void dlopen_interceptor_drain_dynamic_attach_queue_with_budget(size_t max_requests);

static gboolean
dlopen_interceptor_pop_dynamic_attach_request(PeakDlopenDynamicAttachRequest* request)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_queue_length == 0) {
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        return FALSE;
    }

    *request = dynamic_attach_queue[dynamic_attach_queue_head];
    dynamic_attach_queue[dynamic_attach_queue_head].handle = NULL;
    dynamic_attach_queue[dynamic_attach_queue_head].filename = NULL;
    dynamic_attach_queue[dynamic_attach_queue_head].binding_flags = 0;
    dynamic_attach_queue[dynamic_attach_queue_head].scope =
        PEAK_DLOPEN_ATTACH_ALL;
    dynamic_attach_queue[dynamic_attach_queue_head].module_token = NULL;
    dynamic_attach_queue[dynamic_attach_queue_head].handle_retained = FALSE;
    dynamic_attach_queue_head =
        (dynamic_attach_queue_head + 1) % PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
    dynamic_attach_queue_length--;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    return TRUE;
}

static gboolean
dlopen_interceptor_requeue_dynamic_attach_request(PeakDlopenDynamicAttachRequest* request)
{
    gboolean requeued = FALSE;

    if (request->handle == NULL) {
        return FALSE;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dlopen_interceptor_queue_can_accept_unlocked()) {
        dynamic_attach_queue[dynamic_attach_queue_tail] = *request;
        dynamic_attach_queue_tail =
            (dynamic_attach_queue_tail + 1) % PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
        dynamic_attach_queue_length++;
        dynamic_attach_requeue_count++;
        if (dynamic_attach_queue_length > dynamic_attach_queue_max_depth) {
            dynamic_attach_queue_max_depth = dynamic_attach_queue_length;
        }
        request->handle = NULL;
        request->filename = NULL;
        request->module_token = NULL;
        request->handle_retained = FALSE;
        requeued = TRUE;
    } else {
        dynamic_attach_drop_requeue_count++;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    return requeued;
}

static void
dlopen_interceptor_discard_dynamic_attach_queue(void)
{
    PeakDlopenDynamicAttachRequest request = { 0 };

    while (dlopen_interceptor_pop_dynamic_attach_request(&request)) {
        dlopen_interceptor_release_dynamic_attach_request(&request);
    }
}

static gboolean
dlopen_interceptor_enqueue_dynamic_attach_request(
    void* application_handle,
    const char* filename,
    int binding_flags,
    PeakDlopenAttachScope scope)
{
#ifdef RTLD_NOLOAD
    void* retained_handle;
    char* module_filename = NULL;
    gpointer module_token = NULL;
    PeakDlopenDynamicAttachRequest* queued;
    gboolean accepted = FALSE;

    if (application_handle == NULL || filename == NULL) {
        return FALSE;
    }
    /*
     * The real application dlopen has returned, but its result has not yet
     * been handed back to the caller. Pin that exact link-map now, while the
     * application's handle proves both loader namespace and object identity.
     * The controller must never try to reconstruct this identity later from a
     * caller-sensitive filename.
     */
    retained_handle =
        dlopen_interceptor_duplicate_dynamic_handle_reference(
            application_handle,
            filename,
            binding_flags,
            &module_filename,
            &module_token);
    if (retained_handle == NULL) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_drop_noload_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        return FALSE;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_OPEN) {
        dynamic_attach_drop_closed_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    } else if ((queued = dlopen_interceptor_find_queued_request_unlocked(
                    module_filename,
                    module_token)) != NULL) {
        dlopen_interceptor_merge_queued_request_unlocked(queued,
                                                         binding_flags,
                                                         scope);
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        accepted = TRUE;
    } else if (!dlopen_interceptor_queue_can_accept_unlocked()) {
        dynamic_attach_drop_full_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    } else {
        dynamic_attach_queue[dynamic_attach_queue_tail].handle =
            retained_handle;
        dynamic_attach_queue[dynamic_attach_queue_tail].filename =
            module_filename;
        dynamic_attach_queue[dynamic_attach_queue_tail].binding_flags =
            binding_flags;
        dynamic_attach_queue[dynamic_attach_queue_tail].scope = scope;
        dynamic_attach_queue[dynamic_attach_queue_tail].module_token =
            module_token;
        dynamic_attach_queue[dynamic_attach_queue_tail].handle_retained =
            FALSE;
        dynamic_attach_queue_tail =
            (dynamic_attach_queue_tail + 1) %
            PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
        dynamic_attach_queue_length++;
        dynamic_attach_enqueue_count++;
        if (dynamic_attach_queue_length > dynamic_attach_queue_max_depth) {
            dynamic_attach_queue_max_depth = dynamic_attach_queue_length;
        }
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        retained_handle = NULL;
        module_filename = NULL;
        accepted = TRUE;
    }

    if (retained_handle != NULL) {
        peak_general_listener_fast_ignore_current_thread();
        if (dlopen_interceptor != NULL) {
            gum_interceptor_ignore_current_thread(dlopen_interceptor);
        }
        dlclose(retained_handle);
        if (dlopen_interceptor != NULL) {
            gum_interceptor_unignore_current_thread(dlopen_interceptor);
        }
        peak_general_listener_fast_unignore_current_thread();
    }
    g_free(module_filename);
    return accepted;
#else
    (void)application_handle;
    (void)filename;
    (void)binding_flags;
    (void)scope;
    return FALSE;
#endif
}

#ifdef PEAK_ENABLE_TEST_HOOKS
void
dlopen_interceptor_test_reset_dynamic_attach(gboolean open)
{
    GPtrArray* retained_handles;
    GHashTable* completed_fftw_modules;

    dlopen_interceptor_discard_dynamic_attach_queue();

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state =
        open ? PEAK_DLOPEN_CONTROLLER_OPEN : PEAK_DLOPEN_CONTROLLER_CLOSED;
    active_dynamic_attach_count = 0;
    active_dlopen_callback_count = 0;
    dynamic_attach_drain_active = FALSE;
    dynamic_attach_queue_head = 0;
    dynamic_attach_queue_tail = 0;
    dynamic_attach_queue_length = 0;
    dynamic_attach_queue_overflow_reported = FALSE;
    dynamic_attach_test_callback_waiting_for_drain = FALSE;
    dynamic_attach_enqueue_count = 0;
    dynamic_attach_drain_count = 0;
    dynamic_attach_requeue_count = 0;
    dynamic_attach_drop_full_count = 0;
    dynamic_attach_drop_closed_count = 0;
    dynamic_attach_drop_noload_count = 0;
    dynamic_attach_drop_requeue_count = 0;
    dynamic_attach_partial_success_count = 0;
    dynamic_attach_retained_handle_count = 0;
    dynamic_attach_queue_max_depth = 0;
    atomic_store_explicit(&dlopen_listener_owner_pid,
                          open ? getpid() : 0,
                          memory_order_release);
    retained_handles = dynamic_attach_retained_handles;
    dynamic_attach_retained_handles = NULL;
    completed_fftw_modules = dlopen_completed_fftw_modules;
    dlopen_completed_fftw_modules = NULL;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (completed_fftw_modules != NULL) {
        g_hash_table_unref(completed_fftw_modules);
    }
    if (retained_handles != NULL) {
        g_ptr_array_free(retained_handles, TRUE);
    }
    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    dynamic_attach_test_hold_callback = FALSE;
    dynamic_attach_test_callback_is_waiting = FALSE;
    dynamic_attach_test_hold_drain = FALSE;
    dynamic_attach_test_drain_is_waiting = FALSE;
    pthread_cond_broadcast(&dynamic_attach_test_callback_cond);
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
}

void
dlopen_interceptor_test_set_manual_drain(gboolean enabled)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_test_manual_drain = enabled;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static gboolean
dlopen_interceptor_test_enqueue_dynamic_attach(
    const char* filename,
    void* handle)
{
    char* filename_copy = g_strdup(filename != NULL ? filename : "<test>");
    gboolean accepted = FALSE;
    PeakDlopenDynamicAttachRequest* queued;

    if (filename_copy == NULL) {
        return FALSE;
    }
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    queued = dlopen_interceptor_find_queued_request_unlocked(filename_copy,
                                                             NULL);
    if (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
        queued != NULL) {
        dlopen_interceptor_merge_queued_request_unlocked(
            queued,
            RTLD_LAZY,
            PEAK_DLOPEN_ATTACH_ALL);
        accepted = TRUE;
    } else if (dlopen_interceptor_queue_can_accept_unlocked()) {
        dynamic_attach_queue[dynamic_attach_queue_tail].handle = handle;
        dynamic_attach_queue[dynamic_attach_queue_tail].filename = filename_copy;
        dynamic_attach_queue[dynamic_attach_queue_tail].binding_flags =
            RTLD_LAZY;
        dynamic_attach_queue[dynamic_attach_queue_tail].scope =
            PEAK_DLOPEN_ATTACH_ALL;
        dynamic_attach_queue[dynamic_attach_queue_tail].module_token = NULL;
        dynamic_attach_queue[dynamic_attach_queue_tail].handle_retained =
            FALSE;
        filename_copy = NULL;
        dynamic_attach_queue_tail =
            (dynamic_attach_queue_tail + 1) %
            PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
        dynamic_attach_queue_length++;
        dynamic_attach_enqueue_count++;
        if (dynamic_attach_queue_length > dynamic_attach_queue_max_depth) {
            dynamic_attach_queue_max_depth = dynamic_attach_queue_length;
        }
        accepted = TRUE;
    } else if (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
               dynamic_attach_queue_length >=
                   PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY) {
        dynamic_attach_drop_full_count++;
    } else {
        dynamic_attach_drop_closed_count++;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    g_free(filename_copy);
    return accepted;
}

gboolean
dlopen_interceptor_test_enqueue_dummy_dynamic_attach(const char* filename)
{
    return dlopen_interceptor_test_enqueue_dynamic_attach(filename, NULL);
}

gboolean
dlopen_interceptor_test_enqueue_retry_dynamic_attach(const char* filename)
{
    return dlopen_interceptor_test_enqueue_dynamic_attach(
        filename,
        PEAK_DLOPEN_TEST_RETRY_HANDLE);
}

gboolean
dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
    const char* filename,
    void* application_handle)
{
    return dlopen_interceptor_enqueue_dynamic_attach_request(
        application_handle,
        filename,
        RTLD_LAZY,
        PEAK_DLOPEN_ATTACH_ALL);
}

void
dlopen_interceptor_test_drain_dynamic_attach_queue(void)
{
    dynamic_attach_test_explicit_drain = TRUE;
    dlopen_interceptor_drain_dynamic_attach_queue();
    dynamic_attach_test_explicit_drain = FALSE;
}

void
dlopen_interceptor_test_normal_drain_dynamic_attach_queue(void)
{
    dlopen_interceptor_drain_dynamic_attach_queue();
}

void
dlopen_interceptor_test_hold_dynamic_attach_drain(gboolean hold)
{
    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    dynamic_attach_test_hold_drain = hold;
    if (!hold) {
        pthread_cond_broadcast(&dynamic_attach_test_callback_cond);
    }
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
}

gboolean
dlopen_interceptor_test_dynamic_attach_drain_waiting(void)
{
    gboolean waiting;

    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    waiting = dynamic_attach_test_drain_is_waiting;
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
    return waiting;
}

gboolean
dlopen_interceptor_test_callback_waiting_for_drain(void)
{
    gboolean waiting;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    waiting = dynamic_attach_test_callback_waiting_for_drain;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return waiting;
}

gboolean
dlopen_interceptor_test_begin_callback(void)
{
    return dlopen_interceptor_begin_callback();
}

void
dlopen_interceptor_test_end_callback(void)
{
    (void)dlopen_interceptor_end_callback();
}

void
dlopen_interceptor_test_hold_callback(gboolean hold)
{
    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    dynamic_attach_test_hold_callback = hold;
    if (!hold) {
        pthread_cond_broadcast(&dynamic_attach_test_callback_cond);
    }
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
}

gboolean
dlopen_interceptor_test_callback_waiting(void)
{
    gboolean waiting;

    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    waiting = dynamic_attach_test_callback_is_waiting;
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
    return waiting;
}

static void
dlopen_interceptor_test_wait_at_callback_barrier(void)
{
    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    while (dynamic_attach_test_hold_callback) {
        dynamic_attach_test_callback_is_waiting = TRUE;
        pthread_cond_broadcast(&dynamic_attach_test_callback_cond);
        pthread_cond_wait(&dynamic_attach_test_callback_cond,
                          &dynamic_attach_test_callback_mutex);
    }
    dynamic_attach_test_callback_is_waiting = FALSE;
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
}

void
dlopen_interceptor_test_record_noload_drop(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_drop_noload_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

void
dlopen_interceptor_test_record_requeue_drop(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_drop_requeue_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

void
dlopen_interceptor_test_record_partial_success_with_retained_handle(void)
{
    PeakDlopenDynamicAttachRequest request = {
        .handle = PEAK_DLOPEN_TEST_RETAINED_HANDLE
    };

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_partial_success_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    dlopen_interceptor_retain_dynamic_handle(&request, FALSE);
}

void
dlopen_interceptor_test_release_retained_dynamic_handles(void)
{
    dlopen_interceptor_release_retained_dynamic_handles();
}

size_t
dlopen_interceptor_test_retained_handle_slots(void)
{
    size_t slots = 0;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_retained_handles != NULL) {
        slots = dynamic_attach_retained_handles->len;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return slots;
}

gboolean
dlopen_interceptor_test_retryable_prepare_status(int status)
{
    return dlopen_interceptor_dynamic_attach_prepare_is_retryable(
        (PeakDetachStatus)status);
}

void
dlopen_interceptor_test_trace_counters(const char* event)
{
    dlopen_interceptor_init_runtime_config();
    dlopen_interceptor_trace_counters(event);
}

#endif

static void
dlopen_interceptor_release_attach_candidate(
    PeakDlopenAttachCandidate* candidate)
{
    if (candidate->listener != NULL) {
        peak_general_listener_free(PEAKGENERAL_LISTENER(candidate->listener));
        g_object_unref(candidate->listener);
        candidate->listener = NULL;
    }
    g_free(candidate->demangled_name);
    candidate->demangled_name = NULL;
}

static gboolean
dlopen_interceptor_initialize_attach_candidate(
    size_t hook_id,
    gpointer dynamic_hook_address,
    GumInterceptor* target_interceptor,
    PeakDlopenAttachCandidate* candidate,
    PeakDetachRequest* mutation_request)
{
    char* demangled;

    memset(candidate, 0, sizeof(*candidate));
    memset(mutation_request, 0, sizeof(*mutation_request));
    if (!peak_general_listener_attach_target_is_supported(
            peak_hook_strings[hook_id],
            dynamic_hook_address)) {
        g_printerr("[peak] skipping dynamic Gum attach for hook %lu (%s): unsafe Gum prologue\n",
                   (unsigned long)hook_id,
                   peak_hook_strings[hook_id] != NULL
                       ? peak_hook_strings[hook_id]
                       : "<unknown>");
        return FALSE;
    }

    demangled = cxa_demangle(peak_hook_strings[hook_id]);
    candidate->demangled_name =
        g_strdup(demangled != NULL ? demangled : peak_hook_strings[hook_id]);
    free(demangled);
    candidate->listener =
        g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
    PEAKGENERAL_LISTENER(candidate->listener)->hook_id = hook_id;
    if (!peak_general_listener_is_ready(
            PEAKGENERAL_LISTENER(candidate->listener))) {
        g_printerr("[peak] skipping dynamic Gum attach for hook %lu (%s): unable to allocate listener statistics: %s\n",
                   (unsigned long)hook_id,
                   peak_hook_strings[hook_id] != NULL
                       ? peak_hook_strings[hook_id]
                       : "<unknown>",
                   g_strerror(PEAKGENERAL_LISTENER(candidate->listener)
                                  ->fast_stats_errno));
        peak_general_listener_free(
            PEAKGENERAL_LISTENER(candidate->listener));
        g_object_unref(candidate->listener);
        candidate->listener = NULL;
        g_free(candidate->demangled_name);
        candidate->demangled_name = NULL;
        return FALSE;
    }
    peak_gum_target_attach_plan(dynamic_hook_address,
                                &candidate->attach_plan);

    *mutation_request = (PeakDetachRequest){
        .hook_id = hook_id,
        .symbol_name = peak_hook_strings[hook_id],
        .function_address = dynamic_hook_address,
        .interceptor = target_interceptor,
        .listener = candidate->listener,
        .operation = PEAK_DETACH_OPERATION_ATTACH,
        .blocked_pc_start = candidate->attach_plan.mutation_guard_size > 0
            ? candidate->attach_plan.mutation_address
            : NULL,
        .blocked_pc_size = candidate->attach_plan.mutation_guard_size
    };
    return TRUE;
}

static void
dlopen_interceptor_publish_attach_candidate(
    PeakDlopenAttachCandidate* candidate,
    const PeakDetachRequest* mutation_request,
    gboolean* retained_handle_for_hooks)
{
    size_t hook_id = mutation_request->hook_id;

    hook_address[hook_id] = mutation_request->function_address;
    peak_demangled_strings[hook_id] = candidate->demangled_name;
    array_listener[hook_id] = candidate->listener;
    dlopen_interceptor_mark_target_resolved_unlocked(hook_id);
    peak_general_listener_controller_mark_attached_unlocked(hook_id);
    candidate->demangled_name = NULL;
    candidate->listener = NULL;
    *retained_handle_for_hooks = TRUE;
}

static void
dlopen_interceptor_log_attach_prepare_result(
    const PeakDetachRequest* mutation_request,
    PeakDetachStatus status,
    gboolean retryable)
{
    peak_log_debug("[peak] %s dynamic Gum attach for hook %lu (%s): %s\n",
                   retryable ? "retrying" : "skipping",
                   (unsigned long)mutation_request->hook_id,
                   mutation_request->symbol_name != NULL
                       ? mutation_request->symbol_name
                       : "<unknown>",
                   peak_detach_controller_status_string(status));
}

static void
dlopen_interceptor_log_gum_attach_failure(
    const PeakDetachRequest* mutation_request,
    GumAttachReturn status)
{
    g_printerr("[peak] dynamic Gum attach failed for hook %lu (%s), status=%d\n",
               (unsigned long)mutation_request->hook_id,
               mutation_request->symbol_name != NULL
                   ? mutation_request->symbol_name
                   : "<unknown>",
               status);
}

static void
dlopen_interceptor_attach_candidate_scalar(
    PeakDlopenAttachCandidate* candidate,
    PeakDetachRequest* mutation_request,
    gboolean* retained_handle_for_hooks,
    gboolean* retry_later)
{
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

    peak_gum_module_mutation_begin();
    if (!peak_detach_controller_prepare_hook_mutation(mutation_request,
                                                      &detach_status)) {
        gboolean retryable =
            dlopen_interceptor_dynamic_attach_prepare_is_retryable(
                detach_status);
        if (retryable) {
            *retry_later = TRUE;
        }
        dlopen_interceptor_log_attach_prepare_result(mutation_request,
                                                     detach_status,
                                                     retryable);
        dlopen_interceptor_release_attach_candidate(candidate);
        peak_gum_module_mutation_end();
        return;
    }

    gum_interceptor_begin_transaction(mutation_request->interceptor);
    GumAttachReturn attach_status =
        peak_general_listener_gum_attach_target(
            mutation_request->interceptor,
            mutation_request->function_address,
            candidate->listener,
            &candidate->attach_plan);
    gum_interceptor_end_transaction(mutation_request->interceptor);
    if (!peak_detach_controller_finish_hook_mutation(mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish(
            "dynamic attach finish",
            detach_status);
    }
    peak_gum_module_mutation_end();
    if (attach_status == GUM_ATTACH_OK) {
        dlopen_interceptor_publish_attach_candidate(candidate,
                                                    mutation_request,
                                                    retained_handle_for_hooks);
    } else {
        dlopen_interceptor_log_gum_attach_failure(mutation_request,
                                                  attach_status);
        dlopen_interceptor_release_attach_candidate(candidate);
    }
}

static void
dlopen_interceptor_attach_candidate_batch(
    PeakDlopenAttachCandidate* candidates,
    PeakDetachRequest* mutation_requests,
    PeakDetachBatchResult* batch_results,
    GumAttachReturn* attach_statuses,
    size_t candidate_count,
    gboolean* retained_handle_for_hooks,
    gboolean* retry_later)
{
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;
    size_t prepared_count = 0;

    for (size_t i = 0; i < candidate_count; i++) {
        attach_statuses[i] = GUM_ATTACH_WRONG_SIGNATURE;
    }
    peak_gum_module_mutation_begin();
    (void)peak_detach_controller_prepare_hook_mutation_batch(
        mutation_requests,
        candidate_count,
        batch_results,
        &prepared_count,
        &detach_status);

    if (prepared_count > 0) {
        gum_interceptor_begin_transaction(mutation_requests[0].interceptor);
        for (size_t i = 0; i < candidate_count; i++) {
            if (!batch_results[i].prepared) {
                continue;
            }
            attach_statuses[i] =
                peak_general_listener_gum_attach_target(
                    mutation_requests[i].interceptor,
                    mutation_requests[i].function_address,
                    candidates[i].listener,
                    &candidates[i].attach_plan);
        }
        gum_interceptor_end_transaction(mutation_requests[0].interceptor);
        if (!peak_detach_controller_finish_hook_mutation_batch(
                &detach_status)) {
            peak_detach_controller_abort_after_failed_finish(
                "dynamic attach batch finish",
                detach_status);
        }
    }
    peak_gum_module_mutation_end();

    for (size_t i = 0; i < candidate_count; i++) {
        if (batch_results[i].prepared) {
            if (attach_statuses[i] == GUM_ATTACH_OK) {
                dlopen_interceptor_publish_attach_candidate(
                    &candidates[i],
                    &mutation_requests[i],
                    retained_handle_for_hooks);
            } else {
                dlopen_interceptor_log_gum_attach_failure(
                    &mutation_requests[i],
                    attach_statuses[i]);
                dlopen_interceptor_release_attach_candidate(&candidates[i]);
            }
            continue;
        }

        gboolean retryable =
            dlopen_interceptor_dynamic_attach_prepare_is_retryable(
                batch_results[i].status);
        if (retryable) {
            *retry_later = TRUE;
        }
        dlopen_interceptor_log_attach_prepare_result(
            &mutation_requests[i],
            batch_results[i].status,
            retryable);
        dlopen_interceptor_release_attach_candidate(&candidates[i]);
    }
}

static void
dlopen_interceptor_attach_candidates(
    PeakDlopenAttachCandidate* candidates,
    PeakDetachRequest* mutation_requests,
    PeakDetachBatchResult* batch_results,
    GumAttachReturn* attach_statuses,
    size_t candidate_count,
    gboolean use_batch,
    gboolean force_prepare_timeout,
    gboolean* retained_handle_for_hooks,
    gboolean* retry_later)
{
    if (force_prepare_timeout) {
        for (size_t i = 0; i < candidate_count; i++) {
            dlopen_interceptor_log_attach_prepare_result(
                &mutation_requests[i],
                PEAK_DETACH_STATUS_TIMEOUT,
                TRUE);
            dlopen_interceptor_release_attach_candidate(&candidates[i]);
        }
        *retry_later = TRUE;
    } else if (use_batch) {
        dlopen_interceptor_attach_candidate_batch(candidates,
                                                  mutation_requests,
                                                  batch_results,
                                                  attach_statuses,
                                                  candidate_count,
                                                  retained_handle_for_hooks,
                                                  retry_later);
    } else {
        dlopen_interceptor_attach_candidate_scalar(&candidates[0],
                                                   &mutation_requests[0],
                                                   retained_handle_for_hooks,
                                                   retry_later);
    }
}

static PeakDlopenAttachResult
dlopen_interceptor_attach_from_request(PeakDlopenDynamicAttachRequest* request)
{
    gboolean retained_handle_for_hooks = FALSE;
    gboolean retry_later = FALSE;
    gboolean completed_fftw_scan = FALSE;
    gboolean resolved_fftw_from_handle = FALSE;
    gboolean needs_resolution = FALSE;
    gboolean use_batch;
    GumInterceptor* target_interceptor;
    PeakDlopenResolvedTarget* resolved_targets;
    PeakDlopenAttachCandidate* attach_candidates = NULL;
    PeakDetachRequest* mutation_requests = NULL;
    PeakDetachBatchResult* batch_results = NULL;
    GumAttachReturn* attach_statuses = NULL;
    size_t target_count;
    size_t batch_capacity;
    size_t candidate_count = 0;
    size_t resolved_count = 0;
    gboolean unresolved_non_fftw = FALSE;

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (request->handle == PEAK_DLOPEN_TEST_RETRY_HANDLE) {
        return PEAK_DLOPEN_ATTACH_RETRY;
    }
#endif

    if (request->handle == NULL) {
        return PEAK_DLOPEN_ATTACH_DONE;
    }

    peak_general_listener_controller_lock();
    if (interceptor == NULL ||
        hook_address == NULL ||
        array_listener == NULL ||
        peak_hook_strings == NULL ||
        peak_demangled_strings == NULL) {
        peak_general_listener_controller_unlock();
        return PEAK_DLOPEN_ATTACH_DONE;
    }

    target_interceptor = interceptor;
    target_count = peak_hook_address_count;
    resolved_targets = g_try_new0(PeakDlopenResolvedTarget, target_count);
    if (resolved_targets == NULL) {
        peak_general_listener_controller_unlock();
        return PEAK_DLOPEN_ATTACH_DONE;
    }
    for (size_t i = 0; i < target_count; i++) {
        if (!dlopen_interceptor_target_is_unresolved_unlocked(i)) {
            continue;
        }
        if (dlopen_interceptor_target_matches_scope_unlocked(
                i,
                PEAK_DLOPEN_ATTACH_NON_FFTW_ONLY)) {
            unresolved_non_fftw = TRUE;
        }
        if (dlopen_interceptor_target_matches_scope_unlocked(
                i,
                request->scope)) {
            resolved_targets[i].name = peak_hook_strings[i];
            needs_resolution = TRUE;
        }
    }
    atomic_store_explicit(&dlopen_may_have_unresolved_non_fftw,
                          unresolved_non_fftw,
                          memory_order_relaxed);
    peak_general_listener_controller_unlock();

    if (!needs_resolution) {
        g_free(resolved_targets);
        return PEAK_DLOPEN_ATTACH_DONE;
    }

    /*
     * Never hold the general-listener lock while entering the dynamic loader.
     * A dlopen on-leave callback still owns the loader lock on some platforms;
     * the controller taking these locks in the opposite order would deadlock.
     */
    peak_general_listener_fast_ignore_current_thread();
    gum_interceptor_ignore_current_thread(target_interceptor);
    for (size_t i = 0; i < target_count; i++) {
        if (resolved_targets[i].name != NULL) {
            resolved_targets[i].address =
                (gpointer)(fn_void)dlsym(request->handle,
                                         resolved_targets[i].name);
            /*
             * Treat a NULL symbol address as unresolved. Querying the loader
             * error state here can enter gettext/getenv under heavy concurrent
             * loader activity, widening a harmless miss into the crash surface
             * observed at scale.
             */
            if (resolved_targets[i].address != NULL) {
                resolved_count++;
                if (i < dlopen_fftw_scope_target_count &&
                    dlopen_fftw_scope_targets != NULL &&
                    dlopen_fftw_scope_targets[i]) {
                    resolved_fftw_from_handle = TRUE;
                }
            }
        }
    }

    if (resolved_count == 0) {
        gum_interceptor_unignore_current_thread(target_interceptor);
        peak_general_listener_fast_unignore_current_thread();
        g_free(resolved_targets);
        return PEAK_DLOPEN_ATTACH_DONE;
    }

    use_batch = FALSE;
    batch_capacity = 1;
    if (resolved_count > 1 &&
        peak_detach_controller_strict_batch_supported()) {
        size_t max_batch_capacity =
            peak_detach_controller_max_batch_requests();
        if (max_batch_capacity > 1) {
            use_batch = TRUE;
            batch_capacity = resolved_count < max_batch_capacity
                ? resolved_count
                : max_batch_capacity;
        }
    }
    attach_candidates =
        g_try_new0(PeakDlopenAttachCandidate, batch_capacity);
    mutation_requests = g_try_new0(PeakDetachRequest, batch_capacity);
    if (use_batch) {
        batch_results = g_try_new0(PeakDetachBatchResult, batch_capacity);
        attach_statuses = g_try_new0(GumAttachReturn, batch_capacity);
    }
    if (attach_candidates == NULL || mutation_requests == NULL ||
        (use_batch && (batch_results == NULL || attach_statuses == NULL))) {
        g_free(attach_statuses);
        g_free(batch_results);
        g_free(mutation_requests);
        g_free(attach_candidates);
        gum_interceptor_unignore_current_thread(target_interceptor);
        peak_general_listener_fast_unignore_current_thread();
        g_free(resolved_targets);
        return PEAK_DLOPEN_ATTACH_DONE;
    }

    peak_general_listener_controller_lock();
    if (interceptor != target_interceptor ||
        hook_address == NULL ||
        array_listener == NULL ||
        peak_hook_strings == NULL ||
        peak_demangled_strings == NULL ||
        peak_hook_address_count < target_count) {
        peak_general_listener_controller_unlock();
        gum_interceptor_unignore_current_thread(target_interceptor);
        peak_general_listener_fast_unignore_current_thread();
        g_free(attach_statuses);
        g_free(batch_results);
        g_free(mutation_requests);
        g_free(attach_candidates);
        g_free(resolved_targets);
        return PEAK_DLOPEN_ATTACH_DONE;
    }

    for (size_t i = 0; i < target_count; i++) {
        if (hook_address[i] != NULL || array_listener[i] != NULL ||
            peak_demangled_strings[i] != NULL) {
            continue;
        }
        if (peak_hook_strings[i] != resolved_targets[i].name) {
            continue;
        }

        gpointer dynamic_hook_address = resolved_targets[i].address;
        if (dynamic_hook_address == NULL) {
            continue;
        }

        gboolean duplicate_address = FALSE;
        for (size_t j = 0; j < candidate_count; j++) {
            if (mutation_requests[j].function_address ==
                dynamic_hook_address) {
                duplicate_address = TRUE;
                break;
            }
        }
        if (!duplicate_address) {
            for (size_t j = 0; j < target_count; j++) {
                if (hook_address[j] == dynamic_hook_address &&
                    array_listener[j] != NULL) {
                    duplicate_address = TRUE;
                    break;
                }
            }
        }
        if (duplicate_address) {
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
            dlopen_interceptor_mark_target_resolved_unlocked(i);
            peak_log_debug("[peak] skipping duplicate dynamic target %s at %p\n",
                           peak_hook_strings[i],
                           dynamic_hook_address);
            continue;
        }

        if (!dlopen_interceptor_initialize_attach_candidate(
                i,
                dynamic_hook_address,
                target_interceptor,
                &attach_candidates[candidate_count],
                &mutation_requests[candidate_count])) {
            continue;
        }
        candidate_count++;

        if (candidate_count == batch_capacity) {
            dlopen_interceptor_attach_candidates(
                attach_candidates,
                mutation_requests,
                batch_results,
                attach_statuses,
                candidate_count,
                use_batch,
                FALSE,
                &retained_handle_for_hooks,
                &retry_later);
            candidate_count = 0;
        }
    }

    if (candidate_count > 0) {
        dlopen_interceptor_attach_candidates(
            attach_candidates,
            mutation_requests,
            batch_results,
            attach_statuses,
            candidate_count,
            use_batch,
            FALSE,
            &retained_handle_for_hooks,
            &retry_later);
        candidate_count = 0;
    }
    if ((request->scope == PEAK_DLOPEN_ATTACH_FFTW_ONLY ||
         request->scope == PEAK_DLOPEN_ATTACH_ALL) &&
        !retry_later &&
        peak_hook_address_count == target_count &&
        resolved_fftw_from_handle) {
        /*
         * This provider's complete scan reached terminal outcomes.  A Gum
         * signature failure remains unresolved globally so another provider
         * may satisfy it, but this same retained module need not be scanned
         * again. Retryable controller outcomes never get here.
         */
        completed_fftw_scan = TRUE;
    }
    dlopen_interceptor_refresh_unresolved_non_fftw_unlocked();
    peak_general_listener_controller_unlock();
    gum_interceptor_unignore_current_thread(target_interceptor);
    peak_general_listener_fast_unignore_current_thread();
    g_free(attach_statuses);
    g_free(batch_results);
    g_free(mutation_requests);
    g_free(attach_candidates);
    g_free(resolved_targets);

    if (retained_handle_for_hooks && retry_later) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_partial_success_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        /*
         * A single queue-owned reference can safely serve both the attached
         * hooks and later retries. Transfer its eventual close to the retained
         * handle set instead of reopening the module from the controller.
         */
        dlopen_interceptor_retain_dynamic_handle(request, FALSE);
    } else if (retained_handle_for_hooks || request->handle_retained) {
        dlopen_interceptor_retain_dynamic_handle(request,
                                                 completed_fftw_scan);
    }
    if (retry_later) {
        return PEAK_DLOPEN_ATTACH_RETRY;
    }
    return PEAK_DLOPEN_ATTACH_DONE;
}

static void
dlopen_interceptor_drain_dynamic_attach_queue_with_budget(size_t max_requests)
{
    size_t drained = 0;
    size_t close_count = 0;
    size_t initial_queue_length = 0;
    size_t drain_limit;
    void* handles_to_close[PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY] = { 0 };
    PeakDlopenDynamicAttachRequest request = { 0 };

    if (dynamic_attach_drain_reentrant) {
        return;
    }

    if (!dlopen_interceptor_begin_dynamic_attach_drain(&initial_queue_length)) {
        return;
    }

#ifdef PEAK_ENABLE_TEST_HOOKS
    pthread_mutex_lock(&dynamic_attach_test_callback_mutex);
    while (dynamic_attach_test_hold_drain) {
        dynamic_attach_test_drain_is_waiting = TRUE;
        pthread_cond_broadcast(&dynamic_attach_test_callback_cond);
        pthread_cond_wait(&dynamic_attach_test_callback_cond,
                          &dynamic_attach_test_callback_mutex);
    }
    dynamic_attach_test_drain_is_waiting = FALSE;
    pthread_mutex_unlock(&dynamic_attach_test_callback_mutex);
#endif

    drain_limit = initial_queue_length;
    if (max_requests != 0 && max_requests < drain_limit) {
        drain_limit = max_requests;
    }

    dynamic_attach_drain_reentrant = TRUE;
    while (drained < drain_limit &&
           dlopen_interceptor_pop_dynamic_attach_request(&request)) {
        PeakDlopenAttachResult result =
            dlopen_interceptor_attach_from_request(&request);
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_drain_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        if (result == PEAK_DLOPEN_ATTACH_RETRY &&
            dlopen_interceptor_requeue_dynamic_attach_request(&request)) {
            drained++;
            continue;
        }
        void* handle_to_close =
            dlopen_interceptor_release_dynamic_attach_request_metadata(
                &request);
        if (handle_to_close != NULL) {
            handles_to_close[close_count++] = handle_to_close;
        }
        drained++;
    }
    dynamic_attach_drain_reentrant = FALSE;

    dlopen_interceptor_end_dynamic_attach_drain();

    /*
     * A final dlclose may run arbitrary DSO destructors. Those destructors can
     * call dlopen themselves or start another loader thread. Never run them
     * while callback admission is closed by dynamic_attach_drain_active, or a
     * destructor can wait forever for this same controller to end the drain.
     * Keeping the close observable also preserves genuine dynamic loads
     * initiated by application destructors.
     */
    for (size_t i = 0; i < close_count; i++) {
        dlclose(handles_to_close[i]);
    }
}

void
dlopen_interceptor_drain_dynamic_attach_queue(void)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    gboolean skip_for_test;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    skip_for_test =
        dynamic_attach_test_manual_drain && !dynamic_attach_test_explicit_drain;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    if (skip_for_test) {
        return;
    }
#endif

    dlopen_interceptor_drain_dynamic_attach_queue_with_budget(
        PEAK_DLOPEN_DYNAMIC_ATTACH_DRAIN_BUDGET);
}

void
dlopen_interceptor_enable_dynamic_attach(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN &&
        dlopen_interceptor != NULL &&
        dlopen_hook_address != NULL &&
        dlopen_listener != NULL) {
        atomic_store_explicit(&dlopen_listener_owner_pid,
                              getpid(),
                              memory_order_release);
        dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_OPEN;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

gboolean
dlopen_interceptor_shutdown_dynamic_attach(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    atomic_store_explicit(&dlopen_listener_owner_pid,
                          0,
                          memory_order_release);
    dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN;
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (!dlopen_interceptor_wait_for_dynamic_attach_idle()) {
        g_printerr("[peak] dlopen dynamic attach drain timed out; leaving dlopen interceptor state alive\n");
        dlopen_interceptor_trace_counters("shutdown-dynamic-timeout");
        return FALSE;
    }
    if (!dlopen_interceptor_wait_for_callbacks_idle()) {
        g_printerr("[peak] dlopen callback drain timed out with %u active callbacks; leaving dlopen interceptor state alive\n",
                   dlopen_interceptor_active_callback_count());
        dlopen_interceptor_trace_counters("shutdown-callback-timeout");
        return FALSE;
    }

    dlopen_interceptor_discard_dynamic_attach_queue();
    dlopen_interceptor_trace_counters("shutdown");
    return TRUE;
}

void
dlopen_interceptor_release_retained_dynamic_handles(void)
{
    GPtrArray* retained_handles = NULL;
    GHashTable* completed_fftw_modules = NULL;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    retained_handles = dynamic_attach_retained_handles;
    dynamic_attach_retained_handles = NULL;
    completed_fftw_modules = dlopen_completed_fftw_modules;
    dlopen_completed_fftw_modules = NULL;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (completed_fftw_modules != NULL) {
        g_hash_table_unref(completed_fftw_modules);
    }
    if (retained_handles != NULL) {
        g_ptr_array_free(retained_handles, TRUE);
    }
    dlopen_interceptor_trace_counters("release-retained-handles");
}

typedef struct {
    const char* filename;
    int binding_flags;
    gboolean callback_admitted;
    int previous_cancel_state;
} PeakDlopenInvocationData;

static gboolean
dlopen_interceptor_callback_is_admitted(void)
{
    pid_t owner = atomic_load_explicit(&dlopen_listener_owner_pid,
                                       memory_order_acquire);
    return owner != 0 && getpid() == owner;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
gboolean
dlopen_interceptor_test_callback_is_admitted(void)
{
    return dlopen_interceptor_callback_is_admitted();
}

gboolean
dlopen_interceptor_test_shutdown_dynamic_attach(void)
{
    return dlopen_interceptor_shutdown_dynamic_attach();
}
#endif

static void
dlopen_interceptor_on_enter(GumInvocationContext* context, gpointer user_data)
{
    PeakDlopenInvocationData* invocation =
        GUM_IC_GET_INVOCATION_DATA(context, PeakDlopenInvocationData);
    const char* filename =
        gum_invocation_context_get_nth_argument(context, 0);
    int flags = GPOINTER_TO_INT(
        gum_invocation_context_get_nth_argument(context, 1));

    (void)user_data;
    invocation->filename = NULL;
    invocation->binding_flags = RTLD_LAZY;
    invocation->callback_admitted = FALSE;
    invocation->previous_cancel_state = PTHREAD_CANCEL_ENABLE;
    if (!dlopen_interceptor_callback_is_admitted()) {
        return;
    }
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
                               &invocation->previous_cancel_state) != 0) {
        return;
    }
    if (!dlopen_interceptor_begin_callback()) {
        (void)pthread_setcancelstate(invocation->previous_cancel_state, NULL);
        return;
    }
    invocation->callback_admitted = TRUE;
    /* The application must keep the dlopen argument valid until it returns. */
    invocation->filename = filename;
    invocation->binding_flags = flags & (RTLD_LAZY | RTLD_NOW);
    if (invocation->binding_flags == 0) {
        invocation->binding_flags = RTLD_LAZY;
    }
}

static void
dlopen_interceptor_on_leave(GumInvocationContext* context, gpointer user_data)
{
    PeakDlopenInvocationData* invocation =
        GUM_IC_GET_INVOCATION_DATA(context, PeakDlopenInvocationData);
    void* handle = gum_invocation_context_get_return_value(context);
    gboolean enqueued = FALSE;

    (void)user_data;
    if (!invocation->callback_admitted) {
        return;
    }

#ifdef PEAK_ENABLE_TEST_HOOKS
    dlopen_interceptor_test_wait_at_callback_barrier();
#endif

    if (handle != NULL && invocation->filename != NULL) {
        PeakDlopenUnresolvedCounts unresolved =
            dlopen_interceptor_unresolved_counts();
        PeakDlopenAttachScope scope = PEAK_DLOPEN_ATTACH_ALL;

        if (unresolved.fftw > 0 &&
            dlopen_interceptor_fftw_module_scan_completed(handle)) {
            unresolved.fftw = 0;
        }
        if (unresolved.fftw == 0 && unresolved.non_fftw > 0) {
            scope = PEAK_DLOPEN_ATTACH_NON_FFTW_ONLY;
        }
        if (unresolved.fftw > 0 || unresolved.non_fftw > 0) {
            /*
             * The real dlopen has returned and released the loader lock, while
             * the application still cannot dlclose its result. Acquire only a
             * same-namespace ownership reference here. Symbol lookup and every
             * Gum mutation remain deferred to the controller.
             */
            enqueued = dlopen_interceptor_enqueue_dynamic_attach_request(
                handle,
                invocation->filename,
                invocation->binding_flags,
                scope);
        }

        if (!enqueued &&
            (unresolved.fftw > 0 || unresolved.non_fftw > 0)) {
            gboolean should_report_overflow = FALSE;

            pthread_mutex_lock(&dynamic_attach_gate_mutex);
            if (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
                dynamic_attach_queue_length >=
                    PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY &&
                !dynamic_attach_queue_overflow_reported) {
                dynamic_attach_queue_overflow_reported = TRUE;
                should_report_overflow = TRUE;
            }
            pthread_mutex_unlock(&dynamic_attach_gate_mutex);

            if (should_report_overflow) {
                g_printerr("[peak] dlopen dynamic attach queue full; dropping later dynamic attach requests\n");
            }
        }
    }

    invocation->filename = NULL;
    invocation->callback_admitted = FALSE;
    gboolean should_wake = dlopen_interceptor_end_callback();
    if (should_wake) {
        /*
         * Wake only after active_dlopen_callback_count is decremented, so the
         * controller cannot overlap loader work with this callback.
         */
        peak_general_listener_controller_wake();
    }
    (void)pthread_setcancelstate(invocation->previous_cancel_state, NULL);
}

int dlopen_interceptor_attach()
{
    GumAttachReturn attach_status = GUM_ATTACH_WRONG_SIGNATURE;
    gboolean startup_attach_can_skip_stop;
    dlopen_interceptor_init_runtime_config();
    dlopen_interceptor = gum_interceptor_obtain();
    dlopen_hook_address = peak_general_listener_find_function("dlopen");
    dlopen_interceptor_initialize_fftw_target_scope();

    if (dlopen_hook_address == NULL) {
        dlopen_interceptor_reset_fftw_target_scope();
        return attach_status;
    }
    if (!peak_general_listener_attach_target_is_supported("dlopen",
                                                          dlopen_hook_address)) {
        g_printerr("[peak] skipping dlopen Gum listener: unsupported target prologue\n");
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        dlopen_interceptor_reset_fftw_target_scope();
        return attach_status;
    }

    dlopen_listener = gum_make_call_listener(dlopen_interceptor_on_enter,
                                              dlopen_interceptor_on_leave,
                                              NULL,
                                              NULL);
    if (dlopen_listener == NULL) {
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        dlopen_interceptor_reset_fftw_target_scope();
        return attach_status;
    }

    PeakDetachRequest mutation_request = {
        .hook_id = 0,
        .symbol_name = "dlopen",
        .function_address = dlopen_hook_address,
        .interceptor = dlopen_interceptor,
        .listener = dlopen_listener,
        .operation = PEAK_DETACH_OPERATION_ATTACH
    };
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

    /*
     * Install this startup-only listener before PMPI_Init without eagerly
     * starting one helper per MPI rank. Runtime dynamic target mutations still
     * use the controller protocol after peer threads exist.
     */
    startup_attach_can_skip_stop =
        peak_general_listener_startup_attach_can_skip_stop();
    peak_gum_module_mutation_begin();
    if (!startup_attach_can_skip_stop &&
        !peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        g_printerr("[peak] skipping dlopen Gum listener: %s\n",
                   peak_detach_controller_status_string(detach_status));
        g_object_unref(dlopen_listener);
        g_object_unref(dlopen_interceptor);
        dlopen_listener = NULL;
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        dlopen_interceptor_reset_fftw_target_scope();
        peak_gum_module_mutation_end();
        return attach_status;
    }

    gum_interceptor_begin_transaction(dlopen_interceptor);
    attach_status = gum_interceptor_attach(dlopen_interceptor,
                                           dlopen_hook_address,
                                           dlopen_listener,
                                           NULL);
    gum_interceptor_end_transaction(dlopen_interceptor);
    if (!startup_attach_can_skip_stop &&
        !peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("dlopen attach finish",
                                                        detach_status);
    }
    peak_gum_module_mutation_end();
    if (attach_status != GUM_ATTACH_OK) {
        g_object_unref(dlopen_listener);
        g_object_unref(dlopen_interceptor);
        dlopen_listener = NULL;
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        atomic_store_explicit(&dlopen_listener_owner_pid,
                              0,
                              memory_order_release);
        dlopen_interceptor_reset_fftw_target_scope();
    }
    return attach_status;
}

gboolean dlopen_interceptor_dettach()
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    atomic_store_explicit(&dlopen_listener_owner_pid,
                          0,
                          memory_order_release);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    if (dlopen_interceptor != NULL &&
        dlopen_hook_address != NULL &&
        dlopen_listener != NULL) {
        PeakDetachRequest mutation_request = {
            .hook_id = 0,
            .symbol_name = "dlopen",
            .function_address = dlopen_hook_address,
            .interceptor = dlopen_interceptor,
            .listener = dlopen_listener,
            .operation = PEAK_DETACH_OPERATION_SHUTDOWN
        };
        PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

        peak_gum_module_mutation_begin();
        if (!dlopen_interceptor_prepare_hook_mutation_with_retry(
                &mutation_request,
                &detach_status)) {
            g_printerr("[peak] skipping dlopen Gum listener detach: %s\n",
                       peak_detach_controller_status_string(detach_status));
            peak_gum_module_mutation_end();
            return FALSE;
        }
        gum_interceptor_begin_transaction(dlopen_interceptor);
        gum_interceptor_detach(dlopen_interceptor, dlopen_listener);
        gum_interceptor_end_transaction(dlopen_interceptor);
        if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                         &detach_status)) {
            peak_detach_controller_abort_after_failed_finish("dlopen detach finish",
                                                            detach_status);
        }
        peak_gum_module_mutation_end();
    }

    if (!dlopen_interceptor_shutdown_dynamic_attach()) {
        return FALSE;
    }

    if (!dlopen_interceptor_flush_teardown()) {
        g_printerr("[peak] dlopen interceptor teardown did not flush; leaving interceptor state alive\n");
        return FALSE;
    }
    if (dlopen_listener != NULL) {
        g_object_unref(dlopen_listener);
    }
    if (dlopen_interceptor != NULL) {
        g_object_unref(dlopen_interceptor);
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
    dynamic_attach_queue_head = 0;
    dynamic_attach_queue_tail = 0;
    dynamic_attach_queue_length = 0;
    dynamic_attach_queue_overflow_reported = FALSE;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    dlopen_interceptor = NULL;
    dlopen_listener = NULL;
    dlopen_hook_address = NULL;
    atomic_store_explicit(&dlopen_listener_owner_pid,
                          0,
                          memory_order_release);
    dlopen_interceptor_reset_fftw_target_scope();
    return TRUE;
}
