#define _GNU_SOURCE
#include <dlfcn.h>

#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "peak_detach_controller.h"
#include "peak_logging.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef void (*fn_void)(void);

#define PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY 256U
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

typedef struct {
    void* handle;
    char* filename;
    int flags;
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
static unsigned int active_dlopen_replacement_count = 0;
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
dlopen_interceptor_begin_replacement_call(void)
{
    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    active_dlopen_replacement_count++;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
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

#ifdef RTLD_NOLOAD
static int
dlopen_interceptor_noload_flags(int flags)
{
    int binding_flags = flags & RTLD_BINDING_MASK;

    if (binding_flags == 0) {
        binding_flags = RTLD_LAZY;
    }

    return RTLD_NOLOAD | binding_flags;
}
#endif

static void*
dlopen_interceptor_duplicate_dynamic_handle_reference(const char* filename,
                                                      int flags)
{
#ifdef RTLD_NOLOAD
    if (filename == NULL || original_dlopen == NULL) {
        return NULL;
    }
    return original_dlopen(filename, dlopen_interceptor_noload_flags(flags));
#else
    (void)filename;
    (void)flags;
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
    dynamic_attach_queue[dynamic_attach_queue_head].flags = 0;
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
        request->flags = 0;
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
    int flags)
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

    retained_handle = original_dlopen(filename,
                                      dlopen_interceptor_noload_flags(flags));
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
    dynamic_attach_queue[dynamic_attach_queue_tail].flags = flags;
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
    (void)flags;
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
    active_dlopen_replacement_count = 0;
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
        dynamic_attach_queue[dynamic_attach_queue_tail].flags = RTLD_LAZY;
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

static PeakDlopenAttachResult
dlopen_interceptor_attach_from_request(PeakDlopenDynamicAttachRequest* request)
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

        fn_void dynamic_link_func = (fn_void)dlsym(request->handle, peak_hook_strings[i]);
        if (dynamic_link_func == NULL) {
            continue;
        }

        gpointer dynamic_hook_address = (gpointer)dynamic_link_func;
        if (!peak_general_listener_attach_target_is_supported(
                peak_hook_strings[i],
                dynamic_hook_address)) {
            peak_log_debug("[peak] skipping dynamic Gum attach for hook %lu (%s): unsupported target prologue\n",
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
            dlopen_interceptor_duplicate_dynamic_handle_reference(request->filename,
                                                                  request->flags);
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
            dlopen_interceptor_attach_from_request(&request);
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
        original_dlopen != NULL) {
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
    if (!dlopen_interceptor_wait_for_replacement_idle()) {
        g_printerr("[peak] dlopen replacement body drain timed out with %u active replacement calls; leaving dlopen interceptor state alive\n",
                   dlopen_interceptor_active_replacement_count());
        dlopen_interceptor_trace_counters("shutdown-replacement-timeout");
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

static void*
peak_dlopen(const char *filename, int flags) {
    void *handle;

    dlopen_interceptor_begin_replacement_call();
    handle = original_dlopen(filename, flags);
    // If dlopen failed or no filename, don’t do rescan
    if (handle == NULL || filename == NULL) {
        dlopen_interceptor_end_replacement_call();
        return handle;
    }

    if (!dlopen_interceptor_enqueue_dynamic_attach_request(filename, flags)) {
        gboolean should_report_overflow = FALSE;

        pthread_mutex_lock(&dynamic_attach_gate_mutex);
        if (dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN &&
            dynamic_attach_queue_length >= PEAK_DLOPEN_DYNAMIC_ATTACH_QUEUE_CAPACITY &&
            !dynamic_attach_queue_overflow_reported) {
            dynamic_attach_queue_overflow_reported = TRUE;
            should_report_overflow = TRUE;
        }
        pthread_mutex_unlock(&dynamic_attach_gate_mutex);

        if (should_report_overflow) {
            g_printerr("[peak] dlopen dynamic attach queue full; dropping later dynamic attach requests\n");
        }
    }

    dlopen_interceptor_end_replacement_call();
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
        if (peak_general_listener_should_log_attach_diagnostic()) {
            g_printerr("[peak] skipping dlopen Gum replace: unsupported target prologue\n");
        }
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
        dlopen_hook_address = NULL;
        return replace_check;
    }

    gboolean startup_replace_can_skip_stop =
        peak_general_listener_startup_attach_can_skip_stop();
    PeakDetachRequest mutation_request = {
        .hook_id = 0,
        .symbol_name = "dlopen",
        .function_address = dlopen_hook_address,
        .interceptor = dlopen_interceptor,
        .listener = NULL,
        .operation = PEAK_DETACH_OPERATION_REPLACE,
        .avoid_external_helper = TRUE
    };
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;

    /*
     * This replacement is installed during PEAK startup.  If the process is
     * still single-threaded, direct Gum replacement matches the historical
     * startup path and avoids signal-stopping a rank while libc/loader state
     * is still being initialized.  Once another thread exists, keep strict
     * controller ownership of the mutation.
     */
    if (!startup_replace_can_skip_stop &&
        !peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        if (peak_general_listener_should_log_attach_diagnostic()) {
            g_printerr("[peak] skipping dlopen Gum replace: %s\n",
                       peak_detach_controller_status_string(detach_status));
        }
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
    if (!startup_replace_can_skip_stop &&
        !peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("dlopen replace finish",
                                                        detach_status);
    }
    return replace_check;
}

gboolean dlopen_interceptor_dettach()
{
    gboolean entry_physically_restored = FALSE;

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

    if (!dlopen_interceptor_shutdown_dynamic_attach()) {
        return FALSE;
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
    dynamic_attach_queue_head = 0;
    dynamic_attach_queue_tail = 0;
    dynamic_attach_queue_length = 0;
    dynamic_attach_queue_overflow_reported = FALSE;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);

    dlopen_interceptor = NULL;
    dlopen_hook_address = NULL;
    return TRUE;
}
