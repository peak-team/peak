#ifndef PEAK_GUM_PEAK_DARWIN_PATCH_API_H
#define PEAK_GUM_PEAK_DARWIN_PATCH_API_H

#include "frida-gum.h"

#define PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE 32u

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

/* Revalidate a live Gum context without reading its current entry bytes. */
gboolean peak_gum_darwin_get_canonical_address(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    gpointer* canonical_address_out);

#endif /* PEAK_GUM_PEAK_DARWIN_PATCH_API_H */
