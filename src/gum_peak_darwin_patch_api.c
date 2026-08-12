/*
 * Minimal Frida Gum 17.15.3 Darwin Arm64 private-layout overlay.
 *
 * Keep this separate from PEAK's Linux Gum overlay: Darwin physical detach
 * only needs the canonical entry address and the exact active/original entry
 * bytes.  No Linux module-sync, fast-listener, or PC-classification code is
 * shared with this implementation.
 */

#include "internal/gum_peak_darwin_patch_api.h"

#include <stdint.h>
#include <string.h>

#if !defined(__APPLE__) || \
    !(defined(__aarch64__) || defined(__arm64__))
#error "The PEAK Darwin Gum patch overlay only supports macOS Arm64"
#endif

typedef guint8 PeakGumInterceptorType17;

typedef struct {
    GObject parent;
    GRecMutex mutex;
    GHashTable* function_by_address;
} PeakGumInterceptor17;

typedef struct {
    gpointer function_address;
    gpointer grafted_hook;
    gpointer import_target;
    PeakGumInterceptorType17 type;
    guint8 destroyed;
    guint8 activated;
    guint8 has_on_leave_listener;
    guint8 has_unignorable_listener;
    GumCodeSlice* trampoline_slice;
    GumCodeDeflector* trampoline_deflector;
    volatile gint trampoline_usage_counter;
    gpointer on_enter_trampoline;
    guint8* overwritten_prologue;
    guint overwritten_prologue_len;
    guint8* redirect_code;
    gpointer on_invoke_trampoline;
    gpointer on_leave_trampoline;
    volatile GPtrArray* listener_entries;
} PeakGumFunctionContext17;

typedef struct {
#ifndef GUM_DIET
    GumInvocationListenerInterface* listener_interface;
    GumInvocationListener* listener_instance;
#else
    union {
        GumInvocationListener* listener_interface;
        GumInvocationListener* listener_instance;
    };
#endif
    gpointer function_data;
    gboolean unignorable;
} PeakGumListenerEntry17;

G_STATIC_ASSERT(sizeof(gpointer) == 8);
G_STATIC_ASSERT(sizeof(guint) == 4);

static gboolean
peak_gum_darwin_context_has_listener(PeakGumFunctionContext17* context,
                                     GumInvocationListener* listener)
{
    GPtrArray* entries;

    if (listener == NULL) {
        return TRUE;
    }

    entries = (GPtrArray*)g_atomic_pointer_get(&context->listener_entries);
    if (entries == NULL) {
        return FALSE;
    }

    for (guint i = 0; i < entries->len; i++) {
        PeakGumListenerEntry17* entry = g_ptr_array_index(entries, i);

        if (entry != NULL && entry->listener_instance == listener) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_gum_darwin_context_is_entry_patch(PeakGumFunctionContext17* context,
                                       GumInvocationListener* listener)
{
    return context != NULL &&
           !context->destroyed &&
           context->activated &&
           context->function_address != NULL &&
           context->grafted_hook == NULL &&
           context->import_target == NULL &&
           context->overwritten_prologue != NULL &&
           context->overwritten_prologue_len > 0 &&
           context->overwritten_prologue_len <=
               PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE &&
           peak_gum_darwin_context_has_listener(context, listener);
}

static PeakGumFunctionContext17*
peak_gum_darwin_find_context(PeakGumInterceptor17* interceptor,
                             gpointer function_address,
                             GumInvocationListener* listener)
{
    PeakGumFunctionContext17* context = NULL;
    gpointer stripped_address = gum_strip_code_pointer(function_address);

    if (interceptor->function_by_address == NULL) {
        return NULL;
    }

    if (stripped_address != NULL) {
        context = g_hash_table_lookup(interceptor->function_by_address,
                                      stripped_address);
    }
    if (peak_gum_darwin_context_is_entry_patch(context, listener)) {
        return context;
    }

    context = NULL;
    if (listener != NULL) {
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, interceptor->function_by_address);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            PeakGumFunctionContext17* candidate = value;

            if (!peak_gum_darwin_context_is_entry_patch(candidate,
                                                        listener)) {
                continue;
            }
            if (context != NULL) {
                return NULL;
            }
            context = candidate;
        }
    }

    return context;
}

gboolean
peak_gum_darwin_get_function_patch(GumInterceptor* interceptor,
                                   gpointer function_address,
                                   GumInvocationListener* listener,
                                   gpointer* canonical_address_out,
                                   guint8* active_patch,
                                   guint8* original_prologue,
                                   guint* prologue_len)
{
    PeakGumInterceptor17* private_interceptor;
    PeakGumFunctionContext17* context;
    guint len;
    gboolean found = FALSE;

    if (interceptor == NULL || function_address == NULL || listener == NULL ||
        canonical_address_out == NULL || active_patch == NULL ||
        original_prologue == NULL || prologue_len == NULL) {
        return FALSE;
    }

    *canonical_address_out = NULL;
    *prologue_len = 0;
    private_interceptor = (PeakGumInterceptor17*)interceptor;

    g_rec_mutex_lock(&private_interceptor->mutex);
    context = peak_gum_darwin_find_context(private_interceptor,
                                           function_address,
                                           listener);
    if (context != NULL) {
        len = context->overwritten_prologue_len;
        memcpy(original_prologue, context->overwritten_prologue, len);
        memcpy(active_patch, context->function_address, len);
        *canonical_address_out = context->function_address;
        *prologue_len = len;
        found = TRUE;
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return found;
}

gboolean
peak_gum_darwin_get_canonical_address(GumInterceptor* interceptor,
                                      gpointer function_address,
                                      GumInvocationListener* listener,
                                      gpointer* canonical_address_out)
{
    PeakGumInterceptor17* private_interceptor;
    PeakGumFunctionContext17* context;
    gboolean found = FALSE;

    if (interceptor == NULL || function_address == NULL || listener == NULL ||
        canonical_address_out == NULL) {
        return FALSE;
    }

    *canonical_address_out = NULL;
    private_interceptor = (PeakGumInterceptor17*)interceptor;

    g_rec_mutex_lock(&private_interceptor->mutex);
    context = peak_gum_darwin_find_context(private_interceptor,
                                           function_address,
                                           listener);
    if (context != NULL) {
        *canonical_address_out = context->function_address;
        found = TRUE;
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return found;
}
