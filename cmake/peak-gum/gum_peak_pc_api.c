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
#include <stdint.h>
#include <string.h>
#include "frida-gum.h"

#define PEAK_GUM_FAST_DISPATCH_SECTION \
    __attribute__((section("peak_gum_fast_dispatch"), noinline, noclone))

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

typedef guint8 PeakGumInterceptorType17;

typedef struct _PeakGumInterceptorBackend17 PeakGumInterceptorBackend17;
typedef struct _PeakGumFunctionContext17 PeakGumFunctionContext17;
typedef union _PeakGumFunctionContextBackendData17 PeakGumFunctionContextBackendData17;
#if defined(__x86_64__) || defined(__amd64__)
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


G_STATIC_ASSERT(sizeof(PeakGumFunctionContextBackendData17) == 3 * GLIB_SIZEOF_VOID_P);
#if defined(__x86_64__) || defined(__amd64__)
G_STATIC_ASSERT(sizeof(PeakGumX86Relocator17) >= 8 * GLIB_SIZEOF_VOID_P);
#elif defined(__aarch64__)
G_STATIC_ASSERT(sizeof(PeakGumArm64Relocator17) >= 8 * GLIB_SIZEOF_VOID_P);
G_STATIC_ASSERT(sizeof(PeakGumArm64FunctionContextData17) <=
                sizeof(PeakGumFunctionContextBackendData17));
#endif

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
        fast_listener->active_count == NULL) {
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
         context->write_redirect_data == fast_listener)) {
        g_atomic_pointer_set(
            &fast_listener->listener_entries_cookie,
            g_atomic_pointer_get(&context->listener_entries));
        g_atomic_int_set(&fast_listener->release_required, 0);
        g_atomic_pointer_set(&context->write_redirect_data, fast_listener);
        g_atomic_int_set(&fast_listener->enabled, 1);
        enabled = TRUE;
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return enabled;
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
        active = fast_listener->active_count(fast_listener->user_data);
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
gum_interceptor_peak_release_fast_invocation(gpointer function_context)
{
    PeakGumFunctionContext17 * context = function_context;
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener(context);

    if (context != NULL && fast_listener != NULL &&
        g_atomic_int_get(&fast_listener->release_required) != 0) {
        if (!context->destroyed &&
            g_atomic_int_get(&fast_listener->enabled) == 0 &&
            fast_listener->active_count(fast_listener->user_data) == 0) {
            g_atomic_pointer_compare_and_exchange(
                &context->write_redirect_data, fast_listener, NULL);
        }
        g_atomic_int_dec_and_test(&context->trampoline_usage_counter);
    }
}

gboolean PEAK_GUM_FAST_DISPATCH_SECTION
_gum_function_context_begin_invocation(
    PeakGumFunctionContext17 * function_ctx,
    GumCpuContext * cpu_context,
    gpointer * caller_ret_addr,
    gpointer * next_hop)
{
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener_for_enter(function_ctx);

    if (G_LIKELY(fast_listener != NULL)) {
        gpointer stack_address;
        gpointer return_address = *caller_ret_addr;
        GumPeakFastEnterResult result;

#if defined(__x86_64__) || defined(__amd64__)
        stack_address = (gpointer)(uintptr_t)cpu_context->rsp;
#elif defined(__aarch64__)
        stack_address = (gpointer)(uintptr_t)cpu_context->sp;
#else
# error "Unsupported PEAK Gum fast-listener architecture"
#endif

        result = fast_listener->on_enter(fast_listener->user_data,
                                         function_ctx,
                                         stack_address,
                                         return_address);
        if (G_UNLIKELY(result == GUM_PEAK_FAST_ENTER_FALLBACK)) {
            return _gum_function_context_begin_invocation_peak_original(
                function_ctx, cpu_context, caller_ret_addr, next_hop);
        }
        *next_hop = function_ctx->on_invoke_trampoline;
        if (result == GUM_PEAK_FAST_ENTER_INVOKE) {
            *caller_ret_addr = function_ctx->on_leave_trampoline;
        }
        return result == GUM_PEAK_FAST_ENTER_INVOKE;
    }

    return _gum_function_context_begin_invocation_peak_original(
        function_ctx, cpu_context, caller_ret_addr, next_hop);
}

void PEAK_GUM_FAST_DISPATCH_SECTION
_gum_function_context_end_invocation(
    PeakGumFunctionContext17 * function_ctx,
    GumCpuContext * cpu_context,
    gpointer * next_hop)
{
    GumPeakFastListener * fast_listener =
        peak_gum_context_fast_listener(function_ctx);

    if (G_LIKELY(fast_listener != NULL) &&
        fast_listener->is_direct_leave(fast_listener->user_data,
                                       function_ctx)) {
        gboolean release_required =
            g_atomic_int_get(&fast_listener->release_required) != 0;
        gpointer return_address =
            fast_listener->on_leave(fast_listener->user_data, function_ctx);
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
