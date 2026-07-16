#define _GNU_SOURCE
#include <dlfcn.h>

#include "general_listener.h"
#include "internal/general_listener_internal.h"
#include "internal/dlopen_interceptor_internal.h"
#include "dlopen_interceptor.h"
#include "detach_controller.h"
#include "logging.h"

#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
    char* direct_symbol_name;
    char* direct_provider_name;
    GumAddress direct_symbol_address;
    void* direct_provider_pin;
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
static PeakDlopenAttachResult
dlopen_interceptor_ensure_symbol_lookup_listener(void);
static gboolean
dlopen_interceptor_string_equal(gconstpointer left, gconstpointer right);
static void dlopen_interceptor_dlsym_listener_iface_init(gpointer g_iface,
                                                         gpointer iface_data);
#ifdef PEAK_DLOPEN_ASM_ENTRY_STUB
__attribute__((visibility("hidden")))
void* peak_dlopen(const char* filename, int flags);
#endif

static pthread_mutex_t dynamic_attach_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dynamic_attach_gate_cond = PTHREAD_COND_INITIALIZER;
static PeakDlopenControllerState dynamic_attach_state = PEAK_DLOPEN_CONTROLLER_CLOSED;
static unsigned int active_dynamic_attach_count = 0;
_Atomic unsigned int peak_dlopen_active_replacement_count
    __attribute__((visibility("hidden"))) = 0;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "dlopen replacement entry accounting must be lock-free");
#ifdef PEAK_DLOPEN_ASM_ENTRY_STUB
_Static_assert(sizeof(unsigned int) == 4 &&
                   sizeof(_Atomic unsigned int) == 4,
               "assembly dlopen entry accounting requires a 32-bit counter");
_Static_assert(_Alignof(_Atomic unsigned int) >= sizeof(unsigned int),
               "assembly dlopen entry accounting requires an aligned word");
#endif
static gboolean dynamic_attach_drain_active = FALSE;
static PeakDlopenDynamicAttachRequest* dynamic_attach_queue_head = NULL;
static PeakDlopenDynamicAttachRequest* dynamic_attach_queue_tail = NULL;
static size_t dynamic_attach_queue_length = 0;
static GPtrArray* dynamic_attach_retained_handles = NULL;
static GHashTable* dynamic_attach_retained_provider_paths = NULL;
static __thread gboolean dynamic_attach_drain_reentrant = FALSE;
static GumInvocationListener* dynamic_symbol_lookup_listener = NULL;
static gpointer dlsym_hook_address = NULL;
static gpointer dlvsym_hook_address = NULL;
static gboolean dynamic_symbol_lookup_listener_attached = FALSE;
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
static atomic_bool dynamic_symbol_lookup_listener_required =
    ATOMIC_VAR_INIT(false);

#define PEAK_DLSYM_TYPE_LISTENER \
    (dlopen_interceptor_dlsym_listener_get_type())
G_DECLARE_FINAL_TYPE(PeakDlsymListener,
                     dlopen_interceptor_dlsym_listener,
                     PEAK_DLSYM,
                     LISTENER,
                     GObject)
struct _PeakDlsymListener {
    GObject parent_instance;
};
G_DEFINE_TYPE_EXTENDED(
    PeakDlsymListener,
    dlopen_interceptor_dlsym_listener,
    G_TYPE_OBJECT,
    0,
    G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                          dlopen_interceptor_dlsym_listener_iface_init))

typedef struct {
    const char* symbol_name;
    pid_t entry_pid;
    gboolean transaction_started;
} PeakDlsymInvocationData;
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

static gboolean
dlopen_interceptor_is_inherited_fork_child(void)
{
    pid_t owner_pid = atomic_load_explicit(&dynamic_attach_owner_pid,
                                           memory_order_acquire);

    return owner_pid != 0 && owner_pid != getpid();
}

static void
dlopen_interceptor_warn_fork_child_once(void)
{
    static const char warning[] =
        "[peak] fork child dynamic dlopen profiling is unsupported until exec; using the real loader result without inherited PEAK controller state\n";
    pid_t current_pid = getpid();

    if (atomic_exchange_explicit(&dynamic_attach_fork_warning_pid,
                                 current_pid,
                                 memory_order_relaxed) != current_pid) {
        ssize_t bytes_written =
            write(STDERR_FILENO, warning, sizeof(warning) - 1);
        (void)bytes_written;
    }
}

static void
dlopen_interceptor_end_replacement_call_without_inherited_locks(void)
{
    /*
     * Every other parent thread vanished at fork. Its copied entry count is
     * therefore stale, not merely one too large. Reset the lock-free child
     * copy without signaling any inherited condition variable.
     */
    atomic_store_explicit(&peak_dlopen_active_replacement_count,
                          0,
                          memory_order_release);
}

static void
dlopen_interceptor_quarantine_inherited_fork_child(
    gboolean end_replacement_call)
{
    /*
     * The fork caller may have owned this process-private transaction mutex.
     * Its copied pthread owner identity is not valid in the child, so never
     * unlock or signal any inherited synchronization primitive. All future
     * child loader lookups bypass dynamic attach until exec; dropping only the
     * caller's TLS depth is therefore sufficient to abandon the copied state.
     */
    dynamic_load_transaction_depth = 0;
    dlopen_interceptor_warn_fork_child_once();
    if (end_replacement_call) {
        dlopen_interceptor_end_replacement_call_without_inherited_locks();
    }
}

static void
dlopen_interceptor_end_replacement_call(void)
{
    unsigned int previous = atomic_fetch_sub_explicit(
        &peak_dlopen_active_replacement_count,
        1,
        memory_order_acq_rel);

    if (previous == 0) {
        /* Keep an unmatched release fail-closed instead of wrapping to UINT_MAX. */
        atomic_fetch_add_explicit(&peak_dlopen_active_replacement_count,
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
    return atomic_load_explicit(&peak_dlopen_active_replacement_count,
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
    while (atomic_load_explicit(&peak_dlopen_active_replacement_count,
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
        request->direct_provider_pin);
    g_free(request->direct_symbol_name);
    g_free(request->direct_provider_name);
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
    GPtrArray* retained_handles;
    GHashTable* retained_provider_paths;

    dlopen_interceptor_discard_dynamic_attach_queue();

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    dynamic_attach_state =
        open ? PEAK_DLOPEN_CONTROLLER_OPEN : PEAK_DLOPEN_CONTROLLER_CLOSED;
    active_dynamic_attach_count = 0;
    atomic_store_explicit(&peak_dlopen_active_replacement_count,
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
    gboolean needs_symbol_lookup_listener;
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
static gboolean
dlopen_interceptor_mapped_range_is_valid(size_t file_size,
                                         size_t offset,
                                         size_t length)
{
    return offset <= file_size && length <= file_size - offset;
}

static int
dlopen_interceptor_elf64_symbol_type(const unsigned char* contents,
                                    size_t file_size,
                                    const char* symbol_name)
{
    const Elf64_Ehdr* header;
    const Elf64_Shdr* sections;

    if (!dlopen_interceptor_mapped_range_is_valid(
            file_size,
            0,
            sizeof(Elf64_Ehdr))) {
        return -1;
    }
    header = (const Elf64_Ehdr*)contents;
    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_shentsize != sizeof(Elf64_Shdr) ||
        header->e_shnum == 0 ||
        !dlopen_interceptor_mapped_range_is_valid(
            file_size,
            (size_t)header->e_shoff,
            (size_t)header->e_shnum * sizeof(Elf64_Shdr))) {
        return -1;
    }
    sections = (const Elf64_Shdr*)(contents + header->e_shoff);

    for (size_t pass = 0; pass < 2; pass++) {
        Elf64_Word desired_type = pass == 0 ? SHT_DYNSYM : SHT_SYMTAB;

        for (size_t section_index = 0;
             section_index < header->e_shnum;
             section_index++) {
            const Elf64_Shdr* symbol_section = &sections[section_index];
            const Elf64_Shdr* string_section;
            const Elf64_Sym* symbols;
            const char* strings;
            size_t symbol_count;

            if (symbol_section->sh_type != desired_type ||
                symbol_section->sh_entsize != sizeof(Elf64_Sym) ||
                symbol_section->sh_link >= header->e_shnum ||
                !dlopen_interceptor_mapped_range_is_valid(
                    file_size,
                    (size_t)symbol_section->sh_offset,
                    (size_t)symbol_section->sh_size)) {
                continue;
            }
            string_section = &sections[symbol_section->sh_link];
            if (!dlopen_interceptor_mapped_range_is_valid(
                    file_size,
                    (size_t)string_section->sh_offset,
                    (size_t)string_section->sh_size)) {
                continue;
            }
            symbols = (const Elf64_Sym*)(
                contents + symbol_section->sh_offset);
            strings = (const char*)(contents + string_section->sh_offset);
            symbol_count = symbol_section->sh_size / sizeof(Elf64_Sym);

            for (size_t symbol_index = 0;
                 symbol_index < symbol_count;
                 symbol_index++) {
                const Elf64_Sym* symbol = &symbols[symbol_index];
                unsigned char binding = ELF64_ST_BIND(symbol->st_info);
                size_t name_offset = symbol->st_name;

                if ((binding != STB_GLOBAL && binding != STB_WEAK) ||
                    symbol->st_shndx == SHN_UNDEF ||
                    name_offset >= string_section->sh_size ||
                    memchr(strings + name_offset,
                           '\0',
                           string_section->sh_size - name_offset) == NULL ||
                    strcmp(strings + name_offset, symbol_name) != 0) {
                    continue;
                }
                return (int)ELF64_ST_TYPE(symbol->st_info);
            }
        }
    }
    return -1;
}

static int
dlopen_interceptor_elf_symbol_type_from_file(const char* provider_path,
                                             const char* symbol_name)
{
    struct stat file_stat;
    unsigned char* contents;
    size_t file_size;
    int fd;
    int symbol_type = -1;

    if (provider_path == NULL || provider_path[0] == '\0' ||
        symbol_name == NULL || symbol_name[0] == '\0') {
        return -1;
    }
    fd = open(provider_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &file_stat) != 0 || file_stat.st_size <= 0 ||
        (uintmax_t)file_stat.st_size > SIZE_MAX) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    file_size = (size_t)file_stat.st_size;
    contents = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (contents == MAP_FAILED) {
        return -1;
    }
    symbol_type = dlopen_interceptor_elf64_symbol_type(contents,
                                                       file_size,
                                                       symbol_name);
    munmap(contents, file_size);
    return symbol_type;
}

typedef struct {
    const char* symbol_name;
    const char* provider_path;
    gpointer non_ifunc_address;
    gboolean ifunc_found;
} PeakLoadedUnknownCallableLookup;

static gboolean
dlopen_interceptor_find_unknown_callable_symbol(
    const GumSymbolDetails* details,
    gpointer user_data)
{
    PeakLoadedUnknownCallableLookup* lookup = user_data;
    int elf_symbol_type;

    if (!details->is_global ||
        details->type != GUM_SYMBOL_UNKNOWN ||
        details->section == NULL ||
        (details->section->protection & GUM_PAGE_EXECUTE) == 0 ||
        details->address == 0 ||
        strcmp(details->name, lookup->symbol_name) != 0) {
        return TRUE;
    }

    elf_symbol_type = dlopen_interceptor_elf_symbol_type_from_file(
        lookup->provider_path,
        details->name);
#ifdef STT_GNU_IFUNC
    if (elf_symbol_type == STT_GNU_IFUNC) {
        lookup->ifunc_found = TRUE;
        return FALSE;
    }
#endif
    if (elf_symbol_type == STT_NOTYPE) {
        lookup->non_ifunc_address = GSIZE_TO_POINTER(details->address);
        return FALSE;
    }
    return TRUE;
}

static gboolean
dlopen_interceptor_find_unknown_callable_module(GumModule* module,
                                                gpointer user_data)
{
    PeakLoadedUnknownCallableLookup* lookup = user_data;

    lookup->provider_path = gum_module_get_path(module);
    gum_module_enumerate_symbols(
        module,
        dlopen_interceptor_find_unknown_callable_symbol,
        lookup);
    lookup->provider_path = NULL;
    return lookup->non_ifunc_address == NULL && !lookup->ifunc_found;
}

gpointer
dlopen_interceptor_find_loaded_unknown_callable(const char* symbol_name,
                                                gboolean* ifunc_found_out)
{
    PeakLoadedUnknownCallableLookup lookup = {
        .symbol_name = symbol_name
    };

    if (ifunc_found_out != NULL) {
        *ifunc_found_out = FALSE;
    }
    if (symbol_name == NULL || symbol_name[0] == '\0') {
        return NULL;
    }

    gum_process_enumerate_modules(
        dlopen_interceptor_find_unknown_callable_module,
        &lookup);
    if (lookup.ifunc_found) {
        atomic_store_explicit(&dynamic_symbol_lookup_listener_required,
                              true,
                              memory_order_release);
    }
    if (ifunc_found_out != NULL) {
        *ifunc_found_out = lookup.ifunc_found;
    }
    return lookup.non_ifunc_address;
}

/*
 * Gum's ELF export enumeration intentionally emits only STT_FUNC and
 * STT_OBJECT. Callable STT_NOTYPE symbols can be attached at the executable
 * address reported by the ELF symbol table. STT_GNU_IFUNC is different: that
 * address is the resolver, and calling dlsym() here would execute application
 * code before the application's own lookup. Defer IFUNC attachment to the
 * dlsym/dlvsym invocation listener, which observes the exact address selected
 * by the application's lookup and completes its attach before returning it.
 */
static gboolean
dlopen_interceptor_scan_callable_unknown_symbol(
    const GumSymbolDetails* details,
    gpointer user_data)
{
    PeakDlopenExportScan* scan = user_data;
    int elf_symbol_type;

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

    scan->matched = TRUE;
    elf_symbol_type = dlopen_interceptor_elf_symbol_type_from_file(
        scan->provider_name,
        details->name);
#ifdef STT_GNU_IFUNC
    if (elf_symbol_type == STT_GNU_IFUNC) {
        scan->needs_symbol_lookup_listener = TRUE;
        atomic_store_explicit(&dynamic_symbol_lookup_listener_required,
                              true,
                              memory_order_release);
        g_printerr("[peak] deferring IFUNC target %s from %s until the application's actual dlsym/dlvsym lookup; pointers resolved before PEAK initialization cannot be recovered without re-executing the resolver\n",
                   details->name,
                   scan->provider_name != NULL
                       ? scan->provider_name
                       : "<unknown>");
        return TRUE;
    }
#endif
    if (elf_symbol_type != STT_NOTYPE) {
        peak_log_debug("[peak] deferring unknown callable symbol %s from %s until an application symbol lookup returns its callable address\n",
                       details->name,
                       scan->provider_name != NULL
                           ? scan->provider_name
                           : "<unknown>");
        return TRUE;
    }

    return dlopen_interceptor_attach_scanned_symbol(
        scan,
        details->name,
        details->address,
        details->size > 0 ? (gsize)details->size : 0,
        NULL);
}
#endif

#if !defined(__linux__)
gpointer
dlopen_interceptor_find_loaded_unknown_callable(const char* symbol_name,
                                                gboolean* ifunc_found_out)
{
    (void)symbol_name;
    if (ifunc_found_out != NULL) {
        *ifunc_found_out = FALSE;
    }
    return NULL;
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
    gboolean needs_symbol_lookup_listener = FALSE;
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
    if (request->direct_symbol_address != 0 &&
        request->direct_symbol_name != NULL) {
        PeakDynamicAttachResult direct_result =
            peak_general_listener_dynamic_attach_symbol(
                request->direct_symbol_name,
                GSIZE_TO_POINTER(request->direct_symbol_address),
                0,
                request->direct_provider_name);

        if (direct_result == PEAK_DYNAMIC_ATTACH_ATTACHED) {
            dlopen_interceptor_commit_pinned_provider(
                request->direct_provider_pin);
            request->direct_provider_pin = NULL;
            request->retain_handle = TRUE;
            return PEAK_DLOPEN_ATTACH_DONE;
        }
        if (direct_result == PEAK_DYNAMIC_ATTACH_RETRY) {
            return PEAK_DLOPEN_ATTACH_RETRY;
        }
        if (direct_result == PEAK_DYNAMIC_ATTACH_FAILED) {
            return PEAK_DLOPEN_ATTACH_FAILED;
        }
        return PEAK_DLOPEN_ATTACH_DONE;
    }
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
        needs_symbol_lookup_listener |=
            export_scan.needs_symbol_lookup_listener;
        retry |= export_scan.retry;
        failed |= export_scan.failed;
        g_object_unref(module);
    }
    g_hash_table_destroy(seen_modules);

    if (needs_symbol_lookup_listener) {
        PeakDlopenAttachResult listener_result =
            dlopen_interceptor_ensure_symbol_lookup_listener();

        if (listener_result == PEAK_DLOPEN_ATTACH_RETRY) {
            retry = TRUE;
        } else if (listener_result == PEAK_DLOPEN_ATTACH_FAILED) {
            failed = TRUE;
        }
    }

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
        g_printerr("[peak] dynamic attach failed open for %s because a safe mutation could not be established; returning the real loader result without waiting for a retry\n",
                   request->direct_symbol_name != NULL
                       ? request->direct_symbol_name
                       : (request->filename != NULL
                              ? request->filename
                              : "<unknown>"));
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

static void
dlopen_interceptor_dlsym_listener_on_enter(GumInvocationListener* listener,
                                           GumInvocationContext* ic)
{
    PeakDlsymInvocationData* data =
        GUM_IC_GET_INVOCATION_DATA(ic, PeakDlsymInvocationData);
    const char* symbol_name;
    gboolean admission_open;

    (void)listener;
    memset(data, 0, sizeof(*data));
    if (dlopen_interceptor_is_inherited_fork_child()) {
        dlopen_interceptor_warn_fork_child_once();
        return;
    }
    if (dynamic_attach_drain_reentrant ||
        peak_general_listener_controller_is_current_thread()) {
        return;
    }

    symbol_name = gum_invocation_context_get_nth_argument(ic, 1);
    if (symbol_name == NULL || symbol_name[0] == '\0' ||
        !peak_general_listener_dynamic_symbol_matches_any_target(
            symbol_name,
            NULL)) {
        return;
    }

    pthread_mutex_lock(&dynamic_attach_gate_mutex);
    admission_open = dynamic_attach_state == PEAK_DLOPEN_CONTROLLER_OPEN;
    pthread_mutex_unlock(&dynamic_attach_gate_mutex);
    if (!admission_open) {
        return;
    }

    data->symbol_name = symbol_name;
    data->entry_pid = getpid();
    dlopen_interceptor_begin_load_transaction();
    data->transaction_started = TRUE;
}

static void
dlopen_interceptor_dlsym_listener_on_leave(GumInvocationListener* listener,
                                           GumInvocationContext* ic)
{
    PeakDlsymInvocationData* data =
        GUM_IC_GET_INVOCATION_DATA(ic, PeakDlsymInvocationData);
    gpointer address;
    Dl_info provider_info;
    void* provider_pin = NULL;
    PeakDlopenDynamicAttachRequest* request = NULL;
    PeakDlopenRequestState request_state = PEAK_DLOPEN_REQUEST_NO_MATCH;
    gboolean provider_pinned;
    int saved_errno;

    (void)listener;
    if (!data->transaction_started) {
        return;
    }

    if (data->entry_pid != getpid() ||
        dlopen_interceptor_is_inherited_fork_child()) {
        dlopen_interceptor_quarantine_inherited_fork_child(FALSE);
        return;
    }

    saved_errno = errno;
    address = gum_invocation_context_get_return_value(ic);
    memset(&provider_info, 0, sizeof(provider_info));
    if (address == NULL ||
        dladdr(address, &provider_info) == 0 ||
        !peak_general_listener_dynamic_symbol_address_needs_attach(
            data->symbol_name,
            address,
            provider_info.dli_fname)) {
        dlopen_interceptor_end_load_transaction();
        errno = saved_errno;
        return;
    }

    provider_pinned =
        dlopen_interceptor_pin_loaded_provider(address, &provider_pin);
    if (dlopen_interceptor_is_inherited_fork_child()) {
        /* A loader audit callback may have forked during RTLD_NOLOAD. */
        dlopen_interceptor_quarantine_inherited_fork_child(FALSE);
        errno = saved_errno;
        return;
    }
    if (!provider_pinned) {
        g_printerr("[peak] skipping dynamic attach for %s: unable to retain the provider owning the application's resolved address\n",
                   data->symbol_name);
        /* Discard an internal RTLD_NOLOAD failure after a successful dlsym. */
        (void)dlerror();
        dlopen_interceptor_end_load_transaction();
        errno = saved_errno;
        return;
    }
    request = dlopen_interceptor_new_dynamic_attach_request(TRUE);
    request->filename = g_strdup(provider_info.dli_fname);
    request->direct_symbol_name = g_strdup(data->symbol_name);
    request->direct_provider_name = g_strdup(provider_info.dli_fname);
    request->direct_symbol_address = (GumAddress)(guintptr)address;
    request->direct_provider_pin = provider_pin;
    provider_pin = NULL;

    if (dlopen_interceptor_enqueue_dynamic_attach_request(request)) {
        request_state =
            dlopen_interceptor_wait_for_dynamic_attach_request(request);
    }
    dlopen_interceptor_destroy_dynamic_attach_request(request);
    dlopen_interceptor_end_load_transaction();

    if (request_state == PEAK_DLOPEN_REQUEST_FAILED) {
        g_printerr("[peak] dynamic attach reached a permanent failure before returning %s from the application's symbol lookup\n",
                   data->symbol_name);
    }
    errno = saved_errno;
}

static void
dlopen_interceptor_dlsym_listener_class_init(PeakDlsymListenerClass* klass)
{
    (void)klass;
    (void)PEAK_DLSYM_IS_LISTENER;
    (void)glib_autoptr_cleanup_PeakDlsymListener;
}

static void
dlopen_interceptor_dlsym_listener_init(PeakDlsymListener* self)
{
    (void)self;
}

static void
dlopen_interceptor_dlsym_listener_iface_init(gpointer g_iface,
                                             gpointer iface_data)
{
    GumInvocationListenerInterface* iface = g_iface;

    (void)iface_data;
    iface->on_enter = dlopen_interceptor_dlsym_listener_on_enter;
    iface->on_leave = dlopen_interceptor_dlsym_listener_on_leave;
}

/*
 * The caller owns a prepared Gum mutation window and an open interceptor
 * transaction. Keep dlsym and dlvsym atomic: a partial installation cannot
 * satisfy the first-lookup contract and is reverted before publication.
 */
static gboolean
dlopen_interceptor_attach_symbol_lookup_listener_in_transaction(
    GumAttachReturn* dlsym_status_out,
    GumAttachReturn* dlvsym_status_out)
{
    GumAttachReturn dlsym_status = GUM_ATTACH_WRONG_TYPE;
    GumAttachReturn dlvsym_status = GUM_ATTACH_OK;

    if (dlopen_interceptor == NULL ||
        dynamic_symbol_lookup_listener == NULL ||
        dlsym_hook_address == NULL
#if defined(__linux__)
        || dlvsym_hook_address == NULL
#endif
    ) {
        if (dlsym_status_out != NULL) {
            *dlsym_status_out = dlsym_status;
        }
        if (dlvsym_status_out != NULL) {
            *dlvsym_status_out = GUM_ATTACH_WRONG_TYPE;
        }
        return FALSE;
    }

    dlsym_status = gum_interceptor_attach(
        dlopen_interceptor,
        dlsym_hook_address,
        dynamic_symbol_lookup_listener,
        NULL);
    if (dlsym_status == GUM_ATTACH_OK &&
        dlvsym_hook_address != NULL &&
        dlvsym_hook_address != dlsym_hook_address) {
        dlvsym_status = gum_interceptor_attach(
            dlopen_interceptor,
            dlvsym_hook_address,
            dynamic_symbol_lookup_listener,
            NULL);
    }

    if (dlsym_status == GUM_ATTACH_OK &&
        dlvsym_status != GUM_ATTACH_OK) {
        gum_interceptor_detach(dlopen_interceptor,
                               dynamic_symbol_lookup_listener);
    }
    if (dlsym_status_out != NULL) {
        *dlsym_status_out = dlsym_status;
    }
    if (dlvsym_status_out != NULL) {
        *dlvsym_status_out = dlvsym_status;
    }
    return dlsym_status == GUM_ATTACH_OK &&
           dlvsym_status == GUM_ATTACH_OK;
}

static PeakDlopenAttachResult
dlopen_interceptor_ensure_symbol_lookup_listener(void)
{
    PeakDetachRequest mutation_request;
    PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;
    GumAttachReturn dlsym_status = GUM_ATTACH_WRONG_TYPE;
    GumAttachReturn dlvsym_status = GUM_ATTACH_WRONG_TYPE;
    gboolean attached;

    if (dynamic_symbol_lookup_listener_attached) {
        return PEAK_DLOPEN_ATTACH_DONE;
    }
    if (dlopen_interceptor == NULL ||
        dynamic_symbol_lookup_listener == NULL ||
        dlsym_hook_address == NULL
#if defined(__linux__)
        || dlvsym_hook_address == NULL
#endif
    ) {
        g_printerr("[peak] cannot observe application IFUNC resolution: dlsym/dlvsym interception is unavailable\n");
        return PEAK_DLOPEN_ATTACH_FAILED;
    }

    mutation_request = (PeakDetachRequest) {
        .hook_id = 0,
        .symbol_name = "dlsym/dlvsym",
        .function_address = dlsym_hook_address,
        .interceptor = dlopen_interceptor,
        .listener = dynamic_symbol_lookup_listener,
        .operation = PEAK_DETACH_OPERATION_ATTACH
    };
    if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
                                                       &detach_status)) {
        return dlopen_interceptor_dynamic_attach_prepare_is_retryable(
                   detach_status)
            ? PEAK_DLOPEN_ATTACH_RETRY
            : PEAK_DLOPEN_ATTACH_FAILED;
    }

    gum_interceptor_begin_transaction(dlopen_interceptor);
    attached =
        dlopen_interceptor_attach_symbol_lookup_listener_in_transaction(
            &dlsym_status,
            &dlvsym_status);
    if (attached) {
        dynamic_symbol_lookup_listener_attached = TRUE;
    }
    gum_interceptor_end_transaction(dlopen_interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                      &detach_status)) {
        peak_detach_controller_abort_after_failed_finish(
            "dlsym/dlvsym listener attach finish",
            detach_status);
    }

    if (!attached) {
        g_printerr("[peak] cannot observe application IFUNC resolution: dlsym attach status=%d dlvsym attach status=%d\n",
                   dlsym_status,
                   dlvsym_status);
        return PEAK_DLOPEN_ATTACH_FAILED;
    }
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
         * preserve loader ordering. IFUNC resolver execution is deliberately
         * deferred to the application's actual dlsym/dlvsym call.
         */
        dlopen_interceptor_begin_load_transaction();
        handle = original_dlopen(snapshot->path, RTLD_LAZY | RTLD_NOLOAD);
        if (dlopen_interceptor_is_inherited_fork_child()) {
            dlopen_interceptor_quarantine_inherited_fork_child(FALSE);
            return FALSE;
        }
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

__attribute__((visibility("hidden"), noinline, no_instrument_function))
void*
peak_dlopen_body(const char *filename, int flags)
{
    int old_cancel_state;
    void *handle;
    PeakDlopenDynamicAttachRequest* request = NULL;
    PeakDlopenRequestState request_state = PEAK_DLOPEN_REQUEST_NO_MATCH;

    if (dlopen_interceptor_is_inherited_fork_child()) {
        dlopen_interceptor_warn_fork_child_once();
        handle = original_dlopen(filename, flags);
        dlopen_interceptor_end_replacement_call_without_inherited_locks();
        return handle;
    }
    if (dynamic_attach_drain_reentrant ||
        peak_general_listener_controller_is_current_thread()) {
        handle = original_dlopen(filename, flags);
        if (dlopen_interceptor_is_inherited_fork_child()) {
            dlopen_interceptor_quarantine_inherited_fork_child(TRUE);
        } else {
            dlopen_interceptor_end_replacement_call();
        }
        return handle;
    }

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);

    dlopen_interceptor_begin_load_transaction();
#ifdef PEAK_ENABLE_TEST_HOOKS
    dlopen_interceptor_test_pause_replacement_body(filename);
#endif
    handle = original_dlopen(filename, flags);
    if (dlopen_interceptor_is_inherited_fork_child()) {
        dlopen_interceptor_quarantine_inherited_fork_child(TRUE);
        pthread_setcancelstate(old_cancel_state, NULL);
        return handle;
    }
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
    if (dlopen_interceptor_is_inherited_fork_child()) {
        /* Abandon the child copy; destroying it would touch inherited locks. */
        dlopen_interceptor_quarantine_inherited_fork_child(TRUE);
        pthread_setcancelstate(old_cancel_state, NULL);
        return handle;
    }
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
        if (dlopen_interceptor_is_inherited_fork_child()) {
            dlopen_interceptor_quarantine_inherited_fork_child(TRUE);
            pthread_setcancelstate(old_cancel_state, NULL);
            return handle;
        }
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

#ifndef PEAK_DLOPEN_ASM_ENTRY_STUB
static void* __attribute__((noinline, no_instrument_function))
peak_dlopen(const char* filename, int flags)
{
    /* Unsupported strict-detach platforms retain best-effort C accounting. */
    atomic_fetch_add_explicit(&peak_dlopen_active_replacement_count,
                              1,
                              memory_order_acq_rel);
    return peak_dlopen_body(filename, flags);
}
#endif

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
    dlsym_hook_address = NULL;
    dlvsym_hook_address = NULL;
    dynamic_symbol_lookup_listener_attached = FALSE;
    if (dynamic_symbol_lookup_listener != NULL) {
        g_object_unref(dynamic_symbol_lookup_listener);
        dynamic_symbol_lookup_listener = NULL;
    }
    if (dlopen_interceptor != NULL) {
        g_object_unref(dlopen_interceptor);
        dlopen_interceptor = NULL;
    }
}

int dlopen_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    GumAttachReturn dlsym_attach_check = GUM_ATTACH_WRONG_TYPE;
    GumAttachReturn dlvsym_attach_check = GUM_ATTACH_OK;
    gboolean symbol_lookup_listener_required;
    gboolean setup_ok = FALSE;

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
    dlsym_hook_address = peak_general_listener_find_function("dlsym");
#if defined(__linux__)
    dlvsym_hook_address = peak_general_listener_find_function("dlvsym");
#endif
    symbol_lookup_listener_required = atomic_load_explicit(
        &dynamic_symbol_lookup_listener_required,
        memory_order_acquire);

    if (dlopen_hook_address == NULL ||
        (symbol_lookup_listener_required && dlsym_hook_address == NULL)
#if defined(__linux__)
        || (symbol_lookup_listener_required && dlvsym_hook_address == NULL)
#endif
    ) {
        g_printerr("[peak] skipping dynamic loader interception: required dlopen/dlsym/dlvsym entry discovery was incomplete\n");
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
    dynamic_symbol_lookup_listener =
        g_object_new(PEAK_DLSYM_TYPE_LISTENER, NULL);
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
    setup_ok = replace_check == GUM_REPLACE_OK;
    if (setup_ok && symbol_lookup_listener_required) {
        setup_ok =
            dlopen_interceptor_attach_symbol_lookup_listener_in_transaction(
                &dlsym_attach_check,
                &dlvsym_attach_check);
    }
    if (setup_ok) {
        /*
         * Record ownership before ending the Gum transaction can publish the
         * replacement/listeners to another application thread.
         */
        atomic_store_explicit(&dlopen_replacement_installed_by_peak,
                              true,
                              memory_order_release);
        dynamic_symbol_lookup_listener_attached =
            symbol_lookup_listener_required;
    } else {
        if (replace_check == GUM_REPLACE_OK) {
            gum_interceptor_revert(dlopen_interceptor,
                                   dlopen_hook_address);
        }
    }
    gum_interceptor_end_transaction(dlopen_interceptor);
    if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                     &detach_status)) {
        peak_detach_controller_abort_after_failed_finish("dlopen replace finish",
                                                        detach_status);
    }
    if (!setup_ok) {
        if (replace_check == GUM_REPLACE_OK &&
            symbol_lookup_listener_required) {
            g_printerr("[peak] skipping dynamic loader interception: dlsym attach status=%d dlvsym attach status=%d\n",
                       dlsym_attach_check,
                       dlvsym_attach_check);
            replace_check = GUM_REPLACE_WRONG_TYPE;
        }
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
            if (dynamic_symbol_lookup_listener_attached &&
                dynamic_symbol_lookup_listener != NULL) {
                gum_interceptor_detach(dlopen_interceptor,
                                       dynamic_symbol_lookup_listener);
                dynamic_symbol_lookup_listener_attached = FALSE;
            }
            gum_interceptor_revert(dlopen_interceptor, dlopen_hook_address);
            gum_interceptor_end_transaction(dlopen_interceptor);
        } else if (dynamic_symbol_lookup_listener_attached &&
                   dynamic_symbol_lookup_listener != NULL) {
            gum_interceptor_begin_transaction(dlopen_interceptor);
            gum_interceptor_detach(dlopen_interceptor,
                                   dynamic_symbol_lookup_listener);
            gum_interceptor_end_transaction(dlopen_interceptor);
            dynamic_symbol_lookup_listener_attached = FALSE;
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
    if (dynamic_symbol_lookup_listener != NULL) {
        g_object_unref(dynamic_symbol_lookup_listener);
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
    dynamic_symbol_lookup_listener = NULL;
    dlsym_hook_address = NULL;
    dlvsym_hook_address = NULL;
    dynamic_symbol_lookup_listener_attached = FALSE;
    return TRUE;
}
