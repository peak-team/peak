#ifndef PEAK_GENERAL_LISTENER_H
#define PEAK_GENERAL_LISTENER_H

/**
 * @file general_listener.h
 * @brief Define listener accounting, lifecycle requests, and final reporting.
 *
 * The implementation owns PEAK's per-target accounting and coordinates
 * controller-owned attach/detach transitions. Report transports consume an
 * immutable snapshot captured from this state.
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <semaphore.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
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

/** Lock-free report row published for exec-checkpoint snapshot capture. */
#if defined(__GNUC__) || defined(__clang__)
typedef struct __attribute__((aligned(64))) {
#else
typedef struct {
#endif
    _Atomic gulong sequence;
    _Atomic int invalid;
    gulong completed_calls;
    gdouble total_time;
    gdouble exclusive_time;
    gfloat max_time;
    gfloat min_time;
} PeakGeneralListenerCheckpointShadow;

#if defined(__GNUC__) || defined(__clang__)
_Static_assert(sizeof(PeakGeneralListenerCheckpointShadow) == 64,
               "checkpoint shadow slot must occupy one cache line");
_Static_assert(__alignof__(PeakGeneralListenerCheckpointShadow) == 64,
               "checkpoint shadow slot must be cache-line aligned");
#endif

#define PEAKGENERAL_TYPE_LISTENER (peak_general_listener_get_type())
G_DECLARE_FINAL_TYPE(PeakGeneralListener, peak_general_listener, PEAKGENERAL, LISTENER, GObject)

/** Controller-owned lifecycle state for one configured profiling target. */
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

/** Available coordination transports for final report output. */
typedef enum {
    PEAK_OUTPUT_AGGREGATION_LOCAL = 0,
    PEAK_OUTPUT_AGGREGATION_MPI = 1,
    PEAK_OUTPUT_AGGREGATION_SOCKET = 2
} PeakOutputAggregationMode;

/**
 * @struct _PeakGeneralListener
 * @brief Struct representing the Peak General Listener
 *
 * The listener extends GObject and implements GumInvocationListener. One
 * instance owns the per-thread counts and timing arrays for a hooked target.
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
    PeakGeneralListenerCheckpointShadow* checkpoint_shadow;
};

/** Adaptive heartbeat timing and control parameters. */
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
 * The function attempts to attach every resolved target that passes PEAK's Gum
 * safety policy. Unresolved targets remain eligible for later dynamic attach.
 * Attached targets record call counts and timing across tracked threads.
 *
 */
void peak_general_listener_attach();

/** Records the monotonic application start used by final overhead reporting. */
void peak_general_listener_note_runtime_start(double start_time);

/**
 * @brief Freezes final application-boundary overhead inputs.
 *
 * Called after heartbeat shutdown and after the controller has stopped and
 * joined under its existing drain/finalize policy. `peak_main_time` must
 * already be converted to elapsed application runtime. Later per-hook cleanup in
 * `peak_general_listener_dettach()` is intentionally outside this report
 * boundary.
 */
void peak_general_listener_freeze_final_report_snapshot(void);

/**
 * @brief Prints the results of the Peak General Listener.
 *
 * This function prints the results of the Peak General Listener for each function hook.
 *
 * @return TRUE when MPI aggregation completed; FALSE for socket/local output or fallback.
 */
gboolean peak_general_listener_print(PeakOutputAggregationMode aggregation_mode);

/**
 * @brief Returns whether the last print attempt poisoned PEAK's MPI reducer path.
 *
 * A TRUE result means an MPI output reducer collective failed or timed out after
 * PEAK had started MPI payload aggregation. The caller must not issue later MPI
 * calls from teardown, including returning to the real PMPI_Finalize path.
 */
gboolean peak_general_listener_mpi_reducer_failed_closed(void);

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

/**
 * @brief Records that the application has requested MPI finalization.
 *
 * After this point PEAK may continue to count already-pinned callbacks until
 * process exit, but it must not start helper-backed target hook mutations that
 * can fork after MPI/libfabric teardown has begun.
 */
void peak_general_listener_note_mpi_finalize_requested(void);

#if defined(PEAK_ENABLE_TEST_HOOKS) && defined(HAVE_MPI)
gboolean peak_general_listener_test_first_slurm_host(const char* nodelist,
                                                     char* out,
                                                     size_t out_size);

typedef struct {
    gboolean accounting_valid;
    unsigned int local_ranks;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double control_risk_seconds;
    double profile_control_risk_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    double ratio;
} PeakMpiReportTestTuple;

PEAK_API gboolean peak_general_listener_test_reduce_report_tuples(
    const PeakMpiReportTestTuple* local_tuple,
    PeakMpiReportTestTuple maximum_tuples[6],
    int owner_ranks[6]);
PEAK_API void peak_general_listener_test_print_report_tuples(
    const PeakMpiReportTestTuple maximum_tuples[6],
    const int owner_ranks[6]);
PEAK_API int peak_general_listener_test_mpi_uint64_type_size(void);
#endif

#ifdef PEAK_ENABLE_TEST_HOOKS
uint64_t peak_general_listener_test_add_uint64_saturated(uint64_t lhs,
                                                          uint64_t rhs);
PEAK_API int peak_general_listener_test_checkpoint_snapshot_lock_hold(void);
PEAK_API int peak_general_listener_test_checkpoint_snapshot_lock_release(void);
PEAK_API int peak_general_listener_test_checkpoint_mutation_begin(void);
PEAK_API void peak_general_listener_test_checkpoint_mutation_release(void);
PEAK_API int peak_general_listener_test_checkpoint_mutation_contend(void);
PEAK_API int peak_general_listener_test_checkpoint_mutation_contender_invalidated(void);
PEAK_API void peak_general_listener_test_checkpoint_busy_pause_enable(void);
PEAK_API int peak_general_listener_test_checkpoint_busy_is_held(void);
PEAK_API void peak_general_listener_test_checkpoint_busy_release(void);
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
/**
 * @brief Returns whether an internal PEAK support replacement should proceed.
 *
 * Support hooks preserve PEAK runtime hygiene and are not profiling targets;
 * Gum's own replacement result is the authority for these wrappers.
 */
gboolean peak_general_listener_support_attach_target_is_supported(
    const char* symbol_name,
    gpointer address);

/**
 * @brief Resolve a function through Frida's symbol APIs.
 *
 * This keeps PEAK on Gum's dynamic-binary lookup path for both normal and MPI
 * processes. The PEAK-patched Frida Gum devkit validates Gum's online ELF
 * memory fallback before Gum parses a memory source as an ELF object.
 */
gpointer peak_general_listener_find_function(const char* symbol);

/**
 * @brief Returns whether unresolved requested targets require dlopen rescans.
 */
gboolean peak_general_listener_needs_dynamic_attach(void);

/**
 * @brief Detaches the Peak General Listener.
 *
 * The function attempts to detach Gum listeners and release listener-owned
 * state. If teardown cannot be proven safe, it deliberately retains callback
 * state until process exit.
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
 * physical Gum hook is detached to reduce callback overhead. Its listener,
 * statistics, and lifecycle state remain available for reattachment and final
 * reporting. If reattachment is enabled and overhead falls below the
 * threshold, the listener is reattached.
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
#endif /* PEAK_GENERAL_LISTENER_H */
