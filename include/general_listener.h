#ifndef __GENERAL_LISTENER_H
#define __GENERAL_LISTENER_H

/**
 * @file general_listener.h
 * @brief Peak General Listener header file
 *
 * This header file defines the Peak General Listener and State structs and their associated functions.
 * It also contains the main entrance of the library for interception.
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <semaphore.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

#ifdef HAVE_MPI
#include <mpi.h>
#endif

#include "utils/utils.h"
#include "utils/cxx_utils.h"

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_API __attribute__((visibility("default")))
#else
#define PEAK_API
#endif

typedef struct _PeakGeneralListener PeakGeneralListener;

#define PEAKGENERAL_TYPE_LISTENER (peak_general_listener_get_type())
G_DECLARE_FINAL_TYPE(PeakGeneralListener, peak_general_listener, PEAKGENERAL, LISTENER, GObject)

typedef enum {
    PEAK_HOOK_UNRESOLVED = 0,
    PEAK_HOOK_ATTACHED,
    PEAK_HOOK_DETACH_REQUESTED,
    PEAK_HOOK_DETACHING,
    PEAK_HOOK_DETACHED,
    PEAK_HOOK_REATTACH_REQUESTED,
    PEAK_HOOK_REATTACHING,
    PEAK_HOOK_SHUTDOWN
} PeakHookState;

typedef enum {
    PEAK_OUTPUT_AGGREGATION_LOCAL = 0,
    PEAK_OUTPUT_AGGREGATION_MPI = 1,
    PEAK_OUTPUT_AGGREGATION_SOCKET = 2
} PeakOutputAggregationMode;

/**
 * @struct _PeakGeneralListener
 * @brief Struct representing the Peak General Listener
 *
 * This struct represents the Peak General Listener, which extends GObject and implements the GumInvocationListener interface.
 * It keeps track of the total time and number of calls for each hooked function.
 */
struct _PeakGeneralListener {
    GObject parent;

    size_t hook_id;
    gulong* num_calls;
    gdouble* total_time;
    gdouble* exclusive_time;
    gfloat* max_time;
    gfloat* min_time;
    gboolean* target_thread_called;
};

typedef struct {
    unsigned int heartbeat_time;
    unsigned int check_interval;
    unsigned int hb_min_us;
    unsigned int hb_max_us;
    double hb_k_err;
    double hb_k_rate;
    double hb_ema_a;
} PeakHeartbeatArgs;

/**
 * @brief Attaches the Peak General Listener.
 *
 * This function attaches the Peak General Listener to the function hooks specified
 * in `peak_hook_strings`. It will record the number of times each function is called as
 * well as its total execution time in seconds. The time spent in multiple threads 
 * will be summed up.
 *
 * @return void
 */
void peak_general_listener_attach();

/**
 * @brief Prints the results of the Peak General Listener.
 *
 * This function prints the results of the Peak General Listener for each function hook.
 *
 * @return TRUE when MPI aggregation completed; FALSE for socket/local output or fallback.
 */
gboolean peak_general_listener_print(PeakOutputAggregationMode aggregation_mode);

/**
 * @brief Makes still-pinned listener callbacks pass through without accounting.
 *
 * PEAK uses this as soon as it enters the application PMPI_Finalize path.
 * The target entry bytes and Gum listener objects may intentionally remain
 * pinned until process exit, but callbacks must not mutate PEAK accounting,
 * request detach, or keep heartbeat work alive while MPI/PEAK teardown is
 * draining.
 */
void peak_general_listener_suspend_callbacks(void);

#if defined(PEAK_ENABLE_TEST_HOOKS) && defined(HAVE_MPI)
gboolean peak_general_listener_test_first_slurm_host(const char* nodelist,
                                                     char* out,
                                                     size_t out_size);
#endif

/**
 * @brief Frees per-listener statistics arrays before releasing the GObject.
 */
void peak_general_listener_free(PeakGeneralListener* self);

/**
 * @brief Returns whether a target may be attached through Gum safely.
 *
 * This is a fail-closed PEAK-side guard for Gum relocation patterns that are
 * known to change application semantics.
 */
gboolean peak_general_listener_attach_target_is_supported(const char* symbol_name,
                                                          gpointer address);
gboolean peak_general_listener_support_attach_target_is_supported(
    const char* symbol_name,
    gpointer address);

/**
 * @brief Returns whether unresolved requested targets require dlopen rescans.
 */
gboolean peak_general_listener_needs_dynamic_attach(void);

/**
 * @brief Detaches the Peak General Listener.
 *
 * This function detaches the Peak General Listener and frees the memory allocated for it.
 *
 * @return TRUE when Gum teardown flushed and listener-owned state was freed.
 *         FALSE means PEAK intentionally left callback state alive to avoid
 *         freeing data that in-flight Gum trampolines may still reference.
 */
gboolean peak_general_listener_dettach();

/**
 * @brief Stops the background target-hook controller worker.
 *
 * This is separated from detach so final statistics can be printed after
 * pending target hook state transitions have stopped.
 */
PEAK_API void peak_general_listener_controller_stop(void);

/**
 * @brief Processes pending detach/reattach requests for a bounded interval.
 *
 * Returns TRUE when there are no pending target-hook lifecycle requests left.
 * A FALSE result means the caller should keep teardown conservative because at
 * least one transition could not be proven safe before the deadline.
 */
PEAK_API gboolean peak_general_listener_controller_drain(unsigned int timeout_ms);

/**
 * @brief Acquires the general listener controller lock.
 *
 * Dynamic attach uses this to serialize Gum lifecycle mutation with heartbeat
 * detach/reattach until the full controller queue is available.
 */
void peak_general_listener_controller_lock(void);

/**
 * @brief Releases the general listener controller lock.
 */
void peak_general_listener_controller_unlock(void);

/**
 * @brief Wakes the general listener controller worker.
 *
 * Hot callbacks and dynamic dlopen enqueue paths use this to request that the
 * controller thread process pending Gum lifecycle work outside the callback.
 */
void peak_general_listener_controller_wake(void);

/**
 * @brief Marks a dynamically published hook attached.
 *
 * The caller must hold the general listener controller lock.
 */
void peak_general_listener_controller_mark_attached_unlocked(size_t hook_id);

/**
 * @brief Requests controller-owned physical detach for a hooked function.
 *
 * This function records a state transition request. The controller execution
 * path decides when and how to perform Gum lifecycle mutation.
 *
 * @param hook_id Index of the hooked function.
 * @return TRUE if the request is accepted or already pending.
 */
PEAK_API gboolean peak_general_listener_request_detach(size_t hook_id);

/**
 * @brief Requests controller-owned reattach for a detached hooked function.
 *
 * This function records a state transition request. The controller execution
 * path decides when and how to perform Gum lifecycle mutation.
 *
 * @param hook_id Index of the hooked function.
 * @return TRUE if the request is accepted or already pending.
 */
PEAK_API gboolean peak_general_listener_request_reattach(size_t hook_id);

/**
 * @brief Returns the current controller-facing state for a hooked function.
 *
 * @param hook_id Index of the hooked function.
 * @return Current hook state, or PEAK_HOOK_UNRESOLVED for invalid/unpublished hooks.
 */
PEAK_API PeakHookState peak_general_listener_hook_state(size_t hook_id);

/**
 * @brief Monitors the heartbeat of the Peak profiling system.
 *
 * This function periodically checks the profiling overhead and dynamically 
 * adjusts the attachment or detachment of hooks based on the profiling ratio. 
 * If the profiling overhead exceeds a target threshold, the corresponding 
 * listener is detached to reduce resource consumption. If reattachment is 
 * enabled and the overhead falls below the threshold, the listener is reattached.
 *
 * The function runs in a separate thread and continuously monitors the profiling 
 * activity, adjusting accordingly until the monitoring process is stopped.
 *
 * @param arg A pointer to the arguments structure containing heartbeat settings.
 * @return NULL when the monitoring thread exits.
 */
void* peak_heartbeat_monitor(void* arg);
extern _Atomic gboolean heartbeat_running;
extern pthread_mutex_t heartbeat_mutex;
extern pthread_cond_t heartbeat_cond;
#endif /* __GENERAL_LISTENER_H */
