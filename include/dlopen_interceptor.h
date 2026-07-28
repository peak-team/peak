#ifndef PEAK_DLOPEN_INTERCEPTOR_H
#define PEAK_DLOPEN_INTERCEPTOR_H

/**
 * @file dlopen_interceptor.h
 * @brief Dynamic-library load observation and deferred target attachment.
 *
 * PEAK observes the real `dlopen` entry and return with a Gum invocation
 * listener, preserving caller-sensitive loader behavior. The callback only
 * publishes a borrowed result. A loader-API-free `dlclose` guard transfers a
 * racing application reference, while a separate ownership thread otherwise
 * obtains an exact module reference. Resolved targets are then queued for
 * bounded controller-side processing. Queue entries and retained
 * dynamic-library handles are owned by this module.
 */

#include "frida-gum.h"
#include <dlfcn.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_DLOPEN_API __attribute__((visibility("default")))
#else
#define PEAK_DLOPEN_API
#endif

/* The implementation is C even when this header is consumed by C++. */
#ifdef __cplusplus
extern "C" {
#endif

/** @name Dynamic-attach diagnostics
 * @{ */

/** Snapshot of cumulative queue counters and current queue state. */
typedef struct {
    unsigned long long enqueued;         /**< Requests accepted by the queue. */
    unsigned long long drained;          /**< Requests processed by a drain. */
    unsigned long long requeued;         /**< Retryable requests queued again. */
    unsigned long long dropped_full;     /**< Requests rejected at capacity. */
    unsigned long long dropped_closed;   /**< Requests rejected after admission closed. */
    unsigned long long dropped_noload;   /**< Requests lacking an `RTLD_NOLOAD` handle. */
    unsigned long long dropped_requeue;  /**< Retry requests that could not be requeued. */
    unsigned long long partial_success;  /**< Requests with installed hooks plus retry work. */
    unsigned long long retained_handles; /**< Handle references retained cumulatively. */
    size_t max_depth;                     /**< Maximum observed queue length. */
    size_t queue_length;                  /**< Queue length at snapshot time. */
    unsigned int capacity;                /**< Fixed queue capacity in requests. */
    unsigned int drain_budget;            /**< Maximum requests handled per normal drain. */
} PeakDlopenDynamicAttachDiagnostics;

/** @} */

/** @name Listener attachment and controller admission
 * @{ */

/**
 * @brief Attaches dynamic-loader observation and ownership support.
 *
 * The function locates `dlopen` and `dlclose`, validates that Gum may mutate
 * both safely, attaches the `dlopen` call listener, installs the nonblocking
 * `dlclose` ownership guard through the detach-controller mutation protocol,
 * and then starts the ownership thread. It does not open dynamic-attach
 * admission; call `dlopen_interceptor_enable_dynamic_attach()` after the
 * general-listener target arrays have been published.
 *
 * @return `GUM_ATTACH_OK` on success; otherwise the `GumAttachReturn` produced
 *         by Gum, or `GUM_ATTACH_WRONG_SIGNATURE` when setup or safety
 *         preparation fails.  Failed setup does not publish callback
 *         admission or transfer ownership to the caller.
 */
int dlopen_interceptor_attach(void);

/**
 * @brief Enables dynamic attach from the dlopen listener.
 *
 * This should be called only after the general-listener arrays have been
 * allocated and initial hooks have been published.  Admission opens only when
 * the interceptor, hook address, and listener are all present and shutdown has
 * not started.  Otherwise the function leaves the current state unchanged.
 * The current process becomes the listener owner; fork children are not
 * admitted through the inherited listener.
 */
void dlopen_interceptor_enable_dynamic_attach(void);

/** @} */

/** @name Queue processing and shutdown
 * @{ */

/**
 * @brief Drains queued dynamic attach work on the controller path.
 *
 * Each request becomes drainable only after the ownership thread obtains an
 * exact `RTLD_NOLOAD` reference or a racing application `dlclose` transfers
 * its existing reference. The controller then resolves and attaches targets.
 * A normal call processes at most 64 requests from the queue length observed
 * at drain start. Reentrant, concurrent, callback-overlapping, unresolved,
 * closed, and empty drains are no-ops. The function consumes only
 * module-owned queue entries.
 */
void dlopen_interceptor_drain_dynamic_attach_queue(void);

/**
 * @brief Closes callback admission and releases dynamic attach work.
 *
 * The function first closes callback admission permanently for this listener
 * lifecycle, then waits up to 5,000 milliseconds each for active controller
 * drains, callbacks, and asynchronous ownership handoffs. After all become
 * idle, the ownership thread stops and queued requests plus their module-owned
 * handle references are discarded without installing new hooks. The Gum
 * listener and `dlclose` guard are not physically detached by this operation.
 *
 * @retval TRUE Active work, callbacks, and handoffs drained and the queue was
 *               discarded.
 * @retval FALSE A drain, callback, or handoff wait timed out. The controller
 *               remains in the shutting-down state and all state needed by
 *               active users is deliberately retained.
 */
gboolean dlopen_interceptor_shutdown_dynamic_attach(void);

/**
 * @brief Releases dynamic module handles retained for installed hooks.
 *
 * This must only be called after the general listener has physically detached
 * dynamic hooks and Gum teardown has flushed. Until then, PEAK keeps a retained
 * handle so application dlclose() cannot unload code that Gum still patches.
 * The function releases every module-owned retained handle reference and the
 * completed-module scan cache; it is a no-op when neither exists.
 */
void dlopen_interceptor_release_retained_dynamic_handles(void);

/**
 * @brief Copies dynamic attach queue diagnostics.
 *
 * The counters are cumulative for the process lifetime (except when reset by
 * a test hook); `queue_length` is the current value.  The caller owns @p
 * diagnostics, and the function copies a mutex-consistent snapshot into it.
 * Passing `NULL` is a no-op.
 *
 * @param[out] diagnostics Caller-owned destination for the snapshot.
 */
PEAK_DLOPEN_API void dlopen_interceptor_get_dynamic_attach_diagnostics(
    PeakDlopenDynamicAttachDiagnostics* diagnostics);

/** @} */

#ifdef PEAK_ENABLE_TEST_HOOKS
/** @name Dynamic-attach test hooks
 *
 * These entry points are exported only by test-enabled builds.  Pointer
 * arguments are borrowed for the duration of the call unless stated
 * otherwise.
 * @{ */

/**
 * Discards queued work, releases retained handles, resets counters, and
 * selects open/closed state.
 * @param[in] open `TRUE` to select open state owned by the current process;
 *                 `FALSE` to select closed state with no owner.
 */
PEAK_DLOPEN_API void dlopen_interceptor_test_reset_dynamic_attach(gboolean open);

/**
 * Suppresses normal controller drains while allowing the explicit test drain.
 * @param[in] enabled `TRUE` to suppress normal drains; `FALSE` to allow them.
 */
PEAK_DLOPEN_API void dlopen_interceptor_test_set_manual_drain(gboolean enabled);

/** @return `TRUE` when callbacks are currently admitted for this process. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_callback_is_admitted(void);

/** @return The result of the production dynamic-attach shutdown operation. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_shutdown_dynamic_attach(void);

/** @return The result of the complete production loader-hook teardown. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_dettach(void);

/**
 * Enqueues a request without a real handle; the filename is copied on success.
 * @param[in] filename Borrowed label; NULL selects the internal test label.
 * @return `TRUE` if accepted, or `FALSE` if the queue is closed or full.
 */
PEAK_DLOPEN_API gboolean dlopen_interceptor_test_enqueue_dummy_dynamic_attach(
    const char* filename);

/**
 * Enqueues a request carrying the test retry sentinel; the filename is copied.
 * @param[in] filename Borrowed label; NULL selects the internal test label.
 * @return `TRUE` if accepted, or `FALSE` if the queue is closed or full.
 */
PEAK_DLOPEN_API gboolean dlopen_interceptor_test_enqueue_retry_dynamic_attach(
    const char* filename);

/**
 * Enqueues the exact loaded module represented by an application handle and
 * waits for the ownership broker to resolve the handoff. The caller retains
 * ownership of `application_handle`.
 */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
    const char* filename,
    void* application_handle);

/** Runs a drain that bypasses manual-drain suppression for the current thread. */
PEAK_DLOPEN_API void dlopen_interceptor_test_drain_dynamic_attach_queue(void);

/** Runs the normal drain path, including manual-drain suppression. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_normal_drain_dynamic_attach_queue(void);

/** Holds/releases a controller drain after it closes callback admission. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_hold_dynamic_attach_drain(gboolean hold);

/** Holds/releases the ownership broker before it claims borrowed work. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_hold_ownership_broker(gboolean hold);

/** @return `TRUE` while the ownership broker is held by its test barrier. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_ownership_broker_waiting(void);

/** Holds/releases the ownership broker after it publishes PINNING. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_hold_ownership_pin(gboolean hold);

/** @return `TRUE` while the ownership broker is held in PINNING state. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_ownership_pin_waiting(void);

/** Holds/releases an admitted dlclose guard invocation before pass-through. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_hold_dlclose_guard(gboolean hold);

/** @return `TRUE` while a dlclose guard invocation is held by the test hook. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_dlclose_guard_waiting(void);

/** @return `TRUE` while teardown is closing dlclose guard admission. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_dlclose_guard_reverting(void);

/** @return Number of borrowed or in-progress ownership handoffs. */
PEAK_DLOPEN_API size_t
dlopen_interceptor_test_pending_ownership_count(void);

/** @return `TRUE` while a controller drain is held by the test barrier. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_dynamic_attach_drain_waiting(void);

/** @return `TRUE` while callback admission waits for a controller drain. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_callback_waiting_for_drain(void);

/** Admits one synthetic dlopen callback for controller/callback barrier tests. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_begin_callback(void);

/** Ends one synthetic callback admitted by the matching test helper. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_end_callback(void);

/** Holds/releases admitted callbacks at a test-only cancellation barrier. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_hold_callback(gboolean hold);

/** @return `TRUE` while a callback is waiting at the test-only barrier. */
PEAK_DLOPEN_API gboolean
dlopen_interceptor_test_callback_waiting(void);

/** Increments the synthetic `RTLD_NOLOAD`-drop counter. */
PEAK_DLOPEN_API void dlopen_interceptor_test_record_noload_drop(void);

/** Increments the synthetic failed-requeue counter. */
PEAK_DLOPEN_API void dlopen_interceptor_test_record_requeue_drop(void);

/** Records partial success and retains one module-owned test handle slot. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_record_partial_success_with_retained_handle(void);

/** Releases retained test handle slots through the production release path. */
PEAK_DLOPEN_API void
dlopen_interceptor_test_release_retained_dynamic_handles(void);

/** @return The current number of retained handle slots, not the cumulative counter. */
PEAK_DLOPEN_API size_t dlopen_interceptor_test_retained_handle_slots(void);

/**
 * @param[in] status Integer representation of a `PeakDetachStatus` value.
 * @return Whether @p status is classified as a retryable prepare status.
 */
PEAK_DLOPEN_API gboolean dlopen_interceptor_test_retryable_prepare_status(
    int status);

/**
 * Appends or logs a diagnostics snapshot labeled with borrowed @p event.
 * @param[in] event Borrowed event label passed to the trace implementation.
 */
PEAK_DLOPEN_API void dlopen_interceptor_test_trace_counters(const char* event);

/** @} */
#endif

/** @name Physical listener teardown
 * @{ */

/**
 * @brief Detaches dynamic-loader observation and ownership support.
 *
 * Callback admission is closed before the Gum listener is detached. The
 * operation uses the mutation safety protocol, drains dynamic work, callbacks,
 * and ownership handoffs, reverts the `dlclose` guard after all routed readers
 * exit, and requires Gum to flush before releasing listener and interceptor
 * references.
 *
 * @retval TRUE Physical detach, shutdown drain, and Gum flush all completed;
 *              listener state was released.
 * @retval FALSE Safe mutation preparation, shutdown draining, or Gum flushing
 *               failed.  State required for a safe retry or for active users
 *               is retained rather than freed.
 */
gboolean dlopen_interceptor_dettach(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_DLOPEN_INTERCEPTOR_H */
