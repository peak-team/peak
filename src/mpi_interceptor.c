#define _GNU_SOURCE
#include "mpi_interceptor.h"
#include "general_listener.h"
#include "internal/mpi_teardown_guard.h"
#include "logging.h"
#include "utils/env_parser.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PEAK_MPI_FINALIZE_POLICY_ENV "PEAK_MPI_FINALIZE_POLICY"
#define PEAK_MPI_FINALIZE_OWNER_TIMEOUT_MS_ENV \
    "PEAK_MPI_FINALIZE_OWNER_TIMEOUT_MS"
#define PEAK_MPI_FINALIZE_OWNER_TIMEOUT_MS_DEFAULT 5000U

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* mpi_interceptor;

typedef enum {
    PEAK_MPI_FINALIZE_NOT_REQUESTED = 0,
    PEAK_MPI_FINALIZE_REQUESTED = 1,
    PEAK_MPI_FINALIZE_IN_PROGRESS = 2,
    PEAK_MPI_FINALIZE_DONE = 3,
    PEAK_MPI_FINALIZE_FAILED_CLOSED = 4,
} PeakMpiFinalizeState;

static int peak_finalize_state = PEAK_MPI_FINALIZE_NOT_REQUESTED;
static int peak_finalize_result = 0;
static int peak_real_finalize_allowed = 0;
static int peak_finalize_path_active = 0;
static gpointer hook_address;
static pthread_mutex_t peak_finalize_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peak_finalize_cond;
static pthread_once_t peak_finalize_cond_once = PTHREAD_ONCE_INIT;
static _Atomic int peak_finalize_cond_ready;
static _Atomic int peak_finalize_wait_warning_emitted;
static PeakEnvWarningState peak_finalize_timeout_config_warning;
static unsigned int peak_finalize_owner_timeout_ms =
    PEAK_MPI_FINALIZE_OWNER_TIMEOUT_MS_DEFAULT;
#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic unsigned int peak_finalize_test_waiters;
static _Atomic unsigned int peak_finalize_test_original_calls;
#endif

static int (*original_pmpi_finalize)(void);
extern void peak_fini(void);

typedef int (*PeakMpiInitFunction)(int*, char***);
typedef int (*PeakMpiInitThreadFunction)(int*, char***, int, int*);

static PeakMpiInitFunction real_mpi_init;
static PeakMpiInitFunction real_pmpi_init;
static PeakMpiInitThreadFunction real_mpi_init_thread;
static PeakMpiInitThreadFunction real_pmpi_init_thread;
static pthread_once_t real_mpi_init_once = PTHREAD_ONCE_INIT;
static pthread_once_t real_pmpi_init_once = PTHREAD_ONCE_INIT;
static pthread_once_t real_mpi_init_thread_once = PTHREAD_ONCE_INIT;
static pthread_once_t real_pmpi_init_thread_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned int peak_mpi_init_wrapper_depth;

static void
mpi_interceptor_configure_finalize_owner_timeout(void)
{
    PeakEnvUnsignedSchema schema = {
        PEAK_MPI_FINALIZE_OWNER_TIMEOUT_MS_ENV,
        "milliseconds",
        PEAK_MPI_FINALIZE_OWNER_TIMEOUT_MS_DEFAULT,
        1,
        UINT_MAX,
        false,
        &peak_finalize_timeout_config_warning,
        false,
    };

    peak_finalize_owner_timeout_ms =
        (unsigned int)peak_parse_env_unsigned(&schema);
}

static void
mpi_interceptor_finalize_cond_initialize(void)
{
#if defined(__linux__)
    pthread_condattr_t attr;
    int status;

    if (pthread_condattr_init(&attr) != 0) {
        return;
    }
    status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (status == 0) {
        status = pthread_cond_init(&peak_finalize_cond, &attr);
    }
    (void)pthread_condattr_destroy(&attr);
    if (status == 0) {
        atomic_store_explicit(&peak_finalize_cond_ready, 1,
                              memory_order_release);
    }
#elif defined(__APPLE__)
    if (pthread_cond_init(&peak_finalize_cond, NULL) == 0) {
        atomic_store_explicit(&peak_finalize_cond_ready, 1,
                              memory_order_release);
    }
#endif
}

static int
mpi_interceptor_finalize_cond_timedwait(const struct timespec* deadline)
{
#if defined(__linux__)
    return pthread_cond_timedwait(&peak_finalize_cond,
                                  &peak_finalize_mutex,
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
    return pthread_cond_timedwait_relative_np(&peak_finalize_cond,
                                              &peak_finalize_mutex,
                                              &remaining);
#else
    (void)deadline;
    return ENOTSUP;
#endif
}

static void
mpi_interceptor_finalize_mark_done(void)
{
    int state = __atomic_load_n(&peak_finalize_state, __ATOMIC_ACQUIRE);

    while (state == PEAK_MPI_FINALIZE_REQUESTED ||
           state == PEAK_MPI_FINALIZE_IN_PROGRESS) {
        if (__atomic_compare_exchange_n(
                &peak_finalize_state, &state, PEAK_MPI_FINALIZE_DONE,
                FALSE, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

static void
mpi_interceptor_finalize_publish_done(int result)
{
    __atomic_store_n(&peak_finalize_result, result, __ATOMIC_RELEASE);
    if (pthread_once(&peak_finalize_cond_once,
                     mpi_interceptor_finalize_cond_initialize) != 0 ||
        !atomic_load_explicit(&peak_finalize_cond_ready,
                              memory_order_acquire) ||
        pthread_mutex_lock(&peak_finalize_mutex) != 0) {
        mpi_interceptor_finalize_mark_done();
        return;
    }
    mpi_interceptor_finalize_mark_done();
    (void)pthread_cond_broadcast(&peak_finalize_cond);
    (void)pthread_mutex_unlock(&peak_finalize_mutex);
}

static void
mpi_interceptor_finalize_wait_cancel(void* data)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_fetch_sub_explicit(&peak_finalize_test_waiters, 1,
                              memory_order_release);
#endif
    (void)pthread_mutex_unlock((pthread_mutex_t*)data);
}

static double
mpi_interceptor_finalize_elapsed_ms(const struct timespec* started)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1.0;
    }
    return (double)(now.tv_sec - started->tv_sec) * 1000.0 +
           (double)(now.tv_nsec - started->tv_nsec) / 1000000.0;
}

static int
mpi_interceptor_wait_for_finalize_owner(void)
{
    struct timespec started;
    struct timespec deadline;
    int state;
    int wait_status = 0;

    if (pthread_once(&peak_finalize_cond_once,
                     mpi_interceptor_finalize_cond_initialize) != 0 ||
        !atomic_load_explicit(&peak_finalize_cond_ready,
                              memory_order_acquire) ||
        clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        wait_status = ENOTSUP;
        goto failed_closed;
    }
    deadline = started;
    deadline.tv_sec += peak_finalize_owner_timeout_ms / 1000U;
    deadline.tv_nsec +=
        (long)(peak_finalize_owner_timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    if (pthread_mutex_lock(&peak_finalize_mutex) != 0) {
        wait_status = EINVAL;
        goto failed_closed;
    }
    pthread_cleanup_push(mpi_interceptor_finalize_wait_cancel,
                         &peak_finalize_mutex);
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_fetch_add_explicit(&peak_finalize_test_waiters, 1,
                              memory_order_release);
#endif
    while ((state = __atomic_load_n(&peak_finalize_state,
                                     __ATOMIC_ACQUIRE)) ==
           PEAK_MPI_FINALIZE_IN_PROGRESS) {
        wait_status = mpi_interceptor_finalize_cond_timedwait(&deadline);
        if (wait_status != 0) {
            break;
        }
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_fetch_sub_explicit(&peak_finalize_test_waiters, 1,
                              memory_order_release);
#endif
    pthread_cleanup_pop(0);
    (void)pthread_mutex_unlock(&peak_finalize_mutex);
    if (state == PEAK_MPI_FINALIZE_DONE) {
        return __atomic_load_n(&peak_finalize_result, __ATOMIC_ACQUIRE);
    }
    if (state == PEAK_MPI_FINALIZE_FAILED_CLOSED) {
        return MPI_ERR_OTHER;
    }
    if (state != PEAK_MPI_FINALIZE_IN_PROGRESS) {
        return 0;
    }

failed_closed:
    {
        int expected = PEAK_MPI_FINALIZE_IN_PROGRESS;

        if (__atomic_compare_exchange_n(
                &peak_finalize_state,
                &expected,
                PEAK_MPI_FINALIZE_FAILED_CLOSED,
                FALSE,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&peak_real_finalize_allowed, 0,
                             __ATOMIC_RELEASE);
            peak_mpi_teardown_collectives_mark_failed_closed();
            if (atomic_exchange_explicit(
                    &peak_finalize_wait_warning_emitted, 1,
                    memory_order_acq_rel) == 0) {
                double elapsed_ms = wait_status == ENOTSUP ? -1.0 :
                    mpi_interceptor_finalize_elapsed_ms(&started);

                if (elapsed_ms >= 0.0) {
                    peak_log_warn(
                        "[peak] MPI finalizer owner did not complete after "
                        "%.3f ms (timeout=%u ms; %s); failing closed without "
                        "another MPI call\n",
                        elapsed_ms, peak_finalize_owner_timeout_ms,
                        wait_status == ETIMEDOUT ? "deadline expired" :
                                                    "wait unavailable");
                } else {
                    peak_log_warn(
                        "[peak] MPI finalizer owner did not complete "
                        "(elapsed unavailable; timeout=%u ms; %s); failing "
                        "closed without another MPI call\n",
                        peak_finalize_owner_timeout_ms,
                        wait_status == ETIMEDOUT ? "deadline expired" :
                                                    "wait unavailable");
                }
            }
        }
    }
    return MPI_ERR_OTHER;
}

static void
mpi_interceptor_resolve_mpi_init_once(void)
{
    real_mpi_init = (PeakMpiInitFunction)dlsym(RTLD_NEXT, "MPI_Init");
}

static void
mpi_interceptor_resolve_pmpi_init_once(void)
{
    real_pmpi_init = (PeakMpiInitFunction)dlsym(RTLD_NEXT, "PMPI_Init");
}

static void
mpi_interceptor_resolve_mpi_init_thread_once(void)
{
    real_mpi_init_thread =
        (PeakMpiInitThreadFunction)dlsym(RTLD_NEXT, "MPI_Init_thread");
}

static void
mpi_interceptor_resolve_pmpi_init_thread_once(void)
{
    real_pmpi_init_thread =
        (PeakMpiInitThreadFunction)dlsym(RTLD_NEXT, "PMPI_Init_thread");
}

static int
mpi_interceptor_call_init(PeakMpiInitFunction init,
                          const char* symbol,
                          int* argc,
                          char*** argv)
{
    int result;

    peak_mpi_init_wrapper_depth++;
    if (init == NULL) {
        peak_log_warn("[peak] unable to resolve next %s\n", symbol);
        result = MPI_ERR_OTHER;
    } else {
        result = init(argc, argv);
    }
    peak_mpi_init_wrapper_depth--;
    if (peak_mpi_init_wrapper_depth == 0) {
        peak_mpi_init_completed(result);
    }
    return result;
}

static int
mpi_interceptor_call_init_thread(PeakMpiInitThreadFunction init,
                                 const char* symbol,
                                 int* argc,
                                 char*** argv,
                                 int required,
                                 int* provided)
{
    int result;

    peak_mpi_init_wrapper_depth++;
    if (init == NULL) {
        peak_log_warn("[peak] unable to resolve next %s\n", symbol);
        result = MPI_ERR_OTHER;
    } else {
        result = init(argc, argv, required, provided);
    }
    peak_mpi_init_wrapper_depth--;
    if (peak_mpi_init_wrapper_depth == 0) {
        peak_mpi_init_completed(result);
    }
    return result;
}

/*
 * These dynamic symbol interposers deliberately use no Gum state. They are the
 * only PEAK code around MPI startup. In post-init mode they activate the
 * runtime only after the real initializer has finished loading
 * OFI/UCX/libnuma providers.
 */
PEAK_API int
MPI_Init(int* argc, char*** argv)
{
    pthread_once(&real_mpi_init_once, mpi_interceptor_resolve_mpi_init_once);
    return mpi_interceptor_call_init(real_mpi_init, "MPI_Init", argc, argv);
}

PEAK_API int
PMPI_Init(int* argc, char*** argv)
{
    pthread_once(&real_pmpi_init_once, mpi_interceptor_resolve_pmpi_init_once);
    return mpi_interceptor_call_init(real_pmpi_init, "PMPI_Init", argc, argv);
}

PEAK_API int
MPI_Init_thread(int* argc, char*** argv, int required, int* provided)
{
    pthread_once(&real_mpi_init_thread_once,
                 mpi_interceptor_resolve_mpi_init_thread_once);
    return mpi_interceptor_call_init_thread(real_mpi_init_thread,
                                            "MPI_Init_thread",
                                            argc,
                                            argv,
                                            required,
                                            provided);
}

PEAK_API int
PMPI_Init_thread(int* argc, char*** argv, int required, int* provided)
{
    pthread_once(&real_pmpi_init_thread_once,
                 mpi_interceptor_resolve_pmpi_init_thread_once);
    return mpi_interceptor_call_init_thread(real_pmpi_init_thread,
                                            "PMPI_Init_thread",
                                            argc,
                                            argv,
                                            required,
                                            provided);
}

static int
mpi_interceptor_env_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static int
mpi_interceptor_direct_finalize_enabled(void)
{
    const char* value = getenv("PEAK_MPI_FINALIZE_CALL");

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    if (g_ascii_strcasecmp(value, "trampoline") == 0 ||
        g_ascii_strcasecmp(value, "gum") == 0 ||
        g_ascii_strcasecmp(value, "0") == 0 ||
        g_ascii_strcasecmp(value, "false") == 0 ||
        g_ascii_strcasecmp(value, "no") == 0 ||
        g_ascii_strcasecmp(value, "off") == 0) {
        return 0;
    }

    return g_ascii_strcasecmp(value, "direct") == 0 ||
           mpi_interceptor_env_truthy(value);
}

static int
mpi_interceptor_finalize_policy_defer(void)
{
    const char* value = getenv(PEAK_MPI_FINALIZE_POLICY_ENV);

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    return g_ascii_strcasecmp(value, "defer") == 0 ||
           g_ascii_strcasecmp(value, "deferred") == 0 ||
           g_ascii_strcasecmp(value, "continue") == 0 ||
           g_ascii_strcasecmp(value, "exit") == 0;
}

static int
mpi_interceptor_real_finalize_enabled(void)
{
    const char* value = getenv("PEAK_MPI_REAL_FINALIZE");

    if (value == NULL || value[0] == '\0') {
        return 1;
    }

    return mpi_interceptor_env_truthy(value);
}

static int
mpi_interceptor_restore_finalize_for_direct_call(void)
{
    if (mpi_interceptor == NULL || hook_address == NULL) {
        return 0;
    }

    gum_interceptor_begin_transaction(mpi_interceptor);
    gum_interceptor_revert(mpi_interceptor, hook_address);
    gum_interceptor_end_transaction(mpi_interceptor);
    if (!gum_interceptor_flush(mpi_interceptor)) {
        g_printerr("[peak] MPI finalize direct-call restore did not flush; using replacement trampoline\n");
        return 0;
    }

    return 1;
}

static void
mpi_interceptor_mark_finalize_requested(void)
{
    int expected = PEAK_MPI_FINALIZE_NOT_REQUESTED;

    peak_general_listener_note_mpi_finalize_requested();
    (void)__atomic_compare_exchange_n(
        &peak_finalize_state,
        &expected,
        PEAK_MPI_FINALIZE_REQUESTED,
        FALSE,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE);
}

static int
mpi_interceptor_call_original_finalize_once(void)
{
    int direct_finalize = mpi_interceptor_direct_finalize_enabled();

    if (!mpi_interceptor_real_finalize_enabled() ||
        !__atomic_load_n(&peak_real_finalize_allowed, __ATOMIC_ACQUIRE)) {
        mpi_interceptor_finalize_publish_done(0);
        return 0;
    }

    if (original_pmpi_finalize == NULL &&
        (!direct_finalize || hook_address == NULL)) {
        mpi_interceptor_finalize_publish_done(0);
        return 0;
    }

    for (;;) {
        int state = __atomic_load_n(&peak_finalize_state, __ATOMIC_ACQUIRE);

        if (state == PEAK_MPI_FINALIZE_DONE) {
            return __atomic_load_n(&peak_finalize_result, __ATOMIC_ACQUIRE);
        }

        if (state == PEAK_MPI_FINALIZE_FAILED_CLOSED) {
            return MPI_ERR_OTHER;
        }

        if (state == PEAK_MPI_FINALIZE_IN_PROGRESS) {
            return mpi_interceptor_wait_for_finalize_owner();
        }

        if (state != PEAK_MPI_FINALIZE_REQUESTED) {
            return 0;
        }

        int expected = PEAK_MPI_FINALIZE_REQUESTED;
        if (!__atomic_compare_exchange_n(
                &peak_finalize_state,
                &expected,
                PEAK_MPI_FINALIZE_IN_PROGRESS,
                FALSE,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE)) {
            continue;
        }

        int result = 0;
        if (direct_finalize &&
            hook_address != NULL &&
            mpi_interceptor_restore_finalize_for_direct_call()) {
            int (*direct_pmpi_finalize)(void) = (int (*)(void))hook_address;
            result = direct_pmpi_finalize();
        } else if (original_pmpi_finalize != NULL) {
            result = original_pmpi_finalize();
        }
        mpi_interceptor_finalize_publish_done(result);
        return result;
    }
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static int
mpi_interceptor_test_original_finalize_stub(void)
{
    atomic_fetch_add_explicit(&peak_finalize_test_original_calls, 1,
                              memory_order_relaxed);
    return MPI_SUCCESS;
}
#endif

/**
 * @brief Custom implementation of `PMPI_Finalize` function
 *
 * This function is a custom implementation of the `PMPI_Finalize` function. It
 * records the application's finalization request. The default policy lets PEAK
 * emit final output while MPI is still alive on the application's own finalize
 * path, then decides collectively after finalize-participation and
 * report-release gates whether returning to the real MPI finalizer is safe and
 * compatible. Output aggregation selects the report transport and does not
 * change that ordering. Only an explicit PEAK_MPI_FINALIZE_POLICY=defer
 * attempts the real finalizer immediately and leaves PEAK output for normal
 * process teardown; PEAK_MPI_REAL_FINALIZE=0 still disables that call. PEAK
 * does not replay `PMPI_Finalize()` later from process teardown.
 *
 * @return The original `PMPI_Finalize()` result.
 */
static int
peak_pmpi_finalize(void)
{
    mpi_interceptor_mark_finalize_requested();
    if (mpi_interceptor_finalize_policy_defer()) {
        mpi_interceptor_set_real_finalize_allowed(1);
        return mpi_interceptor_call_original_finalize_once();
    }
    __atomic_store_n(&peak_finalize_path_active, 1, __ATOMIC_RELEASE);
    peak_fini();
    __atomic_store_n(&peak_finalize_path_active, 0, __ATOMIC_RELEASE);
    return mpi_interceptor_call_original_finalize_once();
}

int mpi_interceptor_finalize_was_requested()
{
    return __atomic_load_n(&peak_finalize_state, __ATOMIC_ACQUIRE) !=
           PEAK_MPI_FINALIZE_NOT_REQUESTED;
}

int mpi_interceptor_finalize_path_active()
{
    return __atomic_load_n(&peak_finalize_path_active, __ATOMIC_ACQUIRE) != 0;
}

void
mpi_interceptor_set_real_finalize_allowed(int allowed)
{
    __atomic_store_n(&peak_real_finalize_allowed,
                     allowed ? 1 : 0,
                     __ATOMIC_RELEASE);
}

int mpi_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;

    mpi_interceptor_configure_finalize_owner_timeout();
    mpi_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(mpi_interceptor);
    hook_address = peak_general_listener_find_function("PMPI_Finalize");
    if (hook_address) {
        replace_check = gum_interceptor_replace_fast(mpi_interceptor,
                                                     hook_address, &peak_pmpi_finalize,
                                                     (gpointer*)(&original_pmpi_finalize),
                                                     NULL);
    }
    gum_interceptor_end_transaction(mpi_interceptor);
    return replace_check;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_MPI_INTERCEPTOR_API void
mpi_interceptor_test_set_finalize_in_progress(void)
{
    mpi_interceptor_configure_finalize_owner_timeout();
    __atomic_store_n(&peak_finalize_state,
                     PEAK_MPI_FINALIZE_IN_PROGRESS,
                     __ATOMIC_RELEASE);
}

PEAK_MPI_INTERCEPTOR_API int
mpi_interceptor_test_wait_for_finalize_owner(void)
{
    return mpi_interceptor_wait_for_finalize_owner();
}

PEAK_MPI_INTERCEPTOR_API int
mpi_interceptor_test_collectives_failed_closed(void)
{
    return peak_mpi_teardown_collectives_failed_closed() ? 1 : 0;
}

PEAK_MPI_INTERCEPTOR_API unsigned int
mpi_interceptor_test_finalize_waiter_count(void)
{
    return atomic_load_explicit(&peak_finalize_test_waiters,
                                memory_order_acquire);
}

PEAK_MPI_INTERCEPTOR_API void
mpi_interceptor_test_publish_finalize_result(int result)
{
    mpi_interceptor_finalize_publish_done(result);
}

PEAK_MPI_INTERCEPTOR_API void
mpi_interceptor_test_prepare_original_finalize_stub(void)
{
    original_pmpi_finalize = mpi_interceptor_test_original_finalize_stub;
    __atomic_store_n(&peak_real_finalize_allowed, 1, __ATOMIC_RELEASE);
    atomic_store_explicit(&peak_finalize_test_original_calls, 0,
                          memory_order_relaxed);
}

PEAK_MPI_INTERCEPTOR_API int
mpi_interceptor_test_call_original_finalize_once(void)
{
    return mpi_interceptor_call_original_finalize_once();
}

PEAK_MPI_INTERCEPTOR_API unsigned int
mpi_interceptor_test_original_finalize_call_count(void)
{
    return atomic_load_explicit(&peak_finalize_test_original_calls,
                                memory_order_relaxed);
}
#endif

void mpi_interceptor_dettach(int allow_delayed_finalize)
{
    (void)allow_delayed_finalize;

    if (mpi_interceptor == NULL || hook_address == NULL) {
        return;
    }

    /*
     * If we are already on the application's PMPI_Finalize path, keep the
     * replacement pinned until process exit. Reverting/flushing the Gum
     * replacement while executing that replacement leaves Intel MPI finalization
     * in a fragile state on large Frontera jobs.
     */
    if (__atomic_load_n(&peak_finalize_state, __ATOMIC_ACQUIRE) !=
        PEAK_MPI_FINALIZE_NOT_REQUESTED) {
        return;
    }

    gum_interceptor_begin_transaction(mpi_interceptor);
    gum_interceptor_revert(mpi_interceptor, hook_address);
    gum_interceptor_end_transaction(mpi_interceptor);
    if (!gum_interceptor_flush(mpi_interceptor)) {
        g_printerr("[peak] MPI interceptor teardown did not flush; leaving MPI interceptor state alive\n");
        return;
    }
    hook_address = NULL;
}
