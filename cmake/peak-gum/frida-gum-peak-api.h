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
