#define _GNU_SOURCE
#define PEAK_ENABLE_TEST_HOOKS 1
#include "dlopen_interceptor.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void* (*DlopenFunction)(const char*, int);
typedef void* (*DlmopenFunction)(Lmid_t, const char*, int);
typedef int (*DlinfoFunction)(void*, int, void*);
typedef void (*SetManualDrainFunction)(gboolean);
typedef void (*DrainFunction)(void);
typedef void (*GetDiagnosticsFunction)(
    PeakDlopenDynamicAttachDiagnostics*);
typedef gboolean (*EnqueueLoadedFunction)(const char*, void*);
typedef void (*HoldOwnershipBrokerFunction)(gboolean);
typedef gboolean (*OwnershipBrokerWaitingFunction)(void);
typedef void (*HoldOwnershipPinFunction)(gboolean);
typedef gboolean (*OwnershipPinWaitingFunction)(void);
typedef size_t (*PendingOwnershipCountFunction)(void);
typedef gboolean (*ShutdownDynamicAttachFunction)(void);
typedef gboolean (*CallbackIsAdmittedFunction)(void);
typedef gboolean (*DetachDlopenInterceptorFunction)(void);
typedef void (*HoldDlcloseGuardFunction)(gboolean);
typedef gboolean (*DlcloseGuardStateFunction)(void);

typedef enum {
    LOADER_PHASE_IDLE = 0,
    LOADER_PHASE_CALLBACK,
    LOADER_PHASE_DRAIN,
    LOADER_PHASE_DESTRUCTOR
} LoaderPhase;

typedef struct {
    SetManualDrainFunction set_manual_drain;
    DrainFunction drain;
    GetDiagnosticsFunction get_diagnostics;
    EnqueueLoadedFunction enqueue_loaded;
    HoldOwnershipBrokerFunction hold_ownership_broker;
    OwnershipBrokerWaitingFunction ownership_broker_waiting;
    HoldOwnershipPinFunction hold_ownership_pin;
    OwnershipPinWaitingFunction ownership_pin_waiting;
    PendingOwnershipCountFunction pending_ownership_count;
    ShutdownDynamicAttachFunction shutdown_dynamic_attach;
    CallbackIsAdmittedFunction callback_is_admitted;
    DetachDlopenInterceptorFunction detach_dlopen_interceptor;
    HoldDlcloseGuardFunction hold_dlclose_guard;
    DlcloseGuardStateFunction dlclose_guard_waiting;
    DlcloseGuardStateFunction dlclose_guard_reverting;
} PeakTestHooks;

typedef struct {
    const char* module_path;
    unsigned int iterations;
    atomic_int* start;
    atomic_int* remaining;
    atomic_int* failures;
} StressWorkerArgs;

typedef struct {
    ShutdownDynamicAttachFunction shutdown_dynamic_attach;
    gboolean result;
} ShutdownThreadArgs;

typedef struct {
    void* handle;
    int result;
} CloseThreadArgs;

typedef struct {
    DetachDlopenInterceptorFunction detach_dlopen_interceptor;
    gboolean result;
} DetachThreadArgs;

static pthread_once_t real_loader_once = PTHREAD_ONCE_INIT;
static DlopenFunction real_dlopen_function;
static DlopenFunction application_dlopen_function;
static DlmopenFunction real_dlmopen_function;
static DlinfoFunction real_dlinfo_function;
static _Thread_local LoaderPhase loader_phase = LOADER_PHASE_IDLE;
static atomic_ullong callback_loader_calls;
static atomic_ullong callback_noload_calls;
static atomic_ullong drain_loader_calls;
static atomic_ullong drain_noload_calls;
static atomic_ullong broker_noload_calls;
static atomic_ullong callback_dlinfo_calls;
static atomic_ullong broker_dlinfo_calls;
static atomic_ullong destructor_loader_calls;
static atomic_ullong destructor_noload_calls;
static atomic_uint destructor_loader_enabled;
static atomic_uint destructor_loader_successes;
static atomic_uint fixture_loads;
static atomic_uint fixture_unloads;
static int failures;

static void
copy_function_pointer(void* symbol, void* destination, size_t destination_size)
{
    if (symbol == NULL || destination_size != sizeof(symbol)) {
        fprintf(stderr, "unable to resolve loader function: %s\n", dlerror());
        abort();
    }
    memcpy(destination, &symbol, sizeof(symbol));
}

static void
resolve_real_loader_functions(void)
{
    void* symbol;

    dlerror();
    symbol = dlsym(RTLD_NEXT, "dlopen");
    copy_function_pointer(symbol,
                          &real_dlopen_function,
                          sizeof(real_dlopen_function));
    dlerror();
    symbol = dlsym(RTLD_NEXT, "dlmopen");
    copy_function_pointer(symbol,
                          &real_dlmopen_function,
                          sizeof(real_dlmopen_function));
    dlerror();
    symbol = dlsym(RTLD_NEXT, "dlinfo");
    copy_function_pointer(symbol,
                          &real_dlinfo_function,
                          sizeof(real_dlinfo_function));
}

static void
record_loader_call(int flags)
{
    if (loader_phase == LOADER_PHASE_CALLBACK) {
        atomic_fetch_add_explicit(&callback_loader_calls,
                                  1,
                                  memory_order_relaxed);
        if ((flags & RTLD_NOLOAD) != 0) {
            atomic_fetch_add_explicit(&callback_noload_calls,
                                      1,
                                      memory_order_relaxed);
        }
    } else if (loader_phase == LOADER_PHASE_DRAIN) {
        atomic_fetch_add_explicit(&drain_loader_calls,
                                  1,
                                  memory_order_relaxed);
        if ((flags & RTLD_NOLOAD) != 0) {
            atomic_fetch_add_explicit(&drain_noload_calls,
                                      1,
                                      memory_order_relaxed);
        }
    } else if (loader_phase == LOADER_PHASE_DESTRUCTOR) {
        atomic_fetch_add_explicit(&destructor_loader_calls,
                                  1,
                                  memory_order_relaxed);
        if ((flags & RTLD_NOLOAD) != 0) {
            atomic_fetch_add_explicit(&destructor_noload_calls,
                                      1,
                                      memory_order_relaxed);
        }
    } else if ((flags & RTLD_NOLOAD) != 0) {
        atomic_fetch_add_explicit(&broker_noload_calls,
                                  1,
                                  memory_order_relaxed);
    }
}

/*
 * Symbols exported by the executable preempt loader calls made by libpeak.
 * Callback-time RTLD_NOLOAD calls are distinguishable from the outer
 * application load by their flags. A controller drain is identified by a
 * thread-local phase set immediately around the explicit drain hook.
 */
__attribute__((visibility("default")))
void*
dlopen(const char* filename, int flags)
{
    void* result;

    if (pthread_once(&real_loader_once, resolve_real_loader_functions) != 0 ||
        real_dlopen_function == NULL) {
        errno = ENOSYS;
        return NULL;
    }
    record_loader_call(flags);
    result = real_dlopen_function(filename, flags);
    return result;
}

__attribute__((visibility("default")))
void*
dlmopen(Lmid_t namespace_id, const char* filename, int flags)
{
    void* result;

    if (pthread_once(&real_loader_once, resolve_real_loader_functions) != 0 ||
        real_dlmopen_function == NULL) {
        errno = ENOSYS;
        return NULL;
    }
    record_loader_call(flags);
    result = real_dlmopen_function(namespace_id, filename, flags);
    return result;
}

__attribute__((visibility("default")))
int
dlinfo(void* handle, int request, void* argument)
{
    if (pthread_once(&real_loader_once, resolve_real_loader_functions) != 0 ||
        real_dlinfo_function == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (loader_phase == LOADER_PHASE_CALLBACK) {
        atomic_fetch_add_explicit(&callback_dlinfo_calls,
                                  1,
                                  memory_order_relaxed);
    } else if (loader_phase == LOADER_PHASE_IDLE) {
        atomic_fetch_add_explicit(&broker_dlinfo_calls,
                                  1,
                                  memory_order_relaxed);
    }
    return real_dlinfo_function(handle, request, argument);
}

__attribute__((visibility("default")))
void
peak_dlopen_owned_fixture_event(int loaded)
{
    if (loaded) {
        atomic_fetch_add_explicit(&fixture_loads, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&fixture_unloads, 1, memory_order_relaxed);
    }
}

__attribute__((visibility("default")))
void
peak_dlopen_owned_fixture_destructor_loader(void)
{
    void* handle;
    LoaderPhase previous_phase;

    if (atomic_load_explicit(&destructor_loader_enabled,
                             memory_order_acquire) == 0) {
        return;
    }

    previous_phase = loader_phase;
    loader_phase = LOADER_PHASE_DESTRUCTOR;
    handle = application_dlopen_function("libm.so.6",
                                         RTLD_NOW | RTLD_LOCAL);
    if (handle != NULL) {
        atomic_fetch_add_explicit(&destructor_loader_successes,
                                  1,
                                  memory_order_relaxed);
        dlclose(handle);
    }
    loader_phase = previous_phase;
}

static void
check_true(const char* label, int condition)
{
    if (!condition) {
        fprintf(stderr, "not ok - %s\n", label);
        failures++;
    }
}

static void
check_ull(const char* label,
          unsigned long long actual,
          unsigned long long expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "not ok - %s: expected %llu, got %llu\n",
                label,
                expected,
                actual);
        failures++;
    }
}

static void
check_size(const char* label, size_t actual, size_t expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "not ok - %s: expected %zu, got %zu\n",
                label,
                expected,
                actual);
        failures++;
    }
}

static void
resolve_hook(const char* name, void* destination, size_t destination_size)
{
    void* symbol;
    const char* error;

    dlerror();
    symbol = dlsym(RTLD_DEFAULT, name);
    error = dlerror();
    if (error != NULL || symbol == NULL ||
        destination_size != sizeof(symbol)) {
        fprintf(stderr,
                "unable to resolve %s: %s\n",
                name,
                error != NULL ? error : "invalid function pointer");
        exit(EXIT_FAILURE);
    }
    memcpy(destination, &symbol, sizeof(symbol));
}

static PeakTestHooks
load_peak_test_hooks(void)
{
    PeakTestHooks hooks;

    memset(&hooks, 0, sizeof(hooks));
    resolve_hook("dlopen_interceptor_test_set_manual_drain",
                 &hooks.set_manual_drain,
                 sizeof(hooks.set_manual_drain));
    resolve_hook("dlopen_interceptor_test_drain_dynamic_attach_queue",
                 &hooks.drain,
                 sizeof(hooks.drain));
    resolve_hook("dlopen_interceptor_get_dynamic_attach_diagnostics",
                 &hooks.get_diagnostics,
                 sizeof(hooks.get_diagnostics));
    resolve_hook("dlopen_interceptor_test_enqueue_loaded_dynamic_attach",
                 &hooks.enqueue_loaded,
                 sizeof(hooks.enqueue_loaded));
    resolve_hook("dlopen_interceptor_test_hold_ownership_broker",
                 &hooks.hold_ownership_broker,
                 sizeof(hooks.hold_ownership_broker));
    resolve_hook("dlopen_interceptor_test_ownership_broker_waiting",
                 &hooks.ownership_broker_waiting,
                 sizeof(hooks.ownership_broker_waiting));
    resolve_hook("dlopen_interceptor_test_hold_ownership_pin",
                 &hooks.hold_ownership_pin,
                 sizeof(hooks.hold_ownership_pin));
    resolve_hook("dlopen_interceptor_test_ownership_pin_waiting",
                 &hooks.ownership_pin_waiting,
                 sizeof(hooks.ownership_pin_waiting));
    resolve_hook("dlopen_interceptor_test_pending_ownership_count",
                 &hooks.pending_ownership_count,
                 sizeof(hooks.pending_ownership_count));
    resolve_hook("dlopen_interceptor_test_shutdown_dynamic_attach",
                 &hooks.shutdown_dynamic_attach,
                 sizeof(hooks.shutdown_dynamic_attach));
    resolve_hook("dlopen_interceptor_test_callback_is_admitted",
                 &hooks.callback_is_admitted,
                 sizeof(hooks.callback_is_admitted));
    resolve_hook("dlopen_interceptor_test_dettach",
                 &hooks.detach_dlopen_interceptor,
                 sizeof(hooks.detach_dlopen_interceptor));
    resolve_hook("dlopen_interceptor_test_hold_dlclose_guard",
                 &hooks.hold_dlclose_guard,
                 sizeof(hooks.hold_dlclose_guard));
    resolve_hook("dlopen_interceptor_test_dlclose_guard_waiting",
                 &hooks.dlclose_guard_waiting,
                 sizeof(hooks.dlclose_guard_waiting));
    resolve_hook("dlopen_interceptor_test_dlclose_guard_reverting",
                 &hooks.dlclose_guard_reverting,
                 sizeof(hooks.dlclose_guard_reverting));
    return hooks;
}

static PeakDlopenDynamicAttachDiagnostics
get_diagnostics(const PeakTestHooks* hooks)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;

    memset(&diagnostics, 0, sizeof(diagnostics));
    hooks->get_diagnostics(&diagnostics);
    return diagnostics;
}

static void
drain_once(const PeakTestHooks* hooks)
{
    loader_phase = LOADER_PHASE_DRAIN;
    hooks->drain();
    loader_phase = LOADER_PHASE_IDLE;
}

static gboolean
wait_for_ownership_idle(const PeakTestHooks* hooks)
{
    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
        if (hooks->pending_ownership_count() == 0) {
            return TRUE;
        }
        sched_yield();
    }
    return FALSE;
}

static gboolean
wait_for_broker_barrier(const PeakTestHooks* hooks)
{
    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
        if (hooks->ownership_broker_waiting()) {
            return TRUE;
        }
        sched_yield();
    }
    return FALSE;
}

static gboolean
wait_for_ownership_pin_barrier(const PeakTestHooks* hooks)
{
    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
        if (hooks->ownership_pin_waiting()) {
            return TRUE;
        }
        sched_yield();
    }
    return FALSE;
}

static gboolean
wait_for_callback_admission(const PeakTestHooks* hooks, gboolean admitted)
{
    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
        if (hooks->callback_is_admitted() == admitted) {
            return TRUE;
        }
        sched_yield();
    }
    return FALSE;
}

static gboolean
wait_for_dlclose_guard_state(DlcloseGuardStateFunction state)
{
    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
        if (state()) {
            return TRUE;
        }
        sched_yield();
    }
    return FALSE;
}

static PeakDlopenDynamicAttachDiagnostics
drain_until_empty(const PeakTestHooks* hooks)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics = { 0 };

    for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
        drain_once(hooks);
        diagnostics = get_diagnostics(hooks);
        if (diagnostics.queue_length == 0) {
            break;
        }
        sched_yield();
    }
    return diagnostics;
}

static void*
load_with_callback_phase_flags(const char* path, int flags)
{
    void* handle;

    loader_phase = LOADER_PHASE_CALLBACK;
    /*
     * Resolve this call at runtime instead of relying on a same-translation-
     * unit call to the executable's dlopen interposer.  ICC and NVHPC may
     * bind or clone that direct call locally at -O3, bypassing the exported
     * entry that Gum patches.  A runtime function-pointer call exercises the
     * same public application entry on every supported compiler.
     */
    handle = application_dlopen_function(path, flags);
    loader_phase = LOADER_PHASE_IDLE;
    return handle;
}

static void*
load_with_callback_phase(const char* path)
{
    return load_with_callback_phase_flags(path, RTLD_NOW | RTLD_LOCAL);
}

static int
test_callback_owned_handle(const PeakTestHooks* hooks, const char* module_path)
{
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    unsigned long long broker_noload_before =
        atomic_load_explicit(&broker_noload_calls, memory_order_relaxed);
    unsigned long long broker_dlinfo_before =
        atomic_load_explicit(&broker_dlinfo_calls, memory_order_relaxed);
    void* application_handle;

    /*
     * Hold the broker so the application close deterministically wins the
     * ownership race. The dlopen callback must return without waiting and the
    * guarded dlclose must transfer, rather than release, that reference.
     */
    hooks->hold_ownership_broker(TRUE);
    application_handle = load_with_callback_phase(module_path);
    check_true("ownership fixture loaded", application_handle != NULL);
    if (application_handle == NULL) {
        hooks->hold_ownership_broker(FALSE);
        return 1;
    }
    check_true("ownership broker reached deterministic hold barrier",
               wait_for_broker_barrier(hooks));

    PeakDlopenDynamicAttachDiagnostics queued = get_diagnostics(hooks);
    check_ull("real callback enqueued one borrowed request",
              queued.enqueued,
              before.enqueued + 1);
    check_size("real callback left one request queued",
               queued.queue_length,
               before.queue_length + 1);
    check_true("fixture constructor ran",
               atomic_load_explicit(&fixture_loads,
                                    memory_order_relaxed) == 1);
    check_ull("callback made no RTLD_NOLOAD loader call",
              atomic_load_explicit(&callback_noload_calls,
                                   memory_order_relaxed),
              0);
    check_ull("callback made no dlinfo call",
              atomic_load_explicit(&callback_dlinfo_calls,
                                   memory_order_relaxed),
              0);

    check_true("pending application dlclose transferred its reference",
               dlclose(application_handle) == 0);
    check_true("transferred reference kept module loaded",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 0);
    check_size("ownership transfer resolved pending broker work",
               hooks->pending_ownership_count(),
               0);
    hooks->hold_ownership_broker(FALSE);

    PeakDlopenDynamicAttachDiagnostics after_transfer =
        drain_until_empty(hooks);
    check_ull("transferred request drained",
              after_transfer.drained,
              before.drained + 1);
    check_true("drain released transferred module handle",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 1);

    /*
     * Exercise the other race outcome: allow the broker to obtain its exact
     * LMID pin before the application closes its own reference.
     */
    application_handle = load_with_callback_phase(module_path);
    check_true("broker-pin fixture reloaded", application_handle != NULL);
    check_true("broker completed exact ownership pin",
               wait_for_ownership_idle(hooks));
    check_true("broker performed RTLD_NOLOAD outside callback",
               atomic_load_explicit(&broker_noload_calls,
                                    memory_order_relaxed) >
                   broker_noload_before);
    check_true("broker performed dlinfo outside callback",
               atomic_load_explicit(&broker_dlinfo_calls,
                                    memory_order_relaxed) >
                   broker_dlinfo_before);
    check_true("post-pin application dlclose used real close",
               application_handle != NULL &&
               dlclose(application_handle) == 0);
    check_true("broker pin kept reloaded fixture alive",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 1);

    PeakDlopenDynamicAttachDiagnostics after =
        drain_until_empty(hooks);
    check_ull("broker-owned request drained",
              after.drained,
              before.drained + 2);
    check_size("owned queue empty after drain", after.queue_length, 0);
    check_ull("owned requests did not report RTLD_NOLOAD drop",
              after.dropped_noload,
              before.dropped_noload);
    check_ull("controller drain made no dlopen or dlmopen call",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              0);
    check_true("second drain released broker-owned handle",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 2);

    /*
     * Hold the broker after it publishes PINNING. This deterministically
     * covers the third race: dlclose transfers the application reference
     * while the broker is already committed to loader access. Pending must
     * remain nonzero until that access ends, and the redundant exact pin must
     * be released without unloading the transferred module.
     */
    hooks->hold_ownership_pin(TRUE);
    application_handle = load_with_callback_phase(module_path);
    check_true("pinning-race fixture reloaded", application_handle != NULL);
    check_true("ownership broker published PINNING",
               wait_for_ownership_pin_barrier(hooks));
    check_size("PINNING handoff remains pending before close",
               hooks->pending_ownership_count(),
               1);
    check_true("PINNING application dlclose transferred without waiting",
               application_handle != NULL &&
               dlclose(application_handle) == 0);
    check_size("PINNING transfer remains pending through loader access",
               hooks->pending_ownership_count(),
               1);
    check_true("PINNING transfer kept fixture loaded",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 2);
    hooks->hold_ownership_pin(FALSE);
    check_true("PINNING broker completion resolved ownership",
               wait_for_ownership_idle(hooks));
    check_true("redundant broker pin did not unload transferred fixture",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 2);

    PeakDlopenDynamicAttachDiagnostics after_pinning =
        drain_until_empty(hooks);
    check_ull("PINNING-transferred request drained",
              after_pinning.drained,
              before.drained + 3);
    check_true("PINNING drain released exactly one final reference",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 3);
    check_ull("callback path made only the three outer application loads",
              atomic_load_explicit(&callback_loader_calls,
                                   memory_order_relaxed),
              3);

    if (failures == 0) {
        printf("dlopen_loader_ownership_ok callback_loader_calls=%llu callback_noload_calls=%llu callback_dlinfo_calls=%llu broker_noload_calls=%llu drain_loader_calls=%llu\n",
               atomic_load_explicit(&callback_loader_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&callback_dlinfo_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&broker_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_loader_calls,
                                    memory_order_relaxed));
    }
    return failures == 0 ? 0 : 1;
}

static int
test_destructor_reentrant_loader(const PeakTestHooks* hooks,
                                 const char* module_path)
{
    drain_once(hooks);
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    void* application_handle = load_with_callback_phase(module_path);

    check_size("destructor test starts with an empty queue",
               before.queue_length,
               0);
    check_true("destructor fixture loaded", application_handle != NULL);
    if (application_handle == NULL) {
        return 1;
    }
    check_true("destructor fixture broker pin completed",
               wait_for_ownership_idle(hooks));
    check_true("destructor application handle closed",
               dlclose(application_handle) == 0);
    check_true("queue pin delayed fixture destructor",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 0);

    atomic_store_explicit(&destructor_loader_enabled,
                          1,
                          memory_order_release);
    drain_once(hooks);
    atomic_store_explicit(&destructor_loader_enabled,
                          0,
                          memory_order_release);

    PeakDlopenDynamicAttachDiagnostics after_first = get_diagnostics(hooks);
    check_true("barrier-external close ran fixture destructor",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 1);
    check_ull("fixture destructor completed one loader call",
              atomic_load_explicit(&destructor_loader_successes,
                                   memory_order_relaxed),
              1);
    check_ull("fixture destructor made only its outer application load",
              atomic_load_explicit(&destructor_loader_calls,
                                   memory_order_relaxed),
              1);
    check_ull("fixture destructor callback made no RTLD_NOLOAD call",
              atomic_load_explicit(&destructor_noload_calls,
                                   memory_order_relaxed),
              0);
    check_ull("fixture destructor real dlopen callback enqueued",
              after_first.enqueued,
              before.enqueued + 2);
    check_ull("controller itself made no loader open during drain",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              0);
    check_ull("original fixture request drained before destructor request",
              after_first.drained,
              before.drained + 1);
    check_size("destructor loader work remained queued for next drain",
               after_first.queue_length,
               before.queue_length + 1);

    check_true("destructor-created ownership handoff completed",
               wait_for_ownership_idle(hooks));
    PeakDlopenDynamicAttachDiagnostics after_second =
        drain_until_empty(hooks);
    check_ull("destructor-created request drained separately",
              after_second.drained,
              before.drained + 2);
    check_size("destructor reentrant queue empty after second drain",
               after_second.queue_length,
               before.queue_length);
    check_ull("destructor reentrant path had no RTLD_NOLOAD drop",
              after_second.dropped_noload,
              before.dropped_noload);

    if (failures == 0) {
        printf("dlopen_loader_destructor_reentrant_ok destructor_loader_calls=%llu destructor_noload_calls=%llu drain_loader_calls=%llu\n",
               atomic_load_explicit(&destructor_loader_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&destructor_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_loader_calls,
                                    memory_order_relaxed));
    }
    return failures == 0 ? 0 : 1;
}

static int
test_namespace_identity(const PeakTestHooks* hooks, const char* module_path)
{
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    void* application_handles[2] = { NULL, NULL };
    Lmid_t namespace_ids[2] = { LM_ID_BASE, LM_ID_BASE };

    for (size_t i = 0; i < 2; i++) {
        gboolean enqueued;

        application_handles[i] =
            dlmopen(LM_ID_NEWLM, module_path, RTLD_NOW | RTLD_LOCAL);
        check_true("namespace fixture loaded",
                   application_handles[i] != NULL);
        if (application_handles[i] == NULL) {
            continue;
        }
        check_true("loaded module namespace identity available",
                   dlinfo(application_handles[i],
                          RTLD_DI_LMID,
                          &namespace_ids[i]) == 0);
        loader_phase = LOADER_PHASE_CALLBACK;
        enqueued = hooks->enqueue_loaded(module_path,
                                         application_handles[i]);
        loader_phase = LOADER_PHASE_IDLE;
        check_true("namespace callback simulation enqueued loaded module",
                   enqueued);
    }
    check_true("LM_ID_NEWLM loads created distinct namespaces",
               namespace_ids[0] != namespace_ids[1]);

    PeakDlopenDynamicAttachDiagnostics queued = get_diagnostics(hooks);
    check_ull("same filename in two namespaces has two physical enqueues",
              queued.enqueued,
              before.enqueued + 2);
    check_size("same filename in two namespaces occupies two queue slots",
               queued.queue_length,
               before.queue_length + 2);
    check_ull("namespace callback simulation made no RTLD_NOLOAD call",
              atomic_load_explicit(&callback_noload_calls,
                                   memory_order_relaxed),
              0);
    check_ull("namespace callback simulation made no dlinfo call",
              atomic_load_explicit(&callback_dlinfo_calls,
                                   memory_order_relaxed),
              0);
    check_true("namespace pins ran in broker thread",
               atomic_load_explicit(&broker_noload_calls,
                                    memory_order_relaxed) > 0);

    for (size_t i = 0; i < 2; i++) {
        if (application_handles[i] != NULL) {
            check_true("namespace application handle closed",
                       dlclose(application_handles[i]) == 0);
        }
    }

    unsigned long long drain_calls_before =
        atomic_load_explicit(&drain_loader_calls, memory_order_relaxed);
    PeakDlopenDynamicAttachDiagnostics after =
        drain_until_empty(hooks);
    check_ull("both namespace requests drained",
              after.drained,
              before.drained + 2);
    check_size("namespace queue empty after drain", after.queue_length, 0);
    check_ull("namespace drain made no loader open call",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              drain_calls_before);

    if (failures == 0) {
        printf("dlopen_loader_namespace_identity_ok enqueued_delta=%llu drained_delta=%llu callback_noload_calls=%llu callback_dlinfo_calls=%llu broker_noload_calls=%llu drain_loader_calls=%llu\n",
               after.enqueued - before.enqueued,
               after.drained - before.drained,
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&callback_dlinfo_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&broker_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_loader_calls,
                                    memory_order_relaxed));
    }
    return failures == 0 ? 0 : 1;
}

static void*
stress_worker(void* opaque)
{
    StressWorkerArgs* args = opaque;

    while (atomic_load_explicit(args->start, memory_order_acquire) == 0) {
        sched_yield();
    }
    for (unsigned int i = 0; i < args->iterations; i++) {
        void* handle = load_with_callback_phase(args->module_path);
        int close_result;

        if (handle == NULL) {
            atomic_fetch_add_explicit(args->failures,
                                      1,
                                      memory_order_relaxed);
            break;
        }
        close_result = dlclose(handle);
        if (close_result != 0) {
            atomic_fetch_add_explicit(args->failures,
                                      1,
                                      memory_order_relaxed);
            break;
        }
        if ((i & 7U) == 0) {
            sched_yield();
        }
    }
    atomic_fetch_sub_explicit(args->remaining, 1, memory_order_acq_rel);
    return NULL;
}

static int
test_concurrent_loader_and_drain(const PeakTestHooks* hooks,
                                 const char* module_path)
{
    enum { THREAD_COUNT = 32, ITERATIONS = 200 };
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    StressWorkerArgs args;
    pthread_t threads[THREAD_COUNT];
    gboolean created[THREAD_COUNT] = { FALSE };
    atomic_int start = 0;
    atomic_int remaining = 0;
    atomic_int worker_failures = 0;

    args.module_path = module_path;
    args.iterations = ITERATIONS;
    args.start = &start;
    args.remaining = &remaining;
    args.failures = &worker_failures;

    for (size_t i = 0; i < THREAD_COUNT; i++) {
        atomic_fetch_add_explicit(&remaining, 1, memory_order_relaxed);
        if (pthread_create(&threads[i], NULL, stress_worker, &args) == 0) {
            created[i] = TRUE;
        } else {
            atomic_fetch_sub_explicit(&remaining, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&worker_failures,
                                      1,
                                      memory_order_relaxed);
        }
    }
    atomic_store_explicit(&start, 1, memory_order_release);

    while (atomic_load_explicit(&remaining, memory_order_acquire) != 0) {
        drain_once(hooks);
        sched_yield();
    }
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        if (created[i]) {
            pthread_join(threads[i], NULL);
        }
    }

    PeakDlopenDynamicAttachDiagnostics after;
    for (unsigned int attempt = 0; attempt < 100000; attempt++) {
        drain_once(hooks);
        after = get_diagnostics(hooks);
        if (after.queue_length == 0) {
            break;
        }
        sched_yield();
    }
    after = get_diagnostics(hooks);

    check_true("concurrent dlopen/dlclose workers succeeded",
               atomic_load_explicit(&worker_failures,
                                    memory_order_relaxed) == 0);
    check_true("concurrent callbacks enqueued controller work",
               after.enqueued > before.enqueued);
    check_true("concurrent controller drained work",
               after.drained > before.drained);
    check_size("concurrent queue empty", after.queue_length, 0);
    check_ull("concurrent queue never overflowed",
              after.dropped_full,
              before.dropped_full);
    check_ull("concurrent queue-owned handles never dropped at RTLD_NOLOAD",
              after.dropped_noload,
              before.dropped_noload);
    check_ull("concurrent callbacks made no RTLD_NOLOAD call",
              atomic_load_explicit(&callback_noload_calls,
                                   memory_order_relaxed),
              0);
    check_ull("concurrent callbacks made no dlinfo call",
              atomic_load_explicit(&callback_dlinfo_calls,
                                   memory_order_relaxed),
              0);
    check_ull("concurrent drain thread made no loader open call",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              0);
    check_ull("concurrent callback phase made only outer application loads",
              atomic_load_explicit(&callback_loader_calls,
                                   memory_order_relaxed),
              (unsigned long long)THREAD_COUNT * ITERATIONS);
    check_true("concurrent fixture loaded at least once",
               atomic_load_explicit(&fixture_loads,
                                    memory_order_relaxed) > 0);
    check_ull("concurrent fixture load/unload lifetime balanced",
              atomic_load_explicit(&fixture_unloads,
                                   memory_order_relaxed),
              atomic_load_explicit(&fixture_loads,
                                   memory_order_relaxed));

    if (failures == 0) {
        printf("dlopen_loader_concurrent_stress_ok threads=%u iterations=%u enqueued=%llu drained=%llu callback_noload_calls=%llu callback_dlinfo_calls=%llu drain_loader_calls=%llu\n",
               THREAD_COUNT,
               ITERATIONS,
               after.enqueued - before.enqueued,
               after.drained - before.drained,
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&callback_dlinfo_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_loader_calls,
                                    memory_order_relaxed));
    }
    return failures == 0 ? 0 : 1;
}

static int
test_fork_pending_dlclose(const PeakTestHooks* hooks,
                          const char* module_path)
{
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    void* application_handle;
    pid_t child;
    int status = 0;

    hooks->hold_ownership_broker(TRUE);
    /*
     * This test covers PEAK's fork-child PID bypass, not Frida Gum's general
     * post-fork module-unload support. NODELETE prevents the child close from
     * emitting a module-registry unload while preserving the exact pending
     * handle match exercised by the guard.
     */
    application_handle =
        load_with_callback_phase_flags(module_path,
                                       RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    check_true("fork fixture loaded", application_handle != NULL);
    check_true("fork fixture left BORROWED handoff",
               application_handle != NULL &&
               wait_for_broker_barrier(hooks));
    if (application_handle == NULL) {
        hooks->hold_ownership_broker(FALSE);
        return 1;
    }

    child = fork();
    check_true("fork with pending ownership succeeded", child >= 0);
    if (child == 0) {
        alarm(5);
        _exit(dlclose(application_handle) == 0 ? 0 : 2);
    }
    if (child > 0) {
        check_true("fork child wait succeeded",
                   waitpid(child, &status, 0) == child);
        check_true("fork child dlclose failed open without hang",
                   WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    check_size("parent pending handoff survived child close",
               hooks->pending_ownership_count(),
               1);
    check_true("parent close transferred pending reference",
               dlclose(application_handle) == 0);
    hooks->hold_ownership_broker(FALSE);
    PeakDlopenDynamicAttachDiagnostics after =
        drain_until_empty(hooks);
    check_ull("fork parent request drained",
              after.drained,
              before.drained + 1);
    check_true("NODELETE kept fork fixture mapped after balanced closes",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 0);

    if (failures == 0) {
        puts("dlopen_loader_fork_pending_close_ok");
    }
    return failures == 0 ? 0 : 1;
}

static void*
shutdown_dynamic_attach_thread(void* opaque)
{
    ShutdownThreadArgs* args = opaque;

    args->result = args->shutdown_dynamic_attach();
    return NULL;
}

static int
test_shutdown_pending_transfer(const PeakTestHooks* hooks,
                               const char* module_path)
{
    void* application_handle;
    pthread_t shutdown_thread;
    ShutdownThreadArgs args = {
        .shutdown_dynamic_attach = hooks->shutdown_dynamic_attach,
        .result = FALSE
    };
    int create_status;

    hooks->hold_ownership_broker(TRUE);
    application_handle = load_with_callback_phase(module_path);
    check_true("shutdown fixture loaded", application_handle != NULL);
    check_true("shutdown fixture left BORROWED handoff",
               application_handle != NULL &&
               wait_for_broker_barrier(hooks));
    if (application_handle == NULL) {
        hooks->hold_ownership_broker(FALSE);
        return 1;
    }

    create_status = pthread_create(&shutdown_thread,
                                   NULL,
                                   shutdown_dynamic_attach_thread,
                                   &args);
    check_true("shutdown peer started", create_status == 0);
    if (create_status != 0) {
        hooks->hold_ownership_broker(FALSE);
        (void)dlclose(application_handle);
        return 1;
    }
    check_true("shutdown closed callback admission",
               wait_for_callback_admission(hooks, FALSE));
    check_size("shutdown kept ownership handoff pending",
               hooks->pending_ownership_count(),
               1);
    check_true("shutdown-time dlclose transferred pending reference",
               dlclose(application_handle) == 0);
    check_size("shutdown-time transfer resolved pending ownership",
               hooks->pending_ownership_count(),
               0);
    hooks->hold_ownership_broker(FALSE);
    check_true("shutdown peer joined",
               pthread_join(shutdown_thread, NULL) == 0);

    check_true("pending ownership shutdown completed", args.result);
    check_true("shutdown discarded and closed transferred reference",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 1);
    if (failures == 0) {
        puts("dlopen_loader_shutdown_pending_transfer_ok");
    }
    return failures == 0 ? 0 : 1;
}

static void*
close_handle_thread(void* opaque)
{
    CloseThreadArgs* args = opaque;

    args->result = dlclose(args->handle);
    return NULL;
}

static void*
detach_dlopen_interceptor_thread(void* opaque)
{
    DetachThreadArgs* args = opaque;

    args->result = args->detach_dlopen_interceptor();
    return NULL;
}

static int
test_dlclose_guard_revert_lifetime(const PeakTestHooks* hooks,
                                   const char* module_path)
{
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    void* application_handle = load_with_callback_phase(module_path);
    CloseThreadArgs close_args = {
        .handle = application_handle,
        .result = -1
    };
    DetachThreadArgs detach_args = {
        .detach_dlopen_interceptor = hooks->detach_dlopen_interceptor,
        .result = FALSE
    };
    pthread_t close_thread;
    pthread_t detach_thread;
    int close_created;
    int detach_created;

    check_true("guard-revert fixture loaded", application_handle != NULL);
    check_true("guard-revert broker ownership completed",
               application_handle != NULL &&
               wait_for_ownership_idle(hooks));
    PeakDlopenDynamicAttachDiagnostics after_ownership =
        get_diagnostics(hooks);
    check_ull("guard-revert callback enqueued ownership work",
              after_ownership.enqueued,
              before.enqueued + 1);
    drain_once(hooks);
    PeakDlopenDynamicAttachDiagnostics after_drain =
        get_diagnostics(hooks);
    check_ull("guard-revert ownership work drained",
              after_drain.drained,
              before.drained + 1);
    check_true("guard-revert drain left application reference loaded",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 0);
    if (application_handle == NULL) {
        return 1;
    }

    hooks->hold_dlclose_guard(TRUE);
    close_created = pthread_create(&close_thread,
                                   NULL,
                                   close_handle_thread,
                                   &close_args);
    check_true("guarded close peer started", close_created == 0);
    if (close_created != 0) {
        hooks->hold_dlclose_guard(FALSE);
        (void)dlclose(application_handle);
        return 1;
    }
    check_true("guarded close reached active-reader barrier",
               wait_for_dlclose_guard_state(hooks->dlclose_guard_waiting));

    detach_created = pthread_create(&detach_thread,
                                    NULL,
                                    detach_dlopen_interceptor_thread,
                                    &detach_args);
    check_true("dlclose revert peer started", detach_created == 0);
    if (detach_created == 0) {
        check_true("dlclose revert waited for admitted reader",
                   wait_for_dlclose_guard_state(
                       hooks->dlclose_guard_reverting));
    }
    hooks->hold_dlclose_guard(FALSE);
    check_true("guarded close peer joined",
               pthread_join(close_thread, NULL) == 0);
    if (detach_created == 0) {
        check_true("dlclose revert peer joined",
                   pthread_join(detach_thread, NULL) == 0);
    }

    check_true("admitted guarded close used valid trampoline",
               close_args.result == 0);
    check_true("dlclose guard revert completed after reader exit",
               detach_created == 0 && detach_args.result);
    check_true("guard-revert fixture unloaded exactly once",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 1);
    if (failures == 0) {
        puts("dlopen_loader_guard_revert_lifetime_ok");
    }
    return failures == 0 ? 0 : 1;
}

int
main(int argc, char** argv)
{
    if (argc != 2 ||
        (strcmp(argv[1], "ownership") != 0 &&
         strcmp(argv[1], "destructor") != 0 &&
         strcmp(argv[1], "namespace") != 0 &&
         strcmp(argv[1], "stress") != 0 &&
         strcmp(argv[1], "fork") != 0 &&
         strcmp(argv[1], "shutdown") != 0 &&
         strcmp(argv[1], "guard-revert") != 0)) {
        fprintf(stderr,
                "usage: %s ownership|destructor|namespace|stress|fork|shutdown|guard-revert\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    resolve_hook("dlopen",
                 &application_dlopen_function,
                 sizeof(application_dlopen_function));
    PeakTestHooks hooks = load_peak_test_hooks();
    hooks.set_manual_drain(TRUE);

    int result;
    if (strcmp(argv[1], "ownership") == 0) {
        result = test_callback_owned_handle(&hooks,
                                            PEAK_TEST_OWNED_MODULE);
    } else if (strcmp(argv[1], "destructor") == 0) {
        result = test_destructor_reentrant_loader(
            &hooks,
            PEAK_TEST_OWNED_MODULE);
    } else if (strcmp(argv[1], "namespace") == 0) {
        result = test_namespace_identity(&hooks,
                                         PEAK_TEST_OWNED_MODULE);
    } else if (strcmp(argv[1], "stress") == 0) {
        result = test_concurrent_loader_and_drain(&hooks,
                                                  PEAK_TEST_OWNED_MODULE);
    } else if (strcmp(argv[1], "fork") == 0) {
        result = test_fork_pending_dlclose(&hooks,
                                           PEAK_TEST_OWNED_MODULE);
    } else if (strcmp(argv[1], "shutdown") == 0) {
        result = test_shutdown_pending_transfer(&hooks,
                                                 PEAK_TEST_OWNED_MODULE);
    } else {
        result = test_dlclose_guard_revert_lifetime(
            &hooks,
            PEAK_TEST_OWNED_MODULE);
    }

    hooks.set_manual_drain(FALSE);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
