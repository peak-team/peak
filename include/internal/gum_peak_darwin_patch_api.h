#ifndef PEAK_GUM_PEAK_DARWIN_PATCH_API_H
#define PEAK_GUM_PEAK_DARWIN_PATCH_API_H

#include "frida-gum.h"

#define PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE 32u
#define PEAK_GUM_DARWIN_STAGED_ATTACH_API_VERSION 1u

/*
 * Private-layout bridge for the pinned Frida Gum 17.15.3 Darwin Arm64
 * devkit.  The implementation is only compiled for PEAK's downloaded,
 * hash-verified devkit and fails closed for non-entry hooks.
 */
gboolean peak_gum_darwin_get_function_patch(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    gpointer* canonical_address_out,
    guint8* active_patch,
    guint8* original_prologue,
    guint* prologue_len);

/* Revalidate the saved canonical Gum context without reading its entry bytes. */
gboolean peak_gum_darwin_get_canonical_address_exact(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    gpointer* canonical_address_out);

/* Preparation runs with peers running and never changes image entry bytes.
 * Success holds the interceptor lock on the calling thread. The controller
 * must finish on that same thread, after resuming all peers, on every path.
 * Only an exact, previously uninstrumented entry is accepted. */
gboolean peak_gum_darwin_staged_attach_supported(void);
GumAttachReturn peak_gum_darwin_prepare_attach(
    GumInterceptor* interceptor, gpointer function_address,
    GumInvocationListener* listener, const GumAttachOptions* options);
GumReplaceReturn peak_gum_darwin_prepare_replace(
    GumInterceptor* interceptor, gpointer function_address,
    gpointer replacement_function, gpointer* original_function,
    const GumReplaceOptions* options);
gboolean peak_gum_darwin_get_staged_patch(
    GumInterceptor* interceptor, gpointer* canonical_address_out,
    guint8* active_patch, guint8* original_prologue, guint* prologue_len);
void peak_gum_darwin_finish_staged_mutation(
    GumInterceptor* interceptor, gboolean committed);
/* Compatibility spelling for attach-only callers. */
void peak_gum_darwin_finish_attach(GumInterceptor* interceptor,
    GumInvocationListener* listener, gboolean committed);

#endif /* PEAK_GUM_PEAK_DARWIN_PATCH_API_H */
