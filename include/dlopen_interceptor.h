#ifndef __DLOPEN_INTERCEPTOR_H
#define __DLOPEN_INTERCEPTOR_H

/**
 * @file dlopen_interceptor.h
 * @brief Header file for dynamic library load observation functionality.
 *
 * Provides functions to observe `dlopen` without replacing its call path.
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
 * @brief Attaches the dlopen invocation listener.
 *
 * This function observes the real `dlopen` entry and return so caller-sensitive
 * loader behavior remains unchanged.
 *
 * @return 0 on success, or an error code indicating failure.
 */
int dlopen_interceptor_attach(void);

/**
 * @brief Enables dynamic attach from the dlopen listener.
 *
 * This should be called only after the general listener arrays have been
 * allocated and initial hooks have been published.
 */
void dlopen_interceptor_enable_dynamic_attach(void);

/**
 * @brief Drains queued dynamic attach work on the controller path.
 *
 * On Linux with `RTLD_NOLOAD`, runtime FFTW exports are attached before
 * `dlopen` returns. Other unresolved targets, and FFTW when that synchronous
 * path is unavailable, use this controller-drained fallback queue.
 */
void dlopen_interceptor_drain_dynamic_attach_queue(void);

/**
 * @brief Closes and releases dynamic attach work before listener teardown.
 *
 * Queued handles that were not drained by the controller are released without
 * attaching new hooks.
 *
 * @return TRUE when active dynamic attach work and listener callbacks drained.
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
 * drains and shutdown.
 */
PEAK_DLOPEN_API void dlopen_interceptor_get_dynamic_attach_diagnostics(
    PeakDlopenDynamicAttachDiagnostics* diagnostics);

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_DLOPEN_API void dlopen_interceptor_test_reset_dynamic_attach(gboolean open);
PEAK_DLOPEN_API void dlopen_interceptor_test_set_manual_drain(gboolean enabled);
PEAK_DLOPEN_API void
dlopen_interceptor_test_force_sync_prepare_timeout_once(void);
PEAK_DLOPEN_API unsigned long long
dlopen_interceptor_test_sync_scan_count(void);
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_callback_is_admitted(void);
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
 * @brief Detaches the dlopen invocation listener.
 *
 * This function stops observing `dlopen` and releases listener resources.
 */
gboolean dlopen_interceptor_dettach(void);

#endif /* __DLOPEN_INTERCEPTOR_H */
