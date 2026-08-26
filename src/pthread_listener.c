#define _GNU_SOURCE
#include "pthread_listener.h"
#include "general_listener.h"
#include "detach_controller.h"
#include "logging.h"
#include "internal/pthread_slot_registry.h"
#include "internal/signal_policy_internal.h"
#include "utils/env_parser.h"

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <sys/stat.h>
#include <sys/syscall.h>
#endif
#include <time.h>

#define PEAK_PTHREAD_START_HANDSHAKE_TIMEOUT_MS_ENV \
    "PEAK_PTHREAD_START_HANDSHAKE_TIMEOUT_MS"
#define PEAK_PTHREAD_START_HANDSHAKE_TIMEOUT_MS_DEFAULT 5000U
#define PEAK_PTHREAD_RECLAIM_SCAN_BUDGET 64U
#define PEAK_PTHREAD_RECLAIM_SCAN_INTERVAL_MS 100U

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* pthread_create_interceptor;
static GumInvocationListener* pthread_create_listener;
static PthreadState pthread_create_state;
static GumAttachOptions pthread_create_attach_options = {
    .listener_function_data = &pthread_create_state
};
static gboolean tid_mapping_initialized = FALSE;
static PeakPthreadSlotRegistry pthread_slot_registry;
static gpointer pthread_create_hook_address;
static gpointer pthread_join_hook_address;
static gpointer pthread_detach_hook_address;
extern pthread_t heartbeat_thread;
extern gulong peak_max_num_threads;
static unsigned int pthread_start_handshake_timeout_ms =
    PEAK_PTHREAD_START_HANDSHAKE_TIMEOUT_MS_DEFAULT;
static PeakEnvWarningState pthread_start_timeout_config_warning;
static _Atomic int pthread_start_timeout_warning_emitted;
static int pthread_task_directory_fd = -1;
static pthread_mutex_t pthread_reclaim_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct timespec pthread_reclaim_last_scan;
static gboolean pthread_reclaim_last_scan_valid;
static PeakPthreadReclamationDiagnostics pthread_reclaim_diagnostics;

static void pthread_listener_iface_init(gpointer g_iface, gpointer iface_data);
static gboolean pthread_listener_insert_thread_unlocked(pthread_t tid);

/*
 * The callback path reads only this initial-exec TLS record.  The pthread key
 * is deliberately PEAK-owned: it keeps the identity alive across user TLS
 * destructors, whose ordering is unspecified by POSIX.
 */
typedef struct {
    size_t slot;
    uint64_t generation;
    unsigned int destructor_passes;
    gboolean valid;
} PeakPthreadSlotIdentity;

static __thread PeakPthreadSlotIdentity pthread_slot_identity
    __attribute__((tls_model("initial-exec")));
static __thread gboolean pthread_next_create_is_helper
    __attribute__((tls_model("initial-exec")));
static __thread gboolean pthread_thread_excluded
    __attribute__((tls_model("initial-exec")));
static pthread_key_t pthread_slot_key;
static pthread_once_t pthread_slot_key_once = PTHREAD_ONCE_INIT;
static gboolean pthread_slot_key_created = FALSE;
#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic unsigned int pthread_test_fail_slot_publish = 0;
static _Atomic int pthread_test_pause_start_publication;
static _Atomic int pthread_test_release_start_publication;
static _Atomic int pthread_test_join_detach_race_enabled;
static _Atomic unsigned int pthread_test_join_detach_race_paused;
static _Atomic int pthread_test_join_detach_race_release;
#endif

static void peak_pthread_slot_destructor(void* data);

static void
pthread_listener_slot_key_create(void)
{
    pthread_slot_key_created =
        pthread_key_create(&pthread_slot_key, peak_pthread_slot_destructor) == 0;
}

static gboolean
pthread_listener_publish_current_slot(size_t slot, uint64_t generation)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    unsigned int failures = atomic_load_explicit(&pthread_test_fail_slot_publish,
                                                 memory_order_relaxed);
    if (failures != 0 && atomic_compare_exchange_strong_explicit(
                             &pthread_test_fail_slot_publish,
                             &failures,
                             failures - 1,
                             memory_order_relaxed,
                             memory_order_relaxed)) {
        return FALSE;
    }
#endif
    pthread_once(&pthread_slot_key_once, pthread_listener_slot_key_create);
    if (!pthread_slot_key_created) {
        return FALSE;
    }
    pthread_slot_identity.slot = slot;
    pthread_slot_identity.generation = generation;
    pthread_slot_identity.destructor_passes = 0;
    pthread_slot_identity.valid = TRUE;
    if (pthread_setspecific(pthread_slot_key, &pthread_slot_identity) != 0) {
        pthread_slot_identity.valid = FALSE;
        return FALSE;
    }
    return TRUE;
}

typedef void* (*pthread_start_routine_t)(void*);

typedef struct {
    pthread_start_routine_t start_routine;
    void* start_arg;
    gboolean skip_tracking;
    gboolean tracked;
    gboolean detached;
    size_t slot;
    uint64_t generation;
    _Atomic int handshake_state;
    _Atomic unsigned int references;
} PeakPthreadStartContext;

static pid_t
pthread_listener_current_kernel_tid(void)
{
#if defined(__linux__) && defined(SYS_gettid)
    return (pid_t)syscall(SYS_gettid);
#else
    return 0;
#endif
}

static void
pthread_listener_wake_reclaimer_if_pending(gboolean became_exit_pending)
{
    if (became_exit_pending) {
        peak_general_listener_controller_wake();
    }
}

static void
peak_pthread_start_context_release(PeakPthreadStartContext* context)
{
    if (context != NULL &&
        atomic_fetch_sub_explicit(&context->references, 1,
                                  memory_order_acq_rel) == 1) {
        g_free(context);
    }
}

static void
peak_pthread_start_warn_abandoned(double elapsed_ms)
{
    int expected = 0;

    if (atomic_compare_exchange_strong_explicit(
            &pthread_start_timeout_warning_emitted, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        if (elapsed_ms >= 0.0) {
            peak_log_warn(
                "[peak] pthread child metadata publication did not complete "
                "after %.3f ms (timeout=%u ms); running the child untracked\n",
                elapsed_ms, pthread_start_handshake_timeout_ms);
        } else {
            peak_log_warn(
                "[peak] pthread child metadata publication did not complete "
                "(elapsed unavailable; timeout=%u ms); running the child "
                "untracked\n",
                pthread_start_handshake_timeout_ms);
        }
    }
}

static double
peak_pthread_start_elapsed_ms(const struct timespec* started)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1.0;
    }
    return (double)(now.tv_sec - started->tv_sec) * 1000.0 +
           (double)(now.tv_nsec - started->tv_nsec) / 1000000.0;
}

static void
peak_pthread_slot_destructor(void* data)
{
    PeakPthreadSlotIdentity* identity = data;

    if (identity == NULL || !identity->valid) {
        return;
    }

    peak_general_listener_release_current_thread_frames_preserve_slot();

    /*
     * POSIX does not order key destructors.  Re-publish through all available
     * iterations so application destructors keep their slot.  Crucially, this
     * destructor never clears, retires, or reuses the slot: an implementation
     * may run PEAK before an application key in its final iteration. A join
     * handoff or later Linux kernel-TID absence proof is still required.
     */
    identity->destructor_passes++;
#ifdef PTHREAD_DESTRUCTOR_ITERATIONS
    if (identity->destructor_passes < PTHREAD_DESTRUCTOR_ITERATIONS) {
#else
    if (identity->destructor_passes < 4U) {
#endif
        (void)pthread_setspecific(pthread_slot_key, identity);
        return;
    }

    /* Do not remove or mark this map entry dead here.  POSIX may run an
     * application destructor after PEAK in this final iteration, and that
     * destructor may still enter a target.  A detached slot becomes pending,
     * but cannot be retired until the controller proves that its kernel TID
     * has left /proc/self/task. */
    if (tid_mapping_initialized) {
        PeakPthreadSlotToken token = {
            .slot = identity->slot,
            .generation = identity->generation,
        };
        bool became_exit_pending = false;

        (void)peak_pthread_slot_registry_mark_final_destructor(
            &pthread_slot_registry, token, &became_exit_pending);
        pthread_listener_wake_reclaimer_if_pending(became_exit_pending);
    }
}

static void
peak_pthread_start_cleanup(void* data)
{
    PeakPthreadStartContext* context = data;

    peak_pthread_start_context_release(context);
}

static void*
peak_pthread_start(void* data)
{
    PeakPthreadStartContext* context = data;
    pthread_start_routine_t start_routine = context->start_routine;
    void* start_arg = context->start_arg;
    void* ret = NULL;
    PeakPthreadStartHandshakeState handshake_state;
    struct timespec wait_started;
    gboolean wait_clock_available =
        clock_gettime(CLOCK_MONOTONIC, &wait_started) == 0;

    pthread_cleanup_push(peak_pthread_start_cleanup, context);
    handshake_state = peak_pthread_slot_registry_wait_ready(
        &context->handshake_state, pthread_start_handshake_timeout_ms);
    if (handshake_state == PEAK_PTHREAD_START_ABANDONED) {
        peak_pthread_start_warn_abandoned(
            wait_clock_available ?
                peak_pthread_start_elapsed_ms(&wait_started) : -1.0);
    }
    if (!context->skip_tracking &&
        handshake_state == PEAK_PTHREAD_START_READY) {
        if (context->tracked) {
            if (!pthread_listener_publish_current_slot(context->slot,
                                                        context->generation)) {
                /* This is a user thread, not a PEAK helper.  Leave its slot
                 * identity invalid so callbacks are explicitly counted as
                 * dropped rather than silently excluded. */
                pthread_thread_excluded = FALSE;
            } else if (tid_mapping_initialized) {
                PeakPthreadSlotToken token = {
                    .slot = context->slot,
                    .generation = context->generation,
                };
                (void)peak_pthread_slot_registry_mark_kernel_tid(
                    &pthread_slot_registry,
                    token,
                    pthread_listener_current_kernel_tid());
            }
        }
    } else {
        pthread_thread_excluded = TRUE;
    }
    (void)peak_signal_policy_unblock_reserved_for_current_thread();

    peak_detach_controller_wait_for_mutation_window();
    ret = start_routine(start_arg);
    pthread_cleanup_pop(1);

    return ret;
}

/* Forget an identity whose pthread_t is being reused without a slot
 * reservation. The old slot is intentionally not queued: no join proves that
 * its TLS destructors have completed. */
static gboolean
pthread_listener_quarantine_ambiguous_tid_unlocked(pthread_t tid)
{
    if (!tid_mapping_initialized) {
        return FALSE;
    }
    return peak_pthread_slot_registry_quarantine(&pthread_slot_registry, tid);
}

static gboolean
pthread_listener_insert_thread_unlocked(pthread_t tid)
{
    PeakPthreadSlotToken token;

    if (!peak_pthread_slot_registry_reserve_insert(&pthread_slot_registry,
                                                    tid, &token)) {
        return FALSE;
    }
    if (!pthread_listener_publish_current_slot(token.slot, token.generation)) {
        (void)peak_pthread_slot_registry_compare_remove(&pthread_slot_registry,
                                                        tid,
                                                        token.generation,
                                                        true);
        return FALSE;
    }
    return TRUE;
}

#define PTHREAD_TYPE_LISTENER (pthread_listener_get_type())
G_DECLARE_FINAL_TYPE(PthreadListener, pthread_listener, PTHREAD, LISTENER, GObject)
G_DEFINE_TYPE_EXTENDED(PthreadListener,
                       pthread_listener,
                       G_TYPE_OBJECT,
                       0,
                       G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                                             pthread_listener_iface_init))

static void
pthread_listener_on_enter(GumInvocationListener* listener,
                          GumInvocationContext* ic)
{
    PthreadState* thread_state = GUM_IC_GET_THREAD_DATA(ic, PthreadState);
    pthread_t* tid = (pthread_t*)(gum_invocation_context_get_nth_argument(ic, 0));
    if (tid == NULL) {
        pthread_t* replaced_tid = g_new0(pthread_t, 1);
        gum_invocation_context_replace_nth_argument(ic, 0, replaced_tid);
        thread_state->child_tid = replaced_tid;
        thread_state->is_original = FALSE;
    } else {
        thread_state->child_tid = tid;
        thread_state->is_original = TRUE;
    }

    /*
     * Park the creator before the real pthread_create() call while a strict
     * mutation window is active. Waiting only in the child wrapper leaves a
     * kernel-visible newborn task that the helper backend may observe before
     * it has reached PEAK's gate.
     */
    peak_detach_controller_wait_for_mutation_window();

    PeakPthreadStartContext* start_context = g_new0(PeakPthreadStartContext, 1);
    start_context->start_routine =
        (pthread_start_routine_t)gum_invocation_context_get_nth_argument(ic, 2);
    start_context->start_arg = gum_invocation_context_get_nth_argument(ic, 3);
    start_context->skip_tracking =
        (tid == &heartbeat_thread) || pthread_next_create_is_helper;
    const pthread_attr_t* attr =
        gum_invocation_context_get_nth_argument(ic, 1);
    int detach_state = PTHREAD_CREATE_JOINABLE;
    start_context->detached =
        attr != NULL &&
        pthread_attr_getdetachstate(attr, &detach_state) == 0 &&
        detach_state == PTHREAD_CREATE_DETACHED;
    atomic_init(&start_context->handshake_state,
                PEAK_PTHREAD_START_PENDING);
    atomic_init(&start_context->references, 2);
    pthread_next_create_is_helper = FALSE;
    thread_state->start_context = start_context;
    gum_invocation_context_replace_nth_argument(ic, 2, (gpointer)peak_pthread_start);
    gum_invocation_context_replace_nth_argument(ic, 3, start_context);
}

static void
pthread_listener_on_leave(GumInvocationListener* listener,
                          GumInvocationContext* ic)
{
    PthreadState* thread_state = GUM_IC_GET_THREAD_DATA(ic, PthreadState);
    int create_ret = GPOINTER_TO_INT(gum_invocation_context_get_return_value(ic));
    PeakPthreadStartContext* context = thread_state->start_context;
    if (context != NULL) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (create_ret == 0 &&
            atomic_load_explicit(&pthread_test_pause_start_publication,
                                 memory_order_acquire) != 0) {
            while (atomic_load_explicit(
                       &pthread_test_release_start_publication,
                       memory_order_acquire) == 0) {
                sched_yield();
            }
            atomic_store_explicit(&pthread_test_pause_start_publication,
                                  0,
                                  memory_order_release);
        }
#endif
        if (create_ret == 0) {
            if (tid_mapping_initialized && thread_state->child_tid != NULL) {
                PeakPthreadSlotToken token;

                if (!context->skip_tracking &&
                    peak_pthread_slot_registry_reserve_insert(
                        &pthread_slot_registry, *thread_state->child_tid, &token)) {
                    context->slot = token.slot;
                    context->generation = token.generation;
                    context->tracked = TRUE;
                    if (context->detached) {
                        (void)peak_pthread_slot_registry_mark_detached(
                            &pthread_slot_registry,
                            *thread_state->child_tid,
                            context->generation,
                            NULL);
                    }
                } else {
                    /* A detached/unjoined thread may have exited and its
                     * pthread_t can already be reused by this untracked
                     * child.  Delete the stale identity to ensure a later
                     * pthread_join cannot retire or recycle that old slot.
                     * Deliberately do not queue its slot: the stale identity
                     * prevents a generation-safe kernel-TID proof, so the old
                     * slot remains quarantined. */
                    (void)pthread_listener_quarantine_ambiguous_tid_unlocked(
                        *thread_state->child_tid);
                }
            }
        }
        if (create_ret != 0) {
            g_free(context);
            thread_state->start_context = NULL;
        } else {
            gboolean published =
                peak_pthread_slot_registry_publish_ready(
                    &context->handshake_state);

            if (!published && context->tracked) {
                (void)peak_pthread_slot_registry_compare_remove(
                    &pthread_slot_registry,
                    *thread_state->child_tid,
                    context->generation,
                    true);
                context->tracked = FALSE;
            }
            peak_pthread_start_context_release(context);
            thread_state->start_context = NULL;
        }
    }
    if (!thread_state->is_original)
        g_free(thread_state->child_tid);
}

static void
pthread_listener_class_init(PthreadListenerClass* klass)
{
    (void)PTHREAD_IS_LISTENER;
    (void)glib_autoptr_cleanup_PthreadListener;
}

static void
pthread_listener_iface_init(gpointer g_iface,
                            gpointer iface_data)
{
    GumInvocationListenerInterface* iface = g_iface;
    iface->on_enter = pthread_listener_on_enter;
    iface->on_leave = pthread_listener_on_leave;
}

static void
pthread_listener_init(PthreadListener* self)
{
}

static int (*original_pthread_join)(pthread_t thread, void **retval);
static int (*original_pthread_detach)(pthread_t thread);

static void
pthread_listener_test_pause_join_detach_after_capture(gboolean captured)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (captured &&
        atomic_load_explicit(&pthread_test_join_detach_race_enabled,
                             memory_order_acquire) != 0) {
        atomic_fetch_add_explicit(&pthread_test_join_detach_race_paused, 1,
                                  memory_order_acq_rel);
        while (atomic_load_explicit(&pthread_test_join_detach_race_release,
                                    memory_order_acquire) == 0) {
            sched_yield();
        }
    }
#else
    (void)captured;
#endif
}

static gboolean
pthread_listener_capture_thread_token(pthread_t thread,
                                      size_t* slot_out,
                                      uint64_t* generation_out)
{
    PeakPthreadSlotToken token;

    if (!tid_mapping_initialized ||
        !peak_pthread_slot_registry_capture(&pthread_slot_registry, thread,
                                            &token)) {
        return FALSE;
    }
    *slot_out = token.slot;
    *generation_out = token.generation;
    return TRUE;
}

static void
pthread_listener_remove_thread_if_token_unlocked(pthread_t thread,
                                                 uint64_t generation,
                                                 gboolean reusable)
{
    if (tid_mapping_initialized) {
        (void)peak_pthread_slot_registry_compare_remove(&pthread_slot_registry,
                                                        thread, generation,
                                                        reusable);
    }
}

static int
peak_pthread_join(pthread_t thread, void **retval)
{
    size_t slot = 0;
    uint64_t generation = 0;
    gboolean captured =
        pthread_listener_capture_thread_token(thread, &slot, &generation);
    pthread_listener_test_pause_join_detach_after_capture(captured);
    int ret = original_pthread_join(thread, retval);
    if (ret == 0 && captured) {
        PeakPthreadSlotToken token = {
            .slot = slot,
            .generation = generation,
        };

        /* pthread_join returning is the post-termination handoff: the joined
         * thread and every TLS destructor are complete. Claim the generation
         * before touching its physical slot so a detach/join race cannot
         * release and reuse that slot underneath retirement. */
        if (peak_pthread_slot_registry_begin_retire(&pthread_slot_registry,
                                                    token)) {
            gboolean reusable =
                peak_general_listener_retire_current_thread_slot(slot);
            (void)peak_pthread_slot_registry_complete_retire(
                &pthread_slot_registry, token, reusable);
        }
    }
    return ret;
}

static int
peak_pthread_detach(pthread_t thread)
{
    PeakPthreadSlotToken token;
    gboolean captured = tid_mapping_initialized &&
        peak_pthread_slot_registry_capture(&pthread_slot_registry,
                                            thread,
                                            &token);
    pthread_listener_test_pause_join_detach_after_capture(captured);
    int ret = original_pthread_detach(thread);

    if (ret == 0 && captured) {
        bool became_exit_pending = false;

        (void)peak_pthread_slot_registry_mark_detached(
            &pthread_slot_registry,
            thread,
            token.generation,
            &became_exit_pending);
        pthread_listener_wake_reclaimer_if_pending(became_exit_pending);
    }
    return ret;
}

static gboolean
pthread_listener_reclaim_interval_elapsed(const struct timespec* now)
{
    if (!pthread_reclaim_last_scan_valid) {
        return TRUE;
    }

    int64_t elapsed_ns =
        (int64_t)(now->tv_sec - pthread_reclaim_last_scan.tv_sec) *
            1000000000LL +
        (int64_t)(now->tv_nsec - pthread_reclaim_last_scan.tv_nsec);
    return elapsed_ns >=
        (int64_t)PEAK_PTHREAD_RECLAIM_SCAN_INTERVAL_MS * 1000000LL;
}

gboolean
pthread_listener_reclaim_detached_slots(void)
{
#if defined(__linux__)
    PeakPthreadDetachedCandidate
        candidates[PEAK_PTHREAD_RECLAIM_SCAN_BUDGET];
    struct timespec now;
    size_t entries_examined = 0;
    size_t candidate_count;
    gboolean did_work = FALSE;

    if (!tid_mapping_initialized || pthread_task_directory_fd < 0 ||
        peak_pthread_slot_registry_exit_pending_count(
            &pthread_slot_registry, NULL) == 0 ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return FALSE;
    }

    (void)pthread_mutex_lock(&pthread_reclaim_mutex);
    if (!pthread_listener_reclaim_interval_elapsed(&now)) {
        (void)pthread_mutex_unlock(&pthread_reclaim_mutex);
        return FALSE;
    }
    pthread_reclaim_last_scan = now;
    pthread_reclaim_last_scan_valid = TRUE;
    candidate_count = peak_pthread_slot_registry_snapshot_exit_pending(
        &pthread_slot_registry,
        candidates,
        PEAK_PTHREAD_RECLAIM_SCAN_BUDGET,
        &entries_examined);
    pthread_reclaim_diagnostics.scans++;
    pthread_reclaim_diagnostics.entries_examined += entries_examined;
    pthread_reclaim_diagnostics.candidates_checked += candidate_count;

    for (size_t index = 0; index < candidate_count; index++) {
        char tid_name[32];
        struct stat task_stat;
        int written = snprintf(tid_name, sizeof(tid_name), "%ld",
                               (long)candidates[index].kernel_tid);

        if (written <= 0 || (size_t)written >= sizeof(tid_name)) {
            pthread_reclaim_diagnostics.ambiguous_checks++;
            continue;
        }
        errno = 0;
        if (fstatat(pthread_task_directory_fd, tid_name, &task_stat,
                    AT_SYMLINK_NOFOLLOW) == 0) {
            pthread_reclaim_diagnostics.deferred_alive++;
            continue;
        }
        if (errno != ENOENT) {
            pthread_reclaim_diagnostics.ambiguous_checks++;
            continue;
        }

        if (!peak_pthread_slot_registry_begin_retire(
                &pthread_slot_registry, candidates[index].token)) {
            pthread_reclaim_diagnostics.ambiguous_checks++;
            continue;
        }
        gboolean reusable = peak_general_listener_retire_current_thread_slot(
            candidates[index].token.slot);
        if (!reusable) {
            pthread_reclaim_diagnostics.retire_failures++;
            (void)peak_pthread_slot_registry_defer_retire(
                &pthread_slot_registry, candidates[index].token);
            continue;
        }
        if (peak_pthread_slot_registry_complete_retire(
                &pthread_slot_registry, candidates[index].token, true)) {
            pthread_reclaim_diagnostics.reclaimed++;
            did_work = TRUE;
        } else {
            pthread_reclaim_diagnostics.ambiguous_checks++;
        }
    }

    pthread_reclaim_diagnostics.pending =
        peak_pthread_slot_registry_exit_pending_count(
            &pthread_slot_registry,
            &pthread_reclaim_diagnostics.max_pending);
    (void)pthread_mutex_unlock(&pthread_reclaim_mutex);
    return did_work;
#else
    return FALSE;
#endif
}

void
pthread_listener_get_reclamation_diagnostics(
    PeakPthreadReclamationDiagnostics* diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }

    (void)pthread_mutex_lock(&pthread_reclaim_mutex);
    pthread_reclaim_diagnostics.pending =
        peak_pthread_slot_registry_exit_pending_count(
            &pthread_slot_registry,
            &pthread_reclaim_diagnostics.max_pending);
    pthread_reclaim_diagnostics.scan_budget =
        PEAK_PTHREAD_RECLAIM_SCAN_BUDGET;
    pthread_reclaim_diagnostics.scan_interval_ms =
        PEAK_PTHREAD_RECLAIM_SCAN_INTERVAL_MS;
    *diagnostics = pthread_reclaim_diagnostics;
    (void)pthread_mutex_unlock(&pthread_reclaim_mutex);
}

void pthread_listener_attach()
{
    PeakEnvUnsignedSchema handshake_timeout_schema = {
        PEAK_PTHREAD_START_HANDSHAKE_TIMEOUT_MS_ENV,
        "milliseconds",
        PEAK_PTHREAD_START_HANDSHAKE_TIMEOUT_MS_DEFAULT,
        1,
        UINT_MAX,
        false,
        &pthread_start_timeout_config_warning,
        false,
    };

    pthread_start_handshake_timeout_ms = (unsigned int)
        peak_parse_env_unsigned(&handshake_timeout_schema);
    tid_mapping_initialized = peak_pthread_slot_registry_init(
        &pthread_slot_registry, peak_max_num_threads);
    if (tid_mapping_initialized) {
        (void)pthread_listener_insert_thread_unlocked(pthread_self());
    }

    pthread_create_interceptor = gum_interceptor_obtain();
    pthread_create_listener = g_object_new(PTHREAD_TYPE_LISTENER, NULL);
    peak_detach_controller_note_thread_creation_gate_installed(FALSE);

    gum_interceptor_begin_transaction(pthread_create_interceptor);
    pthread_create_hook_address =
        peak_general_listener_find_function("pthread_create");
    if (pthread_create_hook_address) {
        GumAttachReturn attach_status =
            gum_interceptor_attach(pthread_create_interceptor,
                                   pthread_create_hook_address,
                                   pthread_create_listener,
                                   &pthread_create_attach_options);
        if (attach_status == GUM_ATTACH_OK) {
            peak_detach_controller_note_thread_creation_gate_installed(TRUE);
        }
    }
    pthread_join_hook_address =
        peak_general_listener_find_function("pthread_join");
    if (pthread_join_hook_address) {
        gum_interceptor_replace_fast(pthread_create_interceptor,
                                    pthread_join_hook_address, &peak_pthread_join,
                                    (gpointer*)(&original_pthread_join),
                                    NULL);
    }
    pthread_detach_hook_address =
        peak_general_listener_find_function("pthread_detach");
    if (pthread_detach_hook_address) {
        gum_interceptor_replace_fast(pthread_create_interceptor,
                                    pthread_detach_hook_address,
                                    &peak_pthread_detach,
                                    (gpointer*)(&original_pthread_detach),
                                    NULL);
    }
    gum_interceptor_end_transaction(pthread_create_interceptor);

#if defined(__linux__)
    if (pthread_task_directory_fd < 0) {
        pthread_task_directory_fd = open("/proc/self/task",
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
#endif
}

static gboolean
pthread_listener_flush_teardown(void)
{
    const unsigned int max_attempts = 100;

    if (pthread_create_interceptor == NULL) {
        return TRUE;
    }

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++) {
        if (gum_interceptor_flush(pthread_create_interceptor)) {
            return TRUE;
        }
        usleep(1000);
    }

    return gum_interceptor_flush(pthread_create_interceptor);
}

static void
pthread_listener_log_reclamation_diagnostics(void)
{
    PeakPthreadReclamationDiagnostics diagnostics;

    pthread_listener_get_reclamation_diagnostics(&diagnostics);
    if (diagnostics.scans == 0 && diagnostics.pending == 0) {
        return;
    }
    peak_log_info(
        "[peak] detached thread slot reclamation: scans=%llu "
        "entries_examined=%llu candidates=%llu reclaimed=%llu "
        "deferred_alive=%llu ambiguous=%llu retire_failures=%llu "
        "pending=%lu max_pending=%lu budget=%u interval_ms=%u\n",
        (unsigned long long)diagnostics.scans,
        (unsigned long long)diagnostics.entries_examined,
        (unsigned long long)diagnostics.candidates_checked,
        (unsigned long long)diagnostics.reclaimed,
        (unsigned long long)diagnostics.deferred_alive,
        (unsigned long long)diagnostics.ambiguous_checks,
        (unsigned long long)diagnostics.retire_failures,
        (unsigned long)diagnostics.pending,
        (unsigned long)diagnostics.max_pending,
        diagnostics.scan_budget,
        diagnostics.scan_interval_ms);
}

gboolean pthread_listener_dettach()
{
    if (pthread_create_interceptor == NULL) {
        return TRUE;
    }

    pthread_listener_log_reclamation_diagnostics();

    gum_interceptor_begin_transaction(pthread_create_interceptor);
    if (pthread_create_listener != NULL) {
        gum_interceptor_detach(pthread_create_interceptor, pthread_create_listener);
    }
    if (pthread_join_hook_address != NULL) {
        gum_interceptor_revert(pthread_create_interceptor, pthread_join_hook_address);
    }
    if (pthread_detach_hook_address != NULL) {
        gum_interceptor_revert(pthread_create_interceptor,
                               pthread_detach_hook_address);
    }
    gum_interceptor_end_transaction(pthread_create_interceptor);

    if (!pthread_listener_flush_teardown()) {
        g_printerr("[peak] pthread listener teardown did not flush; leaving pthread listener state alive\n");
        return FALSE;
    }

    /*
     * Do not clear the slot registry here. Threads created
     * while PEAK was active may still run the wrapped start routine cleanup
     * after pthread_create interception has been detached.
     */
    if (pthread_create_listener != NULL) {
        g_object_unref(pthread_create_listener);
    }
    if (pthread_create_interceptor != NULL) {
        g_object_unref(pthread_create_interceptor);
    }

    pthread_create_listener = NULL;
    pthread_create_interceptor = NULL;
    pthread_create_hook_address = NULL;
    pthread_join_hook_address = NULL;
    pthread_detach_hook_address = NULL;

    return TRUE;
}

size_t pthread_listener_lookup_thread(pthread_t thread, gboolean* found)
{
    PeakPthreadSlotToken token;
    gboolean mapped_found = tid_mapping_initialized &&
        peak_pthread_slot_registry_capture(&pthread_slot_registry, thread,
                                            &token);

    if (found != NULL) {
        *found = mapped_found;
    }
    return mapped_found ? token.slot : 0;
}

gboolean
pthread_listener_current_thread_slot(size_t* slot_out)
{
    if (!pthread_slot_identity.valid) {
        return FALSE;
    }
    if (slot_out != NULL) {
        *slot_out = pthread_slot_identity.slot;
    }
    return TRUE;
}

void
pthread_listener_exclude_current_thread(void)
{
    pthread_thread_excluded = TRUE;
    if (!pthread_slot_identity.valid) {
        return;
    }
    peak_general_listener_release_current_thread_state();
    /* A late helper exclusion must not recycle a slot whose accounting may
     * already have been touched; quarantine it instead. */
    pthread_t self = pthread_self();
    if (tid_mapping_initialized) {
        (void)peak_pthread_slot_registry_quarantine(&pthread_slot_registry,
                                                     self);
    }
    pthread_slot_identity.valid = FALSE;
    if (pthread_slot_key_created) {
        (void)pthread_setspecific(pthread_slot_key, NULL);
    }
}

void
pthread_listener_mark_next_created_thread_helper(void)
{
    pthread_next_create_is_helper = TRUE;
}

gboolean
pthread_listener_current_thread_excluded(void)
{
    return pthread_thread_excluded;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_API void
pthread_listener_test_fail_slot_publish(unsigned int count)
{
    atomic_store_explicit(&pthread_test_fail_slot_publish,
                          count,
                          memory_order_relaxed);
}

PEAK_API void
pthread_listener_test_clear_current_thread_slot(void)
{
    pthread_slot_identity.valid = FALSE;
    if (pthread_slot_key_created) {
        (void)pthread_setspecific(pthread_slot_key, NULL);
    }
}

PEAK_API int
pthread_listener_test_thread_is_tracked(pthread_t thread)
{
    return tid_mapping_initialized &&
        peak_pthread_slot_registry_contains(&pthread_slot_registry, thread);
}

PEAK_API int
pthread_listener_test_stale_generation_remove_preserves_mapping(
    pthread_t thread)
{
    PeakPthreadSlotToken token;
    int preserved = 0;

    if (tid_mapping_initialized &&
        peak_pthread_slot_registry_capture(&pthread_slot_registry, thread,
                                            &token)) {
        uint64_t stale_generation = token.generation == 1 ? 0 :
                                                   token.generation - 1;

        /* Exercise the same compare-and-remove path with a generation that
         * cannot denote the current mapping. */
        pthread_listener_remove_thread_if_token_unlocked(thread,
                                                          stale_generation,
                                                          FALSE);
        preserved = peak_pthread_slot_registry_contains(&pthread_slot_registry,
                                                         thread);
    }
    return preserved;
}

PEAK_API int
pthread_listener_test_untracked_create_removes_ambiguous_mapping(
    pthread_t thread)
{
    return tid_mapping_initialized &&
        pthread_listener_quarantine_ambiguous_tid_unlocked(thread);
}

void
pthread_listener_test_pause_start_publication_enable(void)
{
    atomic_store_explicit(&pthread_test_release_start_publication,
                          0,
                          memory_order_release);
    atomic_store_explicit(&pthread_test_pause_start_publication,
                          1,
                          memory_order_release);
}

void
pthread_listener_test_release_start_publication(void)
{
    atomic_store_explicit(&pthread_test_release_start_publication,
                          1,
                          memory_order_release);
}

int
pthread_listener_test_current_thread_has_slot(void)
{
    return pthread_slot_identity.valid ? 1 : 0;
}

PEAK_API int
pthread_listener_test_mark_current_thread_final_destructor(void)
{
    bool became_exit_pending = false;

    if (!tid_mapping_initialized || !pthread_slot_identity.valid) {
        return 0;
    }
    PeakPthreadSlotToken token = {
        .slot = pthread_slot_identity.slot,
        .generation = pthread_slot_identity.generation,
    };
    int marked = peak_pthread_slot_registry_mark_final_destructor(
        &pthread_slot_registry, token, &became_exit_pending);
    pthread_listener_wake_reclaimer_if_pending(became_exit_pending);
    return marked;
}

void
pthread_listener_test_join_detach_race_enable(void)
{
    atomic_store_explicit(&pthread_test_join_detach_race_paused, 0,
                          memory_order_release);
    atomic_store_explicit(&pthread_test_join_detach_race_release, 0,
                          memory_order_release);
    atomic_store_explicit(&pthread_test_join_detach_race_enabled, 1,
                          memory_order_release);
}

unsigned int
pthread_listener_test_join_detach_race_paused(void)
{
    return atomic_load_explicit(&pthread_test_join_detach_race_paused,
                                memory_order_acquire);
}

void
pthread_listener_test_join_detach_race_release(void)
{
    atomic_store_explicit(&pthread_test_join_detach_race_enabled, 0,
                          memory_order_release);
    atomic_store_explicit(&pthread_test_join_detach_race_release, 1,
                          memory_order_release);
}
#endif

size_t pthread_listener_snapshot_threads(pthread_t* tids,
                                         size_t* mapped,
                                         size_t capacity,
                                         gboolean* complete)
{
    bool copied_all = true;
    size_t count;

    if (!tid_mapping_initialized) {
        if (complete != NULL) {
            *complete = TRUE;
        }
        return 0;
    }
    count = peak_pthread_slot_registry_snapshot(&pthread_slot_registry, tids,
                                                mapped, capacity, &copied_all);
    if (complete != NULL) {
        *complete = copied_all;
    }
    return count;
}
