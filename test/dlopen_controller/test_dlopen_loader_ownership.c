#define _GNU_SOURCE
#define PEAK_ENABLE_TEST_HOOKS 1
#include "dlopen_interceptor.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* (*DlopenFunction)(const char*, int);
typedef void* (*DlmopenFunction)(Lmid_t, const char*, int);
typedef void (*SetManualDrainFunction)(gboolean);
typedef void (*DrainFunction)(void);
typedef void (*GetDiagnosticsFunction)(
    PeakDlopenDynamicAttachDiagnostics*);
typedef gboolean (*EnqueueLoadedFunction)(const char*, void*);
typedef gboolean (*BeginCallbackFunction)(void);
typedef void (*EndCallbackFunction)(void);

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
    BeginCallbackFunction begin_callback;
    EndCallbackFunction end_callback;
} PeakTestHooks;

typedef struct {
    const char* module_path;
    unsigned int iterations;
    EnqueueLoadedFunction enqueue_loaded;
    atomic_int* start;
    atomic_int* remaining;
    atomic_int* failures;
} StressWorkerArgs;

static pthread_once_t real_loader_once = PTHREAD_ONCE_INIT;
static DlopenFunction real_dlopen_function;
static DlmopenFunction real_dlmopen_function;
static _Thread_local LoaderPhase loader_phase = LOADER_PHASE_IDLE;
static atomic_ullong callback_loader_calls;
static atomic_ullong callback_noload_calls;
static atomic_ullong drain_loader_calls;
static atomic_ullong drain_noload_calls;
static atomic_ullong destructor_loader_calls;
static atomic_ullong destructor_noload_calls;
static atomic_uint destructor_loader_enabled;
static atomic_uint destructor_loader_successes;
static atomic_uint destructor_enqueue_successes;
static atomic_uint fixture_loads;
static atomic_uint fixture_unloads;
static EnqueueLoadedFunction destructor_enqueue_loaded;
static BeginCallbackFunction destructor_begin_callback;
static EndCallbackFunction destructor_end_callback;
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
    gboolean callback_admitted = FALSE;
    gboolean enqueued = FALSE;

    if (atomic_load_explicit(&destructor_loader_enabled,
                             memory_order_acquire) == 0) {
        return;
    }

    previous_phase = loader_phase;
    loader_phase = LOADER_PHASE_DESTRUCTOR;
    handle = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
    if (handle != NULL) {
        atomic_fetch_add_explicit(&destructor_loader_successes,
                                  1,
                                  memory_order_relaxed);
        if (destructor_begin_callback != NULL &&
            destructor_end_callback != NULL &&
            destructor_enqueue_loaded != NULL) {
            callback_admitted = destructor_begin_callback();
            if (callback_admitted) {
                enqueued = destructor_enqueue_loaded("libm.so.6", handle);
                destructor_end_callback();
            }
        }
        if (enqueued) {
            atomic_fetch_add_explicit(&destructor_enqueue_successes,
                                      1,
                                      memory_order_relaxed);
        }
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
    resolve_hook("dlopen_interceptor_test_begin_callback",
                 &hooks.begin_callback,
                 sizeof(hooks.begin_callback));
    resolve_hook("dlopen_interceptor_test_end_callback",
                 &hooks.end_callback,
                 sizeof(hooks.end_callback));
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

static void*
load_with_callback_phase(const char* path)
{
    void* handle;

    loader_phase = LOADER_PHASE_CALLBACK;
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    loader_phase = LOADER_PHASE_IDLE;
    return handle;
}

static int
test_callback_owned_handle(const PeakTestHooks* hooks, const char* module_path)
{
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    void* application_handle;

    application_handle = load_with_callback_phase(module_path);
    check_true("ownership fixture loaded", application_handle != NULL);
    if (application_handle == NULL) {
        return 1;
    }
    /*
     * Run the exact production pin/identity enqueue helper at the point where
     * a Gum on-leave callback still owns the returned application handle.
     * A real listener callback may already have enqueued the same identity;
     * the helper must safely coalesce that case.
     */
    loader_phase = LOADER_PHASE_CALLBACK;
    check_true("callback-time loaded handle enqueue succeeded",
               hooks->enqueue_loaded(module_path, application_handle));
    loader_phase = LOADER_PHASE_IDLE;

    PeakDlopenDynamicAttachDiagnostics queued = get_diagnostics(hooks);
    check_ull("callback enqueued one owned request",
              queued.enqueued,
              before.enqueued + 1);
    check_size("callback left one request queued",
               queued.queue_length,
               before.queue_length + 1);
    check_true("fixture constructor ran",
               atomic_load_explicit(&fixture_loads,
                                    memory_order_relaxed) == 1);
    check_true("callback acquired a queue-owned RTLD_NOLOAD reference",
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed) > 0);

    check_true("application dlclose succeeded",
               dlclose(application_handle) == 0);
    check_true("queue-owned handle kept module loaded after application dlclose",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 0);

    unsigned long long drain_calls_before =
        atomic_load_explicit(&drain_loader_calls, memory_order_relaxed);
    unsigned long long drain_noload_before =
        atomic_load_explicit(&drain_noload_calls, memory_order_relaxed);
    drain_once(hooks);

    PeakDlopenDynamicAttachDiagnostics after = get_diagnostics(hooks);
    check_ull("owned request drained", after.drained, before.drained + 1);
    check_size("owned queue empty after drain", after.queue_length, 0);
    check_ull("owned request did not report RTLD_NOLOAD drop",
              after.dropped_noload,
              before.dropped_noload);
    check_ull("controller drain made no dlopen or dlmopen call",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              drain_calls_before);
    check_ull("controller drain made no RTLD_NOLOAD call",
              atomic_load_explicit(&drain_noload_calls,
                                   memory_order_relaxed),
              drain_noload_before);
    check_true("drain released queue-owned module handle",
               atomic_load_explicit(&fixture_unloads,
                                    memory_order_relaxed) == 1);

    if (failures == 0) {
        printf("dlopen_loader_ownership_ok callback_loader_calls=%llu callback_noload_calls=%llu drain_loader_calls=%llu drain_noload_calls=%llu\n",
               atomic_load_explicit(&callback_loader_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_loader_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_noload_calls,
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
    loader_phase = LOADER_PHASE_CALLBACK;
    check_true("destructor fixture exact handle enqueued",
               hooks->enqueue_loaded(module_path, application_handle));
    loader_phase = LOADER_PHASE_IDLE;
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
    check_ull("fixture destructor crossed the callback barrier and enqueued",
              atomic_load_explicit(&destructor_enqueue_successes,
                                   memory_order_relaxed),
              1);
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

    drain_once(hooks);
    PeakDlopenDynamicAttachDiagnostics after_second = get_diagnostics(hooks);
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
    check_true("namespace pins traversed the loader-call detector",
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed) > 0);

    for (size_t i = 0; i < 2; i++) {
        if (application_handles[i] != NULL) {
            check_true("namespace application handle closed",
                       dlclose(application_handles[i]) == 0);
        }
    }

    unsigned long long drain_calls_before =
        atomic_load_explicit(&drain_loader_calls, memory_order_relaxed);
    drain_once(hooks);
    PeakDlopenDynamicAttachDiagnostics after = get_diagnostics(hooks);
    check_ull("both namespace requests drained",
              after.drained,
              before.drained + 2);
    check_size("namespace queue empty after drain", after.queue_length, 0);
    check_ull("namespace drain made no loader open call",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              drain_calls_before);

    if (failures == 0) {
        printf("dlopen_loader_namespace_identity_ok enqueued_delta=%llu drained_delta=%llu callback_noload_calls=%llu drain_loader_calls=%llu\n",
               after.enqueued - before.enqueued,
               after.drained - before.drained,
               atomic_load_explicit(&callback_noload_calls,
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
        gboolean enqueued;
        int close_result;

        if (handle == NULL) {
            atomic_fetch_add_explicit(args->failures,
                                      1,
                                      memory_order_relaxed);
            break;
        }
        /*
         * Exercise the exact production pin/identity enqueue helper even if a
         * platform's Gum listener does not dispatch callbacks on every worker
         * thread. The application still closes its own handle immediately.
         */
        loader_phase = LOADER_PHASE_CALLBACK;
        enqueued = args->enqueue_loaded(args->module_path, handle);
        loader_phase = LOADER_PHASE_IDLE;
        close_result = dlclose(handle);
        if (!enqueued || close_result != 0) {
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
    enum { THREAD_COUNT = 8, ITERATIONS = 250 };
    PeakDlopenDynamicAttachDiagnostics before = get_diagnostics(hooks);
    StressWorkerArgs args;
    pthread_t threads[THREAD_COUNT];
    gboolean created[THREAD_COUNT] = { FALSE };
    atomic_int start = 0;
    atomic_int remaining = 0;
    atomic_int worker_failures = 0;

    args.module_path = module_path;
    args.iterations = ITERATIONS;
    args.enqueue_loaded = hooks->enqueue_loaded;
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
    check_true("concurrent pins traversed the loader-call detector",
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed) > 0);
    check_ull("concurrent drain thread made no loader open call",
              atomic_load_explicit(&drain_loader_calls,
                                   memory_order_relaxed),
              0);
    check_true("concurrent fixture loaded at least once",
               atomic_load_explicit(&fixture_loads,
                                    memory_order_relaxed) > 0);
    check_ull("concurrent fixture load/unload lifetime balanced",
              atomic_load_explicit(&fixture_unloads,
                                   memory_order_relaxed),
              atomic_load_explicit(&fixture_loads,
                                   memory_order_relaxed));

    if (failures == 0) {
        printf("dlopen_loader_concurrent_stress_ok threads=%u iterations=%u enqueued=%llu drained=%llu callback_noload_calls=%llu drain_loader_calls=%llu\n",
               THREAD_COUNT,
               ITERATIONS,
               after.enqueued - before.enqueued,
               after.drained - before.drained,
               atomic_load_explicit(&callback_noload_calls,
                                    memory_order_relaxed),
               atomic_load_explicit(&drain_loader_calls,
                                    memory_order_relaxed));
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
         strcmp(argv[1], "stress") != 0)) {
        fprintf(stderr,
                "usage: %s ownership|destructor|namespace|stress\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    PeakTestHooks hooks = load_peak_test_hooks();
    destructor_enqueue_loaded = hooks.enqueue_loaded;
    destructor_begin_callback = hooks.begin_callback;
    destructor_end_callback = hooks.end_callback;
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
    } else {
        result = test_concurrent_loader_and_drain(&hooks,
                                                  PEAK_TEST_OWNED_MODULE);
    }

    hooks.set_manual_drain(FALSE);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
