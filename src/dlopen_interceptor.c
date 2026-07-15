#define _GNU_SOURCE
#include <dlfcn.h>

#include "general_listener.h"
#include "internal/general_listener_internal.h"
#include "dlopen_interceptor.h"
#include "detach_controller.h"
#include "logging.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

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
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_cond;
    PeakDlopenRequestState state;
    gboolean caller_waits;
    gboolean retain_handle;
} PeakDlopenDynamicAttachRequest;

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
    PEAK_DLOPEN_ATTACH_RETRY,
    PEAK_DLOPEN_ATTACH_FAILED
} PeakDlopenAttachResult;

static pthread_mutex_t dynamic_attach_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dynamic_attach_gate_cond = PTHREAD_COND_INITIALIZER;
static PeakDlopenControllerState dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
static unsigned int active_dynamic_attach_count = 0;
static unsigned int active_dlopen_replacement_count = 0;
static gboolean dynamic_attach_drain_active = FALSE;
static PeakDlopenDynamicAttachRequest* dynamic_attach_queue_head = NULL;
static PeakDlopenDynamicAttachRequest* dynamic_attach_queue_tail = NULL;
static size_t dynamic_attach_queue_length = 0;
static GPtrArray* dynamic_attach_retained_handles = NULL;
static __thread gboolean dynamic_attach_drain_reentrant = FALSE;
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
#ifdef PEAK_ENABLE_TEST_HOOKS
static gboolean dynamic_attach_test_manual_drain = FALSE;
static __thread gboolean dynamic_attach_test_explicit_drain = FALSE;
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

static gboolean
dlopen_interceptor_dynamic_attach_prepare_is_retryable(PeakDetachStatus status)
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

static gboolean
dlopen_interceptor_revert_prepare_is_retryable(PeakDetachStatus status)
{
    return dlopen_interceptor_dynamic_attach_prepare_is_retryable(status) ||
           status == PEAK_DETACH_STATUS_PERMISSION_DENIED;
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
dlopen_interceptor_begin_replacement_call(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    active_dlopen_replacement_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static void
dlopen_interceptor_begin_load_transaction(void)
{
    if (dynamic_load_transaction_depth == 0) {
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
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (active_dlopen_replacement_count > 0) {
        active_dlopen_replacement_count--;
    }
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_OPEN) {
        pthread_cond_broadcast(&dynamic_attach_gate_cond);
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static unsigned int
dlopen_interceptor_active_replacement_count(void)
{
    unsigned int count;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    count = active_dlopen_replacement_count;
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
dlopen_interceptor_wait_for_replacement_idle(void)
{
    struct timespec deadline;
    gboolean idle = TRUE;

    clock_gettime(CLOCK_REALTIME, &deadline);
    dlopen_interceptor_add_milliseconds(&deadline,
                                        PEAK_DLOPEN_SHUTDOWN_DRAIN_TIMEOUT_MS);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    while (active_dlopen_replacement_count > 0) {
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

    peak_general_listener_controller_wake();

    return TRUE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
void
dlopen_interceptor_test_reset_dynamic_attach(gboolean open)
{
    dlopen_interceptor_discard_dynamic_attach_queue();

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state =
        open ? PEAK_DLOPEN_CONTROLLER_OPEN : PEAK_DLOPEN_CONTROLLER_CLOSED;
    active_dynamic_attach_count = 0;
    active_dlopen_replacement_count = 0;
    dynamic_attach_drain_active = FALSE;
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
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (dynamic_attach_retained_handles != NULL) {
        GPtrArray* retained_handles;

        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        retained_handles = dynamic_attach_retained_handles;
        dynamic_attach_retained_handles = NULL;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);

        if (retained_handles != NULL) {
            g_ptr_array_free(retained_handles, TRUE);
        }
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
    const char* provider_name;
    gboolean matched;
    gboolean retry;
    gboolean failed;
} PeakDlopenExportScan;

typedef struct {
    GQueue* pending_modules;
    GHashTable* seen_modules;
} PeakDlopenDependencyScan;

static gboolean
dlopen_interceptor_string_equal(gconstpointer left, gconstpointer right)
{
    return strcmp(left, right) == 0;
}

static gboolean
dlopen_interceptor_scan_export(const GumExportDetails* details,
                               gpointer user_data)
{
    PeakDlopenExportScan* scan = user_data;
    PeakDynamicAttachResult result;

    if (details->type != GUM_EXPORT_FUNCTION ||
        !peak_general_listener_dynamic_symbol_matches_any_target(
            details->name,
            scan->provider_name)) {
        return TRUE;
    }

    scan->matched = TRUE;
    result = peak_general_listener_dynamic_attach_symbol(
        details->name,
        GSIZE_TO_POINTER(details->address),
        details->size > 0 ? (gsize)details->size : 0,
        scan->provider_name);
    if (result == PEAK_DYNAMIC_ATTACH_ATTACHED) {
        scan->request->retain_handle = TRUE;
    } else if (result == PEAK_DYNAMIC_ATTACH_RETRY) {
        scan->retry = TRUE;
    } else if (result == PEAK_DYNAMIC_ATTACH_FAILED) {
        scan->failed = TRUE;
    }
    return TRUE;
}

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
            .provider_name = provider_name
        };

        gum_module_enumerate_exports(module,
                                     dlopen_interceptor_scan_export,
                                     &export_scan);
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
        PeakDlopenAttachResult result =
            dlopen_interceptor_attach_from_request(request);
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_drain_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        if (result == PEAK_DLOPEN_ATTACH_RETRY &&
            dlopen_interceptor_requeue_dynamic_attach_request(request)) {
            drained++;
            continue;
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
dlopen_interceptor_enable_dynamic_attach(void)
{
    gboolean enabled = FALSE;

    if (!peak_general_listener_controller_is_ready()) {
        g_printerr("[peak] refusing to enable dlopen dynamic attach without a running general listener controller\n");
        return FALSE;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN &&
        dlopen_interceptor != NULL &&
        dlopen_hook_address != NULL &&
        original_dlopen != NULL) {
        dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_OPEN;
        atomic_store_explicit(&dynamic_attach_owner_pid,
                              getpid(),
                              memory_order_release);
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
    if (!dlopen_interceptor_wait_for_replacement_idle()) {
        g_printerr("[peak] dlopen replacement body drain timed out with %u active replacement calls; leaving dlopen interceptor state alive\n",
                   dlopen_interceptor_active_replacement_count());
        dlopen_interceptor_trace_counters("shutdown-replacement-timeout");
        return FALSE;
    }

    dlopen_interceptor_trace_counters("shutdown");
    return TRUE;
}

void
dlopen_interceptor_release_retained_dynamic_handles(void)
{
    GPtrArray* retained_handles = NULL;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    retained_handles = dynamic_attach_retained_handles;
    dynamic_attach_retained_handles = NULL;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (retained_handles != NULL) {
        g_ptr_array_free(retained_handles, TRUE);
    }
    dlopen_interceptor_trace_counters("release-retained-handles");
}

static void*
peak_dlopen(const char *filename, int flags) {
    pid_t owner_pid = atomic_load_explicit(&dynamic_attach_owner_pid,
                                           memory_order_acquire);
    if ((owner_pid != 0 && owner_pid != getpid()) ||
        dynamic_attach_drain_reentrant ||
        peak_general_listener_controller_is_current_thread()) {
        return original_dlopen(filename, flags);
    }

    int old_cancel_state;
    void *handle;
    PeakDlopenDynamicAttachRequest* request = NULL;
    PeakDlopenRequestState request_state = PEAK_DLOPEN_REQUEST_NO_MATCH;

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);

    dlopen_interceptor_begin_replacement_call();
    dlopen_interceptor_begin_load_transaction();
    handle = original_dlopen(filename, flags);
    // If dlopen failed or no filename, don’t do rescan
    if (handle == NULL || filename == NULL) {
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

int dlopen_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    dlopen_interceptor = gum_interceptor_obtain();
    dlopen_hook_address = peak_general_listener_find_function("dlopen");

    if (dlopen_hook_address == NULL) {
        return replace_check;
    }
    if (!peak_general_listener_attach_target_is_supported("dlopen",
                                                          dlopen_hook_address)) {
        g_printerr("[peak] skipping dlopen Gum replace: unsupported target prologue\n");
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
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
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        return replace_check;
    }

    gum_interceptor_begin_transaction(dlopen_interceptor);
    replace_check = gum_interceptor_replace_fast(dlopen_interceptor,
                                                 dlopen_hook_address,
                                                 (gpointer)&peak_dlopen,
                                                 (gpointer*)(&original_dlopen),
                                                 NULL);
    gum_interceptor_end_transaction(dlopen_interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("dlopen replace finish",
                                                        detach_status);
    }
    return replace_check;
}

gboolean dlopen_interceptor_dettach()
{
    gboolean entry_physically_restored = FALSE;

    if (!dlopen_interceptor_shutdown_dynamic_attach()) {
        return FALSE;
    }

    if (dlopen_interceptor != NULL && dlopen_hook_address != NULL) {
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

    if (entry_physically_restored &&
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

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
    dynamic_attach_queue_head = NULL;
    dynamic_attach_queue_tail = NULL;
    dynamic_attach_queue_length = 0;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    dlopen_interceptor = NULL;
    dlopen_hook_address = NULL;
    return TRUE;
}
