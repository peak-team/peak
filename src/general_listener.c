#define _GNU_SOURCE
#include "general_listener.h"
#include "dlopen_interceptor.h"
#include "peak_detach_controller.h"
#include "pthread_listener.h"
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>

#define PEAK_SIG_STOP (SIGRTMIN + 0)
#define PEAK_SIG_CONT (SIGRTMIN + 1)

GumInterceptor* interceptor;
GumInvocationListener** array_listener;
static gboolean* array_listener_detached;
static gboolean* array_listener_reattached;
static gboolean* array_listener_gum_detached;
static gboolean* array_listener_gum_detach_flushed;
static PeakHookState* peak_hook_states;
static double* peak_hook_next_retry_time;
static unsigned int* peak_hook_retry_count;
extern gboolean* peak_need_detach;
extern gboolean* peak_detached;
extern gdouble* heartbeat_overhead;
extern gboolean** peak_target_thread_called;
extern unsigned int check_interval;
gpointer* hook_address = NULL;
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
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
_Atomic gboolean heartbeat_running = true;
static pthread_t general_controller_thread;
static pthread_mutex_t general_controller_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t general_controller_wake_cond = PTHREAD_COND_INITIALIZER;
static gboolean general_controller_running = FALSE;
static gboolean general_controller_thread_started = FALSE;
static gboolean gum_find_functions_matching_initialize = false;
static GHashTable* gum_symbol_demangled_mapping;
static GHashTable* gum_symbol_short_mapping;
static const double peak_general_overhead_floor = 1e-9;
static pthread_mutex_t detach_trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static const double peak_controller_retry_base_delay = 0.001;
static const double peak_controller_retry_max_delay = 0.050;
static const unsigned int peak_controller_shutdown_drain_ms = 1000;

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

static void
peak_general_controller_trace_mutation(size_t hook_id,
                                       PeakDetachOperation operation,
                                       const char* result,
                                       gboolean physical,
                                       PeakDetachStatus status)
{
    const char* path = g_getenv("PEAK_DETACH_TRACE_PATH");
    FILE* fp;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&detach_trace_mutex);
    fp = fopen(path, "a");
    if (fp != NULL) {
        fprintf(fp,
                "%.9f,%lu,%s,%s,%s,%d,%s\n",
                peak_second(),
                (unsigned long)hook_id,
                hook_id < peak_hook_address_count && peak_hook_strings != NULL &&
                        peak_hook_strings[hook_id] != NULL
                    ? peak_hook_strings[hook_id]
                    : "<unknown>",
                peak_detach_controller_operation_string(operation),
                result != NULL ? result : "<unknown>",
                physical ? 1 : 0,
                peak_detach_controller_status_string(status));
        fclose(fp);
    }
    pthread_mutex_unlock(&detach_trace_mutex);
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

void peak_general_listener_controller_mark_attached_unlocked(size_t hook_id)
{
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
}

static gboolean peak_general_listener_request_detach_unlocked(size_t hook_id)
{
    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_ATTACHED:
            peak_general_controller_reset_retry_unlocked(hook_id);
            peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACH_REQUESTED);
            return TRUE;
        case PEAK_HOOK_DETACH_REQUESTED:
        case PEAK_HOOK_DETACHING:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean peak_general_listener_request_reattach_unlocked(size_t hook_id)
{
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
            peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_REATTACH_REQUESTED);
            return TRUE;
        case PEAK_HOOK_REATTACH_REQUESTED:
        case PEAK_HOOK_REATTACHING:
            return TRUE;
        default:
            return FALSE;
    }
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
        peak_general_controller_note_retry_unlocked(hook_id, status);
        return TRUE;
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
    peak_general_controller_reset_retry_unlocked(hook_id);
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
    peak_general_controller_trace_mutation(hook_id,
                                           PEAK_DETACH_OPERATION_DETACH,
                                           "success",
                                           helper_applied_physical_patch,
                                           finish_status);
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
    peak_general_controller_reset_retry_unlocked(hook_id);
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
    peak_general_controller_trace_mutation(hook_id,
                                           PEAK_DETACH_OPERATION_REATTACH,
                                           "success",
                                           helper_applied_physical_patch,
                                           finish_status);
    return TRUE;
}

static gboolean peak_general_controller_shutdown_hook_unlocked(size_t hook_id,
                                                               pthread_t controller_tid,
                                                               pthread_t* tid_keys,
                                                               size_t* mapped_ids,
                                                               int* pause_session_ids,
                                                               int* pause_status,
                                                               size_t snapshot_count,
                                                               gboolean snapshot_complete)
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

static gboolean
peak_general_controller_process_pending_unlocked(pthread_t controller_tid,
                                                 pthread_t* tid_keys,
                                                 size_t* mapped_ids,
                                                 int* pause_session_ids,
                                                 int* pause_status)
{
    gboolean did_work = FALSE;
    double now = peak_second();

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

    gum_interceptor_ignore_current_thread(interceptor);

    for (;;) {
        gboolean should_run;

        pthread_mutex_lock(&general_controller_wake_mutex);
        should_run = general_controller_running;
        pthread_mutex_unlock(&general_controller_wake_mutex);
        if (!should_run) {
            break;
        }

        dlopen_interceptor_drain_dynamic_attach_queue();

        pthread_mutex_lock(&lock);
        (void)peak_general_controller_process_pending_unlocked(controller_tid,
                                                               tid_keys,
                                                               mapped_ids,
                                                               pause_session_ids,
                                                               pause_status);
        pthread_mutex_unlock(&lock);

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

    if (!peak_general_listener_controller_drain(peak_controller_shutdown_drain_ms)) {
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
    double rate;   // for global ordering only
} OverheadEntry;

// rate descending (global detach)
static int compare_rate_de(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->rate < y->rate) return 1;
    if (x->rate > y->rate) return -1;

    // tie-break: larger ratio first
    if (x->ratio < y->ratio) return 1;
    if (x->ratio > y->ratio) return -1;
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

    double* ratio_snapshot = g_new0(double, peak_hook_address_count);
    double* rate_snapshot  = g_new0(double, peak_hook_address_count);
    double* prev_ratio     = g_new0(double, peak_hook_address_count);
    double* prev_time      = g_new0(double, peak_hook_address_count);

    double now0 = peak_second();
    for (size_t i = 0; i < peak_hook_address_count; i++) {
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

    while (atomic_load(&heartbeat_running)) {
        gboolean wake_controller = FALSE;
        heartbeat_counter++;
        double now = peak_second();

        double total_execution_time = now - peak_main_time;
        if (total_execution_time <= 0.0) total_execution_time = 1e-12;

        double global_overhead = 0.0;

        pthread_mutex_lock(&lock);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (!(hook_address[i] && array_listener[i])) {
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
        if (enable_per_target_heartbeat) {
            pthread_mutex_lock(&lock);
            for (size_t i = 0; i < peak_hook_address_count; i++) {
                if (!(hook_address[i] && array_listener[i])) continue;
                if (peak_detached[i]) continue;

                if (ratio_snapshot[i] > target_profile_ratio) {
                    wake_controller |= peak_general_listener_request_detach_unlocked(i);
                }
            }
             pthread_mutex_unlock(&lock);
        }

        // ------------------------------------------------------------
        // 2) Global DETACH
        // ------------------------------------------------------------
        if (enable_global_heartbeat) {
            if (global_overhead > global_target_ratio * peak_global_detach_factor) {
                size_t n_attached = 0;
                pthread_mutex_lock(&lock);
                for (size_t i = 0; i < peak_hook_address_count; i++) {
                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (peak_detached[i]) continue;

                    entries[n_attached].index = i;
                    entries[n_attached].ratio = ratio_snapshot[i];
                    entries[n_attached].rate  = rate_snapshot[i];
                    n_attached++;
                }

                if (n_attached > 1) {
                    qsort(entries, n_attached, sizeof(OverheadEntry), compare_rate_de);
                }

                double reduced = global_overhead;
                for (size_t k = 0; k < n_attached && reduced > global_target_ratio; k++) {
                    size_t idx = entries[k].index;

                    if (!(hook_address[idx] && array_listener[idx])) continue;
                    if (peak_detached[idx]) continue;

                    reduced -= entries[k].ratio;
                    wake_controller |= peak_general_listener_request_detach_unlocked(idx);
                }
                pthread_mutex_unlock(&lock);
            }
        }

        // ------------------------------------------------------------
        // 3) Reattach
        // ------------------------------------------------------------
        if (enable_reattach &&
            check_interval != 0 &&
            (heartbeat_counter % check_interval) == 0) {
            // Per-target REATTACH
            if (enable_per_target_heartbeat) {
                for (size_t i = 0; i < peak_hook_address_count; i++) {
                    pthread_mutex_lock(&lock);
                    gboolean should_consider =
                        (hook_address[i] && array_listener[i] &&
                         peak_detached[i] && !peak_need_detach[i]);
                    pthread_mutex_unlock(&lock);
                    if (!should_consider) continue;

                    if (ratio_snapshot[i] <= target_profile_ratio) {
                        pthread_mutex_lock(&lock);
                        wake_controller |= peak_general_listener_request_reattach_unlocked(i);
                        pthread_mutex_unlock(&lock);
                    }
                }
            }
            // Global REATTACH
            if (enable_global_heartbeat) {
                if (global_overhead <= peak_global_reattach_factor * global_target_ratio) {
                    size_t detached_cnt = 0;
                    pthread_mutex_lock(&lock);
                    for (size_t i = 0; i < peak_hook_address_count; i++) {
                        if (!(hook_address[i] && array_listener[i])) continue;
                        if (!peak_detached[i]) continue;
                        if (peak_need_detach[i]) continue;

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
                        wake_controller |= peak_general_listener_request_reattach_unlocked(i);
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

        pthread_mutex_lock(&heartbeat_mutex);
        if (!atomic_load(&heartbeat_running)) {
            pthread_mutex_unlock(&heartbeat_mutex);
            break;
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
        pthread_mutex_unlock(&heartbeat_mutex);
    }

    g_free(prev_time);
    g_free(prev_ratio);
    g_free(rate_snapshot);
    g_free(ratio_snapshot);
    g_free(entries);

    gum_interceptor_unignore_current_thread(interceptor);
    return NULL;
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (!listener || g_object_is_floating(listener)) {
            return;
    }
    gum_interceptor_ignore_current_thread(interceptor);

    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    pthread_t my_tid = pthread_self();
    gboolean mapped_found = FALSE;
    size_t mapped_tid = pthread_listener_lookup_thread(my_tid, &mapped_found);
    if (!mapped_found || mapped_tid >= peak_max_num_threads) {
        mapped_tid = 0;
    }
    thread_data.self_mapped_id = mapped_tid;
    thread_data.self_mapped_known = mapped_found && mapped_tid < peak_max_num_threads;
    if (peak_detach_cost == 0 && heartbeat_time == 0) {
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
        peak_target_thread_called[hook_id][index] = true;
        gboolean detach_requested = FALSE;

        pthread_mutex_lock(&lock);
        if (self->num_calls[index] >= peak_detach_count) {
            detach_requested = peak_general_listener_request_detach_unlocked(hook_id);
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
    PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
    priv->start_time = peak_second();
    priv->initialized = TRUE;
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    gum_interceptor_ignore_current_thread(interceptor);
    if (peak_detach_cost == 0 && heartbeat_time == 0) {
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
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        thread_data.level--;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] += end_time - thread_data.child_time[thread_data.level];
        thread_data.child_time[thread_data.level] = 0.0;
        priv->initialized = FALSE;
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
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        thread_data.level--;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] += end_time - thread_data.child_time[thread_data.level];
        thread_data.child_time[thread_data.level] = 0.0;
        priv->initialized = FALSE;
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
        g_printerr("[peak] skipping overhead calibration Gum attach: %s\n",
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

static void peak_build_symbol_map_once(void) {
    gum_find_functions_matching_initialize = true;
    GArray* addresses = gum_find_functions_matching("_Z*");
    gum_symbol_demangled_mapping = g_hash_table_new_full(g_str_hash,
                                                         str_equal_function_general,
                                                         g_free,
                                                         (GDestroyNotify) g_ptr_array_unref);
    gum_symbol_short_mapping = g_hash_table_new_full(g_str_hash,
                                                     str_equal_function_general,
                                                     g_free,
                                                     (GDestroyNotify) g_ptr_array_unref);

    for (gsize j = 0; j < addresses->len; j++) {
        gpointer addr = g_array_index(addresses, gpointer, j);
        if (!addr) continue;
        gchar* mangled = gum_symbol_name_from_address(addr);
        if (!mangled) continue;

        char* demangled = cxa_demangle(mangled);
        g_free(mangled);
        if (!demangled) continue;

        GPtrArray* demangled_candidates = g_hash_table_lookup(gum_symbol_demangled_mapping, demangled);
        if (!demangled_candidates) {
            demangled_candidates = g_ptr_array_new();
            g_hash_table_insert(gum_symbol_demangled_mapping, g_strdup(demangled), demangled_candidates);
        }
        g_ptr_array_add(demangled_candidates, addr);

        char* function_name = extract_function_name(demangled);
        GPtrArray* short_candidates = g_hash_table_lookup(gum_symbol_short_mapping, function_name);
        if (!short_candidates) {
            short_candidates = g_ptr_array_new();
            g_hash_table_insert(gum_symbol_short_mapping, g_strdup(function_name), short_candidates);
        }
        g_ptr_array_add(short_candidates, addr);

        free(function_name);
        free(demangled);
    }

    g_array_free(addresses, TRUE);
}

void peak_general_listener_attach()
{
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_reattached = g_new0(gboolean, peak_hook_address_count);
    array_listener_gum_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_gum_detach_flushed = g_new0(gboolean, peak_hook_address_count);
    peak_hook_states = g_new0(PeakHookState, peak_hook_address_count);
    peak_hook_next_retry_time = g_new0(double, peak_hook_address_count);
    peak_hook_retry_count = g_new0(unsigned int, peak_hook_address_count);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    peak_demangled_strings = g_new0(char*, peak_hook_address_count);
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        // replace certain function we are capturing already.
        if (strcmp(peak_hook_strings[i], "MPI_Finalize") == 0) {
            hook_address[i] = gum_find_function("peak_pmpi_finalize");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "close") == 0) {
            hook_address[i] = gum_find_function("peak_close");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "exit") == 0) {
            hook_address[i] = gum_find_function("peak_exit");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "main") == 0) {
            hook_address[i] = NULL;
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelEx") == 0) {
            // C++ API template versions, also use cudaLaunchKernelExC internal
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelExC") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernelEx") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_kernel_ex");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaGraphLaunch") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_graph_launch");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuGraphLaunch") == 0) {
            hook_address[i] = gum_find_function("peak_cu_graph_launch");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "dlopen") == 0) {
            hook_address[i] = gum_find_function("peak_dlopen");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else {
            gpointer ptr = gum_find_function(peak_hook_strings[i]);
            if (ptr) {
                hook_address[i] = ptr;
                char* demangled = cxa_demangle(peak_hook_strings[i]);
                peak_demangled_strings[i] = g_strdup(demangled);
                free(demangled);
            } else {
                if (peak_symbol_should_use_cpp_map(peak_hook_strings[i])) {
                    if (!gum_find_functions_matching_initialize) {
                        peak_build_symbol_map_once();
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
            }
        }
        if (hook_address[i]) {
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);
            gpointer resolved_hook_address = hook_address[i];
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
            if (!peak_detach_controller_prepare_hook_mutation(&mutation_request,
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
            if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
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
        peak_general_overhead_bootstrapping();
        if (peak_detach_cost > 0) {
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

static FILE* peak_stats_csv_open(void) {
    char base[256] = {0};
    char out_csv[512] = {0};

    const char *env_path = getenv("PEAK_STATSLOG_PATH");
    if (env_path && *env_path) {
        size_t n = strlen(env_path);
        if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, env_path, n);
        base[n] = '\0';
    } else {
        snprintf(base, sizeof(base), "./peak_statslog");
    }

    int pid = (int) getpid();
    snprintf(out_csv, 512, "%s-p%d.csv", base, pid);
    
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

static void
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
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            have_output = TRUE;
            break;
        }
    }

    if (!have_output) {
        return;
    }

    FILE* csv = peak_stats_csv_open();

    if (csv) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                fprintf(csv, "\"%s\",%lu,%lu,%lu,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e\n",
                    peak_demangled_strings[i],
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
    peak_general_listener_export_csv_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count,
                                            rank_count
    );

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
    if (have_output) {
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("%*s PEAK Library\n", (row_width - 12) / 2, "");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("Time: %f\n", peak_main_time);
        g_printerr("PEAK done with: %s\n", argv_o);
        g_printerr("Estimated overhead: %.3es per call and %.3es total\n", peak_general_overhead, total_overhead);

        g_printerr("\n%.*s function statistics (call)  %.*s\n", (row_width - 28) / 2, row_separator, (row_width - 28) / 2, row_separator);
        g_printerr(" individual call counts and time (in seconds)\n");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "count",
                   max_col_width, "per thread",
                   max_col_width, "per rank",
                   max_col_width, "max",
                   max_col_width, "min");
        g_printerr("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                if (!array_listener_detached[i])
                    g_printerr("|%*s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                               max_function_width, truncated_name,
                               max_col_width, sum_num_calls[i],
                               max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                               max_col_width, sum_num_calls[i] / rank_count,
                               max_col_width, sum_max_time[i],
                               max_col_width, sum_min_time[i]);
                else {
                    if (!array_listener_reattached[i])
                        g_printerr("|%*s*|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                    else
                        g_printerr("|%*s**|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
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
        g_printerr("%.*s\n", row_width, row_separator);

        g_printerr("\n%.*s function statistics (thread)  %.*s\n", (row_width - 30) / 2, row_separator, (row_width - 30) / 2, row_separator);
        g_printerr(" thread aggregated time (in seconds)\n");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "total",
                   max_col_width, "exclusive",
                   max_col_width, "max",
                   max_col_width, "min",
                   max_col_width, "overhead");
        g_printerr("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                if (!array_listener_detached[i]) {
                    g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                               max_function_width, truncated_name,
                               max_col_width, sum_total_time[i],
                               max_col_width, sum_exclusive_time[i],
                               max_col_width, max_total_time[i],
                               max_col_width, min_total_time[i],
                               max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                              * peak_general_overhead);
                } else {
                    if (!array_listener_reattached[i])
                        g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                    max_function_width, truncated_name,
                                    max_col_width, sum_total_time[i],
                                    max_col_width, sum_exclusive_time[i],
                                    max_col_width, max_total_time[i],
                                    max_col_width, min_total_time[i],
                                    max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                    * peak_general_overhead);
                    else
                        g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
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
        g_printerr("%.*s\n", row_width, row_separator);
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
    }
    g_free(peak_demangled_strings);
    free(argv_o);
    free(row_separator);
}

#ifdef HAVE_MPI
static void
peak_general_listener_reduce_result(gulong* sum_num_calls,
                                    gdouble* sum_total_time,
                                    gdouble* max_total_time,
                                    gdouble* min_total_time,
                                    gdouble* sum_exclusive_time,
                                    gfloat* sum_max_time,
                                    gfloat* sum_min_time,
                                    gulong* thread_count)
{
    int rank, size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if (!init_flag)
        MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    gulong* mpi_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* mpi_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* mpi_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* mpi_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* mpi_thread_count = g_new0(gulong, peak_hook_address_count);
    gboolean* mpi_array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    MPI_Reduce(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(max_total_time, mpi_max_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(min_total_time, mpi_min_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_exclusive_time, mpi_sum_exclusive_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(array_listener_detached, mpi_array_listener_detached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
    g_free(array_listener_detached);
    array_listener_detached = mpi_array_listener_detached;
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
}
#endif

void peak_general_listener_print(int is_MPI)
{
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
    if (is_MPI) {
        peak_general_listener_reduce_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count);
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
            double deadline = peak_second() +
                              ((double)peak_controller_shutdown_drain_ms / 1000.0);

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

                shutdown_ok = peak_general_controller_shutdown_hook_unlocked(
                    i,
                    controller_tid,
                    tid_keys,
                    mapped_ids,
                    pause_session_ids,
                    pause_status,
                    snapshot_count,
                    snapshot_complete);
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
                g_printerr("[peak] Gum shutdown detach was not proven safe; leaving listener state alive\n");
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
    g_free(array_listener_gum_detached);
    g_free(array_listener_gum_detach_flushed);
    g_free(peak_hook_states);
    g_free(peak_hook_next_retry_time);
    g_free(peak_hook_retry_count);
    g_free(array_listener);

    interceptor = NULL;
    hook_address = NULL;
    array_listener_detached = NULL;
    array_listener_reattached = NULL;
    array_listener_gum_detached = NULL;
    array_listener_gum_detach_flushed = NULL;
    peak_hook_states = NULL;
    peak_hook_next_retry_time = NULL;
    peak_hook_retry_count = NULL;
    array_listener = NULL;

    return TRUE;
}
