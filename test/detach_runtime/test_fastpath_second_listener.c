#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "frida-gum.h"

static atomic_ulong second_listener_enters;
static atomic_ulong target_sink;
static atomic_bool park_requested;
static atomic_bool park_release;
static atomic_uint parked_workers;

__attribute__((noinline, used, externally_visible, visibility("default")))
uintptr_t
peak_fastpath_second_listener_target(uintptr_t value)
{
    if (atomic_load_explicit(&park_requested, memory_order_acquire)) {
        atomic_fetch_add_explicit(&parked_workers, 1,
                                  memory_order_release);
        while (!atomic_load_explicit(&park_release,
                                     memory_order_acquire)) {
            sched_yield();
        }
    }
    atomic_fetch_add_explicit(&target_sink, value + 1,
                              memory_order_relaxed);
    return value + 1;
}

static void
second_listener_on_enter(GumInvocationContext* context, gpointer user_data)
{
    (void)context;
    (void)user_data;
    atomic_fetch_add_explicit(&second_listener_enters, 1,
                              memory_order_relaxed);
}

static void*
required_symbol(const char* name)
{
    void* symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol == NULL) {
        fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
    }
    return symbol;
}

static void*
parked_worker(void* argument)
{
    (void)peak_fastpath_second_listener_target((uintptr_t)argument);
    return NULL;
}

int
main(void)
{
    typedef GumInterceptor* (*ObtainFunc)(void);
    typedef GumInvocationListener* (*MakeListenerFunc)(
        GumInvocationCallback,
        GumInvocationCallback,
        gpointer,
        GDestroyNotify);
    typedef GumAttachReturn (*AttachFunc)(
        GumInterceptor*,
        gpointer,
        GumInvocationListener*,
        const GumAttachOptions*);
    typedef void (*TransactionFunc)(GumInterceptor*);
    typedef void (*DetachFunc)(
        GumInterceptor*,
        GumInvocationListener*);
    typedef gboolean (*FlushFunc)(GumInterceptor*);
    typedef gboolean (*PrepareFastDetachFunc)(
        GumInterceptor*,
        gpointer,
        GumInvocationListener*);
    typedef gboolean (*GetDiagnosticsFunc)(
        GumInterceptor*,
        gpointer,
        GumInvocationListener*,
        GumPeakPcDiagnostics*);
    typedef gboolean (*ClassifyPcFunc)(
        GumInterceptor*,
        gpointer,
        GumInvocationListener*,
        gpointer,
        GumPeakFunctionContext**,
        GumPeakPcState*);

    for (uintptr_t i = 0; i < 8; i++) {
        (void)peak_fastpath_second_listener_target(i);
    }

    ObtainFunc obtain = (ObtainFunc)required_symbol(
        "gum_interceptor_obtain");
    MakeListenerFunc make_listener = (MakeListenerFunc)required_symbol(
        "gum_make_call_listener");
    AttachFunc attach = (AttachFunc)required_symbol(
        "gum_interceptor_attach");
    TransactionFunc begin = (TransactionFunc)required_symbol(
        "gum_interceptor_begin_transaction");
    TransactionFunc end = (TransactionFunc)required_symbol(
        "gum_interceptor_end_transaction");
    DetachFunc detach = (DetachFunc)required_symbol(
        "gum_interceptor_detach");
    FlushFunc flush = (FlushFunc)required_symbol(
        "gum_interceptor_flush");
    PrepareFastDetachFunc prepare_fast_detach =
        (PrepareFastDetachFunc)required_symbol(
            "gum_interceptor_peak_prepare_fast_detach");
    GetDiagnosticsFunc get_diagnostics =
        (GetDiagnosticsFunc)required_symbol(
            "gum_interceptor_peak_get_pc_diagnostics");
    ClassifyPcFunc classify_pc = (ClassifyPcFunc)required_symbol(
        "gum_interceptor_peak_classify_pc");
    GumInterceptor** peak_interceptor =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** peak_listeners =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** peak_addresses =
        (gpointer**)required_symbol("hook_address");
    if (obtain == NULL || make_listener == NULL || attach == NULL ||
        begin == NULL || end == NULL || detach == NULL || flush == NULL ||
        prepare_fast_detach == NULL || get_diagnostics == NULL ||
        classify_pc == NULL || peak_interceptor == NULL ||
        peak_listeners == NULL || peak_addresses == NULL ||
        *peak_interceptor == NULL || *peak_listeners == NULL ||
        *peak_addresses == NULL || (*peak_listeners)[0] == NULL ||
        (*peak_addresses)[0] == NULL) {
        return 1;
    }

    GumInterceptor* interceptor = obtain();
    GumInvocationListener* listener =
        make_listener(second_listener_on_enter, NULL, NULL, NULL);
    if (interceptor == NULL || listener == NULL) {
        fputs("failed to create second listener\n", stderr);
        return 1;
    }

    GumPeakPcDiagnostics diagnostics;
    if (!get_diagnostics(*peak_interceptor,
                         (*peak_addresses)[0],
                         (*peak_listeners)[0],
                         &diagnostics) ||
        diagnostics.fast_overlay_dispatch_start == NULL ||
        diagnostics.fast_overlay_dispatch_size == 0 ||
        diagnostics.fast_listener_dispatch_start == NULL ||
        diagnostics.fast_listener_dispatch_size == 0) {
        fputs("missing fast dispatch diagnostics\n", stderr);
        return 1;
    }
    gpointer dispatch_pcs[] = {
        diagnostics.fast_overlay_dispatch_start,
        diagnostics.fast_listener_dispatch_start,
    };
    for (size_t i = 0; i < sizeof(dispatch_pcs) / sizeof(dispatch_pcs[0]);
         i++) {
        GumPeakFunctionContext* context = NULL;
        GumPeakPcState state = GUM_PEAK_PC_UNKNOWN;
        if (!classify_pc(*peak_interceptor,
                         (*peak_addresses)[0],
                         (*peak_listeners)[0],
                         dispatch_pcs[i],
                         &context,
                         &state) ||
            context == NULL || state != GUM_PEAK_PC_IN_DISPATCH) {
            fprintf(stderr,
                    "fast dispatch PC %zu was not classified as dispatch\n",
                    i);
            return 1;
        }
    }

    enum { worker_count = 8 };
    pthread_t workers[worker_count];
    atomic_store_explicit(&park_requested, true, memory_order_release);
    atomic_store_explicit(&park_release, false, memory_order_relaxed);
    atomic_store_explicit(&parked_workers, 0, memory_order_relaxed);
    for (uintptr_t i = 0; i < worker_count; i++) {
        if (pthread_create(&workers[i], NULL, parked_worker,
                           (void*)(i + 1)) != 0) {
            fputs("failed to create parked worker\n", stderr);
            return 1;
        }
    }
    while (atomic_load_explicit(&parked_workers,
                                memory_order_acquire) != worker_count) {
        sched_yield();
    }

    /*
     * Swap Gum's listener array while every worker holds a direct PEAK frame,
     * then detach PEAK while the second listener keeps the context alive.
     * This covers both the listener-cookie invalidation race and the seeded
     * lifetime counter's last-leave path.
     */
    begin(interceptor);
    GumAttachReturn status = attach(
        interceptor,
        (gpointer)peak_fastpath_second_listener_target,
        listener,
        NULL);
    end(interceptor);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "second listener attach failed: %d\n", status);
        return 1;
    }

    if (!prepare_fast_detach(*peak_interceptor,
                             (*peak_addresses)[0],
                             (*peak_listeners)[0])) {
        fputs("fast detach preparation failed\n", stderr);
        return 1;
    }
    begin(*peak_interceptor);
    detach(*peak_interceptor, (*peak_listeners)[0]);
    end(*peak_interceptor);

    atomic_store_explicit(&park_release, true, memory_order_release);
    for (size_t i = 0; i < worker_count; i++) {
        if (pthread_join(workers[i], NULL) != 0) {
            fputs("failed to join parked worker\n", stderr);
            return 1;
        }
    }
    atomic_store_explicit(&park_requested, false, memory_order_release);
    gboolean flushed = FALSE;
    for (unsigned int attempt = 0; attempt < 10000 && !flushed; attempt++) {
        flushed = flush(*peak_interceptor);
        if (!flushed) {
            sched_yield();
        }
    }
    if (!flushed) {
        fputs("seeded direct invocations did not drain\n", stderr);
        return 1;
    }

    /*
     * Restore PEAK's listener so its normal shutdown bookkeeping still
     * matches Gum after this white-box lifetime test.
     */
    begin(*peak_interceptor);
    status = attach(*peak_interceptor,
                    (*peak_addresses)[0],
                    (*peak_listeners)[0],
                    NULL);
    end(*peak_interceptor);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "PEAK listener restore failed: %d\n", status);
        return 1;
    }

    atomic_store_explicit(&second_listener_enters, 0,
                          memory_order_relaxed);
    for (uintptr_t i = 0; i < 128; i++) {
        (void)peak_fastpath_second_listener_target(i);
    }
    unsigned long observed = atomic_load_explicit(
        &second_listener_enters, memory_order_relaxed);
    if (observed != 128) {
        fprintf(stderr,
                "second listener fan-out mismatch: observed=%lu expected=128\n",
                observed);
        return 1;
    }

    puts("fastpath_second_listener_ok");
    return 0;
}
