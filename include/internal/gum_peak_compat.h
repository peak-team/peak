#ifndef PEAK_GUM_PEAK_COMPAT_H
#define PEAK_GUM_PEAK_COMPAT_H

#include "frida-gum.h"

/*
 * Keep the supported stock/prebuilt Gum provider on the generic invocation
 * path. Patched Gum supplies these declarations and the direct fast dispatch;
 * stock Gum deliberately gets local no-op shims and never references a PEAK
 * extension symbol at link or load time.
 */
#if !defined(GUM_PEAK_FAST_LISTENER_VERSION)
typedef enum {
    GUM_PEAK_FAST_ENTER_SKIP = 0,
    GUM_PEAK_FAST_ENTER_INVOKE = 1,
    GUM_PEAK_FAST_ENTER_FALLBACK = 2
} GumPeakFastEnterResult;

typedef GumPeakFastEnterResult (* GumPeakFastEnterFunc)(
    gpointer user_data,
    gpointer function_context,
    gpointer stack_address,
    gpointer caller_return_address,
    guint gum_stack_depth);
typedef gboolean (* GumPeakFastLeaveFunc)(
    gpointer user_data,
    gpointer function_context,
    gpointer stack_address,
    guint* gum_stack_depth,
    gpointer* caller_return_address);
typedef gboolean (* GumPeakFastIsDirectLeaveFunc)(
    gpointer user_data,
    gpointer function_context,
    gpointer stack_address);
typedef guint (* GumPeakFastActiveCountFunc)(gpointer user_data);
typedef guint (* GumPeakFastActiveCloseFunc)(gpointer user_data);
typedef void (* GumPeakFastActiveResetFunc)(gpointer user_data);
typedef gpointer (* GumPeakFastTranslateFunc)(gpointer code_address);

typedef struct _GumPeakFastListener {
    guint version;
    GumPeakFastEnterFunc on_enter;
    GumPeakFastLeaveFunc on_leave;
    GumPeakFastIsDirectLeaveFunc is_direct_leave;
    GumPeakFastActiveCountFunc active_count;
    GumPeakFastActiveCloseFunc active_close;
    GumPeakFastActiveResetFunc active_reset;
    GumPeakFastTranslateFunc translate_return_address;
    gpointer user_data;
    GumInvocationListener* listener_instance;
    gpointer dispatch_start;
    gsize dispatch_size;
    gpointer listener_entries_cookie;
    volatile gint enabled;
    volatile gint release_required;
} GumPeakFastListener;

#define GUM_PEAK_FAST_LISTENER_VERSION 0u

static inline gboolean
gum_interceptor_peak_invocation_stack_entry_matches(
    guint depth,
    gpointer function_address,
    gpointer stack_address)
{
    (void)depth;
    (void)function_address;
    (void)stack_address;
    return FALSE;
}

static inline gboolean
gum_interceptor_peak_enable_fast_listener(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    GumPeakFastListener* fast_listener)
{
    (void)interceptor;
    (void)function_address;
    (void)listener;
    (void)fast_listener;
    return FALSE;
}

static inline gboolean
gum_interceptor_peak_prepare_fast_detach(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener)
{
    (void)interceptor;
    (void)function_address;
    (void)listener;
    return FALSE;
}

static inline void
gum_interceptor_peak_release_fast_invocation(
    gpointer function_context,
    GumPeakFastListener* fast_listener)
{
    (void)function_context;
    (void)fast_listener;
}
#endif

/*
 * The unwind-frame lifecycle belongs to the patched direct-listener path.
 * Stock Gum always uses its generic invocation stack, so it has no PEAK frame
 * records to initialize, reconcile, or retire.  Keep the common lifecycle
 * call sites available without introducing extension-symbol references into
 * the prebuilt provider used on Darwin and other unsupported platforms.
 */
#if !defined(GUM_PEAK_UNWIND_FRAME_API_VERSION)
static inline gboolean
gum_interceptor_peak_unwind_lifecycle_init(void)
{
    return TRUE;
}

static inline gboolean
gum_interceptor_peak_unwind_frame_is_live(
    gpointer function_context,
    gpointer stack_address)
{
    (void)function_context;
    (void)stack_address;
    return FALSE;
}

static inline gboolean
gum_interceptor_peak_unwind_reconcile_current_thread(void)
{
    return FALSE;
}

static inline guint64
gum_interceptor_peak_unwind_thread_cookie(guintptr native_thread)
{
    (void)native_thread;
    return 0;
}

static inline void
gum_interceptor_peak_unwind_thread_joined(
    guint64 cookie,
    guintptr native_thread,
    gboolean token_valid,
    guint64 slot,
    guint64 generation)
{
    (void)cookie;
    (void)native_thread;
    (void)token_valid;
    (void)slot;
    (void)generation;
}

static inline gboolean
gum_interceptor_peak_unwind_reap_threads(void)
{
    return FALSE;
}

static inline void
gum_interceptor_peak_unwind_diagnostics(
    guint64* owners,
    guint64* blocks,
    guint64* active)
{
    if (owners != NULL) {
        *owners = 0;
    }
    if (blocks != NULL) {
        *blocks = 0;
    }
    if (active != NULL) {
        *active = 0;
    }
}
#endif

#endif /* PEAK_GUM_PEAK_COMPAT_H */
