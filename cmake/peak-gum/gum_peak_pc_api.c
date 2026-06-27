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
#include <stdint.h>
#include <string.h>
#include "frida-gum.h"

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
