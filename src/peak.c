#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_MPI
#include <mpi.h>
#include "internal/mpi_teardown_guard.h"
#include "mpi_interceptor.h"
#endif

#ifdef HAVE_CUDA
#include "cuda_interceptor.h"
#endif

#include "general_listener.h"
#include "detach_controller.h"
#include "exec_interceptor.h"
#include "internal/exec_interceptor_internal.h"
#include "internal/general_listener_internal.h"
#if defined(__APPLE__)
#include "internal/general_listener/attach_policy.h"
#endif
#include "internal/general_listener/report_snapshot.h"
#include "internal/general_listener/runtime_config.h"
#include "internal/general_listener/output_identity.h"
#include "internal/jit_provider.h"
#include "logging.h"
#include "pthread_listener.h"
#include "dlopen_interceptor.h"
#include "malloc_interceptor.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"
#include "utils/target_config.h"
#include "utils/utils.h"

#define PEAK_TARGET_ENV                        "PEAK_TARGET"
#define PEAK_TARGET_FILE_ENV                   "PEAK_TARGET_FILE"
#define PEAK_TARGET_GROUP_ENV                  "PEAK_TARGET_GROUP"
#define PEAK_GPU_TARGET_ENV                    "PEAK_GPU_TARGET"
#define PEAK_GPU_TARGET_FILE_ENV               "PEAK_GPU_TARGET_FILE"
#define PEAK_GPU_MONITOR_ALL                   "PEAK_GPU_MONITOR_ALL"
#define PEAK_NAME_TRUNCATE                     "PEAK_NAME_TRUNCATE"
#define PEAK_TARGET_DELIM                     ','
#define PEAK_ENABLE_PER_TARGET_HEARTBEAT_ENV   "PEAK_ENABLE_PER_TARGET_HEARTBEAT"
#define PEAK_ENABLE_GLOBAL_HEARTBEAT_ENV       "PEAK_ENABLE_GLOBAL_HEARTBEAT"
#define PEAK_ENABLE_REATTACH_ENV               "PEAK_ENABLE_REATTACH"
#define PEAK_MAX_NUM_THREADS_ENV               "PEAK_MAX_NUM_THREADS"
/* Each target owns a 64-byte hot slot (plus optional checkpoint storage).
 * Keep the process-wide knob bounded so large target groups cannot turn an
 * accidental environment value into multi-gigabyte listener mappings. */
#define PEAK_MAX_NUM_THREADS_LIMIT             4096UL
#define PEAK_MEMORY_PROFILE                    "PEAK_MEMORY_PROFILE"
#define PEAK_MEMORY_TRACK_ALL                  "PEAK_MEMORY_TRACK_ALL"
#define PEAK_OUTPUT_AGGREGATION_ENV            "PEAK_OUTPUT_AGGREGATION"
#define PEAK_MPI_COLLECTIVE_OUTPUT_ENV         "PEAK_MPI_COLLECTIVE_OUTPUT"
#define PEAK_MPI_ACTIVATION_POLICY_ENV          "PEAK_MPI_ACTIVATION_POLICY"
#define PEAK_MPI_REAL_FINALIZE_ENV             "PEAK_MPI_REAL_FINALIZE"
#ifdef PEAK_ENABLE_TEST_HOOKS
#define PEAK_TEST_MPI_LIBRARY_VERSION_ENV      "PEAK_TEST_MPI_LIBRARY_VERSION"
#endif
#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)


gboolean* peak_need_detach;
gboolean* peak_detached;
gdouble* heartbeat_overhead;
gboolean** peak_target_thread_called;
static size_t peak_target_thread_called_count;
PeakHeartbeatArgs* args;
extern _Atomic gboolean heartbeat_running;
pthread_t heartbeat_thread;
static _Atomic int peak_heartbeat_started = 0;
PEAK_API size_t peak_hook_address_count;
unsigned int heartbeat_time;
unsigned int check_interval;
unsigned long long sig_stop_ack_wait_interval;
unsigned long long sig_cont_wait_interval;
float target_profile_ratio;
float global_target_ratio;
float peak_global_reattach_factor;
float peak_global_detach_factor;
bool enable_per_target_heartbeat;
bool enable_global_heartbeat;
bool enable_reattach;
unsigned int hb_min_us;
unsigned int hb_max_us;
double hb_k_err;
double hb_k_rate;
double hb_ema_a;
size_t peak_gpu_hook_address_count;
char** peak_hook_strings;
char** peak_gpu_hook_strings;
gulong peak_max_num_threads;
double peak_main_time;
float peak_detach_cost;
gboolean peak_gpu_monitor_all = false;
gboolean peak_truncate_function_name = false;
gboolean peak_memory_profile = false;
gboolean peak_memory_track_all = false;
#ifdef HAVE_MPI
static int found_MPI;
#endif

static _Atomic int peak_exit_status_known = 0;
static _Atomic int peak_exit_status_value = 0;
static _Atomic int peak_runtime_active = 0;
static _Atomic pid_t peak_runtime_owner_pid = 0;
static PeakEnvWarningState peak_max_num_threads_warning_emitted;
typedef enum {
    PEAK_RUNTIME_ACTIVATION_NOT_READY = 0,
    PEAK_RUNTIME_ACTIVATION_READY = 1,
    PEAK_RUNTIME_ACTIVATION_IN_PROGRESS = 2,
    PEAK_RUNTIME_ACTIVATION_ACTIVE = 3,
    PEAK_RUNTIME_ACTIVATION_CANCELED = 4,
#if defined(__APPLE__)
    PEAK_RUNTIME_ACTIVATION_REJECTED = 5,
#endif
} PeakRuntimeActivationState;
static _Atomic int peak_runtime_activation_state =
    PEAK_RUNTIME_ACTIVATION_NOT_READY;
static _Atomic int peak_deferred_activation_warning_emitted = 0;
static pthread_t peak_runtime_activation_owner;
static _Atomic int peak_runtime_activation_owner_known = 0;
static _Atomic unsigned long long peak_exec_checkpoint_counter = 0;
static _Atomic unsigned int peak_exec_checkpoint_gate = 0;
#define PEAK_EXEC_CHECKPOINT_GATE_CLOSING (1U << 31)
#define PEAK_EXEC_CHECKPOINT_GATE_READERS \
    (PEAK_EXEC_CHECKPOINT_GATE_CLOSING - 1U)
/* Exec checkpoints are best-effort.  Do not let a canceled checkpoint reader
 * delay the application's normal exit indefinitely. */
#define PEAK_EXEC_CHECKPOINT_FINI_WAIT_TIMEOUT_MS 5000L
static pthread_mutex_t peak_exec_checkpoint_gate_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peak_exec_checkpoint_gate_cond;
static pthread_once_t peak_exec_checkpoint_gate_cond_once = PTHREAD_ONCE_INIT;
static _Atomic int peak_exec_checkpoint_gate_cond_ready = 0;
static _Atomic int peak_exec_checkpoint_fini_skip_warning_emitted = 0;
static _Atomic int peak_exec_checkpoint_gate_failure_warning_emitted = 0;
static _Atomic int peak_fini_completion_wait_warning_emitted = 0;
typedef enum {
    PEAK_FINI_NOT_STARTED = 0,
    PEAK_FINI_IN_PROGRESS = 1,
    PEAK_FINI_DONE = 2,
} PeakFiniState;

static _Atomic int peak_fini_state = PEAK_FINI_NOT_STARTED;

static int
peak_runtime_is_fork_child(void)
{
    pid_t owner = atomic_load_explicit(&peak_runtime_owner_pid,
                                       memory_order_acquire);

    return owner > 0 && getpid() != owner;
}

/*
 * Close the READY -> IN_PROGRESS race against process teardown.  Once this
 * function changes READY to CANCELED, an MPI completion can no longer start
 * Gum/controller activation.  If activation won first, teardown waits for it
 * to publish ACTIVE before continuing.
 */
static PeakRuntimeActivationState
peak_runtime_close_activation_for_teardown(void)
{
    if (peak_runtime_is_fork_child()) {
        return (PeakRuntimeActivationState)atomic_load_explicit(
            &peak_runtime_activation_state, memory_order_acquire);
    }

    for (;;) {
        int state = atomic_load_explicit(&peak_runtime_activation_state,
                                         memory_order_acquire);

        if (state == PEAK_RUNTIME_ACTIVATION_READY) {
            int expected = PEAK_RUNTIME_ACTIVATION_READY;

            if (atomic_compare_exchange_weak_explicit(
                    &peak_runtime_activation_state,
                    &expected,
                    PEAK_RUNTIME_ACTIVATION_CANCELED,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                return PEAK_RUNTIME_ACTIVATION_CANCELED;
            }
            continue;
        }
        if (state != PEAK_RUNTIME_ACTIVATION_IN_PROGRESS) {
            return (PeakRuntimeActivationState)state;
        }
        if (atomic_load_explicit(&peak_runtime_activation_owner_known,
                                 memory_order_acquire) != 0 &&
            pthread_equal(pthread_self(), peak_runtime_activation_owner)) {
            return PEAK_RUNTIME_ACTIVATION_IN_PROGRESS;
        }
        sched_yield();
    }
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic int peak_test_checkpoint_reader_pause = 0;
static _Atomic int peak_test_checkpoint_reader_held = 0;
static _Atomic int peak_test_checkpoint_reader_released = 0;
static _Atomic int peak_test_fini_waiting_for_reader = 0;
static _Atomic int peak_test_fini_checkpoint_wait_count = 0;
static _Atomic int peak_test_fini_checkpoint_timeout_observed = 0;
static _Atomic int peak_test_fini_completion_wait_count_value = 0;
static _Atomic int peak_test_activation_pause = 0;
static _Atomic int peak_test_activation_held = 0;
static _Atomic int peak_test_activation_released = 0;
static _Atomic int peak_test_fail_heartbeat_create = 0;

void
peak_test_fail_heartbeat_create_once(void)
{
    atomic_store_explicit(&peak_test_fail_heartbeat_create,
                          1,
                          memory_order_release);
}

void peak_fini(void);

PEAK_EXEC_API void
peak_test_fini(void)
{
    peak_fini();
}

PEAK_EXEC_API void
peak_test_checkpoint_reader_pause_enable(void)
{
    atomic_store_explicit(&peak_test_checkpoint_reader_released,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_checkpoint_reader_held,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_fini_waiting_for_reader,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_fini_checkpoint_wait_count,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_fini_checkpoint_timeout_observed,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_fini_completion_wait_count_value,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_checkpoint_reader_pause,
                          1,
                          memory_order_release);
}

PEAK_EXEC_API int
peak_test_checkpoint_reader_is_held(void)
{
    return atomic_load_explicit(&peak_test_checkpoint_reader_held,
                                memory_order_acquire);
}

PEAK_EXEC_API void
peak_test_checkpoint_reader_release(void)
{
    atomic_store_explicit(&peak_test_checkpoint_reader_released,
                          1,
                          memory_order_release);
}

PEAK_EXEC_API int
peak_test_fini_waiting_for_checkpoint_reader(void)
{
    return atomic_load_explicit(&peak_test_fini_waiting_for_reader,
                                memory_order_acquire);
}

PEAK_EXEC_API int
peak_test_fini_checkpoint_reader_wait_count(void)
{
    return atomic_load_explicit(&peak_test_fini_checkpoint_wait_count,
                                memory_order_acquire);
}

PEAK_EXEC_API int
peak_test_fini_checkpoint_reader_timeout_observed(void)
{
    return atomic_load_explicit(&peak_test_fini_checkpoint_timeout_observed,
                                memory_order_acquire);
}

PEAK_EXEC_API int
peak_test_fini_completion_wait_count(void)
{
    return atomic_load_explicit(&peak_test_fini_completion_wait_count_value,
                                memory_order_acquire);
}

PEAK_EXEC_API void
peak_test_activation_pause_enable(void)
{
    atomic_store_explicit(&peak_test_activation_released,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_activation_held,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_test_activation_pause,
                          1,
                          memory_order_release);
}

PEAK_EXEC_API int
peak_test_activation_is_held(void)
{
    return atomic_load_explicit(&peak_test_activation_held,
                                memory_order_acquire);
}

PEAK_EXEC_API void
peak_test_activation_release(void)
{
    atomic_store_explicit(&peak_test_activation_released,
                          1,
                          memory_order_release);
}

static void
peak_test_checkpoint_reader_pause_after_acquire(void)
{
    if (atomic_load_explicit(&peak_test_checkpoint_reader_pause,
                             memory_order_acquire) == 0) {
        return;
    }

    atomic_store_explicit(&peak_test_checkpoint_reader_held,
                          1,
                          memory_order_release);
    while (atomic_load_explicit(&peak_test_checkpoint_reader_released,
                                memory_order_acquire) == 0) {
        pthread_testcancel();
        sched_yield();
    }
}

static void
peak_test_activation_pause_before_claim(void)
{
    if (atomic_load_explicit(&peak_test_activation_pause,
                             memory_order_acquire) == 0) {
        return;
    }

    atomic_store_explicit(&peak_test_activation_held,
                          1,
                          memory_order_release);
    while (atomic_load_explicit(&peak_test_activation_released,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
    atomic_store_explicit(&peak_test_activation_pause,
                          0,
                          memory_order_release);
}
#endif

static void
peak_exec_checkpoint_gate_cond_initialize(void)
{
#if defined(__linux__)
    pthread_condattr_t attr;
#endif
    int status;

#if defined(__linux__)
    if (pthread_condattr_init(&attr) != 0) {
        return;
    }
    status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (status == 0) {
        status = pthread_cond_init(&peak_exec_checkpoint_gate_cond, &attr);
    }
    (void)pthread_condattr_destroy(&attr);
#elif defined(__APPLE__)
    status = pthread_cond_init(&peak_exec_checkpoint_gate_cond, NULL);
#else
    return;
#endif
    if (status == 0) {
        atomic_store_explicit(&peak_exec_checkpoint_gate_cond_ready,
                              1,
                              memory_order_release);
    }
}

static gboolean
peak_exec_checkpoint_gate_cond_is_ready(void)
{
    if (pthread_once(&peak_exec_checkpoint_gate_cond_once,
                     peak_exec_checkpoint_gate_cond_initialize) != 0) {
        return FALSE;
    }
    return atomic_load_explicit(&peak_exec_checkpoint_gate_cond_ready,
                                memory_order_acquire) != 0;
}

static gboolean
peak_exec_checkpoint_deadline(struct timespec* deadline)
{
#if defined(__linux__) || defined(__APPLE__)
    if (clock_gettime(
            CLOCK_MONOTONIC,
            deadline) != 0) {
        return FALSE;
    }
    deadline->tv_sec += PEAK_EXEC_CHECKPOINT_FINI_WAIT_TIMEOUT_MS / 1000L;
    deadline->tv_nsec +=
        (PEAK_EXEC_CHECKPOINT_FINI_WAIT_TIMEOUT_MS % 1000L) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return TRUE;
#else
    (void)deadline;
    return FALSE;
#endif
}

static int
peak_exec_checkpoint_cond_timedwait(const struct timespec* deadline)
{
#if defined(__linux__)
    return pthread_cond_timedwait(&peak_exec_checkpoint_gate_cond,
                                  &peak_exec_checkpoint_gate_mutex,
                                  deadline);
#elif defined(__APPLE__)
    struct timespec now;
    struct timespec remaining;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return EINVAL;
    }
    remaining.tv_sec = deadline->tv_sec - now.tv_sec;
    remaining.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (remaining.tv_nsec < 0) {
        remaining.tv_sec--;
        remaining.tv_nsec += 1000000000L;
    }
    if (remaining.tv_sec < 0 ||
        (remaining.tv_sec == 0 && remaining.tv_nsec == 0)) {
        return ETIMEDOUT;
    }
    return pthread_cond_timedwait_relative_np(&peak_exec_checkpoint_gate_cond,
                                              &peak_exec_checkpoint_gate_mutex,
                                              &remaining);
#else
    (void)deadline;
    return ENOTSUP;
#endif
}

static void
peak_exec_checkpoint_warn_gate_failure(const char* operation)
{
    int expected = 0;

    if (atomic_compare_exchange_strong_explicit(
            &peak_exec_checkpoint_gate_failure_warning_emitted,
            &expected,
            1,
            memory_order_acq_rel,
            memory_order_acquire)) {
        peak_log_warn("[peak] Exec checkpoint gate %s failed; skipping unsafe teardown\n",
                      operation);
    }
}

static void
peak_exec_checkpoint_warn_fini_skip(const char* reason)
{
    int expected = 0;

    if (atomic_compare_exchange_strong_explicit(
            &peak_exec_checkpoint_fini_skip_warning_emitted,
            &expected,
            1,
            memory_order_acq_rel,
            memory_order_acquire)) {
        peak_log_warn(
            "[peak] Exec checkpoint readers did not drain (%s); skipping "
            "PEAK final report and teardown "
            "so application exit can continue\n",
            reason);
    }
}

static gboolean
peak_exec_checkpoint_wait_for_readers(gboolean* gate_unlock_failed)
{
    struct timespec deadline;
    gboolean drained = TRUE;

    int wait_status = 0;
    const char* failure_reason = NULL;

    *gate_unlock_failed = FALSE;

    if ((atomic_load_explicit(&peak_exec_checkpoint_gate,
                              memory_order_acquire) &
         PEAK_EXEC_CHECKPOINT_GATE_READERS) == 0) {
        return TRUE;
    }
    if (!peak_exec_checkpoint_gate_cond_is_ready() ||
        !peak_exec_checkpoint_deadline(&deadline)) {
        peak_exec_checkpoint_warn_fini_skip("checkpoint wait unavailable");
        return FALSE;
    }
    if (pthread_mutex_lock(&peak_exec_checkpoint_gate_mutex) != 0) {
        peak_exec_checkpoint_warn_fini_skip("checkpoint gate lock failed");
        return FALSE;
    }
    while ((atomic_load_explicit(&peak_exec_checkpoint_gate,
                                 memory_order_acquire) &
            PEAK_EXEC_CHECKPOINT_GATE_READERS) != 0) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        atomic_store_explicit(&peak_test_fini_waiting_for_reader,
                              1,
                              memory_order_release);
        atomic_fetch_add_explicit(&peak_test_fini_checkpoint_wait_count,
                                  1,
                                  memory_order_release);
#endif
        wait_status = peak_exec_checkpoint_cond_timedwait(&deadline);
        if (wait_status != 0) {
            drained = FALSE;
            failure_reason = wait_status == ETIMEDOUT ?
                "finalization deadline expired" : "checkpoint wait failed";
            break;
        }
    }
    if (pthread_mutex_unlock(&peak_exec_checkpoint_gate_mutex) != 0) {
        drained = FALSE;
        failure_reason = "checkpoint gate unlock failed";
        *gate_unlock_failed = TRUE;
    }

    if (!drained) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (wait_status == ETIMEDOUT) {
            atomic_store_explicit(&peak_test_fini_checkpoint_timeout_observed,
                                  1,
                                  memory_order_release);
        }
#endif
        peak_exec_checkpoint_warn_fini_skip(failure_reason);
    }
    return drained;
}

static void
peak_fini_publish_done_without_gate(void)
{
    atomic_store_explicit(&peak_fini_state,
                          PEAK_FINI_DONE,
                          memory_order_release);
    peak_exec_checkpoint_warn_gate_failure("completion publication after unlock failure");
}

static void
peak_fini_publish_done(void)
{
    if (!peak_exec_checkpoint_gate_cond_is_ready()) {
        atomic_store_explicit(&peak_fini_state,
                              PEAK_FINI_DONE,
                              memory_order_release);
        return;
    }

    if (pthread_mutex_lock(&peak_exec_checkpoint_gate_mutex) != 0) {
        atomic_store_explicit(&peak_fini_state,
                              PEAK_FINI_DONE,
                              memory_order_release);
        peak_exec_checkpoint_warn_gate_failure("completion publication lock");
        return;
    }
    atomic_store_explicit(&peak_fini_state,
                          PEAK_FINI_DONE,
                          memory_order_release);
    if (pthread_cond_broadcast(&peak_exec_checkpoint_gate_cond) != 0) {
        peak_exec_checkpoint_warn_gate_failure("completion broadcast");
    }
    if (pthread_mutex_unlock(&peak_exec_checkpoint_gate_mutex) != 0) {
        peak_exec_checkpoint_warn_gate_failure("completion publication unlock");
    }
}

static void
peak_fini_wait_for_completion(void)
{
    struct timespec deadline;
    int wait_status = 0;
    gboolean still_in_progress;
    int warning_expected = 0;

    if (atomic_load_explicit(&peak_fini_state, memory_order_acquire) !=
        PEAK_FINI_IN_PROGRESS) {
        return;
    }
    if (!peak_exec_checkpoint_gate_cond_is_ready() ||
        !peak_exec_checkpoint_deadline(&deadline)) {
        if (atomic_compare_exchange_strong_explicit(
                &peak_fini_completion_wait_warning_emitted,
                &warning_expected,
                1,
                memory_order_acq_rel,
                memory_order_acquire)) {
            peak_log_warn("[peak] Finalization completion wait unavailable; returning without cleanup\n");
        }
        return;
    }

    if (pthread_mutex_lock(&peak_exec_checkpoint_gate_mutex) != 0) {
        peak_exec_checkpoint_warn_gate_failure("completion wait lock");
        return;
    }
    while (atomic_load_explicit(&peak_fini_state, memory_order_acquire) ==
           PEAK_FINI_IN_PROGRESS) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        atomic_fetch_add_explicit(&peak_test_fini_completion_wait_count_value,
                                  1,
                                  memory_order_release);
#endif
        wait_status = peak_exec_checkpoint_cond_timedwait(&deadline);
        if (wait_status != 0) {
            break;
        }
    }
    still_in_progress =
        atomic_load_explicit(&peak_fini_state, memory_order_acquire) ==
        PEAK_FINI_IN_PROGRESS;
    if (pthread_mutex_unlock(&peak_exec_checkpoint_gate_mutex) != 0) {
        peak_exec_checkpoint_warn_gate_failure("completion wait unlock");
        return;
    }
    if (wait_status != 0 && still_in_progress &&
        atomic_compare_exchange_strong_explicit(
            &peak_fini_completion_wait_warning_emitted,
            &warning_expected,
            1,
            memory_order_acq_rel,
            memory_order_acquire)) {
        peak_log_warn("[peak] Finalization completion wait %s; returning without cleanup\n",
                      wait_status == ETIMEDOUT ? "timed out" : "failed");
    }
}

static int
peak_exec_checkpoint_reader_acquire(void)
{
    unsigned int observed =
        atomic_load_explicit(&peak_exec_checkpoint_gate,
                             memory_order_acquire);

    for (;;) {
        if ((observed & PEAK_EXEC_CHECKPOINT_GATE_CLOSING) != 0 ||
            (observed & PEAK_EXEC_CHECKPOINT_GATE_READERS) ==
                PEAK_EXEC_CHECKPOINT_GATE_READERS) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &peak_exec_checkpoint_gate,
                &observed,
                observed + 1U,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return 1;
        }
    }
}

static void
peak_exec_checkpoint_reader_release(void)
{
    if (!peak_exec_checkpoint_gate_cond_is_ready()) {
        atomic_fetch_sub_explicit(&peak_exec_checkpoint_gate,
                                  1U,
                                  memory_order_release);
        return;
    }

    if (pthread_mutex_lock(&peak_exec_checkpoint_gate_mutex) != 0) {
        peak_exec_checkpoint_warn_gate_failure("reader release lock");
        return;
    }
    atomic_fetch_sub_explicit(&peak_exec_checkpoint_gate,
                              1U,
                              memory_order_release);
    if (pthread_cond_broadcast(&peak_exec_checkpoint_gate_cond) != 0) {
        peak_exec_checkpoint_warn_gate_failure("reader release broadcast");
    }
    if (pthread_mutex_unlock(&peak_exec_checkpoint_gate_mutex) != 0) {
        peak_exec_checkpoint_warn_gate_failure("reader release unlock");
    }
}

static gboolean
peak_fini_cancellation_disable(int* saved_state)
{
    return pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, saved_state) == 0;
}

static void
peak_fini_cancellation_restore(int saved_state)
{
    if (pthread_setcancelstate(saved_state, NULL) != 0) {
        peak_log_warn("[peak] Failed to restore finalizer cancellation state\n");
    }
}

static gboolean
peak_has_requested_work(void)
{
    return peak_hook_address_count > 0 ||
           peak_gpu_hook_address_count > 0 ||
           peak_gpu_monitor_all ||
           peak_memory_profile;
}

#ifdef HAVE_MPI
static gboolean
peak_env_value_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static PeakOutputAggregationMode
peak_output_aggregation_mode_from_value(const char* name,
                                        const char* value,
                                        gboolean legacy_collective)
{
    if (value == NULL || value[0] == '\0') {
        return PEAK_OUTPUT_AGGREGATION_MPI;
    }

    if (legacy_collective && peak_env_value_truthy(value)) {
        return PEAK_OUTPUT_AGGREGATION_MPI;
    }

    if (g_ascii_strcasecmp(value, "mpi") == 0 ||
        g_ascii_strcasecmp(value, "collective") == 0 ||
        peak_env_value_truthy(value)) {
        return PEAK_OUTPUT_AGGREGATION_MPI;
    }

    if (g_ascii_strcasecmp(value, "socket") == 0 ||
        g_ascii_strcasecmp(value, "tcp") == 0 ||
        g_ascii_strcasecmp(value, "interconnect") == 0) {
        return PEAK_OUTPUT_AGGREGATION_SOCKET;
    }

    if (g_ascii_strcasecmp(value, "0") == 0 ||
        g_ascii_strcasecmp(value, "false") == 0 ||
        g_ascii_strcasecmp(value, "no") == 0 ||
        g_ascii_strcasecmp(value, "off") == 0 ||
        g_ascii_strcasecmp(value, "none") == 0 ||
        g_ascii_strcasecmp(value, "local") == 0 ||
        g_ascii_strcasecmp(value, "rank-local") == 0) {
        return PEAK_OUTPUT_AGGREGATION_LOCAL;
    }

    g_printerr("[peak] Unknown %s=%s; disabling aggregate output\n",
               name,
               value);
    return PEAK_OUTPUT_AGGREGATION_LOCAL;
}

static gboolean
peak_mpi_runtime_is_intel_2019(void)
{
    char version[MPI_MAX_LIBRARY_VERSION_STRING] = { 0 };
    int version_len = 0;
    const char* text = NULL;

#ifdef PEAK_ENABLE_TEST_HOOKS
    text = getenv(PEAK_TEST_MPI_LIBRARY_VERSION_ENV);
    if (text != NULL && text[0] == '\0') {
        text = NULL;
    }
#endif
    if (text == NULL) {
        if (MPI_Get_library_version(version, &version_len) != MPI_SUCCESS) {
            return FALSE;
        }
        if (version_len < 0) {
            version_len = 0;
        }
        if (version_len >= (int)sizeof(version)) {
            version_len = (int)sizeof(version) - 1;
        }
        version[version_len] = '\0';
        text = version;
    }

    return (strstr(text, "Intel(R) MPI") != NULL ||
            strstr(text, "Intel MPI") != NULL) &&
           strstr(text, "2019") != NULL;
}

static gboolean
peak_mpi_real_finalize_config_allowed(
    gboolean* intel_2019_workaround)
{
    const char* value = getenv(PEAK_MPI_REAL_FINALIZE_ENV);

    if (intel_2019_workaround != NULL) {
        *intel_2019_workaround = FALSE;
    }
    if (value != NULL && value[0] != '\0') {
        return peak_env_value_truthy(value);
    }
    if (peak_mpi_runtime_is_intel_2019()) {
        if (intel_2019_workaround != NULL) {
            *intel_2019_workaround = TRUE;
        }
        return FALSE;
    }
    return TRUE;
}

static PeakOutputAggregationMode
peak_output_aggregation_mode(void)
{
    const char* aggregation = getenv(PEAK_OUTPUT_AGGREGATION_ENV);
    const char* value = getenv(PEAK_MPI_COLLECTIVE_OUTPUT_ENV);

    if (aggregation != NULL && aggregation[0] != '\0') {
        return peak_output_aggregation_mode_from_value(
            PEAK_OUTPUT_AGGREGATION_ENV,
            aggregation,
            FALSE);
    }

    if (value != NULL && value[0] != '\0') {
        return peak_output_aggregation_mode_from_value(
            PEAK_MPI_COLLECTIVE_OUTPUT_ENV,
            value,
            TRUE);
    }

    return PEAK_OUTPUT_AGGREGATION_MPI;
}

#endif

static size_t
peak_deduplicate_target_names(char** targets, size_t count)
{
    size_t unique_count = 0;

    for (size_t i = 0; i < count; i++) {
        gboolean duplicate = FALSE;
        for (size_t j = 0; j < unique_count; j++) {
            if (strcmp(targets[i], targets[j]) == 0) {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate) {
            free(targets[i]);
        } else {
            targets[unique_count++] = targets[i];
        }
    }
    return unique_count;
}

#ifdef HAVE_MPI
static gboolean
peak_mpi_activation_policy_post_init(void)
{
    const char* value = getenv(PEAK_MPI_ACTIVATION_POLICY_ENV);

    if (value == NULL || value[0] == '\0' ||
        g_ascii_strcasecmp(value, "immediate") == 0) {
        return FALSE;
    }
    if (g_ascii_strcasecmp(value, "post-init") == 0 ||
        g_ascii_strcasecmp(value, "defer") == 0 ||
        g_ascii_strcasecmp(value, "deferred") == 0) {
        return TRUE;
    }

    peak_log_warn(
        "[peak] invalid %s=%s; using immediate activation\n",
        PEAK_MPI_ACTIVATION_POLICY_ENV,
        value);
    return FALSE;
}
#endif

static void
peak_activate_runtime(void)
{
    int expected = PEAK_RUNTIME_ACTIVATION_READY;

#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_test_activation_pause_before_claim();
#endif
    if (!atomic_compare_exchange_strong_explicit(
            &peak_runtime_activation_state,
            &expected,
            PEAK_RUNTIME_ACTIVATION_IN_PROGRESS,
            memory_order_acq_rel,
            memory_order_acquire)) {
        while (expected == PEAK_RUNTIME_ACTIVATION_IN_PROGRESS &&
               atomic_load_explicit(&peak_runtime_activation_state,
                                    memory_order_acquire) ==
                   PEAK_RUNTIME_ACTIVATION_IN_PROGRESS) {
            sched_yield();
        }
        return;
    }
    peak_runtime_activation_owner = pthread_self();
    atomic_store_explicit(&peak_runtime_activation_owner_known,
                          1,
                          memory_order_release);

#if defined(__APPLE__)
    /*
     * Darwin v1 has no strict protocol for installing Gum hooks after peer
     * threads exist.  Prove the process is still single-threaded before even
     * initializing Gum, so pthread, MPI, and target hook installation cannot
     * begin from a post-init or early-constructor multithreaded state.
     */
    if (!peak_general_listener_startup_attach_can_skip_stop()) {
        peak_log_warn(
            "[peak] refusing macOS runtime activation because the process is already multithreaded; Darwin Gum bootstrap requires single-threaded startup and no Gum hooks were installed\n");
        atomic_store_explicit(&peak_runtime_activation_state,
                              PEAK_RUNTIME_ACTIVATION_REJECTED,
                              memory_order_release);
        return;
    }
#endif

    /*
     * The default path reaches this point before application main. An MPI rank
     * using PEAK_MPI_ACTIVATION_POLICY=post-init reaches it only after a
     * successful MPI_Init* return, with no earlier Gum state or code mutation.
     */
    gum_init_embedded();

    atomic_store_explicit(&peak_runtime_owner_pid,
                          getpid(),
                          memory_order_release);

    pthread_listener_attach();
    if (peak_hook_address_count > 0
#ifdef HAVE_MPI
        && !found_MPI
#endif
    ) {
        peak_detach_controller_warmup_backend();
    }
#ifdef HAVE_MPI
    if (found_MPI && mpi_interceptor_attach() != 0) {
        found_MPI = 0;
    }
#endif
#ifdef HAVE_CUDA
    cuda_interceptor_attach();
#endif
    /* General-listener hooks depend on pthread and MPI interception setup. */
    peak_target_thread_called = g_new0(gboolean*, peak_hook_address_count);
    peak_target_thread_called_count = peak_hook_address_count;
    for (gint i = 0; i < peak_hook_address_count; i++) {
        peak_target_thread_called[i] = g_new0(gboolean, peak_max_num_threads);
    }
    peak_need_detach = g_new0(gboolean, peak_hook_address_count);
    peak_detached = g_new0(gboolean, peak_hook_address_count);
#if !defined(__APPLE__)
    peak_jit_provider_enable();
#endif
    /*
     * This is the post-MPI module rescan. Gum initialization and this scan
     * both occur in the deferred activation, so symbol lookup sees UCX,
     * libfabric, libnuma, and any other providers that MPI_Init loaded before
     * target attachment begins.
     */
    peak_general_listener_attach();
#if !defined(__APPLE__)
    gboolean need_dynamic_attach = peak_general_listener_needs_dynamic_attach();
    gboolean dynamic_attach_listener_ready = FALSE;
    if (need_dynamic_attach) {
        dynamic_attach_listener_ready = dlopen_interceptor_attach() == 0;
    }
#endif
    peak_main_time = peak_second();
    peak_general_listener_note_runtime_start(peak_main_time);
    if (heartbeat_time != 0) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (getenv("PEAK_TEST_FAIL_HEARTBEAT_SETUP_ALLOCATION") != NULL) {
            heartbeat_overhead = NULL;
            args = NULL;
        } else
#endif
        {
        heartbeat_overhead = g_try_new0(gdouble, peak_hook_address_count);
        args = g_try_new0(PeakHeartbeatArgs, 1);
        }
        if ((peak_hook_address_count != 0 && heartbeat_overhead == NULL) ||
            args == NULL) {
            peak_report_snapshot_note_degraded(
                PEAK_PROFILER_DEGRADED_HEARTBEAT,
                "heartbeat setup allocation failed");
            g_free(args);
            args = NULL;
            g_free(heartbeat_overhead);
            heartbeat_overhead = NULL;
            heartbeat_time = 0;
        }
    }
    if (heartbeat_time != 0) {
        args->heartbeat_time = heartbeat_time;
        args->check_interval = check_interval;
        args->hb_min_us = hb_min_us;
        args->hb_max_us = hb_max_us;
        args->hb_k_err = hb_k_err;
        args->hb_k_rate = hb_k_rate;
        args->hb_ema_a = hb_ema_a;
    }
    if (peak_memory_profile) {
        PeakMallocInterceptorAttachResult memory_attach_result =
            malloc_interceptor_attach();
        if (memory_attach_result != PEAK_MALLOC_ATTACH_OK) {
            peak_report_snapshot_note_degraded(
                PEAK_PROFILER_DEGRADED_MEMORY_TRACKING,
                memory_attach_result == PEAK_MALLOC_ATTACH_ROLLED_BACK
                    ? "memory interceptor installation rolled back safely"
                    : "memory tracking setup failed before installation");
            peak_memory_profile = false;
        }
    }
    peak_general_listener_controller_start();
#if !defined(__APPLE__)
    if (dynamic_attach_listener_ready) {
        dlopen_interceptor_enable_dynamic_attach();
    }
#endif
    if (heartbeat_time != 0) {
        pthread_mutex_lock(&heartbeat_mutex);
        atomic_store(&heartbeat_running, true);
        pthread_mutex_unlock(&heartbeat_mutex);
        int heartbeat_create_result;
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (atomic_exchange_explicit(&peak_test_fail_heartbeat_create,
                                     0,
                                     memory_order_acq_rel) != 0 ||
            (getenv("PEAK_TEST_FAIL_HEARTBEAT_CREATE") != NULL &&
             getenv("PEAK_TEST_FAIL_HEARTBEAT_CREATE")[0] != '\0')) {
            heartbeat_create_result = EAGAIN;
        } else
#endif
        {
            pthread_listener_mark_next_created_thread_helper();
            heartbeat_create_result = pthread_create(&heartbeat_thread,
                                                      NULL,
                                                      peak_heartbeat_monitor,
                                                      args);
        }
        if (heartbeat_create_result != 0) {
            peak_report_snapshot_note_degraded(
                PEAK_PROFILER_DEGRADED_HEARTBEAT,
                "heartbeat thread creation failed");
            g_free(args);
            args = NULL;
            g_free(heartbeat_overhead);
            heartbeat_overhead = NULL;
            heartbeat_time = 0;
            pthread_mutex_lock(&heartbeat_mutex);
            atomic_store(&heartbeat_running, false);
            pthread_mutex_unlock(&heartbeat_mutex);
        } else {
            atomic_store_explicit(&peak_heartbeat_started,
                                  1,
                                  memory_order_release);
        }
    }

    atomic_store_explicit(&peak_runtime_active, 1, memory_order_release);
    atomic_store_explicit(&peak_runtime_activation_state,
                          PEAK_RUNTIME_ACTIVATION_ACTIVE,
                          memory_order_release);
}

#ifdef HAVE_MPI
PEAK_API void
peak_mpi_init_completed(int result)
{
    if (result == MPI_SUCCESS) {
        peak_activate_runtime();
    } else if (atomic_load_explicit(&peak_runtime_activation_state,
                                    memory_order_acquire) ==
               PEAK_RUNTIME_ACTIVATION_READY) {
        peak_log_warn(
            "[peak] MPI initialization failed while runtime activation was "
            "deferred; falling back to immediate non-MPI activation\n");
        found_MPI = 0;
        peak_detach_controller_configure_mpi_process(FALSE);
        peak_activate_runtime();
    }
}
#endif

static gulong
peak_parse_max_num_threads(void)
{
    const char* value = getenv(PEAK_MAX_NUM_THREADS_ENV);
    long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    gulong default_value = 2;
    PeakEnvUnsignedSchema schema;
    unsigned long long parsed;

    if (online_cpus > 0) {
        if ((unsigned long)online_cpus > PEAK_MAX_NUM_THREADS_LIMIT / 2UL) {
            default_value = PEAK_MAX_NUM_THREADS_LIMIT;
        } else {
            default_value = (gulong)online_cpus * 2UL;
        }
    }

    if (value == NULL) {
        return default_value;
    }
    schema = (PeakEnvUnsignedSchema){
        PEAK_MAX_NUM_THREADS_ENV, "thread slots", default_value,
        0, G_MAXULONG, true, &peak_max_num_threads_warning_emitted,
        true,
    };
    if (!peak_parse_env_unsigned_checked(&schema, &parsed)) {
        return (gulong)peak_parse_env_unsigned(&schema);
    }
    if (parsed == 0) {
        if (__atomic_exchange_n(&peak_max_num_threads_warning_emitted.emitted,
                                1,
                                __ATOMIC_RELAXED) == 0) {
            peak_log_warn("[peak] %s=0 is unsafe; using 1\n",
                          PEAK_MAX_NUM_THREADS_ENV);
        }
        return 1;
    }
    if (parsed > PEAK_MAX_NUM_THREADS_LIMIT) {
        if (__atomic_exchange_n(&peak_max_num_threads_warning_emitted.emitted,
                                1,
                                __ATOMIC_RELAXED) == 0) {
            peak_log_warn("[peak] %s=%s exceeds supported capacity %lu; clamping\n",
                          PEAK_MAX_NUM_THREADS_ENV,
                          value,
                          PEAK_MAX_NUM_THREADS_LIMIT);
        }
        return PEAK_MAX_NUM_THREADS_LIMIT;
    }
    return (gulong)parsed;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
/* Deliberately re-read the environment so table-driven tests can exercise the
 * parser without reinitializing the full preload runtime. */
PEAK_API gulong
peak_test_parse_max_num_threads(void)
{
    return peak_parse_max_num_threads();
}
#endif

void peak_init()
{
    peak_output_identity_initialize();
    PeakRuntimeNumericConfig numeric_config;

    peak_log_configure();
    peak_max_num_threads = peak_parse_max_num_threads();
    peak_hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &peak_hook_strings);
    peak_hook_address_count += load_profiling_symbols(PEAK_TARGET_FILE_ENV, &peak_hook_strings, peak_hook_address_count);
    peak_hook_address_count += load_symbols_from_array(PEAK_TARGET_GROUP_ENV, &peak_hook_strings, peak_hook_address_count);
    peak_hook_address_count = peak_deduplicate_target_names(
        peak_hook_strings,
        peak_hook_address_count);
    peak_gpu_hook_address_count = parse_env_w_delim(PEAK_GPU_TARGET_ENV, PEAK_TARGET_DELIM, &peak_gpu_hook_strings);
    peak_gpu_hook_address_count += load_profiling_symbols(PEAK_GPU_TARGET_FILE_ENV, &peak_gpu_hook_strings, peak_gpu_hook_address_count);
    numeric_config = peak_parse_runtime_numeric_config();
    peak_detach_cost = numeric_config.detach_cost;
    peak_gpu_monitor_all = parse_env_to_bool(PEAK_GPU_MONITOR_ALL);
    peak_truncate_function_name = parse_env_to_bool(PEAK_NAME_TRUNCATE);
    heartbeat_time = numeric_config.heartbeat_interval_us;
    check_interval = numeric_config.hibernation_cycle;
    target_profile_ratio = numeric_config.target_overhead_ratio;
    global_target_ratio = numeric_config.global_overhead_ratio;
    peak_global_detach_factor = numeric_config.global_detach_factor;
    peak_global_reattach_factor = numeric_config.global_reattach_factor;
    enable_per_target_heartbeat = parse_env_to_bool(PEAK_ENABLE_PER_TARGET_HEARTBEAT_ENV);
    enable_global_heartbeat = parse_env_to_bool(PEAK_ENABLE_GLOBAL_HEARTBEAT_ENV);
    const char* enable_reattach_env = getenv(PEAK_ENABLE_REATTACH_ENV);
    enable_reattach =
        (enable_reattach_env == NULL) || parse_env_to_bool(PEAK_ENABLE_REATTACH_ENV);
    sig_stop_ack_wait_interval = numeric_config.pause_timeout_ns;
    sig_cont_wait_interval = numeric_config.sig_cont_timeout_ns;
    hb_min_us = numeric_config.heartbeat_min_us;
    hb_max_us = numeric_config.heartbeat_max_us;
    hb_k_err = numeric_config.heartbeat_error_gain;
    hb_k_rate = numeric_config.heartbeat_rate_gain;
    hb_ema_a = numeric_config.heartbeat_ema_alpha;
    peak_memory_profile = parse_env_to_bool(PEAK_MEMORY_PROFILE);
    peak_memory_track_all = parse_env_to_bool(PEAK_MEMORY_TRACK_ALL);
#if defined(__APPLE__)
    if (peak_memory_profile) {
        peak_log_warn(
            "[peak] rejecting PEAK_MEMORY_PROFILE on macOS; only named CPU profiling is supported\n");
    }
    peak_memory_profile = false;
    peak_memory_track_all = false;
#endif

    gboolean has_requested_work = peak_has_requested_work();
    peak_set_process_requests_work(has_requested_work);
    if (!has_requested_work) {
        return;
    }
    {
        int force_log_output_failure = 0;
#ifdef PEAK_ENABLE_TEST_HOOKS
        force_log_output_failure =
            getenv("PEAK_TEST_FAIL_LOG_DESCRIPTOR_DUP") != NULL;
#endif
        peak_log_initialize_output(force_log_output_failure);
    }
    /*
     * Publish process ownership before READY can be inherited across fork().
     * A child must never wait for a parent thread that was activating PEAK.
     */
    atomic_store_explicit(&peak_runtime_owner_pid,
                          getpid(),
                          memory_order_release);

#ifdef HAVE_MPI
    found_MPI = check_MPI();
    if (found_MPI) {
        int is_parent_MPI = check_parent_process();
        if (is_parent_MPI > 0) {
            found_MPI = 0;
        }
    }
    gboolean activate_post_mpi_init =
        peak_mpi_activation_policy_post_init();
    /*
     * An explicit post-init policy is authoritative even for singleton MPI
     * launches whose runtime provides none of the rank environment variables
     * used by check_MPI(). Programs without a traditional interposable
     * MPI_Init* lifecycle must leave the default immediate policy selected.
     */
    if (activate_post_mpi_init) {
        found_MPI = 1;
    }
    peak_detach_controller_configure_mpi_process(found_MPI != 0);
#else
    peak_detach_controller_configure_mpi_process(FALSE);
#endif
    atomic_store_explicit(&peak_runtime_activation_state,
                          PEAK_RUNTIME_ACTIVATION_READY,
                          memory_order_release);
#ifdef HAVE_MPI
    if (activate_post_mpi_init) {
        int initialized = 0;
        int query_result = MPI_Initialized(&initialized);

        if (query_result == MPI_SUCCESS && !initialized) {
            /*
             * A non-Gum PMPI_Init* interposer calls peak_mpi_init_completed()
             * after the real runtime returns. This opt-in policy intentionally
             * omits pre-MPI profiling so there are no Gum mutations, module
             * ownership/pinning, controller threads, or heartbeat until then.
             */
            return;
        }
        if (query_result != MPI_SUCCESS) {
            peak_log_warn(
                "[peak] MPI_Initialized failed under %s=post-init; using "
                "immediate activation\n",
                PEAK_MPI_ACTIVATION_POLICY_ENV);
        }
    }
#endif
    peak_activate_runtime();
}

#ifdef HAVE_MPI
static int
peak_mpi_runtime_allows_collectives(void)
{
    int initialized = 0;
    int finalized = 0;

    MPI_Initialized(&initialized);
    MPI_Finalized(&finalized);
    return initialized && !finalized;
}

#endif

static void
peak_fini_impl(void)
{
    double peak_runtime_start_time = peak_main_time;

#ifdef HAVE_MPI
    int mpi_finalize_path =
        found_MPI && mpi_interceptor_finalize_path_active();
    if (mpi_finalize_path) {
        peak_general_listener_suspend_callbacks();
    }
#endif

    if (atomic_load_explicit(&peak_heartbeat_started, memory_order_acquire)) {
        pthread_mutex_lock(&heartbeat_mutex);
        atomic_store(&heartbeat_running, false);
        pthread_cond_signal(&heartbeat_cond);
        pthread_mutex_unlock(&heartbeat_mutex);
        pthread_join(heartbeat_thread, NULL);
        atomic_store_explicit(&peak_heartbeat_started, 0, memory_order_release);
        if (args) {
            g_free(args);
            args = NULL;
        }
    }

#if defined(GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION) && \
    GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION >= 2
    /*
     * The module-sync worker may emit Gum registry notifications that acquire
     * interceptor state. Stop it before controller/listener teardown starts
     * taking those locks in the opposite order.
     */
    gum_interceptor_peak_quiesce_deferred_module_sync();
#endif

    peak_general_listener_controller_stop();
    if (peak_runtime_start_time > 0.0) {
        peak_main_time = peak_second() - peak_runtime_start_time;
    }
    peak_general_listener_freeze_final_report_snapshot();
    if (heartbeat_overhead) {
        g_free(heartbeat_overhead);
        heartbeat_overhead = NULL;
    }
    peak_jit_provider_disable();
#if !defined(__APPLE__)
    if (
#ifdef HAVE_MPI
        !mpi_finalize_path &&
#endif
        peak_memory_profile) {
        malloc_interceptor_detach();
    }
#endif
    gboolean dlopen_shutdown_flushed = TRUE;
#if defined(__APPLE__)
    /*
     * Darwin runtime dynamic attach is unsupported, so no loader listener,
     * dlclose guard, ownership thread, or queue was installed.
     */
#else
#ifdef HAVE_MPI
    if (mpi_finalize_path) {
        /*
         * PMPI_Finalize may load DSOs after PEAK writes its report. Keep the
         * Gum listener physically pinned, but close admission and drain every
         * callback before report metadata is released.
         */
        dlopen_shutdown_flushed =
            dlopen_interceptor_shutdown_dynamic_attach();
    } else
#endif
    {
        dlopen_shutdown_flushed = dlopen_interceptor_dettach();
    }
#endif
    if (!dlopen_shutdown_flushed) {
        g_printerr("[peak] Skipping remaining PEAK teardown because dlopen listener teardown was not proven safe\n");
        return;
    }
#ifdef HAVE_MPI
    int exit_status_known =
        atomic_load_explicit(&peak_exit_status_known, memory_order_acquire);
    int exit_status =
        atomic_load_explicit(&peak_exit_status_value, memory_order_acquire);
    int abnormal_exit = exit_status_known == 2 && exit_status != 0;
    PeakOutputAggregationMode aggregation_mode =
        found_MPI ? peak_output_aggregation_mode()
                  : PEAK_OUTPUT_AGGREGATION_LOCAL;
    int local_requested_mpi_finalize =
        mpi_interceptor_finalize_was_requested();
    int mpi_collectives_failed_closed =
        peak_mpi_teardown_collectives_failed_closed();
    gboolean report_write_succeeded = FALSE;
    gboolean used_mpi_aggregation = FALSE;
    int report_published = 0;
    PeakOutputAggregationMode output_mode = PEAK_OUTPUT_AGGREGATION_LOCAL;
    int mpi_log_rank = 1;

    /*
     * Rank-local and strict socket output do not need MPI. Publish their
     * immutable snapshots before any MPI teardown proof, then combine finalize
     * participation with the long post-publication release gate. Error exits
     * and a previously poisoned collective path also publish local evidence
     * without touching MPI. The MPI backend keeps its existing proof-first
     * ordering.
     */
    int publish_before_finalize_proof =
        mpi_finalize_path &&
        (aggregation_mode != PEAK_OUTPUT_AGGREGATION_MPI ||
         abnormal_exit ||
         mpi_collectives_failed_closed);
    if (publish_before_finalize_proof) {
        long env_rank = -1;
        long env_size = -1;
        if (peak_general_listener_mpi_env_rank_size(&env_rank, &env_size)) {
            (void)env_size;
            mpi_log_rank = env_rank == 0;
        }

        output_mode =
            aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET &&
            !abnormal_exit &&
            !mpi_collectives_failed_closed ?
                PEAK_OUTPUT_AGGREGATION_SOCKET :
                PEAK_OUTPUT_AGGREGATION_LOCAL;
        if (found_MPI && abnormal_exit &&
            local_requested_mpi_finalize && mpi_log_rank) {
            g_printerr("[peak] PMPI_Finalize was requested before nonzero exit status %d; skipping aggregate output\n",
                       exit_status);
        } else if (found_MPI &&
                   output_mode == PEAK_OUTPUT_AGGREGATION_SOCKET &&
                   mpi_log_rank) {
            peak_log_info("[peak] Writing PEAK-owned socket-reduced output before MPI teardown coordination, MPI finalization, or process exit\n");
        } else if (found_MPI &&
                   aggregation_mode == PEAK_OUTPUT_AGGREGATION_LOCAL &&
                   mpi_log_rank) {
            peak_log_info("[peak] Aggregate output is disabled for strict teardown; writing rank-local output before MPI teardown coordination, MPI finalization, or process exit\n");
        } else if (found_MPI && mpi_collectives_failed_closed && mpi_log_rank) {
            g_printerr("[peak] PEAK MPI collective path was already failed closed; writing rank-local output without touching MPI again\n");
        }
        used_mpi_aggregation =
            peak_general_listener_print_with_mpi_job_policy(
                output_mode,
                TRUE,
                &report_write_succeeded);
        report_published = 1;
    }

    /*
     * Error exits often happen while only a subset of ranks is unwinding, and
     * a failed-closed request may still own MPI buffers. Do not make any MPI
     * call in either case.
     */
    int mpi_runtime_can_collect =
        found_MPI &&
        !abnormal_exit &&
        !mpi_collectives_failed_closed &&
        peak_mpi_runtime_allows_collectives();
    if (mpi_runtime_can_collect) {
        int mpi_rank = 0;
        MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
        mpi_log_rank = mpi_rank == 0;
    }
    int all_ranks_requested_mpi_finalize = 0;
    int need_mpi_finalize_proof =
        mpi_finalize_path ||
        (aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
         local_requested_mpi_finalize);
    if (need_mpi_finalize_proof &&
        mpi_runtime_can_collect &&
        !abnormal_exit &&
        !publish_before_finalize_proof) {
        all_ranks_requested_mpi_finalize =
            peak_mpi_teardown_all_ranks_requested_finalize(
                local_requested_mpi_finalize ? 1 : 0);
    }
    mpi_collectives_failed_closed =
        peak_mpi_teardown_collectives_failed_closed();
    int use_mpi_collective_output =
        !report_published &&
        aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
        mpi_runtime_can_collect &&
        !abnormal_exit &&
        !mpi_collectives_failed_closed &&
        all_ranks_requested_mpi_finalize;
    int use_socket_output =
        !report_published &&
        aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET &&
        !abnormal_exit &&
        !mpi_collectives_failed_closed;
    if (!report_published) {
        output_mode =
            use_mpi_collective_output ? PEAK_OUTPUT_AGGREGATION_MPI :
            use_socket_output ? PEAK_OUTPUT_AGGREGATION_SOCKET :
            PEAK_OUTPUT_AGGREGATION_LOCAL;
    }
    int report_release_gate_allowed =
        mpi_finalize_path &&
        mpi_runtime_can_collect &&
        !abnormal_exit &&
        !mpi_collectives_failed_closed &&
        (publish_before_finalize_proof ||
         all_ranks_requested_mpi_finalize);
    gboolean intel_2019_finalize_workaround = FALSE;
    int real_mpi_finalize_config_allowed =
        report_release_gate_allowed ?
            peak_mpi_real_finalize_config_allowed(
                &intel_2019_finalize_workaround) : 0;
    int allow_real_mpi_finalize = 0;
    if (!report_published) {
        if (found_MPI && abnormal_exit &&
            local_requested_mpi_finalize && mpi_log_rank) {
            g_printerr("[peak] PMPI_Finalize was requested before nonzero exit status %d; skipping aggregate output\n",
                       exit_status);
        } else if (found_MPI && use_mpi_collective_output && mpi_log_rank) {
            peak_log_info("[peak] PMPI_Finalize was observed on every rank; writing MPI-reduced output before MPI finalization or process exit\n");
        } else if (found_MPI && use_socket_output && mpi_log_rank) {
            peak_log_info("[peak] Writing PEAK-owned socket-reduced output before MPI finalization or process exit\n");
        } else if (found_MPI &&
                   aggregation_mode == PEAK_OUTPUT_AGGREGATION_LOCAL &&
                   mpi_log_rank) {
            peak_log_info("[peak] Aggregate output is disabled for strict teardown; writing rank-local output before MPI finalization or process exit\n");
        } else if (found_MPI &&
                   aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
                   !local_requested_mpi_finalize &&
                   mpi_log_rank) {
            g_printerr("[peak] PMPI_Finalize was not observed on every rank; writing rank-local output before process exit\n");
        } else if (found_MPI && !mpi_runtime_can_collect && mpi_log_rank) {
            g_printerr("[peak] MPI runtime is not in an output-safe state; writing rank-local output before process exit\n");
        } else if (found_MPI && mpi_collectives_failed_closed && mpi_log_rank) {
            g_printerr("[peak] PEAK MPI collective proof failed or timed out; writing rank-local output without touching MPI again\n");
        } else if (found_MPI &&
                   aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
                   !all_ranks_requested_mpi_finalize &&
                   mpi_log_rank) {
            g_printerr("[peak] PMPI_Finalize was not observed on every rank; writing rank-local output before process exit\n");
        } else if (found_MPI &&
                   output_mode == PEAK_OUTPUT_AGGREGATION_LOCAL &&
                   mpi_log_rank) {
            g_printerr("[peak] Aggregate output was not proven safe; writing rank-local output before process exit\n");
        }
        used_mpi_aggregation =
            peak_general_listener_print_with_mpi_job_policy(
                output_mode,
                found_MPI ? TRUE : FALSE,
                &report_write_succeeded);
        report_published = 1;
    }
    /*
     * Keep this check adjacent to PEAK output. If the payload reducer started
     * a nonblocking collective and then failed or timed out, the request has no
     * portable cancellation path. From this point onward PEAK must not return
     * to the real finalizer or issue any other MPI teardown calls.
     */
    int mpi_reducer_failed_closed =
        found_MPI && peak_general_listener_mpi_reducer_failed_closed();
    if (mpi_reducer_failed_closed) {
        peak_mpi_teardown_collectives_mark_failed_closed();
        use_mpi_collective_output = 0;
        allow_real_mpi_finalize = 0;
        if (mpi_log_rank) {
            g_printerr("[peak] MPI output reducer failed or timed out; skipping real PMPI_Finalize and avoiding further MPI teardown calls\n");
        }
    }
    #ifdef HAVE_CUDA
        cuda_interceptor_print_with_mpi_job_policy(
            (mpi_reducer_failed_closed ||
             (output_mode == PEAK_OUTPUT_AGGREGATION_MPI &&
              !used_mpi_aggregation)) ?
                PEAK_OUTPUT_AGGREGATION_LOCAL : output_mode,
            found_MPI ? TRUE : FALSE);
        mpi_reducer_failed_closed = found_MPI &&
            peak_general_listener_mpi_reducer_failed_closed();
        if (mpi_reducer_failed_closed) {
            peak_mpi_teardown_collectives_mark_failed_closed();
            allow_real_mpi_finalize = 0;
            use_mpi_collective_output = 0;
            if (mpi_log_rank) {
                g_printerr("[peak] CUDA MPI reducer failed or timed out; skipping real PMPI_Finalize\n");
            }
        }
        errno = 0;
        if (peak_log_flush() != 0) {
            report_write_succeeded = FALSE;
            peak_log_warn("[peak] failed to flush the complete CUDA report: %s\n",
                          strerror(errno != 0 ? errno : EIO));
        }
        if (!mpi_finalize_path) {
            cuda_interceptor_dettach();
        }
    #else
        (void)used_mpi_aggregation;
    #endif
    bool all_reports_succeeded = false;
    bool all_real_mpi_finalize_config_allowed = false;
    int report_release_protocol_completed = 0;
    if (report_release_gate_allowed && !mpi_reducer_failed_closed) {
        if (publish_before_finalize_proof) {
            bool all_requested_finalize = false;
            /*
             * output_mode records the attempted publication transport and
             * remains SOCKET if that backend waited and then fell back local.
             * Do not use the later use_socket_output eligibility boolean: it
             * is false after early publication and would lose the socket
             * R+2T arrival budget on exactly that fallback path.
             */
            PeakReportTimeoutBudget timeout_budget =
                peak_general_listener_report_timeout_budget();
            unsigned int publication_timeout_minimum_ms =
                output_mode == PEAK_OUTPUT_AGGREGATION_SOCKET
                    ? timeout_budget
                          .socket_combined_release_minimum_ms
                    : 0U;

            report_release_protocol_completed =
                peak_mpi_teardown_complete_post_publication_release(
                    local_requested_mpi_finalize ? 1 : 0,
                    report_write_succeeded ? 1 : 0,
                    real_mpi_finalize_config_allowed ? 1 : 0,
                    publication_timeout_minimum_ms,
                    &all_requested_finalize,
                    &all_reports_succeeded,
                    &all_real_mpi_finalize_config_allowed);
            all_ranks_requested_mpi_finalize =
                all_requested_finalize ? 1 : 0;
        } else {
            report_release_protocol_completed =
                peak_mpi_teardown_complete_report_release(
                    report_write_succeeded ? 1 : 0,
                    real_mpi_finalize_config_allowed ? 1 : 0,
                    &all_reports_succeeded,
                    &all_real_mpi_finalize_config_allowed);
        }
        if (mpi_log_rank) {
            if (report_release_protocol_completed) {
                if (publish_before_finalize_proof) {
                    peak_log_info("[peak] All-rank report publication release completed: all_reports_succeeded=%d all_real_finalize_allowed=%d all_requested_finalize=%d (combined finalize/report gate)\n",
                                  all_reports_succeeded ? 1 : 0,
                                  all_real_mpi_finalize_config_allowed ? 1 : 0,
                                  all_ranks_requested_mpi_finalize);
                } else {
                    peak_log_info("[peak] All-rank report publication release completed: all_reports_succeeded=%d all_real_finalize_allowed=%d\n",
                                  all_reports_succeeded ? 1 : 0,
                                  all_real_mpi_finalize_config_allowed ? 1 : 0);
                }
            } else {
                g_printerr("[peak] All-rank report publication release failed; skipping real PMPI_Finalize and avoiding later MPI teardown calls\n");
            }
        }
    }
    int base_real_mpi_finalize_allowed =
        report_release_gate_allowed &&
        all_ranks_requested_mpi_finalize;
    allow_real_mpi_finalize =
        base_real_mpi_finalize_allowed &&
        report_release_protocol_completed &&
        all_real_mpi_finalize_config_allowed;
    if (found_MPI && mpi_finalize_path) {
        mpi_interceptor_set_real_finalize_allowed(allow_real_mpi_finalize);
        if (mpi_log_rank) {
            if (report_release_protocol_completed &&
                all_reports_succeeded) {
                peak_log_report("[peak] PEAK output is complete; report publication and release succeeded\n");
            }
            if (allow_real_mpi_finalize) {
                if (all_reports_succeeded) {
                    peak_log_info("[peak] Returning to real PMPI_Finalize after successful PEAK report release\n");
                } else {
                    g_printerr("[peak] PEAK report publication failed on at least one rank; returning to real PMPI_Finalize for clean MPI teardown\n");
                }
            } else if (mpi_reducer_failed_closed) {
                g_printerr("[peak] PEAK output reducer did not complete cleanly; skipping real PMPI_Finalize\n");
            } else if (!base_real_mpi_finalize_allowed) {
                g_printerr("[peak] Real PMPI_Finalize is not proven all-rank safe; skipping real MPI finalizer\n");
            } else if (!report_release_protocol_completed) {
                g_printerr("[peak] Real PMPI_Finalize is not safe after report-release protocol failure; skipping real MPI finalizer\n");
            } else if (intel_2019_finalize_workaround) {
                peak_log_info("[peak] PEAK output release is complete; skipping real PMPI_Finalize for Intel MPI 2019 compatibility; set PEAK_MPI_REAL_FINALIZE=1 to override\n");
            } else if (!real_mpi_finalize_config_allowed) {
                g_printerr("[peak] PEAK report release succeeded; skipping real PMPI_Finalize because PEAK_MPI_REAL_FINALIZE=0\n");
            } else if (!all_real_mpi_finalize_config_allowed) {
                g_printerr("[peak] At least one rank disabled real PMPI_Finalize by runtime or environment policy; all ranks are skipping it\n");
            } else {
                g_printerr("[peak] Real PMPI_Finalize is disabled by policy; skipping real MPI finalizer\n");
            }
        }
    }
    if (found_MPI && !mpi_finalize_path && !local_requested_mpi_finalize) {
        mpi_interceptor_dettach(0);
    }
    if (found_MPI && (mpi_finalize_path || local_requested_mpi_finalize)) {
        if (mpi_log_rank) {
            if (mpi_finalize_path) {
                peak_log_info("[peak] Leaving PEAK target hooks pinned and restoring support wrappers before application PMPI_Finalize\n");
            } else {
                peak_log_info("[peak] Leaving PEAK target hooks pinned after application PMPI_Finalize to avoid post-finalize helper-backed Gum teardown\n");
            }
        }
#if !defined(__APPLE__)
        if (!pthread_listener_dettach()) {
            g_printerr("[peak] Leaving pthread listener bookkeeping allocated before application PMPI_Finalize\n");
        }
#endif
        peak_log_shutdown();
        return;
    }
#else
    (void)peak_general_listener_print(0, NULL);
    #ifdef HAVE_CUDA
    cuda_interceptor_print_with_mpi_job_policy(
        PEAK_OUTPUT_AGGREGATION_LOCAL, FALSE);
    cuda_interceptor_dettach();
    #endif
#endif
    gboolean general_listener_shutdown_flushed = peak_general_listener_dettach();
#if defined(__APPLE__)
    (void)general_listener_shutdown_flushed;
    peak_log_info("[peak] Darwin process-exit teardown leaves Gum support hook state alive\n");
    peak_log_shutdown();
    return;
#else
    if (general_listener_shutdown_flushed) {
        dlopen_interceptor_release_retained_dynamic_handles();
    }
    if (general_listener_shutdown_flushed) {
        if (!pthread_listener_dettach()) {
            g_printerr("[peak] Leaving pthread listener bookkeeping allocated for in-flight callbacks\n");
        }
    } else {
        g_printerr("[peak] Skipping pthread listener cleanup because general listener teardown is still reachable\n");
    }
    free_parsed_result(peak_hook_strings, peak_hook_address_count);
    if (general_listener_shutdown_flushed) {
        for (size_t i = 0; i < peak_target_thread_called_count; i++) {
            g_free(peak_target_thread_called[i]);
        }
        g_free(peak_target_thread_called);
        peak_target_thread_called = NULL;
        peak_target_thread_called_count = 0;
        g_free(peak_need_detach);
        g_free(peak_detached);
    } else {
        g_printerr("[peak] Leaving general listener bookkeeping allocated for in-flight callbacks\n");
    }
    peak_log_shutdown();
#endif
}

void peak_fini()
{
    int expected = PEAK_FINI_NOT_STARTED;
    int saved_cancel_state;
    gboolean checkpoint_gate_unlock_failed;
    PeakRuntimeActivationState activation_state;

    if (peak_runtime_is_fork_child()) {
        return;
    }
    activation_state = peak_runtime_close_activation_for_teardown();
    if (atomic_load_explicit(&peak_runtime_active, memory_order_acquire) == 0) {
        int warning_expected = 0;

        if (activation_state == PEAK_RUNTIME_ACTIVATION_CANCELED &&
            atomic_compare_exchange_strong_explicit(
                &peak_deferred_activation_warning_emitted,
                &warning_expected,
                1,
                memory_order_acq_rel,
                memory_order_acquire)) {
            peak_log_warn(
                "[peak] post-init activation was requested, but no "
                "interposed MPI_Init* completion was observed; no profile "
                "was collected\n");
        }
        return;
    }

    /* Initialize the shared completion protocol before any finalizer can
     * publish IN_PROGRESS, so followers never fall back to polling. */
    (void)peak_exec_checkpoint_gate_cond_is_ready();
    if (!peak_fini_cancellation_disable(&saved_cancel_state)) {
        peak_log_warn("[peak] Failed to protect finalization from cancellation; skipping PEAK teardown\n");
        return;
    }

    if (atomic_compare_exchange_strong_explicit(
            &peak_fini_state,
            &expected,
            PEAK_FINI_IN_PROGRESS,
            memory_order_acq_rel,
            memory_order_acquire)) {
        atomic_fetch_or_explicit(&peak_exec_checkpoint_gate,
                                 PEAK_EXEC_CHECKPOINT_GATE_CLOSING,
                                 memory_order_acq_rel);
        if (!peak_exec_checkpoint_wait_for_readers(
                &checkpoint_gate_unlock_failed)) {
            if (checkpoint_gate_unlock_failed) {
                peak_fini_publish_done_without_gate();
            } else {
                peak_fini_publish_done();
            }
            peak_fini_cancellation_restore(saved_cancel_state);
            return;
        }
        peak_fini_impl();
        peak_fini_publish_done();
        peak_fini_cancellation_restore(saved_cancel_state);
        return;
    }

    peak_fini_wait_for_completion();
    peak_fini_cancellation_restore(saved_cancel_state);
}

PEAK_EXEC_API int
peak_runtime_is_active_for_checkpoint(void)
{
    return atomic_load_explicit(&peak_runtime_active,
                                memory_order_acquire) != 0 &&
           !peak_runtime_is_fork_child() &&
           atomic_load_explicit(&peak_fini_state,
                                memory_order_acquire) == PEAK_FINI_NOT_STARTED;
}

PEAK_EXEC_API int
peak_checkpoint_for_exec(const char* path, char* const argv[])
{
    int saved_errno = errno;
    unsigned long long checkpoint_index;
    gboolean wrote;

    (void)path;
    (void)argv;

    if (!peak_exec_checkpoint_enabled_at_startup() ||
        !peak_runtime_is_active_for_checkpoint()) {
        errno = saved_errno;
        return -1;
    }

    if (!peak_exec_checkpoint_reader_acquire()) {
        errno = saved_errno;
        return -1;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_test_checkpoint_reader_pause_after_acquire();
#endif
    if (!peak_runtime_is_active_for_checkpoint()) {
        peak_exec_checkpoint_reader_release();
        errno = saved_errno;
        return -1;
    }

    checkpoint_index =
        atomic_fetch_add_explicit(&peak_exec_checkpoint_counter,
                                  1,
                                  memory_order_acq_rel) + 1;
    wrote = peak_general_listener_checkpoint_for_exec(checkpoint_index);
    peak_exec_checkpoint_reader_release();
    errno = saved_errno;
    return wrote ? 0 : -1;
}

#if defined(__APPLE__)
__attribute__((constructor)) static void
peak_macos_init(void)
{
    peak_init();
}

__attribute__((destructor)) static void
peak_macos_fini(void)
{
    peak_fini();
}
#elif defined(__ELF__)
typedef int (*main_fn)(int, char**, char**);
typedef int (*libc_start_main_fn)(main_fn, int, char**,
                                  int (*)(int, char**, char**),
                                  void (*)(void), void (*)(void), void*);

static main_fn real_main = NULL;
static libc_start_main_fn real___libc_start_main = NULL;

static gboolean
peak_should_wrap_main(int argc, char** argv)
{
    return peak_should_profile_command(argc, argv);
}

/* Original function pointer for exit(). */
static void (*original_exit)(int) = NULL;
static _Atomic int peak_exit_interposer_unavailable = 0;

static void
peak_resolve_real_exit(void)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* forced_failure = getenv("PEAK_TEST_FAIL_EXIT_INTERPOSER_INSTALL");
    gboolean force_libc_fallback =
        forced_failure != NULL && forced_failure[0] != '\0';
#else
    gboolean force_libc_fallback = FALSE;
#endif

    if (original_exit == NULL && !force_libc_fallback) {
        original_exit = (void (*)(int))dlsym(RTLD_NEXT, "exit");
    }
    if (original_exit == NULL) {
        /* RTLD_NEXT normally finds libc.  Resolve libc explicitly as a
         * fail-open fallback when a loader namespace prevents that lookup. */
        void* libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);

        if (libc != NULL) {
            /* Keep this handle open: original_exit must remain valid. */
            original_exit = (void (*)(int))dlsym(libc, "exit");
        }
    }
    if (force_libc_fallback) {
        atomic_store_explicit(&peak_exit_interposer_unavailable,
                              1,
                              memory_order_release);
        return;
    }
    if (original_exit == NULL) {
        atomic_store_explicit(&peak_exit_interposer_unavailable,
                              1,
                              memory_order_release);
    }
}

static void
peak_publish_exit_status(int status)
{
    int expected = 0;

    if (atomic_compare_exchange_strong_explicit(&peak_exit_status_known,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_store_explicit(&peak_exit_status_value,
                              status,
                              memory_order_release);
        atomic_store_explicit(&peak_exit_status_known,
                              2,
                              memory_order_release);
    } else {
        while (atomic_load_explicit(&peak_exit_status_known,
                                    memory_order_acquire) == 1) {
            sched_yield();
        }
    }
}

PEAK_API void
peak_exit(int status)
{
    PeakRuntimeActivationState activation_state;

    peak_resolve_real_exit();
    if (original_exit == NULL) {
        _exit(status);
    }
    if (atomic_load_explicit(&peak_exit_interposer_unavailable,
                             memory_order_acquire) != 0) {
        original_exit(status);
        __builtin_unreachable();
    }

    if (peak_runtime_is_fork_child()) {
        original_exit(status);
        __builtin_unreachable();
    }
    activation_state = peak_runtime_close_activation_for_teardown();

    if (activation_state != PEAK_RUNTIME_ACTIVATION_ACTIVE) {
        peak_fini();
        original_exit(status);
        __builtin_unreachable();
    }

    peak_publish_exit_status(status);
    peak_fini();

    /* Terminate through the original exit(). */
    original_exit(status);
    __builtin_unreachable();
}

PEAK_API void
exit(int status)
{
    peak_exit(status);
    __builtin_unreachable();
}

static int main_wrapper(int argc, char** argv, char** envp) {
    /* Initialize PEAK immediately before the application main(). */
    peak_init();

    int ret = real_main(argc, argv, envp);

    /*
     * Finalize before libc enters exit() and before the dynamic loader starts
     * running DSO finalizers. In particular, the Gum module-sync worker must
     * be joined while this thread is outside both an exit replacement and the
     * loader's teardown critical section. Publish the return status first so
     * MPI fail-closed handling observes nonzero returns just as it observes
     * explicit exit(status) calls.
     */
    peak_publish_exit_status(ret);
    peak_fini();

    return ret;
}

__attribute__((visibility("default")))
int __libc_start_main(main_fn main, int argc, char** argv,
                      int (*init)(int, char**, char**),
                      void (*fini)(void), void (*rtld_fini)(void), void* stack_end) {
    if (!real___libc_start_main) {
        real___libc_start_main = (libc_start_main_fn)dlsym(RTLD_NEXT, "__libc_start_main");
        if (!real___libc_start_main) {
            peak_log_warn("[peak] Error: dlsym failed to find __libc_start_main\n");
            _exit(1);
        }
    }
    peak_resolve_real_exit();

    /* Retain the application entry point for main_wrapper(). */
    real_main = main;

    /* Install main_wrapper() only when the command requests PEAK work. */
    int requested_work = peak_process_requests_work();
    if (atomic_load_explicit(&peak_exit_interposer_unavailable,
                             memory_order_acquire) != 0) {
        peak_report_snapshot_note_degraded(
            PEAK_PROFILER_DEGRADED_EXIT_INTERPOSER,
            "exit interposer installation unavailable");
    }
    /* A missing explicit-exit resolver does not prevent normal main-return
     * teardown, which remains safe and supplies the degraded report metadata. */
    gboolean should_wrap = peak_should_wrap_main(argc, argv) && requested_work;
    peak_set_process_profile_enabled(should_wrap);
    peak_set_process_requests_work(requested_work);
    if (should_wrap) {
        return real___libc_start_main(main_wrapper, argc, argv, init, fini, rtld_fini, stack_end);
    } else {
        return real___libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end);
    }
}
#else
#error Unsupported platform
#endif
