#ifndef __DLOPEN_INTERCEPTOR_H
#define __DLOPEN_INTERCEPTOR_H

/**
 * @file dlopen_interceptor.h
 * @brief Header file for dynamic library open interception functionality.
 *
 * Provides functions to attach and detach an interceptor for 
 * `dlopen`.
 */

#include "frida-gum.h"
#include <dlfcn.h> 
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_DLOPEN_API __attribute__((visibility("default")))
#else
#define PEAK_DLOPEN_API
#endif

typedef struct {
    unsigned long long enqueued;
    unsigned long long drained;
    unsigned long long requeued;
    unsigned long long dropped_full;
    unsigned long long dropped_closed;
    unsigned long long dropped_noload;
    unsigned long long dropped_requeue;
    unsigned long long partial_success;
    unsigned long long retained_handles;
    size_t max_depth;
    size_t queue_length;
    unsigned int capacity;
    unsigned int drain_budget;
} PeakDlopenDynamicAttachDiagnostics;

/**
 * @brief Attaches the dlopen interceptor.
 *
 * This function initializes and attaches an interceptor 
 * for the `dlopen` function. It obtains a GumInterceptor instance, 
 * locates the `dlopen` function's memory address, and replaces it with 
 * a custom implementation. 
 *
 * @return 0 on success, or an error code indicating failure.
 */
int dlopen_interceptor_attach(void);

/**
 * @brief Opens dynamic-attach admission after the controller is running.
 *
 * @return TRUE only when the interceptor and its controller are both ready.
 */
gboolean dlopen_interceptor_enable_dynamic_attach(void);

/**
 * @brief Drains queued dynamic attach work on the controller path.
 *
 * The dlopen replacement enqueues a request with a private module reference and
 * waits for that exact request to reach a terminal state. Distinct application
 * threads serialize the real load and this completion handshake, while nested
 * dlopen calls on the same thread remain reentrant. This keeps a dynamic Gum
 * mutation from stopping another dlopen path while it owns Gum loader state.
 * This function scans the loaded module and its dependency closure, then
 * attaches matching symbols under the general listener controller lock.
 */
void dlopen_interceptor_drain_dynamic_attach_queue(void);

/**
 * @brief Closes and releases dynamic attach work before listener teardown.
 *
 * After this call succeeds, `peak_dlopen` continues to forward to the real
 * dlopen but will not touch general-listener-owned state. Queued requests that
 * were not drained by the controller are cancelled, their waiters are released,
 * and their private handles are closed unless an installed hook needs them.
 *
 * @return TRUE when active dynamic attach and replacement bodies drained.
 */
gboolean dlopen_interceptor_shutdown_dynamic_attach(void);

/**
 * @brief Releases dynamic module handles retained for installed hooks.
 *
 * This must only be called after the general listener has physically detached
 * dynamic hooks and Gum teardown has flushed. Until then, PEAK keeps a retained
 * handle so application dlclose() cannot unload code that Gum still patches.
 */
void dlopen_interceptor_release_retained_dynamic_handles(void);

/**
 * @brief Copies dynamic attach queue diagnostics.
 *
 * The counters are intentionally cumulative for the process lifetime so trace
 * rows and offline diagnostics can correlate queue pressure across controller
 * drains and shutdown. A capacity of zero denotes the unbounded request queue;
 * dropped_full is retained as an ABI-compatible legacy counter and remains zero.
 */
PEAK_DLOPEN_API void dlopen_interceptor_get_dynamic_attach_diagnostics(
    PeakDlopenDynamicAttachDiagnostics* diagnostics);

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_DLOPEN_API void dlopen_interceptor_test_reset_dynamic_attach(gboolean open);
PEAK_DLOPEN_API void dlopen_interceptor_test_set_manual_drain(gboolean enabled);
PEAK_DLOPEN_API gboolean dlopen_interceptor_test_enqueue_dummy_dynamic_attach(
    const char* filename);
PEAK_DLOPEN_API gboolean dlopen_interceptor_test_enqueue_retry_dynamic_attach(
    const char* filename);
PEAK_DLOPEN_API void dlopen_interceptor_test_drain_dynamic_attach_queue(void);
PEAK_DLOPEN_API void
dlopen_interceptor_test_normal_drain_dynamic_attach_queue(void);
PEAK_DLOPEN_API void dlopen_interceptor_test_record_noload_drop(void);
PEAK_DLOPEN_API void dlopen_interceptor_test_record_requeue_drop(void);
PEAK_DLOPEN_API void
dlopen_interceptor_test_record_partial_success_with_retained_handle(void);
PEAK_DLOPEN_API void
dlopen_interceptor_test_release_retained_dynamic_handles(void);
PEAK_DLOPEN_API size_t dlopen_interceptor_test_retained_handle_slots(void);
PEAK_DLOPEN_API gboolean dlopen_interceptor_test_retryable_prepare_status(
    int status);
PEAK_DLOPEN_API void dlopen_interceptor_test_trace_counters(const char* event);
#endif

/**
 * @brief Detaches the dlopen interceptor.
 *
 * This function reverts the replacement of the intercepted `dlopen`
 * and releases the resources associated with the interceptor.
 */
gboolean dlopen_interceptor_dettach(void);

#endif /* __DLOPEN_INTERCEPTOR_H */
