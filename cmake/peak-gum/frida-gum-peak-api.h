#ifndef PEAK_FRIDA_GUM_PEAK_API_H
#define PEAK_FRIDA_GUM_PEAK_API_H

/* PEAK extension ABI for Frida Gum PC classification. */
#define GUM_PEAK_PC_API_VERSION 1
#define GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_X86_64 0x01171503u
#define GUM_PEAK_PC_ABI_FRIDA_GUM_17_15_3_LINUX_ARM64 0x02171503u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GumPeakFunctionContext GumPeakFunctionContext;

GUM_API guint gum_interceptor_peak_abi_fingerprint(void);

#define GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION 1

/*
 * Test/diagnostic helper. Production synchronization is owned by Gum's
 * wrapped module-registry lifecycle and its dedicated worker.
 */
GUM_API gboolean gum_interceptor_peak_drain_deferred_module_sync(void);

/*
 * PEAK-only direct listener dispatch.
 *
 * Gum's generic invocation path maintains a shared trampoline counter and a
 * pthread-keyed invocation stack on every call. PEAK already owns a TLS stack
 * and stops all relevant threads before mutating a target, so its listeners
 * can use these callbacks without duplicating that machinery.
 */
typedef enum {
    GUM_PEAK_FAST_ENTER_SKIP = 0,
    GUM_PEAK_FAST_ENTER_INVOKE = 1,
    GUM_PEAK_FAST_ENTER_FALLBACK = 2
} GumPeakFastEnterResult;

typedef GumPeakFastEnterResult (* GumPeakFastEnterFunc)(
    gpointer user_data,
    gpointer function_context,
    gpointer stack_address,
    gpointer caller_return_address);
typedef gpointer (* GumPeakFastLeaveFunc)(
    gpointer user_data,
    gpointer function_context);
typedef gboolean (* GumPeakFastIsDirectLeaveFunc)(
    gpointer user_data,
    gpointer function_context);
typedef guint (* GumPeakFastActiveCountFunc)(gpointer user_data);

typedef struct _GumPeakFastListener {
    guint version;
    GumPeakFastEnterFunc on_enter;
    GumPeakFastLeaveFunc on_leave;
    GumPeakFastIsDirectLeaveFunc is_direct_leave;
    GumPeakFastActiveCountFunc active_count;
    gpointer user_data;
    GumInvocationListener * listener_instance;
    gpointer dispatch_start;
    gsize dispatch_size;
    gpointer listener_entries_cookie;
    volatile gint enabled;
    volatile gint release_required;
} GumPeakFastListener;

#define GUM_PEAK_FAST_LISTENER_VERSION 2u

GUM_API gboolean gum_interceptor_peak_enable_fast_listener(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    GumPeakFastListener * fast_listener);

/*
 * Called while PEAK's existing stop-the-world mutation guard is active.
 * Seeds Gum's deferred-destruction counter from PEAK's per-thread active
 * slots, so the steady state needs no shared atomic reference count.
 */
GUM_API gboolean gum_interceptor_peak_prepare_fast_detach(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener);

/* Releases a fast frame skipped by non-local unwind or thread exit. */
G_GNUC_INTERNAL void gum_interceptor_peak_release_fast_invocation(
    gpointer function_context);

typedef enum {
    GUM_PEAK_PC_SAFE = 0,
    GUM_PEAK_PC_AT_PATCH_ENTRY,
    GUM_PEAK_PC_IN_ENTER_TRAMPOLINE,
    GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE,
    GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE,
    GUM_PEAK_PC_IN_DISPATCH,
    GUM_PEAK_PC_UNKNOWN
} GumPeakPcState;

typedef struct _GumPeakPcDiagnostics {
    gpointer function_address;
    guint overwritten_prologue_len;
    gpointer trampoline_slice_start;
    gsize trampoline_slice_size;
    gpointer on_enter_trampoline;
    gpointer on_leave_trampoline;
    gpointer on_invoke_trampoline;
    gpointer enter_thunk_start;
    gsize enter_thunk_size;
    gpointer leave_thunk_start;
    gsize leave_thunk_size;
    gpointer fast_overlay_dispatch_start;
    gsize fast_overlay_dispatch_size;
    gpointer fast_listener_dispatch_start;
    gsize fast_listener_dispatch_size;
} GumPeakPcDiagnostics;

GUM_API gboolean gum_interceptor_peak_classify_pc(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    gpointer pc,
    GumPeakFunctionContext ** ctx,
    GumPeakPcState * state);

GUM_API gpointer gum_interceptor_peak_safe_pc(
    GumPeakFunctionContext * ctx,
    gpointer pc,
    GumPeakPcState state);

GUM_API gboolean gum_interceptor_peak_get_pc_diagnostics(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    GumPeakPcDiagnostics * diagnostics);

#define GUM_PEAK_MAX_PROLOGUE_SIZE 32u

GUM_API gboolean gum_interceptor_peak_get_function_patch(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    guint8 * active_patch,
    guint8 * original_prologue,
    guint * prologue_len);

#if defined(__x86_64__) || defined(__amd64__)
# define GUM_PEAK_EXACT_ATTACH_API_VERSION 1

/* Attach to the supplied entry without following its leading redirect. */
GUM_API GumAttachReturn gum_interceptor_peak_attach_exact(
    GumInterceptor * interceptor,
    gpointer function_address,
    GumInvocationListener * listener,
    const GumAttachOptions * options);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PEAK_FRIDA_GUM_PEAK_API_H */
