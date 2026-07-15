#define _GNU_SOURCE
#include <dlfcn.h>

#include "general_listener.h"
#include "internal/general_listener_internal.h"
#include "internal/dlopen_interceptor_internal.h"
#include "dlopen_interceptor.h"
#include "detach_controller.h"
#include "logging.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <link.h>
#endif

#define PEAK_DLOPEN_DYNAMIC_ATTACH_DRAIN_BUDGET 64U
#define PEAK_DLOPEN_SHUTDOWN_DRAIN_TIMEOUT_MS 5000L
#define PEAK_DLOPEN_ENTRY_GUARD_BYTES 256U
#define PEAK_DLOPEN_PREPARE_RETRY_ATTEMPTS 50U
#define PEAK_DLOPEN_PREPARE_RETRY_SLEEP_NS 1000000L

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* dlopen_interceptor;
extern GumInterceptor* interceptor;
extern GumInvocationListener** array_listener;
extern gpointer* hook_address;
static gpointer* dlopen_hook_address = NULL;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
extern char** peak_demangled_strings;

static void* (*original_dlopen)(const char *filename, int flags);

#ifndef RTLD_BINDING_MASK
#define RTLD_BINDING_MASK (RTLD_LAZY | RTLD_NOW)
#endif

typedef enum {
    PEAK_DLOPEN_CONTROLLER_CLOSED = 0,
    PEAK_DLOPEN_CONTROLLER_OPEN,
    PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN
} PeakDlopenControllerState;

typedef enum {
    PEAK_DLOPEN_REQUEST_PENDING = 0,
    PEAK_DLOPEN_REQUEST_COMPLETE,
    PEAK_DLOPEN_REQUEST_NO_MATCH,
    PEAK_DLOPEN_REQUEST_FAILED,
    PEAK_DLOPEN_REQUEST_CANCELLED
} PeakDlopenRequestState;

typedef struct PeakDlopenDynamicAttachRequest {
    struct PeakDlopenDynamicAttachRequest* next;
    void* handle;
    char* filename;
    GumAddress module_address;
    int binding_flags;
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_cond;
    PeakDlopenRequestState state;
    gboolean caller_waits;
    gboolean retain_handle;
    const char* resolution_provider_name;
    const char* resolution_symbol_name;
    GumAddress resolution_address;
    void* resolution_provider_pin;
    gboolean resolution_provider_pin_failed;
    gboolean resolution_requested;
    gboolean resolution_complete;
    unsigned long long resolution_nested_generation;
#ifdef PEAK_ENABLE_TEST_HOOKS
    gboolean test_force_attach_failure;
#endif
} PeakDlopenDynamicAttachRequest;

typedef struct PeakDlopenProviderPin {
    void* handle;
    gchar* provider_path;
} PeakDlopenProviderPin;

#ifdef PEAK_ENABLE_TEST_HOOKS
static char peak_dlopen_test_retry_handle_marker;
static char peak_dlopen_test_retained_handle_marker;
#define PEAK_DLOPEN_TEST_RETRY_HANDLE \
    ((void*)&peak_dlopen_test_retry_handle_marker)
#define PEAK_DLOPEN_TEST_RETAINED_HANDLE \
    ((void*)&peak_dlopen_test_retained_handle_marker)

static gboolean
dlopen_interceptor_test_forced_replace_failure(
    GumReplaceReturn* result_out)
{
    const char* value = g_getenv("PEAK_TEST_DLOPEN_REPLACE_RESULT");
    char* end = NULL;
    gint64 parsed;

    if (value == NULL || value[0] == '\0' || result_out == NULL) {
        return FALSE;
    }
    parsed = g_ascii_strtoll(value, &end, 10);
    if (end == value || *end != '\0' ||
        parsed < GUM_REPLACE_WRONG_TYPE ||
        parsed > GUM_REPLACE_WRONG_SIGNATURE) {
        return FALSE;
    }
    *result_out = (GumReplaceReturn)parsed;
    return TRUE;
}
#endif

typedef enum {
    PEAK_DLOPEN_ATTACH_DONE = 0,
    PEAK_DLOPEN_ATTACH_RETRY,
    PEAK_DLOPEN_ATTACH_FAILED
} PeakDlopenAttachResult;

static PeakDlopenAttachResult
dlopen_interceptor_attach_from_request(PeakDlopenDynamicAttachRequest* request);
static gboolean
dlopen_interceptor_service_resolution_nested_request(void);
static gboolean
dlopen_interceptor_string_equal(gconstpointer left, gconstpointer right);

static pthread_mutex_t dynamic_attach_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dynamic_attach_gate_cond = PTHREAD_COND_INITIALIZER;
static PeakDlopenControllerState dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
static unsigned int active_dynamic_attach_count = 0;
static _Atomic unsigned int active_dlopen_replacement_count = 0;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "dlopen replacement entry accounting must be lock-free");
static gboolean dynamic_attach_drain_active = FALSE;
static PeakDlopenDynamicAttachRequest* dynamic_attach_queue_head = NULL;
static PeakDlopenDynamicAttachRequest* dynamic_attach_queue_tail = NULL;
static size_t dynamic_attach_queue_length = 0;
static GPtrArray* dynamic_attach_retained_handles = NULL;
static GHashTable* dynamic_attach_retained_provider_paths = NULL;
static __thread gboolean dynamic_attach_drain_reentrant = FALSE;
static __thread PeakDlopenDynamicAttachRequest*
    dynamic_attach_resolution_wait_request = NULL;
/*
 * A dynamic attach stops the other application threads while Gum mutates its
 * interceptor metadata.  Do not let that stop window freeze another thread
 * part-way through a Gum-backed dlopen replacement: it could own Gum's
 * internal allocator lock and deadlock the controller doing the attach.
 *
 * The depth makes same-thread dlopen recursion (for example from a DSO
 * constructor) reentrant while keeping distinct application threads out of
 * the load-and-attach transaction until the exact request is complete.
 */
static pthread_mutex_t dynamic_load_transaction_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned int dynamic_load_transaction_depth = 0;
/*
 * Once an ordinary controller mutation loses the transaction race, stop
 * admitting new outer dlopen bodies until that controller mutation gets a
 * turn.  Callers already inside (or already admitted to) the transaction may
 * finish, and nested same-thread loader work remains reentrant.
 */
static gboolean dynamic_load_controller_mutation_pending = FALSE;
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
static _Atomic pid_t dynamic_attach_owner_pid = 0;
static _Atomic pid_t dynamic_attach_fork_warning_pid = 0;
static atomic_bool dlopen_replacement_installed_by_peak =
    ATOMIC_VAR_INIT(false);
#ifdef PEAK_ENABLE_TEST_HOOKS
static gboolean dynamic_attach_test_manual_drain = FALSE;
static __thread gboolean dynamic_attach_test_explicit_drain = FALSE;
static pthread_mutex_t dynamic_load_test_pause_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dynamic_load_test_pause_cond = PTHREAD_COND_INITIALIZER;
static atomic_bool dynamic_load_test_pause_consumed = ATOMIC_VAR_INIT(false);
static atomic_bool dynamic_load_test_replacement_paused =
    ATOMIC_VAR_INIT(false);
static atomic_bool dynamic_load_test_replacement_released =
    ATOMIC_VAR_INIT(false);
static atomic_bool dynamic_load_test_entry_physically_restored =
    ATOMIC_VAR_INIT(false);
static _Atomic unsigned long long
    dynamic_load_test_controller_mutation_deferrals = 0;
#endif

static gboolean
dlopen_interceptor_debug_enabled(void)
{
    const char* value = g_getenv("PEAK_DLOPEN_DEBUG");
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
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
    *capacity = 0;
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
    diagnostics->capacity = 0;
    diagnostics->drain_budget = PEAK_DLOPEN_DYNAMIC_ATTACH_DRAIN_BUDGET;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static void
dlopen_interceptor_trace_counters(const char* event)
{
    const char* path = g_getenv("PEAK_DLOPEN_TRACE_PATH");
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

gboolean
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
        case PEAK_DETACH_STATUS_ERROR:
        default:
            return FALSE;
    }
}

static gboolean
dlopen_interceptor_revert_prepare_is_retryable(PeakDetachStatus status)
{
    /* Preserve the existing interceptor-teardown retry policy. */
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
    return dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN;
}

static void
dlopen_interceptor_begin_load_transaction(void)
{
    if (dynamic_load_transaction_depth == 0) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        while (dynamic_load_controller_mutation_pending &&
               dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN) {
            pthread_cond_wait(&dynamic_attach_gate_cond,
                              &dynamic_attach_gate_mutex);
        }
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        pthread_mutex_lock(&dynamic_load_transaction_mutex);
    }
    dynamic_load_transaction_depth++;
}

static void
dlopen_interceptor_end_load_transaction(void)
{
    if (dynamic_load_transaction_depth == 0) {
        return;
    }

    dynamic_load_transaction_depth--;
    if (dynamic_load_transaction_depth == 0) {
        pthread_mutex_unlock(&dynamic_load_transaction_mutex);
    }
}

static void
dlopen_interceptor_end_replacement_call(void)
{
    unsigned int previous = atomic_fetch_sub_explicit(
        &active_dlopen_replacement_count,
        1,
        memory_order_acq_rel);

    if (previous == 0) {
        /* Keep an unmatched release fail-closed instead of wrapping to UINT_MAX. */
        atomic_fetch_add_explicit(&active_dlopen_replacement_count,
                                  1,
                                  memory_order_relaxed);
        return;
    }
    if (previous == 1) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        pthread_cond_broadcast(&dynamic_attach_gate_cond);
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    }
}

static unsigned int
dlopen_interceptor_active_replacement_count(void)
{
    return atomic_load_explicit(&active_dlopen_replacement_count,
                                memory_order_acquire);
}

gboolean
dlopen_interceptor_try_begin_controller_mutation(void)
{
    /*
     * Publish writer intent before trying the transaction.  If an application
     * thread currently owns it, later outer dlopen callers wait at admission
     * instead of forming an endless stream ahead of heartbeat/JIT work.
     */
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_load_controller_mutation_pending = TRUE;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (pthread_mutex_trylock(&dynamic_load_transaction_mutex) != 0) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        atomic_fetch_add_explicit(
            &dynamic_load_test_controller_mutation_deferrals,
            1,
            memory_order_relaxed);
#endif
        return FALSE;
    }

    /* Allow a same-thread loader call made by the guarded mutation to nest. */
    dynamic_load_transaction_depth = 1;
    return TRUE;
}

void
dlopen_interceptor_end_controller_mutation(void)
{
    dlopen_interceptor_end_load_transaction();

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_load_controller_mutation_pending = FALSE;
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static void
dlopen_interceptor_test_pause_replacement_body(const char* filename)
{
    const char* match = g_getenv("PEAK_TEST_DLOPEN_BODY_PAUSE_MATCH");
    _Bool expected = 0;

    if (match == NULL || filename == NULL || g_strcmp0(match, filename) != 0 ||
        !atomic_compare_exchange_strong_explicit(
            &dynamic_load_test_pause_consumed,
            &expected,
            1,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }

    pthread_mutex_lock(&dynamic_load_test_pause_mutex);
    atomic_store_explicit(&dynamic_load_test_replacement_paused,
                          TRUE,
                          memory_order_release);
    while (!atomic_load_explicit(&dynamic_load_test_replacement_released,
                                 memory_order_acquire)) {
        pthread_cond_wait(&dynamic_load_test_pause_cond,
                          &dynamic_load_test_pause_mutex);
    }
    atomic_store_explicit(&dynamic_load_test_replacement_paused,
                          FALSE,
                          memory_order_release);
    pthread_mutex_unlock(&dynamic_load_test_pause_mutex);
}

gboolean
dlopen_interceptor_test_replacement_body_is_paused(void)
{
    return atomic_load_explicit(&dynamic_load_test_replacement_paused,
                                memory_order_acquire);
}

void
dlopen_interceptor_test_release_replacement_body(void)
{
    pthread_mutex_lock(&dynamic_load_test_pause_mutex);
    atomic_store_explicit(&dynamic_load_test_replacement_released,
                          TRUE,
                          memory_order_release);
    pthread_cond_broadcast(&dynamic_load_test_pause_cond);
    pthread_mutex_unlock(&dynamic_load_test_pause_mutex);
}

gboolean
dlopen_interceptor_test_entry_physically_restored(void)
{
    return atomic_load_explicit(
        &dynamic_load_test_entry_physically_restored,
        memory_order_acquire);
}

gboolean
dlopen_interceptor_test_replacement_installed_by_peak(void)
{
    return atomic_load_explicit(&dlopen_replacement_installed_by_peak,
                                memory_order_acquire);
}

gboolean
dlopen_interceptor_test_uninstalled_replacement_state_is_clean(void)
{
    return !atomic_load_explicit(&dlopen_replacement_installed_by_peak,
                                 memory_order_acquire) &&
           dlopen_interceptor == NULL &&
           dlopen_hook_address == NULL &&
           original_dlopen == NULL &&
           atomic_load_explicit(&dynamic_attach_owner_pid,
                                memory_order_acquire) == 0;
}

int
dlopen_interceptor_test_attach(void)
{
    return dlopen_interceptor_attach();
}

gboolean
dlopen_interceptor_test_dettach(void)
{
    return dlopen_interceptor_dettach();
}

unsigned long long
dlopen_interceptor_test_controller_mutation_deferrals(void)
{
    return atomic_load_explicit(
        &dynamic_load_test_controller_mutation_deferrals,
        memory_order_relaxed);
}
#endif

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
dlopen_interceptor_wait_for_replacement_idle(void)
{
    struct timespec deadline;
    gboolean idle = TRUE;

    clock_gettime(CLOCK_REALTIME, &deadline);
    dlopen_interceptor_add_milliseconds(&deadline,
                                        PEAK_DLOPEN_SHUTDOWN_DRAIN_TIMEOUT_MS);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    while (atomic_load_explicit(&active_dlopen_replacement_count,
                                memory_order_acquire) > 0) {
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
dlopen_interceptor_retain_dynamic_handle(void* handle)
{
    if (handle == NULL) {
        return;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_retained_handles == NULL) {
        dynamic_attach_retained_handles =
            g_ptr_array_new_with_free_func(dlopen_interceptor_close_retained_handle);
    }
    g_ptr_array_add(dynamic_attach_retained_handles, handle);
    dynamic_attach_retained_handle_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

gboolean
dlopen_interceptor_pin_loaded_provider(gpointer address, void** pin_out)
{
    GumModule* main_module;
    const GumMemoryRange* main_range;
    GumAddress target_address;
    Dl_info provider_info;
    gchar* provider_path = NULL;
    void* handle = NULL;
    PeakDlopenProviderPin* pin;
    gboolean already_retained;

    if (pin_out == NULL || address == NULL) {
        return FALSE;
    }
    *pin_out = NULL;

    /* Gum owns the cached main-module reference; callers must not unref it. */
    main_module = gum_process_get_main_module();
    main_range = main_module != NULL ? gum_module_get_range(main_module) : NULL;
    target_address = (GumAddress)(guintptr)address;
    if (main_range != NULL &&
        target_address >= main_range->base_address &&
        target_address - main_range->base_address < main_range->size) {
        return TRUE;
    }

    memset(&provider_info, 0, sizeof(provider_info));
    if (dladdr(address, &provider_info) == 0 ||
        provider_info.dli_fname == NULL ||
        provider_info.dli_fname[0] == '\0') {
        return FALSE;
    }
    provider_path = g_strdup(provider_info.dli_fname);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    already_retained =
        dynamic_attach_retained_provider_paths != NULL &&
        g_hash_table_contains(dynamic_attach_retained_provider_paths,
                              provider_path);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    if (already_retained) {
        g_free(provider_path);
        return TRUE;
    }

#ifdef RTLD_NOLOAD
    int flags = RTLD_LAZY | RTLD_NOLOAD;
#else
    int flags = RTLD_LAZY;
#endif
    if (original_dlopen != NULL) {
        handle = original_dlopen(provider_path, flags);
    } else {
        handle = dlopen(provider_path, flags);
    }
    if (handle == NULL) {
        g_free(provider_path);
        return FALSE;
    }

    pin = g_new0(PeakDlopenProviderPin, 1);
    pin->handle = handle;
    pin->provider_path = provider_path;
    *pin_out = pin;
    return TRUE;
}

void
dlopen_interceptor_commit_pinned_provider(void* pin_value)
{
    PeakDlopenProviderPin* pin = pin_value;

    if (pin == NULL) {
        return;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_retained_provider_paths == NULL) {
        dynamic_attach_retained_provider_paths =
            g_hash_table_new_full(g_str_hash,
                                  dlopen_interceptor_string_equal,
                                  g_free,
                                  NULL);
    }
    if (!g_hash_table_contains(dynamic_attach_retained_provider_paths,
                               pin->provider_path)) {
        if (dynamic_attach_retained_handles == NULL) {
            dynamic_attach_retained_handles =
                g_ptr_array_new_with_free_func(
                    dlopen_interceptor_close_retained_handle);
        }
        g_hash_table_add(dynamic_attach_retained_provider_paths,
                         pin->provider_path);
        g_ptr_array_add(dynamic_attach_retained_handles, pin->handle);
        dynamic_attach_retained_handle_count++;
        pin->provider_path = NULL;
        pin->handle = NULL;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    dlopen_interceptor_close_retained_handle(pin->handle);
    g_free(pin->provider_path);
    g_free(pin);
}

void
dlopen_interceptor_release_pinned_provider(void* pin_value)
{
    PeakDlopenProviderPin* pin = pin_value;

    if (pin == NULL) {
        return;
    }
    dlopen_interceptor_close_retained_handle(pin->handle);
    g_free(pin->provider_path);
    g_free(pin);
}

static PeakDlopenDynamicAttachRequest*
dlopen_interceptor_new_dynamic_attach_request(gboolean caller_waits)
{
    PeakDlopenDynamicAttachRequest* request = g_new0(PeakDlopenDynamicAttachRequest, 1);

    pthread_mutex_init(&request->completion_mutex, NULL);
    pthread_cond_init(&request->completion_cond, NULL);
    request->state = PEAK_DLOPEN_REQUEST_PENDING;
    request->caller_waits = caller_waits;
    return request;
}

static void
dlopen_interceptor_destroy_dynamic_attach_request(
    PeakDlopenDynamicAttachRequest* request)
{
    if (request == NULL) {
        return;
    }
    if (request->handle != NULL) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (request->handle != PEAK_DLOPEN_TEST_RETRY_HANDLE)
#endif
        {
            dlclose(request->handle);
        }
    }
    dlopen_interceptor_release_pinned_provider(
        request->resolution_provider_pin);
    g_free(request->filename);
    pthread_cond_destroy(&request->completion_cond);
    pthread_mutex_destroy(&request->completion_mutex);
    g_free(request);
}

static void
dlopen_interceptor_complete_dynamic_attach_request(
    PeakDlopenDynamicAttachRequest* request,
    PeakDlopenRequestState state)
{
    gboolean caller_waits;

    if (request->retain_handle && request->handle != NULL) {
        dlopen_interceptor_retain_dynamic_handle(request->handle);
        request->handle = NULL;
    }

    pthread_mutex_lock(&request->completion_mutex);
    caller_waits = request->caller_waits;
    request->state = state;
    pthread_cond_broadcast(&request->completion_cond);
    pthread_mutex_unlock(&request->completion_mutex);

    /*
     * A caller-owned waiter may destroy request as soon as the completion
     * mutex is released.  Never dereference request after that release on the
     * caller-waits path; the local ownership snapshot decides who frees it.
     */
    if (!caller_waits) {
        dlopen_interceptor_destroy_dynamic_attach_request(request);
    }
}

static PeakDlopenRequestState
dlopen_interceptor_wait_for_dynamic_attach_request(
    PeakDlopenDynamicAttachRequest* request)
{
    PeakDlopenRequestState state;

    pthread_mutex_lock(&request->completion_mutex);
    while (request->state == PEAK_DLOPEN_REQUEST_PENDING) {
        if (request->resolution_requested &&
            !request->resolution_complete) {
            const char* provider_name = request->resolution_provider_name;
            const char* symbol_name = request->resolution_symbol_name;
            GumAddress resolved_address = 0;
            void* resolved_provider_pin = NULL;
            gboolean provider_pin_failed = FALSE;

            pthread_mutex_unlock(&request->completion_mutex);
#ifdef RTLD_NOLOAD
            if (provider_name != NULL && symbol_name != NULL) {
                PeakDlopenDynamicAttachRequest* previous_resolution_request =
                    dynamic_attach_resolution_wait_request;
                void* provider_handle;
                void* address = NULL;

                /*
                 * The caller may still own the dynamic linker's recursive
                 * load lock when this dlopen originated in a DSO
                 * constructor.  Resolve on that same thread.  Resolving on
                 * the controller would invert the loader-lock/controller
                 * handshake and deadlock both threads.
                 */
                dynamic_attach_resolution_wait_request = request;
                provider_handle = original_dlopen(
                    provider_name,
                    request->binding_flags | RTLD_NOLOAD);
                if (provider_handle != NULL) {
                    dlerror();
                    address = dlsym(provider_handle, symbol_name);
                    if (dlerror() != NULL) {
                        address = NULL;
                    }
                    /*
                     * An IFUNC may return code from an unrelated DSO.  Pin
                     * the dladdr() owner while this loader thread can safely
                     * take a recursive RTLD_NOLOAD reference; retaining only
                     * provider_handle would leave Gum pointing into an
                     * unloadable implementation mapping.
                     */
                    if (address != NULL &&
                        !dlopen_interceptor_pin_loaded_provider(
                            address,
                            &resolved_provider_pin)) {
                        provider_pin_failed = TRUE;
                        address = NULL;
                    }
                    dlclose(provider_handle);
                }
                dynamic_attach_resolution_wait_request =
                    previous_resolution_request;
                resolved_address = (GumAddress)(guintptr)address;
            }
#else
            (void)provider_name;
            (void)symbol_name;
#endif
            pthread_mutex_lock(&request->completion_mutex);
            request->resolution_address = resolved_address;
            request->resolution_provider_pin = resolved_provider_pin;
            request->resolution_provider_pin_failed =
                provider_pin_failed;
            request->resolution_complete = TRUE;
            pthread_cond_broadcast(&request->completion_cond);
            continue;
        }
        pthread_cond_wait(&request->completion_cond,
                          &request->completion_mutex);
    }
    state = request->state;
    pthread_mutex_unlock(&request->completion_mutex);
    return state;
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
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static void dlopen_interceptor_drain_dynamic_attach_queue_with_budget(size_t max_requests);

static PeakDlopenDynamicAttachRequest*
dlopen_interceptor_pop_dynamic_attach_request(void)
{
    PeakDlopenDynamicAttachRequest* request;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_queue_length == 0) {
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        return NULL;
    }

    request = dynamic_attach_queue_head;
    dynamic_attach_queue_head = request->next;
    request->next = NULL;
    dynamic_attach_queue_length--;
    if (dynamic_attach_queue_head == NULL) {
        dynamic_attach_queue_tail = NULL;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    return request;
}

static gboolean
dlopen_interceptor_requeue_dynamic_attach_request(PeakDlopenDynamicAttachRequest* request)
{
    gboolean requeued = FALSE;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dlopen_interceptor_queue_can_accept_unlocked()) {
        request->next = NULL;
        if (dynamic_attach_queue_tail != NULL) {
            dynamic_attach_queue_tail->next = request;
        } else {
            dynamic_attach_queue_head = request;
        }
        dynamic_attach_queue_tail = request;
        dynamic_attach_queue_length++;
        dynamic_attach_requeue_count++;
        if (dynamic_attach_queue_length > dynamic_attach_queue_max_depth) {
            dynamic_attach_queue_max_depth = dynamic_attach_queue_length;
        }
        pthread_cond_signal(&dynamic_attach_gate_cond);
        requeued = TRUE;
    } else {
        dynamic_attach_drop_requeue_count++;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (requeued) {
        peak_general_listener_controller_wake();
    }
    return requeued;
}

static void
dlopen_interceptor_discard_dynamic_attach_queue(void)
{
    PeakDlopenDynamicAttachRequest* request;

    while ((request = dlopen_interceptor_pop_dynamic_attach_request()) != NULL) {
        dlopen_interceptor_complete_dynamic_attach_request(
            request,
            PEAK_DLOPEN_REQUEST_CANCELLED);
    }
}

static gboolean
dlopen_interceptor_enqueue_dynamic_attach_request(
    PeakDlopenDynamicAttachRequest* request)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (!dlopen_interceptor_queue_can_accept_unlocked()) {
        dynamic_attach_drop_closed_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        return FALSE;
    }

    request->next = NULL;
    if (dynamic_attach_queue_tail != NULL) {
        dynamic_attach_queue_tail->next = request;
    } else {
        dynamic_attach_queue_head = request;
    }
    dynamic_attach_queue_tail = request;
    dynamic_attach_queue_length++;
    dynamic_attach_enqueue_count++;
    if (dynamic_attach_queue_length > dynamic_attach_queue_max_depth) {
        dynamic_attach_queue_max_depth = dynamic_attach_queue_length;
    }
    pthread_cond_signal(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    /*
     * The controller may currently be waiting for this same caller to finish
     * an IFUNC lookup.  Wake that outer request so it can service the nested
     * dlopen before the resolver makes its first call into the new provider.
     */
    PeakDlopenDynamicAttachRequest* resolution_request =
        dynamic_attach_resolution_wait_request;
    if (resolution_request != NULL) {
        pthread_mutex_lock(&resolution_request->completion_mutex);
        resolution_request->resolution_nested_generation++;
        pthread_cond_broadcast(&resolution_request->completion_cond);
        pthread_mutex_unlock(&resolution_request->completion_mutex);
    }

    peak_general_listener_controller_wake();

    return TRUE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
void
dlopen_interceptor_test_reset_dynamic_attach(gboolean open)
{
    GPtrArray* retained_handles;
    GHashTable* retained_provider_paths;

    dlopen_interceptor_discard_dynamic_attach_queue();

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state =
        open ? PEAK_DLOPEN_CONTROLLER_OPEN : PEAK_DLOPEN_CONTROLLER_CLOSED;
    active_dynamic_attach_count = 0;
    atomic_store_explicit(&active_dlopen_replacement_count,
                          0,
                          memory_order_release);
    dynamic_attach_drain_active = FALSE;
    dynamic_load_controller_mutation_pending = FALSE;
    dynamic_attach_queue_head = NULL;
    dynamic_attach_queue_tail = NULL;
    dynamic_attach_queue_length = 0;
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
    retained_handles = dynamic_attach_retained_handles;
    dynamic_attach_retained_handles = NULL;
    retained_provider_paths = dynamic_attach_retained_provider_paths;
    dynamic_attach_retained_provider_paths = NULL;
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (retained_handles != NULL) {
        g_ptr_array_free(retained_handles, TRUE);
    }
    if (retained_provider_paths != NULL) {
        g_hash_table_destroy(retained_provider_paths);
    }
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
    PeakDlopenDynamicAttachRequest* request =
        dlopen_interceptor_new_dynamic_attach_request(FALSE);
    gboolean accepted;

    request->filename = g_strdup(filename != NULL ? filename : "<test>");
    request->handle = handle;
    accepted = dlopen_interceptor_enqueue_dynamic_attach_request(request);
    if (!accepted) {
        dlopen_interceptor_destroy_dynamic_attach_request(request);
    }
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
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_partial_success_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    dlopen_interceptor_retain_dynamic_handle(PEAK_DLOPEN_TEST_RETAINED_HANDLE);
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
    dlopen_interceptor_trace_counters(event);
}
#endif

typedef struct {
    PeakDlopenDynamicAttachRequest* request;
    GumModule* module;
    const char* provider_name;
    gboolean matched;
    gboolean retry;
    gboolean failed;
} PeakDlopenExportScan;

typedef struct {
    GQueue* pending_modules;
    GHashTable* seen_modules;
} PeakDlopenDependencyScan;

#if defined(__linux__) && defined(RTLD_NOLOAD)
typedef struct {
    const char* provider_name;
    gboolean matched;
} PeakDlopenLoadedModuleProbe;

typedef struct {
    gchar* path;
    GumAddress address;
} PeakDlopenLoadedModuleSnapshot;

static void
dlopen_interceptor_free_loaded_module_snapshot(gpointer value)
{
    PeakDlopenLoadedModuleSnapshot* snapshot = value;

    if (snapshot == NULL) {
        return;
    }
    g_free(snapshot->path);
    g_free(snapshot);
}

static gboolean
dlopen_interceptor_probe_loaded_export(const GumExportDetails* details,
                                       gpointer user_data)
{
    PeakDlopenLoadedModuleProbe* probe = user_data;

    if (details->type == GUM_EXPORT_FUNCTION &&
        peak_general_listener_dynamic_symbol_matches_any_target(
            details->name,
            probe->provider_name)) {
        probe->matched = TRUE;
        return FALSE;
    }
    return TRUE;
}

static gboolean
dlopen_interceptor_probe_loaded_callable_unknown(
    const GumSymbolDetails* details,
    gpointer user_data)
{
    PeakDlopenLoadedModuleProbe* probe = user_data;

    if (details->is_global &&
        details->type == GUM_SYMBOL_UNKNOWN &&
        details->section != NULL &&
        (details->section->protection & GUM_PAGE_EXECUTE) != 0 &&
        details->address != 0 &&
        peak_general_listener_dynamic_symbol_matches_any_target(
            details->name,
            probe->provider_name)) {
        probe->matched = TRUE;
        return FALSE;
    }
    return TRUE;
}

static gboolean
dlopen_interceptor_collect_loaded_matching_module(GumModule* module,
                                                  gpointer user_data)
{
    GPtrArray* snapshots = user_data;
    const char* path = gum_module_get_path(module);
    const GumMemoryRange* range = gum_module_get_range(module);
    PeakDlopenLoadedModuleProbe probe = {
        .provider_name = path,
        .matched = FALSE
    };

    if (path == NULL || path[0] == '\0' || range == NULL) {
        return TRUE;
    }

    gum_module_enumerate_exports(module,
                                 dlopen_interceptor_probe_loaded_export,
                                 &probe);
    if (!probe.matched) {
        gum_module_enumerate_symbols(
            module,
            dlopen_interceptor_probe_loaded_callable_unknown,
            &probe);
    }
    if (probe.matched) {
        PeakDlopenLoadedModuleSnapshot* snapshot =
            g_new0(PeakDlopenLoadedModuleSnapshot, 1);
        snapshot->path = g_strdup(path);
        snapshot->address = range->base_address;
        g_ptr_array_add(snapshots, snapshot);
    }
    return TRUE;
}
#endif

static gboolean
dlopen_interceptor_string_equal(gconstpointer left, gconstpointer right)
{
    return strcmp(left, right) == 0;
}

static gboolean
dlopen_interceptor_attach_scanned_symbol(PeakDlopenExportScan* scan,
                                         const char* symbol_name,
                                         GumAddress symbol_address,
                                         gsize symbol_size,
                                         void* resolved_provider_pin)
{
    PeakDynamicAttachResult result;

    scan->matched = TRUE;
    result = peak_general_listener_dynamic_attach_symbol(
        symbol_name,
        GSIZE_TO_POINTER(symbol_address),
        symbol_size,
        scan->provider_name);
    if (result == PEAK_DYNAMIC_ATTACH_ATTACHED) {
        dlopen_interceptor_commit_pinned_provider(
            resolved_provider_pin);
        resolved_provider_pin = NULL;
        scan->request->retain_handle = TRUE;
    } else if (result == PEAK_DYNAMIC_ATTACH_RETRY) {
        scan->retry = TRUE;
    } else if (result == PEAK_DYNAMIC_ATTACH_FAILED) {
        scan->failed = TRUE;
    }
    dlopen_interceptor_release_pinned_provider(
        resolved_provider_pin);
    return TRUE;
}

static gboolean
dlopen_interceptor_scan_export(const GumExportDetails* details,
                               gpointer user_data)
{
    PeakDlopenExportScan* scan = user_data;

    if (details->type != GUM_EXPORT_FUNCTION ||
        !peak_general_listener_dynamic_symbol_matches_any_target(
            details->name,
            scan->provider_name)) {
        return TRUE;
    }

    return dlopen_interceptor_attach_scanned_symbol(
        scan,
        details->name,
        details->address,
        details->size > 0 ? (gsize)details->size : 0,
        NULL);
}

#if defined(__linux__)
static GumAddress
dlopen_interceptor_resolve_symbol_on_waiting_caller(
    PeakDlopenDynamicAttachRequest* request,
    const char* provider_name,
    const char* symbol_name,
    void** provider_pin_out,
    gboolean* provider_pin_failed_out)
{
    GumAddress resolved_address;

    *provider_pin_out = NULL;
    *provider_pin_failed_out = FALSE;

    if (!request->caller_waits) {
        return 0;
    }

    pthread_mutex_lock(&request->completion_mutex);
    if (request->state != PEAK_DLOPEN_REQUEST_PENDING) {
        pthread_mutex_unlock(&request->completion_mutex);
        return 0;
    }

    request->resolution_provider_name = provider_name;
    request->resolution_symbol_name = symbol_name;
    request->resolution_address = 0;
    request->resolution_provider_pin = NULL;
    request->resolution_provider_pin_failed = FALSE;
    request->resolution_complete = FALSE;
    request->resolution_requested = TRUE;
    pthread_cond_broadcast(&request->completion_cond);

    while (!request->resolution_complete &&
           request->state == PEAK_DLOPEN_REQUEST_PENDING) {
        unsigned long long observed_nested_generation =
            request->resolution_nested_generation;
        gboolean serviced_nested_request;

        pthread_mutex_unlock(&request->completion_mutex);
        serviced_nested_request =
            dlopen_interceptor_service_resolution_nested_request();
        pthread_mutex_lock(&request->completion_mutex);
        if (!request->resolution_complete &&
            request->state == PEAK_DLOPEN_REQUEST_PENDING &&
            !serviced_nested_request &&
            request->resolution_nested_generation ==
                observed_nested_generation) {
            pthread_cond_wait(&request->completion_cond,
                              &request->completion_mutex);
        }
    }
    resolved_address = request->resolution_address;
    *provider_pin_out = request->resolution_provider_pin;
    *provider_pin_failed_out = request->resolution_provider_pin_failed;
    request->resolution_provider_name = NULL;
    request->resolution_symbol_name = NULL;
    request->resolution_address = 0;
    request->resolution_provider_pin = NULL;
    request->resolution_provider_pin_failed = FALSE;
    request->resolution_requested = FALSE;
    request->resolution_complete = FALSE;
    pthread_mutex_unlock(&request->completion_mutex);

    return resolved_address;
}

/*
 * Gum's ELF export enumeration intentionally emits only STT_FUNC and
 * STT_OBJECT.  Callable STT_GNU_IFUNC and STT_NOTYPE symbols therefore need
 * a second path.  Symbol enumeration establishes that the exact provider
 * defines a global executable symbol.  The original dlopen caller performs
 * provider-scoped lookup because it may still own the dynamic linker's lock;
 * this also delegates IFUNC selection to the linker and returns the callable
 * entry point, not the resolver stub recorded in the ELF symbol table.
 */
static gboolean
dlopen_interceptor_scan_callable_unknown_symbol(
    const GumSymbolDetails* details,
    gpointer user_data)
{
    PeakDlopenExportScan* scan = user_data;
    GumAddress resolved_address;
    void* resolved_provider_pin = NULL;
    gboolean provider_pin_failed = FALSE;

    if (!details->is_global ||
        details->type != GUM_SYMBOL_UNKNOWN ||
        details->section == NULL ||
        (details->section->protection & GUM_PAGE_EXECUTE) == 0 ||
        details->address == 0 ||
        !peak_general_listener_dynamic_symbol_matches_any_target(
            details->name,
            scan->provider_name)) {
        return TRUE;
    }

    resolved_address = dlopen_interceptor_resolve_symbol_on_waiting_caller(
        scan->request,
        scan->provider_name,
        details->name,
        &resolved_provider_pin,
        &provider_pin_failed);
    if (resolved_address == 0 && !scan->request->caller_waits) {
        resolved_address = gum_module_find_export_by_name(scan->module,
                                                           details->name);
        if (resolved_address != 0 &&
            !dlopen_interceptor_pin_loaded_provider(
                GSIZE_TO_POINTER(resolved_address),
                &resolved_provider_pin)) {
            provider_pin_failed = TRUE;
            resolved_address = 0;
        }
    }
    if (provider_pin_failed) {
        scan->matched = TRUE;
        scan->failed = TRUE;
        g_printerr("[peak] skipping dynamic attach for %s from %s: unable to retain the provider owning its resolved address\n",
                   details->name,
                   scan->provider_name != NULL
                       ? scan->provider_name
                       : "<unknown>");
        dlopen_interceptor_release_pinned_provider(
            resolved_provider_pin);
        return TRUE;
    }
    if (resolved_address == 0) {
        dlopen_interceptor_release_pinned_provider(
            resolved_provider_pin);
        return TRUE;
    }

    return dlopen_interceptor_attach_scanned_symbol(
        scan,
        details->name,
        resolved_address,
        details->size > 0 ? (gsize)details->size : 0,
        resolved_provider_pin);
}
#endif

static void
dlopen_interceptor_add_scan_module(PeakDlopenDependencyScan* scan,
                                   GumModule* module)
{
    const char* path;

    if (module == NULL) {
        return;
    }
    path = gum_module_get_path(module);
    if (path == NULL || g_hash_table_contains(scan->seen_modules, path)) {
        g_object_unref(module);
        return;
    }
    g_hash_table_add(scan->seen_modules, g_strdup(path));
    g_queue_push_tail(scan->pending_modules, module);
}

static gboolean
dlopen_interceptor_scan_dependency(const GumDependencyDetails* details,
                                   gpointer user_data)
{
    PeakDlopenDependencyScan* scan = user_data;
    GumModule* dependency = gum_process_find_module_by_name(details->name);

    dlopen_interceptor_add_scan_module(scan, dependency);
    return TRUE;
}

static gboolean
dlopen_interceptor_loaded_handle_may_match(void* handle,
                                           const char* filename)
{
    GQueue pending_modules = G_QUEUE_INIT;
    GHashTable* seen_modules;
    GPtrArray* target_names;
    GumModule* root_module = NULL;
    GumAddress module_address = 0;
    gboolean matched = FALSE;

    if (handle == NULL || filename == NULL) {
        return FALSE;
    }

#if defined(__linux__)
    {
        struct link_map* module_map = NULL;

        if (dlinfo(handle, RTLD_DI_LINKMAP, &module_map) == 0 &&
            module_map != NULL) {
            module_address = module_map->l_addr != 0
                ? (GumAddress)module_map->l_addr
                : (GumAddress)module_map->l_ld;
        }
    }
#endif
    if (module_address != 0) {
        root_module = gum_process_find_module_by_address(module_address);
    }
    if (root_module == NULL) {
        root_module = gum_process_find_module_by_name(filename);
        if (root_module == NULL) {
            const char* basename = strrchr(filename, '/');

            if (basename != NULL && basename[1] != '\0') {
                root_module = gum_process_find_module_by_name(basename + 1);
            }
        }
    }
    if (root_module == NULL) {
        /* Unknown module identity is not proof of no match. */
        return TRUE;
    }

    target_names =
        peak_general_listener_snapshot_dynamic_target_names();
    seen_modules = g_hash_table_new_full(g_str_hash,
                                         dlopen_interceptor_string_equal,
                                         g_free,
                                         NULL);
    PeakDlopenDependencyScan dependency_scan = {
        .pending_modules = &pending_modules,
        .seen_modules = seen_modules
    };
    dlopen_interceptor_add_scan_module(&dependency_scan, root_module);

    while (!matched && !g_queue_is_empty(&pending_modules)) {
        GumModule* module = g_queue_pop_head(&pending_modules);
        const char* provider_name = gum_module_get_path(module);

        for (guint i = 0; i < target_names->len; i++) {
            const char* target_name = g_ptr_array_index(target_names, i);
            GumAddress symbol_address =
                gum_module_find_symbol_by_name(module, target_name);

            if (symbol_address != 0 &&
                peak_general_listener_dynamic_symbol_address_needs_attach(
                    target_name,
                    GSIZE_TO_POINTER(symbol_address),
                    provider_name)) {
                matched = TRUE;
                break;
            }
        }
        if (!matched) {
            gum_module_enumerate_dependencies(
                module,
                dlopen_interceptor_scan_dependency,
                &dependency_scan);
        }
        g_object_unref(module);
    }

    while (!g_queue_is_empty(&pending_modules)) {
        g_object_unref(g_queue_pop_head(&pending_modules));
    }
    g_hash_table_destroy(seen_modules);
    g_ptr_array_free(target_names, TRUE);
    return matched;
}

static PeakDlopenAttachResult
dlopen_interceptor_attach_from_request(PeakDlopenDynamicAttachRequest* request)
{
    GQueue pending_modules = G_QUEUE_INIT;
    GHashTable* seen_modules;
    GumModule* root_module;
    gboolean matched = FALSE;
    gboolean retry = FALSE;
    gboolean failed = FALSE;

#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* delay_value = g_getenv("PEAK_TEST_DLOPEN_ATTACH_DELAY_MS");
    if (delay_value != NULL && delay_value[0] != '\0') {
        char* end = NULL;
        guint64 delay_ms = g_ascii_strtoull(delay_value, &end, 10);
        if (end != delay_value && *end == '\0' && delay_ms <= 10000) {
            g_usleep(delay_ms * 1000);
        }
    }
    if (request->handle == PEAK_DLOPEN_TEST_RETRY_HANDLE) {
        return PEAK_DLOPEN_ATTACH_RETRY;
    }
    if (request->test_force_attach_failure) {
        g_printerr("[peak] test forcing first startup provider rescan failure for %s\n",
                   request->filename != NULL
                       ? request->filename
                       : "<unknown>");
        return PEAK_DLOPEN_ATTACH_FAILED;
    }
#endif
    if (request->handle == NULL) {
        return PEAK_DLOPEN_ATTACH_FAILED;
    }

    root_module = request->module_address != 0
        ? gum_process_find_module_by_address(request->module_address)
        : NULL;
    if (root_module == NULL && request->filename != NULL) {
        root_module = gum_process_find_module_by_name(request->filename);
        if (root_module == NULL) {
            const char* basename = strrchr(request->filename, '/');
            if (basename != NULL && basename[1] != '\0') {
                root_module = gum_process_find_module_by_name(basename + 1);
            }
        }
    }
    if (root_module == NULL) {
        g_printerr("[peak] dynamic attach could not identify loaded module %s\n",
                   request->filename != NULL ? request->filename : "<unknown>");
        return PEAK_DLOPEN_ATTACH_FAILED;
    }

    seen_modules = g_hash_table_new_full(g_str_hash,
                                         dlopen_interceptor_string_equal,
                                         g_free,
                                         NULL);
    PeakDlopenDependencyScan dependency_scan = {
        .pending_modules = &pending_modules,
        .seen_modules = seen_modules
    };
    dlopen_interceptor_add_scan_module(&dependency_scan, root_module);

    while (!g_queue_is_empty(&pending_modules)) {
        GumModule* module = g_queue_pop_head(&pending_modules);
        const char* provider_name = gum_module_get_path(module);
        PeakDlopenExportScan export_scan = {
            .request = request,
            .module = module,
            .provider_name = provider_name
        };

        gum_module_enumerate_exports(module,
                                     dlopen_interceptor_scan_export,
                                     &export_scan);
#if defined(__linux__)
        gum_module_enumerate_symbols(
            module,
            dlopen_interceptor_scan_callable_unknown_symbol,
            &export_scan);
#endif
        gum_module_enumerate_dependencies(module,
                                          dlopen_interceptor_scan_dependency,
                                          &dependency_scan);
        matched |= export_scan.matched;
        retry |= export_scan.retry;
        failed |= export_scan.failed;
        g_object_unref(module);
    }
    g_hash_table_destroy(seen_modules);

    if (request->retain_handle && retry) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_partial_success_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    }
    if (failed) {
        g_printerr("[peak] dynamic attach permanently failed for one or more targets from %s\n",
                   request->filename != NULL ? request->filename : "<unknown>");
    }
    if (retry) {
        return PEAK_DLOPEN_ATTACH_RETRY;
    }
    if (failed) {
        return PEAK_DLOPEN_ATTACH_FAILED;
    }
    (void)matched;
    return PEAK_DLOPEN_ATTACH_DONE;
}

static void
dlopen_interceptor_process_dynamic_attach_request(
    PeakDlopenDynamicAttachRequest* request)
{
    PeakDlopenAttachResult result =
        dlopen_interceptor_attach_from_request(request);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_drain_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    if (result == PEAK_DLOPEN_ATTACH_RETRY &&
        !request->caller_waits &&
        dlopen_interceptor_requeue_dynamic_attach_request(request)) {
        return;
    }
    if (result == PEAK_DLOPEN_ATTACH_RETRY && request->caller_waits) {
        /*
         * TIMEOUT/CLASSIFY_FAILED means this bounded safety preparation could
         * not prove that an initial attach is safe.  A synchronous dlopen
         * caller must fail open at that observed result: requeueing it would
         * create an untimed application stall with no state transition that
         * guarantees a later attempt can succeed.  Background requests keep
         * their existing retry behavior.
         */
        g_printerr("[peak] dynamic attach failed open for %s because a safe mutation could not be established; returning the original dlopen handle without waiting for a retry\n",
                   request->filename != NULL ? request->filename : "<unknown>");
    }
    dlopen_interceptor_complete_dynamic_attach_request(
        request,
        result == PEAK_DLOPEN_ATTACH_RETRY
            ? PEAK_DLOPEN_REQUEST_CANCELLED
            : (result == PEAK_DLOPEN_ATTACH_FAILED
                   ? PEAK_DLOPEN_REQUEST_FAILED
                   : (request->retain_handle
                          ? PEAK_DLOPEN_REQUEST_COMPLETE
                          : PEAK_DLOPEN_REQUEST_NO_MATCH)));
}

static gboolean
dlopen_interceptor_service_resolution_nested_request(void)
{
    PeakDlopenDynamicAttachRequest* request =
        dlopen_interceptor_pop_dynamic_attach_request();

    if (request == NULL) {
        return FALSE;
    }
    dlopen_interceptor_process_dynamic_attach_request(request);
    return TRUE;
}

static void
dlopen_interceptor_drain_dynamic_attach_queue_with_budget(size_t max_requests)
{
    size_t drained = 0;
    size_t initial_queue_length = 0;
    size_t drain_limit;
    PeakDlopenDynamicAttachRequest* request;

    if (dynamic_attach_drain_reentrant) {
        return;
    }

    if (!dlopen_interceptor_begin_dynamic_attach_drain(&initial_queue_length)) {
        return;
    }

    drain_limit = initial_queue_length;
    if (max_requests != 0 && max_requests < drain_limit) {
        drain_limit = max_requests;
    }

    dynamic_attach_drain_reentrant = TRUE;
    while (drained < drain_limit &&
           (request = dlopen_interceptor_pop_dynamic_attach_request()) != NULL) {
        dlopen_interceptor_process_dynamic_attach_request(request);
        drained++;
    }
    dynamic_attach_drain_reentrant = FALSE;

    dlopen_interceptor_end_dynamic_attach_drain();
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

gboolean
dlopen_interceptor_rescan_loaded_modules(void)
{
#if defined(__linux__) && defined(RTLD_NOLOAD)
    GPtrArray* snapshots;
    gboolean success = TRUE;
    gboolean admission_open;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    admission_open =
        dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
        original_dlopen != NULL;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    if (!admission_open) {
        return FALSE;
    }

    snapshots = g_ptr_array_new_with_free_func(
        dlopen_interceptor_free_loaded_module_snapshot);
    gum_process_enumerate_modules(
        dlopen_interceptor_collect_loaded_matching_module,
        snapshots);

    for (guint i = 0; i < snapshots->len; i++) {
        PeakDlopenLoadedModuleSnapshot* snapshot =
            g_ptr_array_index(snapshots, i);
        PeakDlopenDynamicAttachRequest* request;
        PeakDlopenRequestState request_state;
        void* handle;

        /*
         * The startup constructor may still own the loader's recursive lock.
         * Keep controller lifecycle/JIT mutations outside this exact scan and
         * let any IFUNC resolution run on this original caller below.
         */
        dlopen_interceptor_begin_load_transaction();
        handle = original_dlopen(snapshot->path, RTLD_LAZY | RTLD_NOLOAD);
        if (handle == NULL) {
            dlopen_interceptor_end_load_transaction();
            continue;
        }

        request = dlopen_interceptor_new_dynamic_attach_request(TRUE);
        request->filename = g_strdup(snapshot->path);
        request->handle = handle;
        request->module_address = snapshot->address;
        request->binding_flags = RTLD_LAZY;
#ifdef PEAK_ENABLE_TEST_HOOKS
        {
            const char* fail_first =
                g_getenv("PEAK_TEST_STARTUP_RESCAN_FAIL_FIRST");
            request->test_force_attach_failure =
                i == 0 && fail_first != NULL && fail_first[0] != '\0' &&
                strcmp(fail_first, "0") != 0;
        }
#endif
        if (!dlopen_interceptor_enqueue_dynamic_attach_request(request)) {
            dlopen_interceptor_destroy_dynamic_attach_request(request);
            dlopen_interceptor_end_load_transaction();
            success = FALSE;
            break;
        }

        request_state =
            dlopen_interceptor_wait_for_dynamic_attach_request(request);
        dlopen_interceptor_destroy_dynamic_attach_request(request);
        dlopen_interceptor_end_load_transaction();

        if (request_state == PEAK_DLOPEN_REQUEST_FAILED ||
            request_state == PEAK_DLOPEN_REQUEST_CANCELLED) {
            gboolean admission_still_open;

            success = FALSE;
            pthread_mutex_lock(&dynamic_attach_gate_mutex);
            admission_still_open =
                dlopen_interceptor_queue_can_accept_unlocked();
            pthread_mutex_unlock(&dynamic_attach_gate_mutex);
            if (!admission_still_open) {
                break;
            }
        }
    }

    g_ptr_array_free(snapshots, TRUE);
    return success;
#else
    return TRUE;
#endif
}

gboolean
dlopen_interceptor_enable_dynamic_attach(void)
{
    gboolean enabled = FALSE;

    if (!peak_general_listener_controller_is_ready()) {
        g_printerr("[peak] refusing to enable dlopen dynamic attach without a running general listener controller\n");
        return FALSE;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN &&
        atomic_load_explicit(&dlopen_replacement_installed_by_peak,
                             memory_order_acquire) &&
        dlopen_interceptor != NULL &&
        dlopen_hook_address != NULL &&
        original_dlopen != NULL) {
        dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_OPEN;
        enabled = TRUE;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return enabled;
}

gboolean
dlopen_interceptor_shutdown_dynamic_attach(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN;
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    dlopen_interceptor_discard_dynamic_attach_queue();

    if (!dlopen_interceptor_wait_for_dynamic_attach_idle()) {
        g_printerr("[peak] dlopen dynamic attach drain timed out; leaving dlopen interceptor state alive\n");
        dlopen_interceptor_trace_counters("shutdown-dynamic-timeout");
        return FALSE;
    }

    dlopen_interceptor_trace_counters("shutdown");
    return TRUE;
}

void
dlopen_interceptor_release_retained_dynamic_handles(void)
{
    GPtrArray* retained_handles = NULL;
    GHashTable* retained_provider_paths = NULL;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    retained_handles = dynamic_attach_retained_handles;
    dynamic_attach_retained_handles = NULL;
    retained_provider_paths = dynamic_attach_retained_provider_paths;
    dynamic_attach_retained_provider_paths = NULL;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (retained_handles != NULL) {
        g_ptr_array_free(retained_handles, TRUE);
    }
    if (retained_provider_paths != NULL) {
        g_hash_table_destroy(retained_provider_paths);
    }
    dlopen_interceptor_trace_counters("release-retained-handles");
}

static void* __attribute__((noinline, no_instrument_function))
peak_dlopen(const char *filename, int flags) {
    static const char fork_warning[] =
        "[peak] fork child dynamic dlopen profiling is unsupported until exec; using the real dlopen without inherited PEAK controller state\n";
    pid_t current_pid;
    pid_t owner_pid;
    int old_cancel_state;
    void *handle;
    PeakDlopenDynamicAttachRequest* request = NULL;
    PeakDlopenRequestState request_state = PEAK_DLOPEN_REQUEST_NO_MATCH;

    /*
     * This must remain the first executable statement.  Strict teardown
     * restores the real dlopen entry while peers are stopped, then waits for
     * this count before releasing Gum's trampoline metadata.  The compile-time
     * lock-free assertion above ensures registration cannot leave the guarded
     * entry range through a helper call before the body becomes visible.
     */
    atomic_fetch_add_explicit(&active_dlopen_replacement_count,
                              1,
                              memory_order_acq_rel);

    current_pid = getpid();
    owner_pid = atomic_load_explicit(&dynamic_attach_owner_pid,
                                     memory_order_acquire);
    if (owner_pid != 0 && owner_pid != current_pid) {
        if (atomic_exchange_explicit(&dynamic_attach_fork_warning_pid,
                                     current_pid,
                                     memory_order_relaxed) != current_pid) {
            (void)write(STDERR_FILENO,
                        fork_warning,
                        sizeof(fork_warning) - 1);
        }
        handle = original_dlopen(filename, flags);
        dlopen_interceptor_end_replacement_call();
        return handle;
    }
    if (dynamic_attach_drain_reentrant ||
        peak_general_listener_controller_is_current_thread()) {
        handle = original_dlopen(filename, flags);
        dlopen_interceptor_end_replacement_call();
        return handle;
    }

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);

    dlopen_interceptor_begin_load_transaction();
#ifdef PEAK_ENABLE_TEST_HOOKS
    dlopen_interceptor_test_pause_replacement_body(filename);
#endif
    handle = original_dlopen(filename, flags);
    // If dlopen failed or no filename, don’t do rescan
    if (handle == NULL || filename == NULL) {
        dlopen_interceptor_end_load_transaction();
        dlopen_interceptor_end_replacement_call();
        pthread_setcancelstate(old_cancel_state, NULL);
        return handle;
    }

    /*
     * Most application loads do not expose a configured PEAK target.  Resolve
     * only the configured names against the new handle first; avoid request
     * allocation, RTLD_NOLOAD reference churn, a controller wakeup, and a
     * complete dependency/export/symbol scan when every visible address is
     * absent or already attached.  The controller scan remains authoritative
     * after this conservative may-match check.
     */
    if (!dlopen_interceptor_loaded_handle_may_match(handle, filename)) {
        dlopen_interceptor_end_load_transaction();
        dlopen_interceptor_end_replacement_call();
        pthread_setcancelstate(old_cancel_state, NULL);
        return handle;
    }

    request = dlopen_interceptor_new_dynamic_attach_request(TRUE);
    request->filename = g_strdup(filename);
#ifdef RTLD_NOLOAD
    int binding_flags = flags & RTLD_BINDING_MASK;
    if (binding_flags == 0) {
        binding_flags = RTLD_LAZY;
    }
    request->binding_flags = binding_flags;
    request->handle = original_dlopen(filename, binding_flags | RTLD_NOLOAD);
#endif
    if (request->handle != NULL) {
#if defined(__linux__)
        struct link_map* module_map = NULL;
        if (dlinfo(request->handle, RTLD_DI_LINKMAP, &module_map) == 0 &&
            module_map != NULL) {
            request->module_address = module_map->l_addr != 0
                ? (GumAddress)module_map->l_addr
                : (GumAddress)module_map->l_ld;
        }
#endif
    } else {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_drop_noload_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    }

    gboolean attach_enqueued = request->handle != NULL &&
        dlopen_interceptor_enqueue_dynamic_attach_request(request);
    if (!attach_enqueued) {
        dlopen_interceptor_destroy_dynamic_attach_request(request);
        request = NULL;
    } else {
        request_state = dlopen_interceptor_wait_for_dynamic_attach_request(request);
        dlopen_interceptor_destroy_dynamic_attach_request(request);
        request = NULL;
    }

    dlopen_interceptor_end_load_transaction();
    dlopen_interceptor_end_replacement_call();
    pthread_setcancelstate(old_cancel_state, NULL);
    if (request_state == PEAK_DLOPEN_REQUEST_FAILED) {
        g_printerr("[peak] dynamic attach reached a permanent failure before dlopen returned for %s\n",
                   filename);
    }
    return handle;
}

static void
dlopen_interceptor_release_uninstalled_replacement(void)
{
    atomic_store_explicit(&dlopen_replacement_installed_by_peak,
                          false,
                          memory_order_release);
    atomic_store_explicit(&dynamic_attach_owner_pid, 0, memory_order_release);
    atomic_store_explicit(&dynamic_attach_fork_warning_pid,
                          0,
                          memory_order_relaxed);
    original_dlopen = NULL;
    dlopen_hook_address = NULL;
    if (dlopen_interceptor != NULL) {
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
    }
}

int dlopen_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;

    if (atomic_load_explicit(&dlopen_replacement_installed_by_peak,
                             memory_order_acquire)) {
        g_printerr("[peak] dlopen Gum replacement is already installed by PEAK, preserving existing ownership\n");
        return GUM_REPLACE_ALREADY_REPLACED;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_store_explicit(&dynamic_load_test_entry_physically_restored,
                          FALSE,
                          memory_order_release);
#endif
    dlopen_interceptor = gum_interceptor_obtain();
    dlopen_hook_address = peak_general_listener_find_function("dlopen");

    if (dlopen_hook_address == NULL) {
        dlopen_interceptor_release_uninstalled_replacement();
        return replace_check;
    }
    if (!peak_general_listener_attach_target_is_supported("dlopen",
                                                          dlopen_hook_address)) {
        g_printerr("[peak] skipping dlopen Gum replace: unsupported target prologue\n");
        dlopen_interceptor_release_uninstalled_replacement();
        return replace_check;
    }

    PeakDetachRequest mutation_request = {
        .hook_id = 0,
        .symbol_name = "dlopen",
        .function_address = dlopen_hook_address,
        .interceptor = dlopen_interceptor,
        .listener = NULL,
        .operation = PEAK_DETACH_OPERATION_REPLACE
    };
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        g_printerr("[peak] skipping dlopen Gum replace: %s\n",
                   peak_detach_controller_status_string(detach_status));
        dlopen_interceptor_release_uninstalled_replacement();
        return replace_check;
    }

    /*
     * Publish fork ownership before the replacement becomes executable.
     * A child created after the patch is visible must bypass inherited parent
     * mutexes even while dynamic-attach admission is still closed.
     */
    atomic_store_explicit(&dynamic_attach_owner_pid,
                          getpid(),
                          memory_order_release);
    gum_interceptor_begin_transaction(dlopen_interceptor);
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (!dlopen_interceptor_test_forced_replace_failure(&replace_check))
#endif
    {
        replace_check = gum_interceptor_replace_fast(
            dlopen_interceptor,
            dlopen_hook_address,
            (gpointer)&peak_dlopen,
            (gpointer*)(&original_dlopen),
            NULL);
    }
    if (replace_check == GUM_REPLACE_OK) {
        /*
         * Record ownership before ending the Gum transaction can publish the
         * replacement to another application thread.
         */
        atomic_store_explicit(&dlopen_replacement_installed_by_peak,
                              true,
                              memory_order_release);
    }
    gum_interceptor_end_transaction(dlopen_interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("dlopen replace finish",
                                                        detach_status);
    }
    if (replace_check != GUM_REPLACE_OK) {
        g_printerr("[peak] skipping dlopen Gum replace: replace-fast-failed-%d\n",
                   replace_check);
        dlopen_interceptor_release_uninstalled_replacement();
        return replace_check;
    }
    return replace_check;
}

gboolean dlopen_interceptor_dettach()
{
    gboolean entry_physically_restored = FALSE;

    if (!dlopen_interceptor_shutdown_dynamic_attach()) {
        return FALSE;
    }

    if (atomic_load_explicit(&dlopen_replacement_installed_by_peak,
                             memory_order_acquire) &&
        dlopen_interceptor != NULL && dlopen_hook_address != NULL) {
        PeakDetachRequest mutation_request = {
            .hook_id = 0,
            .symbol_name = "dlopen",
            .function_address = dlopen_hook_address,
            .interceptor = dlopen_interceptor,
            .listener = NULL,
            .operation = PEAK_DETACH_OPERATION_REVERT,
            .blocked_pc_start = (gpointer)&peak_dlopen,
            .blocked_pc_size = PEAK_DLOPEN_ENTRY_GUARD_BYTES
        };
        PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

        if (!dlopen_interceptor_prepare_hook_mutation_with_retry(
                &mutation_request,
                &detach_status)) {
            g_printerr("[peak] skipping dlopen Gum revert: %s\n",
                       peak_detach_controller_status_string(detach_status));
            return FALSE;
        }
        entry_physically_restored =
            peak_detach_controller_current_mutation_uses_physical_patch();
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (entry_physically_restored) {
            atomic_store_explicit(
                &dynamic_load_test_entry_physically_restored,
                TRUE,
                memory_order_release);
        }
#endif
        if (!entry_physically_restored) {
            gum_interceptor_begin_transaction(dlopen_interceptor);
            gum_interceptor_revert(dlopen_interceptor, dlopen_hook_address);
            gum_interceptor_end_transaction(dlopen_interceptor);
        }
        if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                         &detach_status)) {
            peak_detach_controller_abort_after_failed_finish("dlopen revert finish",
                                                            detach_status);
        }
    }

    /*
     * In strict mode the first guarded REVERT above restores the real entry
     * bytes without releasing Gum's replacement metadata.  Only now is the
     * active count closed against new peak_dlopen entrants, so waiting for
     * already-entered bodies cannot race a later admission.  Compatibility
     * mode performs Gum's single-phase revert above for the same entry-first
     * ordering, without claiming the strict stop-the-world proof.
     */
    if (!dlopen_interceptor_wait_for_replacement_idle()) {
        g_printerr("[peak] dlopen replacement body drain timed out with %u active replacement calls; leaving dlopen interceptor state alive\n",
                   dlopen_interceptor_active_replacement_count());
        dlopen_interceptor_trace_counters("shutdown-replacement-timeout");
        return FALSE;
    }

    if (entry_physically_restored &&
        atomic_load_explicit(&dlopen_replacement_installed_by_peak,
                             memory_order_acquire) &&
        dlopen_interceptor != NULL &&
        dlopen_hook_address != NULL) {
        PeakDetachRequest mutation_request = {
            .hook_id = 0,
            .symbol_name = "dlopen",
            .function_address = dlopen_hook_address,
            .interceptor = dlopen_interceptor,
            .listener = NULL,
            .operation = PEAK_DETACH_OPERATION_REVERT,
            .blocked_pc_start = (gpointer)&peak_dlopen,
            .blocked_pc_size = PEAK_DLOPEN_ENTRY_GUARD_BYTES
        };
        PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

        if (!dlopen_interceptor_prepare_hook_mutation_with_retry(
                &mutation_request,
                &detach_status)) {
            g_printerr("[peak] skipping dlopen Gum metadata revert: %s\n",
                       peak_detach_controller_status_string(detach_status));
            return FALSE;
        }
        gum_interceptor_begin_transaction(dlopen_interceptor);
        gum_interceptor_revert(dlopen_interceptor, dlopen_hook_address);
        gum_interceptor_end_transaction(dlopen_interceptor);
        if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                         &detach_status)) {
            peak_detach_controller_abort_after_failed_finish("dlopen metadata revert finish",
                                                            detach_status);
        }
    }

    if (!dlopen_interceptor_flush_teardown()) {
        g_printerr("[peak] dlopen interceptor teardown did not flush; leaving interceptor state alive\n");
        return FALSE;
    }
    if (dlopen_interceptor != NULL) {
        g_object_unref(dlopen_interceptor);
    }

    atomic_store_explicit(&dlopen_replacement_installed_by_peak,
                          false,
                          memory_order_release);
    atomic_store_explicit(&dynamic_attach_owner_pid, 0, memory_order_release);
    atomic_store_explicit(&dynamic_attach_fork_warning_pid,
                          0,
                          memory_order_relaxed);
    original_dlopen = NULL;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
    dynamic_load_controller_mutation_pending = FALSE;
    dynamic_attach_queue_head = NULL;
    dynamic_attach_queue_tail = NULL;
    dynamic_attach_queue_length = 0;
    pthread_cond_broadcast(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    dlopen_interceptor = NULL;
    dlopen_hook_address = NULL;
    return TRUE;
}
