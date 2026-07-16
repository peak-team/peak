#define _GNU_SOURCE
#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>
#endif

#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "detach_controller.h"
#include "logging.h"

#include <errno.h>
#include <pthread.h>
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
static gboolean dlopen_sync_fftw_enabled = FALSE;
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

typedef struct {
    void* handle;
    char* filename;
    int binding_flags;
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
    return dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
           dynamic_attach_queue_length < PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
}

static void
dlopen_interceptor_begin_callback(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    active_dlopen_callback_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

static void
dlopen_interceptor_end_callback(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (active_dlopen_callback_count > 0) {
        active_dlopen_callback_count--;
    }
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_OPEN) {
        pthread_cond_broadcast(&dynamic_attach_gate_cond);
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
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

static void*
dlopen_interceptor_open_unobserved(const char* filename, int flags)
{
    void* handle;

    if (filename == NULL || dlopen_interceptor == NULL) {
        return NULL;
    }

    gum_interceptor_ignore_current_thread(dlopen_interceptor);
    handle = dlopen(filename, flags);
    gum_interceptor_unignore_current_thread(dlopen_interceptor);
    return handle;
}

static void*
dlopen_interceptor_duplicate_dynamic_handle_reference(const char* filename,
                                                      int binding_flags)
{
#ifdef RTLD_NOLOAD
    return dlopen_interceptor_open_unobserved(filename,
                                              binding_flags | RTLD_NOLOAD);
#else
    (void)filename;
    (void)binding_flags;
    return NULL;
#endif
}

static void
dlopen_interceptor_release_dynamic_attach_request(PeakDlopenDynamicAttachRequest* request)
{
    if (request->handle != NULL) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (request->handle != PEAK_DLOPEN_TEST_RETRY_HANDLE)
#endif
        {
            dlclose(request->handle);
        }
        request->handle = NULL;
    }

    g_free(request->filename);
    request->filename = NULL;
    request->binding_flags = 0;
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
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_OPEN &&
        active_dynamic_attach_count == 0) {
        pthread_cond_broadcast(&dynamic_attach_gate_cond);
    }
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
        pthread_cond_signal(&dynamic_attach_gate_cond);
        request->handle = NULL;
        request->filename = NULL;
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
    PeakDlopenDynamicAttachRequest request = { 0 };

    while (dlopen_interceptor_pop_dynamic_attach_request(&request)) {
        dlopen_interceptor_release_dynamic_attach_request(&request);
    }
}

static gboolean
dlopen_interceptor_enqueue_dynamic_attach_request(
    const char* filename,
    int binding_flags)
{
#ifdef RTLD_NOLOAD
    gboolean can_accept;
    void* retained_handle;
    char* filename_copy;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    can_accept = dlopen_interceptor_queue_can_accept_unlocked();
    if (!can_accept) {
        if (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
            dynamic_attach_queue_length >= PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY) {
            dynamic_attach_drop_full_count++;
        } else {
            dynamic_attach_drop_closed_count++;
        }
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (!can_accept) {
        return FALSE;
    }

    retained_handle = dlopen_interceptor_open_unobserved(
        filename,
        binding_flags | RTLD_NOLOAD);
    if (retained_handle == NULL) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_drop_noload_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        return FALSE;
    }

    filename_copy = g_strdup(filename);

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (!dlopen_interceptor_queue_can_accept_unlocked()) {
        if (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
            dynamic_attach_queue_length >= PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY) {
            dynamic_attach_drop_full_count++;
        } else {
            dynamic_attach_drop_closed_count++;
        }
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        dlclose(retained_handle);
        g_free(filename_copy);
        return FALSE;
    }

    dynamic_attach_queue[dynamic_attach_queue_tail].handle = retained_handle;
    dynamic_attach_queue[dynamic_attach_queue_tail].filename = filename_copy;
    dynamic_attach_queue[dynamic_attach_queue_tail].binding_flags =
        binding_flags;
    dynamic_attach_queue_tail =
        (dynamic_attach_queue_tail + 1) % PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY;
    dynamic_attach_queue_length++;
    dynamic_attach_enqueue_count++;
    if (dynamic_attach_queue_length > dynamic_attach_queue_max_depth) {
        dynamic_attach_queue_max_depth = dynamic_attach_queue_length;
    }
    pthread_cond_signal(&dynamic_attach_gate_cond);
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    peak_general_listener_controller_wake();

    return TRUE;
#else
    (void)filename;
    (void)binding_flags;
    return FALSE;
#endif
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
    active_dlopen_callback_count = 0;
    dynamic_attach_drain_active = FALSE;
    dynamic_attach_queue_head = 0;
    dynamic_attach_queue_tail = 0;
    dynamic_attach_queue_length = 0;
    dynamic_attach_queue_overflow_reported = FALSE;
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
    char* filename_copy = g_strdup(filename != NULL ? filename : "<test>");
    gboolean accepted = FALSE;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dlopen_interceptor_queue_can_accept_unlocked()) {
        dynamic_attach_queue[dynamic_attach_queue_tail].handle = handle;
        dynamic_attach_queue[dynamic_attach_queue_tail].filename = filename_copy;
        dynamic_attach_queue[dynamic_attach_queue_tail].binding_flags =
            RTLD_LAZY;
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

static gboolean
dlopen_interceptor_is_fftw_symbol(const char* name)
{
    return name != NULL &&
           (strncmp(name, "fftw", 4) == 0 ||
            strncmp(name, "dfftw", 5) == 0 ||
            strncmp(name, "rfftw", 5) == 0);
}

#if defined(__linux__)
static gboolean
dlopen_interceptor_collect_function_export(const GumExportDetails* details,
                                           gpointer user_data)
{
    GHashTable* exports = user_data;

    if (details->type == GUM_EXPORT_FUNCTION &&
        dlopen_interceptor_is_fftw_symbol(details->name) &&
        details->address != 0 &&
        g_hash_table_lookup(exports, details->name) == NULL) {
        g_hash_table_insert(exports,
                            g_strdup(details->name),
                            GSIZE_TO_POINTER((gsize)details->address));
    }
    return TRUE;
}

static gboolean
dlopen_interceptor_string_equal(gconstpointer left, gconstpointer right)
{
    return strcmp(left, right) == 0;
}

typedef struct {
    GPtrArray* modules;
    GHashTable* visited_bases;
} PeakDlopenModuleScan;

static void
dlopen_interceptor_add_module_dependencies(PeakDlopenModuleScan* scan,
                                           GumModule* module);

static gboolean
dlopen_interceptor_collect_dependency(const GumDependencyDetails* details,
                                      gpointer user_data)
{
    PeakDlopenModuleScan* scan = user_data;
    GumModule* dependency = gum_process_find_module_by_name(details->name);

    if (dependency != NULL) {
        dlopen_interceptor_add_module_dependencies(scan, dependency);
        g_object_unref(dependency);
    }
    return TRUE;
}

static void
dlopen_interceptor_add_module_dependencies(PeakDlopenModuleScan* scan,
                                           GumModule* module)
{
    const GumMemoryRange* range = gum_module_get_range(module);

    if (range == NULL || range->base_address == 0) {
        return;
    }

    gpointer base_key = GSIZE_TO_POINTER((gsize)range->base_address);
    if (g_hash_table_contains(scan->visited_bases, base_key)) {
        return;
    }

    g_hash_table_add(scan->visited_bases, base_key);
    g_ptr_array_add(scan->modules, g_object_ref(module));
    gum_module_enumerate_dependencies(module,
                                      dlopen_interceptor_collect_dependency,
                                      scan);
}
#endif

static PeakDlopenAttachResult
dlopen_interceptor_attach_from_request(PeakDlopenDynamicAttachRequest* request,
                                       GHashTable* resolved_exports,
                                       gboolean stop_on_retry)
{
    gboolean retained_handle_for_hooks = FALSE;
    gboolean retry_later = FALSE;

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

    gum_interceptor_ignore_current_thread(interceptor);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] != NULL || array_listener[i] != NULL) {
            continue;
        }

        gpointer dynamic_hook_address = resolved_exports != NULL
            ? g_hash_table_lookup(resolved_exports, peak_hook_strings[i])
            : (gpointer)(fn_void)dlsym(request->handle, peak_hook_strings[i]);
        if (dynamic_hook_address == NULL) {
            continue;
        }

        if (!peak_general_listener_attach_target_is_supported(
                peak_hook_strings[i],
                dynamic_hook_address)) {
            g_printerr("[peak] skipping dynamic Gum attach for hook %lu (%s): unsafe Gum prologue\n",
                       (unsigned long)i,
                       peak_hook_strings[i] != NULL ? peak_hook_strings[i] : "<unknown>");
            continue;
        }
        char* demangled = cxa_demangle(peak_hook_strings[i]);
        char* demangled_copy =
            g_strdup(demangled != NULL ? demangled : peak_hook_strings[i]);
        free(demangled);

        GumInvocationListener* new_listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
        PEAKGENERAL_LISTENER(new_listener)->hook_id = i;

        PeakDetachRequest mutation_request = {
            .hook_id = i,
            .symbol_name = peak_hook_strings[i],
            .function_address = dynamic_hook_address,
            .interceptor = interceptor,
            .listener = new_listener,
            .operation = PEAK_DETACH_OPERATION_ATTACH
        };
        PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                          &detach_status)) {
            if (dlopen_interceptor_dynamic_attach_prepare_is_retryable(
                    detach_status)) {
                retry_later = TRUE;
            }
            peak_log_debug("[peak] %s dynamic Gum attach for hook %lu (%s): %s\n",
                       retry_later ? "retrying" : "skipping",
                       (unsigned long)i,
                       peak_hook_strings[i] != NULL ? peak_hook_strings[i] : "<unknown>",
                       peak_detach_controller_status_string(detach_status));
            peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
            g_object_unref(new_listener);
            g_free(demangled_copy);
            if (retry_later && stop_on_retry) {
                break;
            }
            continue;
        }

        gum_interceptor_begin_transaction(interceptor);
        GumAttachReturn attach_status =
            gum_interceptor_attach(interceptor,
                                   dynamic_hook_address,
                                   new_listener,
                                   NULL);
        gum_interceptor_end_transaction(interceptor);
        if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                         &detach_status)) {
            peak_detach_controller_abort_after_failed_finish("dynamic attach finish",
                                                            detach_status);
        }
        if (attach_status == GUM_ATTACH_OK) {
            hook_address[i] = dynamic_hook_address;
            peak_demangled_strings[i] = demangled_copy;
            array_listener[i] = new_listener;
            peak_general_listener_controller_mark_attached_unlocked(i);
            retained_handle_for_hooks = TRUE;
        } else {
            peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
            g_object_unref(new_listener);
            g_free(demangled_copy);
        }
    }
    gum_interceptor_unignore_current_thread(interceptor);
    peak_general_listener_controller_unlock();

    if (retained_handle_for_hooks && retry_later) {
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_partial_success_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        void* hook_lifetime_handle =
            dlopen_interceptor_duplicate_dynamic_handle_reference(
                request->filename,
                request->binding_flags);
        if (hook_lifetime_handle != NULL) {
            dlopen_interceptor_retain_dynamic_handle(hook_lifetime_handle);
        } else {
            g_printerr("[peak] dynamic attach partially succeeded but could not duplicate handle for retry; keeping attached hook lifetime and dropping retry for unresolved hooks\n");
            pthread_mutex_lock(&dynamic_attach_gate_mutex);
            dynamic_attach_drop_requeue_count++;
            pthread_mutex_unlock(&dynamic_attach_gate_mutex);
            dlopen_interceptor_retain_dynamic_handle(request->handle);
            request->handle = NULL;
            retry_later = FALSE;
        }
    } else if (retained_handle_for_hooks) {
        dlopen_interceptor_retain_dynamic_handle(request->handle);
        request->handle = NULL;
    }
    if (retry_later) {
        return PEAK_DLOPEN_ATTACH_RETRY;
    }
    return PEAK_DLOPEN_ATTACH_DONE;
}

static gboolean
dlopen_interceptor_dynamic_attach_is_open(void)
{
    gboolean is_open;

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    is_open = dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    return is_open;
}

static void
dlopen_interceptor_attach_fftw_before_return(void* handle, int binding_flags)
{
#if defined(__linux__) && defined(RTLD_NOLOAD)
    struct link_map* map = NULL;
    GumModule* module = NULL;
    GPtrArray* modules = NULL;
    GHashTable* visited_bases = NULL;
    GHashTable* exports = NULL;
    PeakDlopenDynamicAttachRequest request = { 0 };
    gboolean request_requeued = FALSE;

    if (!dlopen_sync_fftw_enabled || handle == NULL ||
        !dlopen_interceptor_dynamic_attach_is_open()) {
        return;
    }

    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 ||
        map == NULL || map->l_ld == NULL) {
        goto done;
    }

    module = gum_process_find_module_by_address(
        (GumAddress)(guintptr)map->l_ld);
    if (module == NULL) {
        goto done;
    }

    const char* module_path = gum_module_get_path(module);
    if (module_path == NULL || module_path[0] == '\0') {
        goto done;
    }

    request.handle = dlopen_interceptor_open_unobserved(
        module_path,
        binding_flags | RTLD_NOLOAD);
    if (request.handle == NULL) {
        goto done;
    }
    request.filename = g_strdup(module_path);
    request.binding_flags = binding_flags;

    exports = g_hash_table_new_full(g_str_hash,
                                    dlopen_interceptor_string_equal,
                                    g_free,
                                    NULL);
    modules = g_ptr_array_new_with_free_func(g_object_unref);
    visited_bases = g_hash_table_new(g_direct_hash, g_direct_equal);
    PeakDlopenModuleScan scan = {
        .modules = modules,
        .visited_bases = visited_bases
    };
    dlopen_interceptor_add_module_dependencies(&scan, module);
    for (guint i = 0; i < modules->len; i++) {
        gum_module_enumerate_exports(g_ptr_array_index(modules, i),
                                     dlopen_interceptor_collect_function_export,
                                     exports);
    }

    PeakDlopenAttachResult result =
        dlopen_interceptor_attach_from_request(&request, exports, TRUE);
    if (result == PEAK_DLOPEN_ATTACH_RETRY) {
        request_requeued =
            dlopen_interceptor_requeue_dynamic_attach_request(&request);
    }
    if (!request_requeued) {
        dlopen_interceptor_release_dynamic_attach_request(&request);
    }

done:
    if (exports != NULL) {
        g_hash_table_destroy(exports);
    }
    if (visited_bases != NULL) {
        g_hash_table_destroy(visited_bases);
    }
    if (modules != NULL) {
        g_ptr_array_free(modules, TRUE);
    }
    if (module != NULL) {
        g_object_unref(module);
    }
#else
    (void)handle;
    (void)binding_flags;
#endif
}

static void
dlopen_interceptor_drain_dynamic_attach_queue_with_budget(size_t max_requests)
{
    size_t drained = 0;
    size_t initial_queue_length = 0;
    size_t drain_limit;
    PeakDlopenDynamicAttachRequest request = { 0 };

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
           dlopen_interceptor_pop_dynamic_attach_request(&request)) {
        PeakDlopenAttachResult result =
            dlopen_interceptor_attach_from_request(&request, NULL, FALSE);
        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        dynamic_attach_drain_count++;
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);
        if (result == PEAK_DLOPEN_ATTACH_RETRY &&
            dlopen_interceptor_requeue_dynamic_attach_request(&request)) {
            drained++;
            continue;
        }
        dlopen_interceptor_release_dynamic_attach_request(&request);
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

void
dlopen_interceptor_enable_dynamic_attach(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    if (dynamic_attach_state != PEAK_DLOPEN_CONTROLLER_SHUTTING_DOWN &&
        dlopen_interceptor != NULL &&
        dlopen_hook_address != NULL &&
        dlopen_listener != NULL) {
        dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_OPEN;
    }
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
}

gboolean
dlopen_interceptor_shutdown_dynamic_attach(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
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

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    retained_handles = dynamic_attach_retained_handles;
    dynamic_attach_retained_handles = NULL;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    if (retained_handles != NULL) {
        g_ptr_array_free(retained_handles, TRUE);
    }
    dlopen_interceptor_trace_counters("release-retained-handles");
}

typedef struct {
    char* filename;
    int binding_flags;
} PeakDlopenInvocationData;

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
    dlopen_interceptor_begin_callback();
    invocation->filename = g_strdup(filename);
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

    (void)user_data;
    if (handle != NULL && invocation->filename != NULL) {
        gboolean enqueued;

        dlopen_interceptor_attach_fftw_before_return(
            handle,
            invocation->binding_flags);
        enqueued = dlopen_interceptor_enqueue_dynamic_attach_request(
            invocation->filename,
            invocation->binding_flags);
        if (!enqueued) {
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

    g_free(invocation->filename);
    invocation->filename = NULL;
    dlopen_interceptor_end_callback();
}

int dlopen_interceptor_attach()
{
    GumAttachReturn attach_status = GUM_ATTACH_WRONG_SIGNATURE;
    dlopen_interceptor = gum_interceptor_obtain();
    dlopen_hook_address = peak_general_listener_find_function("dlopen");
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (dlopen_interceptor_is_fftw_symbol(peak_hook_strings[i])) {
            dlopen_sync_fftw_enabled = TRUE;
            break;
        }
    }

    if (dlopen_hook_address == NULL) {
        return attach_status;
    }
    if (!peak_general_listener_attach_target_is_supported("dlopen",
                                                          dlopen_hook_address)) {
        g_printerr("[peak] skipping dlopen Gum listener: unsupported target prologue\n");
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
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

    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        g_printerr("[peak] skipping dlopen Gum listener: %s\n",
                   peak_detach_controller_status_string(detach_status));
        g_object_unref(dlopen_listener);
        g_object_unref(dlopen_interceptor);
        dlopen_listener = NULL;
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        return attach_status;
    }

    gum_interceptor_begin_transaction(dlopen_interceptor);
    attach_status = gum_interceptor_attach(dlopen_interceptor,
                                           dlopen_hook_address,
                                           dlopen_listener,
                                           NULL);
    gum_interceptor_end_transaction(dlopen_interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("dlopen attach finish",
                                                        detach_status);
    }
    if (attach_status != GUM_ATTACH_OK) {
        g_object_unref(dlopen_listener);
        g_object_unref(dlopen_interceptor);
        dlopen_listener = NULL;
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
    }
    return attach_status;
}

gboolean dlopen_interceptor_dettach()
{
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

        if (!dlopen_interceptor_prepare_hook_mutation_with_retry(
                &mutation_request,
                &detach_status)) {
            g_printerr("[peak] skipping dlopen Gum listener detach: %s\n",
                       peak_detach_controller_status_string(detach_status));
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
    dlopen_sync_fftw_enabled = FALSE;
    return TRUE;
}
