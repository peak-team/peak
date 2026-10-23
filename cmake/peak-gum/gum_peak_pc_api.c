#define _GNU_SOURCE
/*
 * PEAK Frida Gum 17.15.3 devkit overlay.
 *
 * This file is compiled as an extra archive member and is intentionally tied to
 * the 17.15.3 Linux x86_64 and arm64 devkits downloaded by PEAK. It mirrors only the Gum
 * private fields needed for PC classification and fails closed for ambiguous
 * trampoline PCs.
 */

#include <setjmp.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unwind.h>
#include <unistd.h>
#include "frida-gum.h"

#define PEAK_GUM_FAST_DISPATCH_SECTION \
    __attribute__((section("peak_gum_fast_dispatch"), noinline, noclone))

typedef struct {
    GObject parent;
    GumPeakFastTranslateFunc translate;
} PeakGumFastUnwindTranslator;

typedef struct {
    GObjectClass parent;
} PeakGumFastUnwindTranslatorClass;

static void peak_gum_fast_unwind_translator_iface_init(gpointer iface,
                                                       gpointer data);
static void peak_gum_fast_unwind_translator_class_init(
    PeakGumFastUnwindTranslatorClass * klass);
static void peak_gum_fast_unwind_translator_init(
    PeakGumFastUnwindTranslator * self);
G_DEFINE_TYPE_EXTENDED(PeakGumFastUnwindTranslator,
                      peak_gum_fast_unwind_translator,
                      G_TYPE_OBJECT,
                      0,
                      G_IMPLEMENT_INTERFACE(
                          GUM_TYPE_UNWIND_PC_TRANSLATOR,
                          peak_gum_fast_unwind_translator_iface_init))

G_LOCK_DEFINE_STATIC(peak_gum_fast_unwind);
static GumUnwindBroker * peak_gum_fast_unwind_broker;
static PeakGumFastUnwindTranslator * peak_gum_fast_unwind_translator;
static GumInterceptor * peak_gum_fast_unwind_interceptor;
static gpointer peak_gum_fast_unwind_forced_original;
static _Thread_local guint peak_gum_fast_unwind_forced_depth;

typedef _Unwind_Reason_Code (* PeakGumForcedUnwindFunc)(
    struct _Unwind_Exception * exception,
    _Unwind_Stop_Fn stop,
    void * stop_argument);

extern _Unwind_Reason_Code _Unwind_ForcedUnwind(
    struct _Unwind_Exception * exception,
    _Unwind_Stop_Fn stop,
    void * stop_argument) __attribute__((weak));

static _Unwind_Reason_Code
peak_gum_fast_unwind_forced(struct _Unwind_Exception * exception,
                            _Unwind_Stop_Fn stop,
                            void * stop_argument)
{
    PeakGumForcedUnwindFunc original = GUM_POINTER_TO_FUNCPTR(
        PeakGumForcedUnwindFunc, peak_gum_fast_unwind_forced_original);
    _Unwind_Reason_Code result;

    peak_gum_fast_unwind_forced_depth++;
    result = original(exception, stop, stop_argument);
    peak_gum_fast_unwind_forced_depth--;
    return result;
}

static GumAddress
peak_gum_fast_unwind_translate(GumUnwindPcTranslator * translator,
                               GumAddress code_address)
{
    PeakGumFastUnwindTranslator * self =
        (PeakGumFastUnwindTranslator *)translator;

    /* A forced unwind has no handler lookup to preserve. Translating a Gum
     * leave trampoline to its caller makes libgcc apply the caller's CFA
     * rules to the trampoline frame, which can make _Unwind_ForcedUnwind()
     * fail before glibc's stop callback reaches its cancellation boundary.
     * Retain Gum's own trampoline unwind behavior for that path. */
    if (peak_gum_fast_unwind_forced_depth != 0) {
        return 0;
    }
    return GUM_ADDRESS(self->translate(GSIZE_TO_POINTER(code_address)));
}

static void
peak_gum_fast_unwind_translator_iface_init(gpointer iface, gpointer data)
{
    (void)data;
    ((GumUnwindPcTranslatorInterface *)iface)->translate =
        peak_gum_fast_unwind_translate;
}

static void
peak_gum_fast_unwind_translator_class_init(
    PeakGumFastUnwindTranslatorClass * klass)
{
    (void)klass;
}

static void
peak_gum_fast_unwind_translator_init(PeakGumFastUnwindTranslator * self)
{
    self->translate = NULL;
}

static gboolean
peak_gum_fast_unwind_ensure(GumUnwindBroker * broker,
                            GumPeakFastTranslateFunc translate)
{
    gboolean ready;

    if (broker == NULL || translate == NULL) {
        return FALSE;
    }
    G_LOCK(peak_gum_fast_unwind);
    if (peak_gum_fast_unwind_translator == NULL) {
        GumReplaceReturn replace_status;
        PeakGumFastUnwindTranslator * translator = g_object_new(
            peak_gum_fast_unwind_translator_get_type(), NULL);

        if (_Unwind_ForcedUnwind == NULL) {
            g_object_unref(translator);
            G_UNLOCK(peak_gum_fast_unwind);
            return FALSE;
        }
        peak_gum_fast_unwind_interceptor = gum_interceptor_obtain();
        replace_status = gum_interceptor_replace(
            peak_gum_fast_unwind_interceptor,
            GUM_FUNCPTR_TO_POINTER(_Unwind_ForcedUnwind),
            GUM_FUNCPTR_TO_POINTER(peak_gum_fast_unwind_forced),
            &peak_gum_fast_unwind_forced_original,
            NULL);
        if (replace_status != GUM_REPLACE_OK) {
            g_clear_object(&peak_gum_fast_unwind_interceptor);
            g_object_unref(translator);
            G_UNLOCK(peak_gum_fast_unwind);
            return FALSE;
        }
        translator->translate = translate;
        peak_gum_fast_unwind_broker = g_object_ref(broker);
        gum_unwind_broker_add_pc_translator(
            broker, GUM_UNWIND_PC_TRANSLATOR(translator));
        peak_gum_fast_unwind_translator = translator;
    }
    ready = peak_gum_fast_unwind_broker == broker &&
        peak_gum_fast_unwind_translator->translate == translate;
    G_UNLOCK(peak_gum_fast_unwind);
    return ready;
}

static void peak_invoke_api_shutdown(void);
static gboolean peak_invoke_api_ensure(GumInterceptor *interceptor);

static void
peak_gum_fast_unwind_shutdown(void)
{
    peak_invoke_api_shutdown();
    G_LOCK(peak_gum_fast_unwind);
    if (peak_gum_fast_unwind_translator != NULL) {
        /* Removal takes the broker lock, so any in-flight callback completes
         * before either the callback object or its PEAK code is released.
         * The translator owns no target/context/listener reference. */
        gum_unwind_broker_remove_pc_translator(
            peak_gum_fast_unwind_broker,
            GUM_UNWIND_PC_TRANSLATOR(peak_gum_fast_unwind_translator));
        gum_interceptor_revert(
            peak_gum_fast_unwind_interceptor,
            GUM_FUNCPTR_TO_POINTER(_Unwind_ForcedUnwind));
        peak_gum_fast_unwind_forced_original = NULL;
        g_clear_object(&peak_gum_fast_unwind_interceptor);
        g_clear_object(&peak_gum_fast_unwind_translator);
        g_clear_object(&peak_gum_fast_unwind_broker);
    }
    G_UNLOCK(peak_gum_fast_unwind);
}

extern const guint8 __start_peak_gum_fast_dispatch[];
extern const guint8 __stop_peak_gum_fast_dispatch[];

#if !defined(__linux__) || \
    !(defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__))
# error "PEAK Gum PC overlay is only implemented for linux-x86_64 and linux-arm64"
#endif

#if !defined(GUM_PEAK_PC_API_VERSION) || GUM_PEAK_PC_API_VERSION != 1
# error "PEAK Gum PC API declarations were not appended to frida-gum.h"
#endif

#ifndef GUM_MAX_LISTENERS_PER_FUNCTION
# error "Unexpected Frida Gum devkit header: missing listener constants"
#endif

typedef void (* PeakGumSynchronizeModulesFunc)(void);

typedef enum {
    PEAK_GUM_MODULE_SYNC_PASS_THROUGH = 0,
    PEAK_GUM_MODULE_SYNC_STARTING,
    PEAK_GUM_MODULE_SYNC_ACTIVE,
    PEAK_GUM_MODULE_SYNC_QUIESCING,
    PEAK_GUM_MODULE_SYNC_INACTIVE
} PeakGumModuleSyncState;

static PeakGumSynchronizeModulesFunc peak_gum_module_sync;
static guint peak_gum_module_sync_state = PEAK_GUM_MODULE_SYNC_PASS_THROUGH;
static guint peak_gum_module_sync_pending;
static guint peak_gum_module_sync_draining;
static guint peak_gum_module_sync_publishers;
static guint peak_gum_module_sync_active_drains;
static gint peak_gum_module_sync_event_fd = -1;
static pthread_t peak_gum_module_sync_thread;
static guint peak_gum_module_sync_thread_started;
static guint peak_gum_module_sync_atfork_registered;
static guint peak_gum_module_sync_forked_child;
static pthread_mutex_t peak_gum_module_sync_mutation_gate =
    PTHREAD_MUTEX_INITIALIZER;

extern void peak_gum_module_sync_test_quiesce_cleanup_gate(void)
    __attribute__((weak));

G_STATIC_ASSERT(sizeof(PeakGumSynchronizeModulesFunc) == sizeof(gpointer));
/*
 * The overlay is restricted above to 64-bit x86 and Arm, where naturally
 * aligned word-sized loads/stores and 32-bit read-modify-write operations are
 * lock-free.  Spell this as ABI size assertions instead of using
 * __atomic_always_lock_free() in G_STATIC_ASSERT: Intel C 19 implements the
 * __atomic builtins used below, but rejects that predicate in an integer
 * constant-expression context.
 */
G_STATIC_ASSERT(sizeof(gpointer) == 8);
G_STATIC_ASSERT(sizeof(guint) == 4);
G_STATIC_ASSERT(sizeof(gint) == 4);

G_GNUC_INTERNAL void
_gum_module_registry_handle_rtld_notification_peak_original(
    PeakGumSynchronizeModulesFunc sync,
    GumInvocationContext * ic);
G_GNUC_INTERNAL void
_gum_module_registry_activate_peak_original(GumModuleRegistry * registry);
G_GNUC_INTERNAL void
_gum_module_registry_deactivate_peak_original(GumModuleRegistry * registry);
G_GNUC_INTERNAL void gum_deinit_peak_original(void);
G_GNUC_INTERNAL void gum_deinit_embedded_peak_original(void);
G_GNUC_INTERNAL void gum_shutdown_peak_original(void);

#if defined(__aarch64__)
G_GNUC_INTERNAL long peak_aarch64_raw_syscall6_raw(long number,
                                                   long arg1,
                                                   long arg2,
                                                   long arg3,
                                                   long arg4,
                                                   long arg5,
                                                   long arg6);
#endif

static inline __attribute__((always_inline)) void
peak_gum_module_sync_wake(void)
{
    gint fd = __atomic_load_n(&peak_gum_module_sync_event_fd,
                              __ATOMIC_RELAXED);
    uint64_t value = 1;

    if (fd == -1) {
        return;
    }

#if defined(__x86_64__) || defined(__amd64__)
    long result;

    do {
        __asm__ volatile(
            "syscall"
            : "=a"(result)
            : "0"((long)SYS_write),
              "D"((long)fd),
              "S"(&value),
              "d"((long)sizeof(value))
            : "rcx", "r11", "memory");
    } while (result == -(long)EINTR);
    (void)result;
#elif defined(__aarch64__)
    long result;

    do {
        result = peak_aarch64_raw_syscall6_raw((long)SYS_write,
                                               (long)fd,
                                               (long)&value,
                                               (long)sizeof(value),
                                               0,
                                               0,
                                               0);
    } while (result == -(long)EINTR);
    (void)result;
#endif
}

static inline __attribute__((always_inline)) void
peak_gum_module_sync_close_after_fork(gint fd)
{
#if defined(__x86_64__) || defined(__amd64__)
    long result;

    __asm__ volatile(
        "syscall"
        : "=a"(result)
        : "0"((long)SYS_close),
          "D"((long)fd)
        : "rcx", "r11", "memory");
    (void)result;
#elif defined(__aarch64__)
    (void)peak_aarch64_raw_syscall6_raw((long)SYS_close,
                                        (long)fd,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0);
#endif
}

static gboolean
peak_gum_module_sync_try_drain(gboolean allow_quiescing)
{
    PeakGumSynchronizeModulesFunc sync;
    guint expected = 0;
    guint state = __atomic_load_n(&peak_gum_module_sync_state,
                                  __ATOMIC_ACQUIRE);
    gboolean allowed =
        state == PEAK_GUM_MODULE_SYNC_ACTIVE ||
        (allow_quiescing && state == PEAK_GUM_MODULE_SYNC_QUIESCING);
    gboolean did_work = FALSE;

    if (!allowed) {
        return FALSE;
    }

    __atomic_fetch_add(&peak_gum_module_sync_active_drains,
                       1,
                       __ATOMIC_ACQ_REL);
    state = __atomic_load_n(&peak_gum_module_sync_state, __ATOMIC_ACQUIRE);
    allowed =
        state == PEAK_GUM_MODULE_SYNC_ACTIVE ||
        (allow_quiescing && state == PEAK_GUM_MODULE_SYNC_QUIESCING);
    if (!allowed) {
        __atomic_fetch_sub(&peak_gum_module_sync_active_drains,
                           1,
                           __ATOMIC_RELEASE);
        return FALSE;
    }

    if (!__atomic_compare_exchange_n(&peak_gum_module_sync_draining,
                                     &expected,
                                     1,
                                     FALSE,
                                     __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        __atomic_fetch_sub(&peak_gum_module_sync_active_drains,
                           1,
                           __ATOMIC_RELEASE);
        return FALSE;
    }

    if (__atomic_exchange_n(&peak_gum_module_sync_pending,
                            0,
                            __ATOMIC_ACQUIRE) != 0) {
        sync = __atomic_load_n(&peak_gum_module_sync, __ATOMIC_ACQUIRE);
        if (sync != NULL) {
            pthread_mutex_lock(&peak_gum_module_sync_mutation_gate);
            sync();
            pthread_mutex_unlock(&peak_gum_module_sync_mutation_gate);
            did_work = TRUE;
        }
    }

    __atomic_store_n(&peak_gum_module_sync_draining, 0, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&peak_gum_module_sync_active_drains,
                       1,
                       __ATOMIC_RELEASE);
    return did_work;
}

static void*
peak_gum_module_sync_worker(void* data)
{
    GumInterceptor* worker_interceptor = NULL;
    struct pollfd event = {
        .fd = GPOINTER_TO_INT(data),
        .events = POLLIN,
        .revents = 0,
    };

    for (;;) {
        uint64_t value;
        guint state;
        int poll_result;

        if (__atomic_load_n(&peak_gum_module_sync_forked_child,
                            __ATOMIC_ACQUIRE) != 0) {
            break;
        }
        do {
            poll_result = poll(&event, 1, -1);
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result == -1) {
            break;
        }
        while (read(event.fd, &value, sizeof(value)) == -1 &&
               errno == EINTR) {
        }

        state = __atomic_load_n(&peak_gum_module_sync_state,
                                __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&peak_gum_module_sync_forked_child,
                            __ATOMIC_ACQUIRE) != 0 ||
            state == PEAK_GUM_MODULE_SYNC_INACTIVE) {
            break;
        }
        if (state == PEAK_GUM_MODULE_SYNC_STARTING) {
            continue;
        }
        if (worker_interceptor == NULL &&
            (state == PEAK_GUM_MODULE_SYNC_ACTIVE ||
             state == PEAK_GUM_MODULE_SYNC_QUIESCING)) {
            worker_interceptor = gum_interceptor_obtain();
            gum_interceptor_ignore_current_thread(worker_interceptor);
        }

        if (state == PEAK_GUM_MODULE_SYNC_QUIESCING) {
            while (__atomic_load_n(&peak_gum_module_sync_publishers,
                                   __ATOMIC_ACQUIRE) != 0) {
                sched_yield();
            }
            while (peak_gum_module_sync_try_drain(TRUE)) {
            }
            break;
        }

        while (peak_gum_module_sync_try_drain(FALSE)) {
        }
    }

    if (worker_interceptor != NULL &&
        __atomic_load_n(&peak_gum_module_sync_forked_child,
                        __ATOMIC_ACQUIRE) == 0) {
        gum_interceptor_unignore_current_thread(worker_interceptor);
    }
    return NULL;
}

static void
peak_gum_module_sync_quiesce(void)
{
    guint state;

    for (;;) {
        state = __atomic_load_n(&peak_gum_module_sync_state,
                                __ATOMIC_ACQUIRE);
        if (state == PEAK_GUM_MODULE_SYNC_QUIESCING) {
            while (__atomic_load_n(&peak_gum_module_sync_state,
                                   __ATOMIC_ACQUIRE) ==
                   PEAK_GUM_MODULE_SYNC_QUIESCING) {
                sched_yield();
            }
            return;
        }
        /*
         * INACTIVE is the completion publication. The owner clears
         * thread_started immediately after join, but still has drain and fd
         * cleanup to perform before callers may continue into Gum teardown.
         */
        if (__atomic_load_n(&peak_gum_module_sync_thread_started,
                            __ATOMIC_ACQUIRE) == 0) {
            return;
        }
        if (state != PEAK_GUM_MODULE_SYNC_STARTING &&
            state != PEAK_GUM_MODULE_SYNC_ACTIVE) {
            return;
        }
        if (__atomic_compare_exchange_n(&peak_gum_module_sync_state,
                                        &state,
                                        PEAK_GUM_MODULE_SYNC_QUIESCING,
                                        FALSE,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    peak_gum_module_sync_wake();
    pthread_join(peak_gum_module_sync_thread, NULL);

    if (peak_gum_module_sync_test_quiesce_cleanup_gate != NULL) {
        peak_gum_module_sync_test_quiesce_cleanup_gate();
    }
    while (__atomic_load_n(&peak_gum_module_sync_active_drains,
                           __ATOMIC_ACQUIRE) != 0) {
        sched_yield();
    }
    __atomic_store_n(&peak_gum_module_sync_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&peak_gum_module_sync, NULL, __ATOMIC_RELEASE);

    gint event_fd =
        __atomic_exchange_n(&peak_gum_module_sync_event_fd,
                            -1,
                            __ATOMIC_ACQ_REL);
    if (event_fd != -1) {
        close(event_fd);
    }
    __atomic_store_n(&peak_gum_module_sync_thread_started,
                     0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&peak_gum_module_sync_state,
                     PEAK_GUM_MODULE_SYNC_INACTIVE,
                     __ATOMIC_RELEASE);
}

void
gum_interceptor_peak_quiesce_deferred_module_sync(void)
{
    peak_gum_module_sync_quiesce();
}

void
gum_interceptor_peak_begin_module_mutation(void)
{
    pthread_mutex_lock(&peak_gum_module_sync_mutation_gate);
}

void
gum_interceptor_peak_end_module_mutation(void)
{
    pthread_mutex_unlock(&peak_gum_module_sync_mutation_gate);
}

static void
peak_gum_module_sync_atfork_child(void)
{
    gint event_fd;

    /*
     * Never wait in an atfork prepare handler: fork may run from a DSO
     * constructor while its thread owns the loader lock, and the sync worker
     * may be waiting for that same lock.  The child cannot safely use or tear
     * down Gum because any of its private locks may have been owned by a
     * vanished thread at the snapshot.  Fail closed and let process exit
     * reclaim the inherited mappings.
     */
    __atomic_store_n(&peak_gum_module_sync_forked_child,
                     1,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&peak_gum_module_sync_state,
                     PEAK_GUM_MODULE_SYNC_INACTIVE,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&peak_gum_module_sync_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&peak_gum_module_sync, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&peak_gum_module_sync_thread_started,
                     0,
                     __ATOMIC_RELEASE);

    event_fd = __atomic_exchange_n(&peak_gum_module_sync_event_fd,
                                   -1,
                                   __ATOMIC_ACQ_REL);
    if (event_fd != -1) {
        peak_gum_module_sync_close_after_fork(event_fd);
    }
}

/*
 * Gum 17.15.3 normally synchronizes its module registry directly from the
 * _dl_debug_state interception callback. On glibc that executes while loader
 * link_map nodes are being mutated. Besides allocating and taking Gum locks,
 * the upstream implementation uses a process-global "syncing from rtld" flag,
 * so an unrelated thread may walk an unstable link_map and dereference a stale
 * l_name. In deferred mode this callback must remain leaf-like: publish the
 * callback before the pending bit and return.
 */
G_GNUC_INTERNAL void
_gum_module_registry_handle_rtld_notification(
    PeakGumSynchronizeModulesFunc sync,
    GumInvocationContext * ic)
{
    guint state = __atomic_load_n(&peak_gum_module_sync_state,
                                  __ATOMIC_ACQUIRE);

    if (state == PEAK_GUM_MODULE_SYNC_PASS_THROUGH) {
        _gum_module_registry_handle_rtld_notification_peak_original(sync, ic);
        return;
    }
    if (state != PEAK_GUM_MODULE_SYNC_STARTING &&
        state != PEAK_GUM_MODULE_SYNC_ACTIVE) {
        return;
    }

    (void)ic;
    __atomic_fetch_add(&peak_gum_module_sync_publishers,
                       1,
                       __ATOMIC_ACQ_REL);
    state = __atomic_load_n(&peak_gum_module_sync_state, __ATOMIC_ACQUIRE);
    if (state == PEAK_GUM_MODULE_SYNC_STARTING ||
        state == PEAK_GUM_MODULE_SYNC_ACTIVE) {
        __atomic_store_n(&peak_gum_module_sync, sync, __ATOMIC_RELEASE);
        __atomic_store_n(&peak_gum_module_sync_pending, 1, __ATOMIC_RELEASE);
        peak_gum_module_sync_wake();
    }
    __atomic_fetch_sub(&peak_gum_module_sync_publishers,
                       1,
                       __ATOMIC_RELEASE);
}

gboolean
gum_interceptor_peak_drain_deferred_module_sync(void)
{
    return peak_gum_module_sync_try_drain(FALSE);
}

gboolean
gum_interceptor_peak_deferred_module_sync_available(void)
{
    return __atomic_load_n(&peak_gum_module_sync_state, __ATOMIC_ACQUIRE) ==
        PEAK_GUM_MODULE_SYNC_ACTIVE;
}

G_GNUC_INTERNAL __attribute__((no_sanitize_address)) void
_gum_module_registry_activate(GumModuleRegistry * registry)
{
    gint event_fd = -1;
    gboolean worker_unavailable = FALSE;

    if (__atomic_load_n(&peak_gum_module_sync_atfork_registered,
                        __ATOMIC_ACQUIRE) == 0) {
        if (pthread_atfork(NULL,
                           NULL,
                           peak_gum_module_sync_atfork_child) == 0) {
            __atomic_store_n(&peak_gum_module_sync_atfork_registered,
                             1,
                             __ATOMIC_RELEASE);
        } else {
            worker_unavailable = TRUE;
        }
    }
    if (!worker_unavailable) {
        event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }

    __atomic_store_n(&peak_gum_module_sync, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&peak_gum_module_sync_pending, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&peak_gum_module_sync_draining, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&peak_gum_module_sync_publishers, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&peak_gum_module_sync_active_drains, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&peak_gum_module_sync_thread_started,
                     0,
                     __ATOMIC_RELAXED);

    if (event_fd != -1) {
        __atomic_store_n(&peak_gum_module_sync_event_fd,
                         event_fd,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&peak_gum_module_sync_state,
                         PEAK_GUM_MODULE_SYNC_STARTING,
                         __ATOMIC_RELEASE);
        if (pthread_create(&peak_gum_module_sync_thread,
                           NULL,
                           peak_gum_module_sync_worker,
                           GINT_TO_POINTER(event_fd)) == 0) {
            __atomic_store_n(&peak_gum_module_sync_thread_started,
                             1,
                             __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&peak_gum_module_sync_state,
                             PEAK_GUM_MODULE_SYNC_INACTIVE,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&peak_gum_module_sync_event_fd,
                             -1,
                             __ATOMIC_RELEASE);
            close(event_fd);
            worker_unavailable = TRUE;
        }
    } else {
        __atomic_store_n(&peak_gum_module_sync_state,
                         PEAK_GUM_MODULE_SYNC_INACTIVE,
                         __ATOMIC_RELEASE);
        worker_unavailable = TRUE;
    }

    _gum_module_registry_activate_peak_original(registry);

    if (__atomic_load_n(&peak_gum_module_sync_thread_started,
                        __ATOMIC_ACQUIRE) != 0) {
        __atomic_store_n(&peak_gum_module_sync_state,
                         PEAK_GUM_MODULE_SYNC_ACTIVE,
                         __ATOMIC_RELEASE);
        peak_gum_module_sync_wake();
    } else if (worker_unavailable) {
        g_warning("PEAK deferred Gum module-sync worker unavailable; "
                  "dynamic module-registry updates are disabled");
    }
}

G_GNUC_INTERNAL void
_gum_module_registry_deactivate(GumModuleRegistry * registry)
{
    peak_gum_module_sync_quiesce();

    _gum_module_registry_deactivate_peak_original(registry);
    __atomic_store_n(&peak_gum_module_sync_state,
                     PEAK_GUM_MODULE_SYNC_INACTIVE,
                     __ATOMIC_RELEASE);
}

void
gum_deinit(void)
{
    if (__atomic_load_n(&peak_gum_module_sync_forked_child,
                        __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    peak_gum_module_sync_quiesce();
    peak_gum_fast_unwind_shutdown();
    gum_deinit_peak_original();
}

void
gum_deinit_embedded(void)
{
    if (__atomic_load_n(&peak_gum_module_sync_forked_child,
                        __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    peak_gum_module_sync_quiesce();
    peak_gum_fast_unwind_shutdown();
    gum_deinit_embedded_peak_original();
}

void
gum_shutdown(void)
{
    if (__atomic_load_n(&peak_gum_module_sync_forked_child,
                        __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    peak_gum_module_sync_quiesce();
    peak_gum_fast_unwind_shutdown();
    gum_shutdown_peak_original();
}

typedef guint8 PeakGumInterceptorType17;

typedef struct _PeakGumInterceptorBackend17 PeakGumInterceptorBackend17;
typedef struct _PeakGumFunctionContext17 PeakGumFunctionContext17;
typedef union _PeakGumFunctionContextBackendData17 PeakGumFunctionContextBackendData17;
#if defined(__linux__) && defined(__aarch64__)
/* The pinned relocator starts its CHECKED walk after the relocated prefix.
 * Include alternate paths reached by branches inside that prefix before
 * allowing the backend's 8- or 16-byte entry patch. Keep Gum's other policies
 * and patch-size decisions unchanged. */
/* The public devkit amalgamation omits this exported relocator API. */
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint *, arm64_reg *);

gboolean
peak_gum_arm64_can_relocate(gpointer address, guint min_bytes,
                            GumRelocationScenario scenario,
                            GumRelocationPolicy policy, guint * maximum,
                            arm64_reg * scratch)
{
    guint original_maximum = 0, candidate_span;
    gboolean result = gum_arm64_relocator_can_relocate(address, min_bytes,
        scenario, policy, &original_maximum, scratch);
    uintptr_t entry = (uintptr_t)address;
    uintptr_t pending[4096];
    guint head = 0, tail = 0, examined = 0;
    GHashTable * visited = NULL;
    csh capstone = 0;
    cs_insn * insn = NULL;
    guint8 * cache = NULL;
    uintptr_t cache_base = 0;
    gsize cache_size = 0;
    gboolean safe = TRUE;

    if (maximum != NULL) *maximum = original_maximum;
    if (min_bytes != 16 || scenario != GUM_SCENARIO_ONLINE ||
        policy != GUM_RELOCATION_CHECKED)
        return result;
    candidate_span = result ? 16 : original_maximum >= 8 ? 8 :
        original_maximum >= 4 ? 4 : 0;
    /* A single aligned instruction has no interior instruction entry. */
    if (candidate_span <= 4) return result;
    if (entry > UINTPTR_MAX - candidate_span) {
        if (maximum != NULL) *maximum = 0;
        return FALSE;
    }
    /* Gum already checks the ordinary continuation of a successful 16-byte
     * relocation. An 8-byte fallback may have stopped at an ONLINE call or
     * terminal branch, so it must also check the reachable continuation. */
    if (candidate_span == 16) {
        guint8 * prefix;
        gsize size = 0;
        gboolean has_branch = FALSE;
        prefix = gum_memory_read(address, 16, &size);
        if (prefix == NULL || size != 16) {
            g_free(prefix);
            if (maximum != NULL) *maximum = 0;
            return FALSE;
        }
        for (guint offset = 0; offset < 16; offset += 4) {
            guint32 word;
            memcpy(&word, prefix + offset, sizeof(word));
            if ((word & 0xfc000000u) == 0x14000000u ||
                (word & 0xff000000u) == 0x54000000u ||
                (word & 0x7e000000u) == 0x34000000u ||
                (word & 0x7e000000u) == 0x36000000u)
                has_branch = TRUE;
        }
        g_free(prefix);
        if (!has_branch) return result;
    }
    if (cs_open(CS_ARCH_ARM64, GUM_DEFAULT_CS_ENDIAN, &capstone) != CS_ERR_OK) {
        if (maximum != NULL) *maximum = 0;
        return FALSE;
    }
    if (cs_option(capstone, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
        safe = FALSE;
        goto done;
    }
    insn = cs_malloc(capstone);
    visited = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (insn == NULL || visited == NULL) {
        safe = FALSE;
        goto done;
    }

    /* Seed only branch targets missed by the original post-prefix walk. */
    for (guint offset = 0; offset < candidate_span; offset += 4)
        pending[tail++] = entry + offset;
    while (head < tail) {
        uintptr_t pc = pending[head++];
        const guint8 * code;
        size_t available = 4;
        uint64_t decoded_pc = pc;
        cs_arm64 * operands;
        gint target_operand = -1;
        gboolean fall_through = TRUE;
        gboolean prefix = pc >= entry && pc < entry + candidate_span;

        if (g_hash_table_contains(visited, (gpointer)pc)) continue;
        if (++examined > 4096 || (pc & 3) != 0) {
            safe = FALSE;
            break;
        }
        g_hash_table_add(visited, (gpointer)pc);
        if (cache == NULL || pc < cache_base || pc - cache_base + 4 > cache_size) {
            g_free(cache);
            cache_base = pc & ~((uintptr_t)1023);
            cache = gum_memory_read((gpointer)cache_base, 1024, &cache_size);
            if (cache == NULL || pc - cache_base + 4 > cache_size) {
                safe = FALSE;
                break;
            }
        }
        code = cache + (pc - cache_base);
        if (!cs_disasm_iter(capstone, &code, &available, &decoded_pc, insn)) {
            safe = FALSE;
            break;
        }
        operands = &insn->detail->arm64;
        switch (insn->id) {
        case ARM64_INS_BC:
            /* The pinned relocator does not relocate FEAT_HBC BC.cond and
             * Capstone may omit its generic jump group. Do not treat this
             * unproven control transfer as an ordinary instruction. */
            safe = FALSE;
            break;
        case ARM64_INS_B:
            target_operand = 0;
            fall_through = operands->cc != ARM64_CC_INVALID &&
                operands->cc != ARM64_CC_AL && operands->cc != ARM64_CC_NV;
            break;
        case ARM64_INS_CBZ: case ARM64_INS_CBNZ:
            target_operand = 1;
            break;
        case ARM64_INS_TBZ: case ARM64_INS_TBNZ:
            target_operand = 2;
            break;
        case ARM64_INS_RET: case ARM64_INS_RETAA: case ARM64_INS_RETAB:
            fall_through = FALSE;
            break;
        case ARM64_INS_BR: case ARM64_INS_BRAA: case ARM64_INS_BRAAZ:
        case ARM64_INS_BRAB: case ARM64_INS_BRABZ:
        case ARM64_INS_ERET: case ARM64_INS_ERETAA: case ARM64_INS_ERETAB:
        case ARM64_INS_DRPS:
            /* An indirect alternate-path jump could land inside the patch;
             * unlike a return, its destination has not been established. */
            safe = FALSE;
            break;
        case ARM64_INS_BL: case ARM64_INS_BLR:
        case ARM64_INS_BLRAA: case ARM64_INS_BLRAAZ:
        case ARM64_INS_BLRAB: case ARM64_INS_BLRABZ:
            /* Calls return to this path's next instruction, outside the
             * overwritten entry. Callee control flow follows Gum's normal
             * call/return assumption; it is not treated as a tail jump. */
            break;
        default:
            if (cs_insn_group(capstone, insn, CS_GRP_JUMP) ||
                cs_insn_group(capstone, insn, CS_GRP_IRET) ||
                cs_insn_group(capstone, insn, CS_GRP_RET))
                safe = FALSE;
            break;
        }
        if (!safe) break;
        if (target_operand >= 0) {
            uintptr_t destination;
            if (operands->operands[target_operand].type != ARM64_OP_IMM) {
                safe = FALSE;
                break;
            }
            destination = (uintptr_t)operands->operands[target_operand].imm;
            if (destination > entry && destination < entry + candidate_span) {
                safe = FALSE;
                break;
            }
            /* Returning to the entry is valid: the normal hook handles it.
             * Prefix instructions themselves have already been examined. */
            if (destination < entry || destination >= entry + candidate_span) {
                if (tail == G_N_ELEMENTS(pending)) { safe = FALSE; break; }
                pending[tail++] = destination;
            }
        }
        if ((!prefix || (candidate_span == 8 &&
                pc == entry + candidate_span - 4)) && fall_through) {
            if (pc > UINTPTR_MAX - 4 || tail == G_N_ELEMENTS(pending)) {
                safe = FALSE;
                break;
            }
            pending[tail++] = pc + 4;
        }
    }
done:
    g_free(cache);
    if (visited != NULL) g_hash_table_unref(visited);
    if (insn != NULL) cs_free(insn, 1);
    cs_close(&capstone);
    if (!safe && maximum != NULL) *maximum = 0;
    return safe ? result : FALSE;
}
#endif

#if defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__)
typedef struct _GumInterceptorBackend GumInterceptorBackend;

G_GNUC_INTERNAL gpointer
_gum_interceptor_backend_resolve_redirect(
    GumInterceptorBackend * backend,
    gpointer address);

static _Thread_local gpointer peak_gum_exact_attach_target;

/*
 * The devkit patch routes guminterceptor.c's one backend-resolver reference
 * through this dispatch.  Gum's backend implementation remains untouched.
 * Only the thread performing peak_attach_exact suppresses the first redirect
 * at the requested entry; this is attach-time only and adds no profiled-call
 * cost.
 */
G_GNUC_INTERNAL gpointer
_gum_interceptor_backend_resolve_redirect_peak_dispatch(
    GumInterceptorBackend * backend,
    gpointer address)
{
    if (address == peak_gum_exact_attach_target) {
        return NULL;
    }

    return _gum_interceptor_backend_resolve_redirect(backend, address);
}

GumAttachReturn
gum_interceptor_peak_attach_exact(GumInterceptor * interceptor,
                                  gpointer function_address,
                                  GumInvocationListener * listener,
                                  const GumAttachOptions * options)
{
    gpointer previous_target = peak_gum_exact_attach_target;

    peak_gum_exact_attach_target = function_address;
    GumAttachReturn result = gum_interceptor_attach(interceptor,
                                                    function_address,
                                                    listener,
                                                    options);
    peak_gum_exact_attach_target = previous_target;
    return result;
}
#endif

#if defined(__x86_64__) || defined(__amd64__)
#define PEAK_GUM_PC_ABI_FINGERPRINT \
    GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64
typedef struct _PeakGumX86Relocator17 PeakGumX86Relocator17;

struct _PeakGumX86Relocator17 {
    volatile gint ref_count;
    csh capstone;
    const guint8 * input_start;
    const guint8 * input_cur;
    GumAddress input_pc;
    cs_insn ** input_insns;
    GumX86Writer * output;
    guint inpos;
    guint outpos;
    gboolean eob;
    gboolean eoi;
};

struct _PeakGumInterceptorBackend17 {
    GumCodeAllocator * allocator;
    GumX86Writer writer;
    PeakGumX86Relocator17 relocator;
    GumCodeSlice * enter_thunk;
    GumCodeSlice * leave_thunk;
};
#elif defined(__aarch64__)
#define PEAK_GUM_PC_ABI_FINGERPRINT \
    GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64
typedef struct _PeakGumArm64Relocator17 PeakGumArm64Relocator17;
typedef struct _PeakGumArm64ThunkSet17 PeakGumArm64ThunkSet17;
typedef struct _PeakGumArm64FunctionContextData17 PeakGumArm64FunctionContextData17;

struct _PeakGumArm64Relocator17 {
    volatile gint ref_count;
    csh capstone;
    const guint8 * input_start;
    const guint8 * input_cur;
    GumAddress input_pc;
    cs_insn ** input_insns;
    GumArm64Writer * output;
    guint inpos;
    guint outpos;
    gboolean eob;
    gboolean eoi;
};

struct _PeakGumInterceptorBackend17 {
    GRecMutex * mutex;
    GumCodeAllocator * allocator;
    GumArm64Writer writer;
    PeakGumArm64Relocator17 relocator;
    GHashTable * thunks_by_scratch_reg;
};

struct _PeakGumArm64ThunkSet17 {
    gpointer page;
    gpointer enter_thunk;
    gpointer leave_thunk;
};

struct _PeakGumArm64FunctionContextData17 {
    guint redirect_code_size;
    gint scratch_reg;
    guint available_space;
};
#else
# error "Unsupported PEAK Gum PC overlay architecture"
#endif

typedef struct _PeakGumInterceptorTransaction17 {
    gboolean is_dirty;
    gint level;
    GQueue * pending_destroy_tasks;
    GHashTable * pending_update_tasks;
    GumInterceptor * interceptor;
} PeakGumInterceptorTransaction17;

typedef struct _PeakGumInterceptor17 {
    GObject parent;
    GRecMutex mutex;
    GHashTable * function_by_address;
    PeakGumInterceptorBackend17 * backend;
    GumCodeAllocator allocator;
    GumInterceptorOptions options;
    volatile guint selected_thread_id;
    PeakGumInterceptorTransaction17 current_transaction;
    gpointer unwind_broker;
} PeakGumInterceptor17;

#if defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__)
gboolean
gum_interceptor_peak_resolve_redirect_chain(
    GumInterceptor * interceptor,
    gpointer function_address,
    gpointer * resolved_address)
{
    PeakGumInterceptor17 * private_interceptor;
    gpointer visited[64];
    gpointer current;
    guint depth;

    if (interceptor == NULL || function_address == NULL ||
        resolved_address == NULL) {
        return FALSE;
    }

    private_interceptor = (PeakGumInterceptor17 *)interceptor;
    if (private_interceptor->backend == NULL) {
        return FALSE;
    }

    current = function_address;
    for (depth = 0; depth != G_N_ELEMENTS(visited); ++depth) {
        gpointer next;
        guint previous;

        visited[depth] = current;
        next = _gum_interceptor_backend_resolve_redirect(
            (GumInterceptorBackend *)private_interceptor->backend,
            current);
        if (next == NULL) {
            *resolved_address = current;
            return TRUE;
        }
        for (previous = 0; previous <= depth; ++previous) {
            if (visited[previous] == next) {
                return FALSE;
            }
        }
        current = next;
    }

    return FALSE;
}
#endif

union _PeakGumFunctionContextBackendData17 {
    gchar storage[3 * GLIB_SIZEOF_VOID_P];
    gpointer p[3];
};

struct _PeakGumFunctionContext17 {
    gpointer function_address;
    gpointer grafted_hook;
    gpointer import_target;
    PeakGumInterceptorType17 type;
    guint8 destroyed;
    guint8 activated;
    guint8 has_on_leave_listener;
    guint8 has_unignorable_listener;
    GumCodeSlice * trampoline_slice;
    GumCodeDeflector * trampoline_deflector;
    volatile gint trampoline_usage_counter;
    gpointer on_enter_trampoline;
    guint8 * overwritten_prologue;
    guint overwritten_prologue_len;
    guint8 * redirect_code;
    gpointer on_invoke_trampoline;
    gpointer on_leave_trampoline;
    volatile GPtrArray * listener_entries;
    gpointer replacement_function;
    gpointer replacement_data;
    gint scratch_register;
    GumInterceptorScenario scenario;
    GumRelocationPolicy relocation_policy;
    GumWriteRedirectFunc write_redirect;
    gpointer write_redirect_data;
    guint redirect_space_hint;
    PeakGumFunctionContextBackendData17 backend_data;
    GumInterceptor * interceptor;
};

typedef GArray PeakGumInvocationStack17;

typedef struct _PeakGumInvocationStackEntry17 {
    PeakGumFunctionContext17 * function_ctx;
    gpointer caller_ret_addr;
    gpointer stack_address;
    GumInvocationContext invocation_context;
    GumCpuContext cpu_context;
    guint8 listener_invocation_data[GUM_MAX_LISTENERS_PER_FUNCTION]
        [GUM_MAX_LISTENER_DATA];
    gboolean calling_replacement;
    gboolean only_invoke_unignorable_listeners;
    gint original_system_error;
} PeakGumInvocationStackEntry17;

typedef struct _PeakGumInterceptorThreadContext17 {
    GumInvocationBackend listener_backend;
    GumInvocationBackend replacement_backend;
    gint ignore_level;
    PeakGumInvocationStack17 * stack;
    GArray * listener_data_slots;
} PeakGumInterceptorThreadContext17;

typedef struct _PeakGumListenerEntry17 {
#ifndef GUM_DIET
    GumInvocationListenerInterface * listener_interface;
    GumInvocationListener * listener_instance;
#else
    union {
        GumInvocationListener * listener_interface;
        GumInvocationListener * listener_instance;
    };
#endif
    gpointer function_data;
    gboolean unignorable;
} PeakGumListenerEntry17;

G_GNUC_INTERNAL gboolean
_gum_function_context_begin_invocation_peak_original(
    PeakGumFunctionContext17 * function_ctx,
    GumCpuContext * cpu_context,
    gpointer * caller_ret_addr,
    gpointer * next_hop);

G_GNUC_INTERNAL void
_gum_function_context_end_invocation_peak_original(
    PeakGumFunctionContext17 * function_ctx,
    GumCpuContext * cpu_context,
    gpointer * next_hop);

G_GNUC_INTERNAL PeakGumInterceptorThreadContext17 *
peak_gum_get_interceptor_thread_context(void);

G_GNUC_INTERNAL extern GumSpinlock peak_gum_interceptor_thread_context_lock;
G_GNUC_INTERNAL extern GHashTable *peak_gum_interceptor_thread_contexts;


G_STATIC_ASSERT(sizeof(PeakGumFunctionContextBackendData17) == 3 * GLIB_SIZEOF_VOID_P);
#if defined(__x86_64__) || defined(__amd64__)
G_STATIC_ASSERT(sizeof(PeakGumX86Relocator17) >= 8 * GLIB_SIZEOF_VOID_P);
#elif defined(__aarch64__)
G_STATIC_ASSERT(sizeof(PeakGumArm64Relocator17) >= 8 * GLIB_SIZEOF_VOID_P);
G_STATIC_ASSERT(sizeof(PeakGumArm64FunctionContextData17) <=
                sizeof(PeakGumFunctionContextBackendData17));
#endif

static _Thread_local PeakGumInvocationStack17 *
    peak_gum_cached_invocation_stack
        __attribute__((tls_model("initial-exec")));

static gboolean
peak_gum_pointer_in_range(gpointer pointer, gpointer start, gsize size)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)start;
    uintptr_t end;

    if (start == NULL || size == 0) {
        return FALSE;
    }

    end = begin + (uintptr_t)size;
    return value >= begin && value < end && end >= begin;
}

static PeakGumInvocationStack17 * PEAK_GUM_FAST_DISPATCH_SECTION
peak_gum_invocation_stack(void)
{
    PeakGumInvocationStack17 * stack = peak_gum_cached_invocation_stack;

    if (G_UNLIKELY(stack == NULL)) {
        PeakGumInterceptorThreadContext17 * thread_context =
            peak_gum_get_interceptor_thread_context();

        if (thread_context != NULL) {
            stack = thread_context->stack;
            peak_gum_cached_invocation_stack = stack;
        }
    }
    return stack;
}

static guint PEAK_GUM_FAST_DISPATCH_SECTION
peak_gum_invocation_stack_depth(void)
{
    PeakGumInvocationStack17 * stack = peak_gum_invocation_stack();

    if (stack == NULL) {
        return 0;
    }
    return stack->len;
}

static void PEAK_GUM_FAST_DISPATCH_SECTION
peak_gum_invocation_stack_reap_to_depth(guint target_depth)
{
    PeakGumInvocationStack17 * stack = peak_gum_invocation_stack();

    if (stack == NULL) {
        return;
    }
    if (target_depth > stack->len) {
        return;
    }

    while (stack->len > target_depth) {
        PeakGumInvocationStackEntry17 * entry =
            &g_array_index(stack,
                           PeakGumInvocationStackEntry17,
                           stack->len - 1);

        g_atomic_int_dec_and_test(
            &entry->function_ctx->trampoline_usage_counter);
        g_array_set_size(stack, stack->len - 1);
    }
}

static void PEAK_GUM_FAST_DISPATCH_SECTION
peak_gum_invocation_stack_reap_unwound(gpointer live_stack_address)
{
    PeakGumInvocationStack17 * stack = peak_gum_invocation_stack();

    if (stack == NULL) {
        return;
    }
    while (stack->len > 0) {
        PeakGumInvocationStackEntry17 * entry =
            &g_array_index(stack,
                           PeakGumInvocationStackEntry17,
                           stack->len - 1);

        if ((guint8 *)entry->stack_address > (guint8 *)live_stack_address ||
            (entry->stack_address == live_stack_address &&
             gum_interceptor_peak_unwind_frame_is_live(entry->function_ctx,
                                                       entry->stack_address))) {
            break;
        }
        g_atomic_int_dec_and_test(
            &entry->function_ctx->trampoline_usage_counter);
        g_array_set_size(stack, stack->len - 1);
    }
}

gboolean PEAK_GUM_FAST_DISPATCH_SECTION
gum_interceptor_peak_invocation_stack_entry_matches(
    guint depth,
    gpointer function_address,
    gpointer stack_address)
{
    PeakGumInvocationStack17 * stack = peak_gum_invocation_stack();
    PeakGumInvocationStackEntry17 * entry;

    if (stack == NULL || depth >= stack->len) {
        return FALSE;
    }
    entry = &g_array_index(stack, PeakGumInvocationStackEntry17, depth);
    return entry->invocation_context.function == function_address &&
           entry->stack_address == stack_address;
}

static gboolean
peak_gum_pointer_between_labels(gpointer pointer, gpointer start, gpointer end)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)start;
    uintptr_t finish = (uintptr_t)end;

    if (start == NULL || end == NULL || finish <= begin) {
        return FALSE;
    }

    return value >= begin && value < finish;
}

static gboolean
peak_gum_context_has_listener(PeakGumFunctionContext17 * context,
                              GumInvocationListener * listener)
{
    GPtrArray * entries;
    guint i;

    if (listener == NULL) {
        return TRUE;
    }

    entries = (GPtrArray *)g_atomic_pointer_get(&context->listener_entries);
    if (entries == NULL) {
        return FALSE;
    }

    for (i = 0; i < entries->len; i++) {
        PeakGumListenerEntry17 * entry = g_ptr_array_index(entries, i);
        if (entry != NULL && entry->listener_instance == listener) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_gum_context_has_only_listener(PeakGumFunctionContext17 * context,
                                   GumInvocationListener * listener)
{
    GPtrArray * entries;
    guint match_count = 0;
    guint active_count = 0;

    entries = (GPtrArray *)g_atomic_pointer_get(&context->listener_entries);
    if (entries == NULL) {
        return FALSE;
    }

    for (guint i = 0; i < entries->len; i++) {
        PeakGumListenerEntry17 * entry = g_ptr_array_index(entries, i);
        if (entry == NULL) {
            continue;
        }
        active_count++;
        if (entry->listener_instance == listener) {
            match_count++;
        }
    }

    return active_count == 1 && match_count == 1;
}

static GumPeakFastListener * PEAK_GUM_FAST_DISPATCH_SECTION
peak_gum_context_fast_listener(PeakGumFunctionContext17 * context)
{
    GumPeakFastListener * fast_listener;

    if (context == NULL || context->write_redirect != NULL) {
        return NULL;
    }

    fast_listener = (GumPeakFastListener *)g_atomic_pointer_get(
        &context->write_redirect_data);
    if (fast_listener == NULL ||
        fast_listener->version != GUM_PEAK_FAST_LISTENER_VERSION ||
        fast_listener->on_enter == NULL ||
        fast_listener->on_leave == NULL ||
        fast_listener->is_direct_leave == NULL ||
        fast_listener->active_count == NULL) {
        return NULL;
    }

    return fast_listener;
}

static GumPeakFastListener * PEAK_GUM_FAST_DISPATCH_SECTION
peak_gum_context_fast_listener_for_enter(
    PeakGumFunctionContext17 * context)
{
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener(context);

    if (fast_listener == NULL ||
        g_atomic_int_get(&fast_listener->enabled) == 0) {
        return NULL;
    }
    if (g_atomic_pointer_get(&context->replacement_function) != NULL) {
        g_atomic_int_set(&fast_listener->enabled, 0);
        return NULL;
    }
    /*
     * Gum publishes listener-list changes by replacing the GPtrArray pointer.
     * Comparing the immutable cookie avoids traversing an array that a
     * concurrent attach transaction may already have retired.  It also keeps
     * the steady path to one atomic pointer load.
     */
    if (g_atomic_pointer_get(&context->listener_entries) !=
        g_atomic_pointer_get(&fast_listener->listener_entries_cookie)) {
        /*
         * A later listener attachment invalidates direct dispatch.  Existing
         * direct frames still use the descriptor on leave, but all subsequent
         * entries fall back to Gum's complete listener fan-out.
        */
        g_atomic_int_set(&fast_listener->enabled, 0);
        return NULL;
    }

    return fast_listener;
}

static gboolean
peak_gum_context_is_usable(PeakGumFunctionContext17 * context)
{
    return context != NULL && !context->destroyed && context->activated;
}

static PeakGumFunctionContext17 *
peak_gum_find_context_by_listener(PeakGumInterceptor17 * interceptor,
                                  GumInvocationListener * listener)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    PeakGumFunctionContext17 * match = NULL;

    if (interceptor == NULL ||
        interceptor->function_by_address == NULL ||
        listener == NULL) {
        return NULL;
    }

    g_hash_table_iter_init(&iter, interceptor->function_by_address);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PeakGumFunctionContext17 * context =
            (PeakGumFunctionContext17 *)value;

        (void)key;
        if (peak_gum_context_is_usable(context) &&
            peak_gum_context_has_listener(context, listener)) {
            if (match != NULL) {
                return NULL;
            }
            match = context;
        }
    }

    return match;
}

static PeakGumFunctionContext17 *
peak_gum_find_configurable_context(PeakGumInterceptor17 * interceptor,
                                   gpointer function_address,
                                   GumInvocationListener * listener)
{
    PeakGumFunctionContext17 * context = NULL;
    PeakGumFunctionContext17 * match = NULL;
    GHashTableIter iter;
    gpointer value;

    if (interceptor == NULL || interceptor->function_by_address == NULL ||
        listener == NULL) {
        return NULL;
    }

    if (function_address != NULL) {
        context = g_hash_table_lookup(interceptor->function_by_address,
                                      function_address);
        if (context != NULL && !context->destroyed &&
            peak_gum_context_has_listener(context, listener)) {
            return context;
        }
    }

    g_hash_table_iter_init(&iter, interceptor->function_by_address);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        context = value;
        if (context == NULL || context->destroyed ||
            !peak_gum_context_has_listener(context, listener)) {
            continue;
        }
        if (match != NULL) {
            return NULL;
        }
        match = context;
    }

    return match;
}

static gboolean
peak_gum_pc_in_shared_thunk(PeakGumInterceptor17 * interceptor,
                            PeakGumFunctionContext17 * context,
                            gpointer pc)
{
    PeakGumInterceptorBackend17 * backend;

    if (interceptor == NULL || interceptor->backend == NULL || pc == NULL) {
        return FALSE;
    }

    backend = interceptor->backend;
#if defined(__x86_64__) || defined(__amd64__)
    return (backend->enter_thunk != NULL &&
            peak_gum_pointer_in_range(pc,
                                      backend->enter_thunk->data,
                                      backend->enter_thunk->size)) ||
           (backend->leave_thunk != NULL &&
            peak_gum_pointer_in_range(pc,
                                      backend->leave_thunk->data,
                                      backend->leave_thunk->size));
#elif defined(__aarch64__)
    PeakGumArm64FunctionContextData17 * data;
    PeakGumArm64ThunkSet17 * thunks;

    if (context == NULL || backend->thunks_by_scratch_reg == NULL) {
        return FALSE;
    }

    data = (PeakGumArm64FunctionContextData17 *)context->backend_data.storage;
    thunks = (PeakGumArm64ThunkSet17 *)g_hash_table_lookup(
        backend->thunks_by_scratch_reg, GINT_TO_POINTER(data->scratch_reg));
    if (thunks == NULL) {
        return FALSE;
    }

    return peak_gum_pointer_in_range(pc,
                                     thunks->page,
                                     (gsize)gum_query_page_size());
#else
    return FALSE;
#endif
}

static void
peak_gum_fill_shared_thunk_diagnostics(
    PeakGumInterceptorBackend17 * backend,
    PeakGumFunctionContext17 * context,
    GumPeakPcDiagnostics * diagnostics)
{
    if (backend == NULL || diagnostics == NULL) {
        return;
    }

#if defined(__x86_64__) || defined(__amd64__)
    if (backend->enter_thunk != NULL) {
        diagnostics->enter_thunk_start = backend->enter_thunk->data;
        diagnostics->enter_thunk_size = backend->enter_thunk->size;
    }
    if (backend->leave_thunk != NULL) {
        diagnostics->leave_thunk_start = backend->leave_thunk->data;
        diagnostics->leave_thunk_size = backend->leave_thunk->size;
    }
#elif defined(__aarch64__)
    PeakGumArm64FunctionContextData17 * data;
    PeakGumArm64ThunkSet17 * thunks;

    if (context == NULL || backend->thunks_by_scratch_reg == NULL) {
        return;
    }

    data = (PeakGumArm64FunctionContextData17 *)context->backend_data.storage;
    thunks = (PeakGumArm64ThunkSet17 *)g_hash_table_lookup(
        backend->thunks_by_scratch_reg, GINT_TO_POINTER(data->scratch_reg));
    if (thunks == NULL) {
        return;
    }

    if (thunks->page != NULL) {
        diagnostics->enter_thunk_start = thunks->page;
        diagnostics->enter_thunk_size = (gsize)gum_query_page_size();
    }
    if (thunks->leave_thunk != NULL) {
        guint8 * page_end = thunks->page != NULL
            ? (guint8 *)thunks->page + gum_query_page_size()
            : NULL;
        guint8 * leave_start = (guint8 *)thunks->leave_thunk;

        diagnostics->leave_thunk_start = thunks->leave_thunk;
        if (page_end != NULL && leave_start < page_end) {
            diagnostics->leave_thunk_size = (gsize)(page_end - leave_start);
        }
    }
#endif
}


static PeakGumFunctionContext17 *
peak_gum_find_context(GumInterceptor * interceptor,
                      gpointer function_address,
                      GumInvocationListener * listener)
{
    PeakGumInterceptor17 * private_interceptor;
    PeakGumFunctionContext17 * private_context;

    if (interceptor == NULL || function_address == NULL) {
        return NULL;
    }

    private_interceptor = (PeakGumInterceptor17 *)interceptor;
    private_context = private_interceptor->function_by_address != NULL
        ? (PeakGumFunctionContext17 *)g_hash_table_lookup(
              private_interceptor->function_by_address, function_address)
        : NULL;

    if (peak_gum_context_is_usable(private_context) &&
        peak_gum_context_has_listener(private_context, listener)) {
        return private_context;
    }

    return peak_gum_find_context_by_listener(private_interceptor, listener);
}

gboolean
gum_interceptor_peak_enable_fast_listener(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    GumPeakFastListener * fast_listener)
{
    PeakGumInterceptor17 * private_interceptor;
    PeakGumFunctionContext17 * context;
    gboolean enabled = FALSE;

    if (interceptor == NULL || function_address == NULL || listener == NULL ||
        fast_listener == NULL ||
        fast_listener->version != GUM_PEAK_FAST_LISTENER_VERSION ||
        fast_listener->on_enter == NULL ||
        fast_listener->on_leave == NULL ||
        fast_listener->is_direct_leave == NULL ||
        fast_listener->listener_instance != listener ||
        fast_listener->dispatch_start == NULL ||
        fast_listener->dispatch_size == 0 ||
        fast_listener->active_count == NULL ||
        fast_listener->active_close == NULL ||
        fast_listener->active_reset == NULL ||
        fast_listener->translate_return_address == NULL) {
        return FALSE;
    }

    private_interceptor = (PeakGumInterceptor17 *)interceptor;
    g_rec_mutex_lock(&private_interceptor->mutex);
    context = peak_gum_find_configurable_context(private_interceptor,
                                                 function_address,
                                                 listener);
    if (context != NULL &&
        context->replacement_function == NULL &&
        context->write_redirect == NULL &&
        peak_gum_context_has_only_listener(context, listener) &&
        context->trampoline_usage_counter == 0 &&
        (context->write_redirect_data == NULL ||
         context->write_redirect_data == fast_listener) &&
#if defined(__x86_64__)
        peak_invoke_api_ensure(interceptor)) {
#else
        peak_gum_fast_unwind_ensure(private_interceptor->unwind_broker,
                                    fast_listener->translate_return_address)) {
#endif
        g_atomic_pointer_set(
            &fast_listener->listener_entries_cookie,
            g_atomic_pointer_get(&context->listener_entries));
        fast_listener->active_reset(fast_listener->user_data);
        g_atomic_int_set(&fast_listener->release_required, 0);
        g_atomic_pointer_set(&context->write_redirect_data, fast_listener);
        g_atomic_int_set(&fast_listener->enabled, 1);
        enabled = TRUE;
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return enabled;
}

gpointer
gum_interceptor_peak_translate_fast_return_address(
    gpointer function_context,
    gpointer caller_return_address,
    gpointer code_address)
{
    PeakGumFunctionContext17 * context = function_context;

    if (context != NULL &&
        context->on_leave_trampoline == code_address) {
        return caller_return_address;
    }
    return NULL;
}

gboolean
gum_interceptor_peak_prepare_fast_detach(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener)
{
    PeakGumInterceptor17 * private_interceptor;
    PeakGumFunctionContext17 * context;
    GumPeakFastListener * fast_listener;
    guint active;
    gint current;
    gboolean prepared = FALSE;

    if (interceptor == NULL || function_address == NULL || listener == NULL) {
        return FALSE;
    }

    private_interceptor = (PeakGumInterceptor17 *)interceptor;
    g_rec_mutex_lock(&private_interceptor->mutex);
    context = peak_gum_find_context(interceptor, function_address, listener);
    fast_listener = peak_gum_context_fast_listener(context);
    if (context != NULL && fast_listener == NULL) {
        /* Direct dispatch was already disabled and drained. */
        prepared = TRUE;
    } else if (fast_listener != NULL) {
        g_atomic_int_set(&fast_listener->enabled, 0);
        active = fast_listener->active_close(fast_listener->user_data);
        current = g_atomic_int_get(&context->trampoline_usage_counter);
        if (g_atomic_int_get(&fast_listener->release_required) != 0) {
            prepared = TRUE;
        } else if (current >= 0 &&
                   active <= (guint)(G_MAXINT - current)) {
            if (active != 0) {
                g_atomic_int_add(&context->trampoline_usage_counter,
                                 (gint)active);
                g_atomic_int_set(&fast_listener->release_required, 1);
            } else {
                g_atomic_int_set(&fast_listener->release_required, 1);
                g_atomic_pointer_compare_and_exchange(
                    &context->write_redirect_data, fast_listener, NULL);
            }
            prepared = TRUE;
        }
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return prepared;
}

void PEAK_GUM_FAST_DISPATCH_SECTION
gum_interceptor_peak_release_fast_invocation(
    gpointer function_context,
    GumPeakFastListener * fast_listener)
{
    PeakGumFunctionContext17 * context = function_context;

    if (context != NULL && fast_listener != NULL &&
        fast_listener->version == GUM_PEAK_FAST_LISTENER_VERSION) {
        while (g_atomic_int_get(&fast_listener->release_required) == 0) {
#if defined(__x86_64__) || defined(__amd64__)
            __asm__ volatile("pause");
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
        if (!context->destroyed &&
            g_atomic_int_get(&fast_listener->enabled) == 0 &&
            fast_listener->active_count(fast_listener->user_data) == 0) {
            g_atomic_pointer_compare_and_exchange(
                &context->write_redirect_data, fast_listener, NULL);
        }
        g_atomic_int_dec_and_test(&context->trampoline_usage_counter);
    }
}

guint gum_interceptor_peak_unwind_frame_api_version(void) { return GUM_PEAK_UNWIND_FRAME_API_VERSION; }

static guintptr peak_invoke_normalize_address(guintptr pc);

#include "peak_gum_unwind_frames.inc"
#include "peak_gum_invoke_cfi.inc"

gboolean PEAK_GUM_FAST_DISPATCH_SECTION
_gum_function_context_begin_invocation(
    PeakGumFunctionContext17 * function_ctx,
    GumCpuContext * cpu_context,
    gpointer * caller_ret_addr,
    gpointer * next_hop)
{
    if (peak_uw_internal != 0)
        return _gum_function_context_begin_invocation_peak_original(function_ctx, cpu_context, caller_ret_addr, next_hop);
    gpointer peak_original_return = *caller_ret_addr;
    peak_uw_reap(caller_ret_addr);
#if defined(__x86_64__)
    peak_gum_invocation_stack_reap_unwound(caller_ret_addr);
#endif
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener_for_enter(function_ctx);
    PeakUwFrame *peak_reserved = NULL;
#if defined(__x86_64__)
    gboolean peak_needs_frame = fast_listener != NULL &&
        function_ctx->function_address != (gpointer)_Unwind_Find_FDE &&
        function_ctx->function_address != (gpointer)_Unwind_GetIP &&
        function_ctx->function_address != (gpointer)_Unwind_GetIPInfo &&
        function_ctx->function_address != (gpointer)_Unwind_GetRegionStart &&
        function_ctx->function_address != (gpointer)_Unwind_GetLanguageSpecificData;
    if (peak_needs_frame) {
        peak_reserved = peak_uw_prepare();
        if (peak_reserved == NULL) {
            /* Preserve Gum replacement/listener semantics on resource failure.
             * This is explicitly degraded admission, not a repaired EH path. */
            if (peak_general_listener_note_dropped_current_thread)
                peak_general_listener_note_dropped_current_thread();
            return _gum_function_context_begin_invocation_peak_original(function_ctx, cpu_context, caller_ret_addr, next_hop);
        }
    }
#endif
    if (G_LIKELY(fast_listener != NULL)) {
        gpointer stack_address;
        gpointer return_address = *caller_ret_addr;
        guint gum_stack_depth;
        GumPeakFastEnterResult result;

#if defined(__x86_64__) || defined(__amd64__)
        /*
         * The enter thunk may temporarily bias the reconstructed RSP by one
         * word depending on its redirect form.  caller_ret_addr is the exact
         * application return slot and matches the RSP reconstructed by the
         * leave thunk.
         */
        stack_address = caller_ret_addr;
#elif defined(__aarch64__)
        stack_address = (gpointer)(uintptr_t)cpu_context->sp;
#else
# error "Unsupported PEAK Gum fast-listener architecture"
#endif

        /*
         * Gum normally performs this synchronization in its generic begin
         * path. Direct dispatch must do the same before snapshotting the live
         * generic boundary, otherwise PEAK's TLS stack can lag behind Gum's
         * independently reaped invocation stack.
         */
        peak_gum_invocation_stack_reap_unwound(stack_address);
        gum_stack_depth = peak_gum_invocation_stack_depth();
        result = fast_listener->on_enter(fast_listener->user_data,
                                         function_ctx,
                                         stack_address,
                                         return_address,
                                         gum_stack_depth);
        if (G_UNLIKELY(result == GUM_PEAK_FAST_ENTER_FALLBACK)) {
            gboolean invoke = _gum_function_context_begin_invocation_peak_original(
                function_ctx, cpu_context, caller_ret_addr, next_hop);
            peak_uw_bind_gum_context();
            peak_uw_enter(peak_reserved, function_ctx, caller_ret_addr,
                          peak_original_return, invoke, TRUE);
            return invoke || peak_reserved != NULL;
        }
        peak_uw_bind_gum_context();
        *next_hop = function_ctx->on_invoke_trampoline;
        if (result == GUM_PEAK_FAST_ENTER_INVOKE) {
            *caller_ret_addr = function_ctx->on_leave_trampoline;
            peak_uw_enter(peak_reserved, function_ctx, caller_ret_addr,
                          peak_original_return, TRUE, FALSE);
        }
        else peak_uw_enter(peak_reserved, function_ctx, caller_ret_addr,
                           peak_original_return, FALSE, FALSE);
        return result == GUM_PEAK_FAST_ENTER_INVOKE || peak_reserved != NULL;
    }

    gboolean invoke = _gum_function_context_begin_invocation_peak_original(
        function_ctx, cpu_context, caller_ret_addr, next_hop);
    peak_uw_bind_gum_context();
    peak_uw_enter(peak_reserved, function_ctx, caller_ret_addr,
                  peak_original_return, invoke, TRUE);
    return invoke || peak_reserved != NULL;
}

void PEAK_GUM_FAST_DISPATCH_SECTION
_gum_function_context_end_invocation(
    PeakGumFunctionContext17 * function_ctx,
    GumCpuContext * cpu_context,
    gpointer * next_hop)
{
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener(function_ctx);
    gpointer stack_address;

#if defined(__x86_64__) || defined(__amd64__)
    /*
     * Gum's x86 leave thunk reconstructs the caller context with RSP pointing
     * at the same return-address slot reported by the entry thunk.
     */
    stack_address = (gpointer)(uintptr_t)cpu_context->rsp;
    gpointer peak_caller = NULL;
    if (peak_uw_leave(function_ctx, stack_address, &peak_caller)) {
        *next_hop = peak_caller;
        return;
    }
#elif defined(__aarch64__)
    /* AArch64 returns through LR without changing SP. */
    stack_address = (gpointer)(uintptr_t)cpu_context->sp;
#else
# error "Unsupported PEAK Gum fast-listener architecture"
#endif

    if (G_LIKELY(fast_listener != NULL) &&
        fast_listener->is_direct_leave(fast_listener->user_data,
                                       function_ctx,
                                       stack_address)) {
        gpointer return_address = NULL;
        guint gum_stack_depth = 0;
        gboolean release_required =
            fast_listener->on_leave(fast_listener->user_data,
                                    function_ctx,
                                    stack_address,
                                    &gum_stack_depth,
                                    &return_address);
        peak_gum_invocation_stack_reap_to_depth(gum_stack_depth);
        if (G_UNLIKELY(release_required)) {
            while (g_atomic_int_get(&fast_listener->release_required) == 0) {
#if defined(__x86_64__) || defined(__amd64__)
                __asm__ volatile("pause");
#elif defined(__aarch64__)
                __asm__ volatile("yield");
#endif
            }
        }
        gboolean clear_disabled =
            release_required &&
            g_atomic_int_get(&fast_listener->enabled) == 0 &&
            fast_listener->active_count(fast_listener->user_data) == 0;
        gboolean destroyed = function_ctx->destroyed;

        (void)cpu_context;
        *next_hop = return_address;
        if (G_UNLIKELY(clear_disabled && !destroyed)) {
            g_atomic_pointer_compare_and_exchange(
                &function_ctx->write_redirect_data, fast_listener, NULL);
        }
        if (G_UNLIKELY(release_required)) {
            g_atomic_int_dec_and_test(
                &function_ctx->trampoline_usage_counter);
        }
        return;
    }

    _gum_function_context_end_invocation_peak_original(
        function_ctx, cpu_context, next_hop);
}

guint
gum_interceptor_peak_abi_fingerprint(void)
{
    return PEAK_GUM_PC_ABI_FINGERPRINT;
}

gboolean
gum_interceptor_peak_get_function_patch(GumInterceptor * interceptor,
                                        gpointer function_address,
                                        GumInvocationListener * listener,
                                        guint8 * active_patch,
                                        guint8 * original_prologue,
                                        guint * prologue_len)
{
    PeakGumFunctionContext17 * private_context;
    guint len;

    if (active_patch == NULL || original_prologue == NULL ||
        prologue_len == NULL) {
        return FALSE;
    }

    *prologue_len = 0;
    private_context = peak_gum_find_context(interceptor,
                                            function_address,
                                            listener);
    if (private_context == NULL) {
        return FALSE;
    }

    len = private_context->overwritten_prologue_len;
    if (len == 0 || len > GUM_PEAK_MAX_PROLOGUE_SIZE ||
        private_context->overwritten_prologue == NULL) {
        return FALSE;
    }

    memcpy(original_prologue, private_context->overwritten_prologue, len);
    memcpy(active_patch, private_context->function_address, len);
    *prologue_len = len;
    return TRUE;
}

gboolean
gum_interceptor_peak_get_pc_diagnostics(GumInterceptor * interceptor,
                                        gpointer function_address,
                                        GumInvocationListener * listener,
                                        GumPeakPcDiagnostics * diagnostics)
{
    PeakGumInterceptor17 * private_interceptor;
    PeakGumFunctionContext17 * private_context;
    PeakGumInterceptorBackend17 * backend;

    if (diagnostics == NULL) {
        return FALSE;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));

    private_context = peak_gum_find_context(interceptor,
                                            function_address,
                                            listener);
    if (private_context == NULL) {
        return FALSE;
    }

    private_interceptor = (PeakGumInterceptor17 *)interceptor;
    backend = private_interceptor->backend;

    diagnostics->function_address = private_context->function_address;
    diagnostics->overwritten_prologue_len =
        private_context->overwritten_prologue_len;
    if (private_context->trampoline_slice != NULL) {
        diagnostics->trampoline_slice_start =
            private_context->trampoline_slice->data;
        diagnostics->trampoline_slice_size =
            private_context->trampoline_slice->size;
    }
    diagnostics->on_enter_trampoline = private_context->on_enter_trampoline;
    diagnostics->on_leave_trampoline = private_context->on_leave_trampoline;
    diagnostics->on_invoke_trampoline = private_context->on_invoke_trampoline;
    diagnostics->fast_overlay_dispatch_start =
        (gpointer)__start_peak_gum_fast_dispatch;
    diagnostics->fast_overlay_dispatch_size =
        (gsize)(__stop_peak_gum_fast_dispatch -
                __start_peak_gum_fast_dispatch);
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener(private_context);
    if (fast_listener != NULL) {
        diagnostics->fast_listener_dispatch_start =
            fast_listener->dispatch_start;
        diagnostics->fast_listener_dispatch_size =
            fast_listener->dispatch_size;
    }
    peak_gum_fill_shared_thunk_diagnostics(backend, private_context, diagnostics);

    return TRUE;
}

gboolean
gum_interceptor_peak_classify_pc(GumInterceptor * interceptor,
                                 gpointer function_address,
                                 GumInvocationListener * listener,
                                 gpointer pc,
                                 GumPeakFunctionContext ** ctx,
                                 GumPeakPcState * state)
{
    PeakGumInterceptor17 * private_interceptor;
    PeakGumFunctionContext17 * private_context;
    gpointer slice_start;
    gsize slice_size;

    if (ctx == NULL || state == NULL) {
        return FALSE;
    }

    *ctx = NULL;
    *state = GUM_PEAK_PC_UNKNOWN;

    if (interceptor == NULL || function_address == NULL || pc == NULL) {
        return FALSE;
    }

    private_context = peak_gum_find_context(interceptor, function_address, listener);

    if (private_context == NULL) {
        *state = GUM_PEAK_PC_SAFE;
        return TRUE;
    }

    private_interceptor = (PeakGumInterceptor17 *)interceptor;
    *ctx = (GumPeakFunctionContext *)private_context;

    if (peak_gum_pointer_in_range(pc, private_context->function_address,
                                  private_context->overwritten_prologue_len)) {
        *state = GUM_PEAK_PC_AT_PATCH_ENTRY;
        return TRUE;
    }

    if (peak_gum_pc_in_shared_thunk(private_interceptor, private_context, pc)) {
        *state = GUM_PEAK_PC_IN_DISPATCH;
        return TRUE;
    }

    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener(private_context);
    if (peak_gum_pointer_in_range(
            pc,
            (gpointer)__start_peak_gum_fast_dispatch,
            (gsize)(__stop_peak_gum_fast_dispatch -
                    __start_peak_gum_fast_dispatch)) ||
        (fast_listener != NULL &&
         peak_gum_pointer_in_range(pc,
                                   fast_listener->dispatch_start,
                                   fast_listener->dispatch_size))) {
        *state = GUM_PEAK_PC_IN_DISPATCH;
        return TRUE;
    }

    if (private_context->trampoline_slice == NULL) {
        *state = GUM_PEAK_PC_SAFE;
        return TRUE;
    }

    slice_start = private_context->trampoline_slice->data;
    slice_size = private_context->trampoline_slice->size;
    if (!peak_gum_pointer_in_range(pc, slice_start, slice_size)) {
        *state = GUM_PEAK_PC_SAFE;
        return TRUE;
    }

    if (peak_gum_pointer_between_labels(pc,
                                        private_context->on_enter_trampoline,
                                        private_context->on_leave_trampoline)) {
        *state = GUM_PEAK_PC_IN_ENTER_TRAMPOLINE;
        return TRUE;
    }

    if (peak_gum_pointer_between_labels(pc,
                                        private_context->on_leave_trampoline,
                                        private_context->on_invoke_trampoline)) {
        *state = GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE;
        return TRUE;
    }

    if (private_context->on_invoke_trampoline != NULL &&
        peak_gum_pointer_in_range(pc, private_context->on_invoke_trampoline,
            (gsize)(((uintptr_t)slice_start + slice_size) -
                    (uintptr_t)private_context->on_invoke_trampoline))) {
        *state = GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE;
        return TRUE;
    }

    *state = GUM_PEAK_PC_UNKNOWN;
    return TRUE;
}

gpointer
gum_interceptor_peak_safe_pc(GumPeakFunctionContext * ctx,
                             gpointer pc,
                             GumPeakPcState state)
{
    PeakGumFunctionContext17 * private_context =
        (PeakGumFunctionContext17 *)ctx;

    if (private_context == NULL || pc == NULL) {
        return NULL;
    }

    if (state == GUM_PEAK_PC_IN_ENTER_TRAMPOLINE &&
        pc == private_context->on_enter_trampoline) {
        return private_context->function_address;
    }

    return NULL;
}
