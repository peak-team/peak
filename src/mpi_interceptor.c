#include "mpi_interceptor.h"
#include "general_listener.h"
#include "logging.h"

#include <sched.h>
#include <stdlib.h>
#include <string.h>

#define PEAK_MPI_FINALIZE_POLICY_ENV "PEAK_MPI_FINALIZE_POLICY"

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* mpi_interceptor;

typedef enum {
    PEAK_MPI_FINALIZE_NOT_REQUESTED = 0,
    PEAK_MPI_FINALIZE_REQUESTED = 1,
    PEAK_MPI_FINALIZE_IN_PROGRESS = 2,
    PEAK_MPI_FINALIZE_DONE = 3,
} PeakMpiFinalizeState;

static int peak_finalize_state = PEAK_MPI_FINALIZE_NOT_REQUESTED;
static int peak_finalize_result = 0;
static int peak_real_finalize_allowed = 0;
static int peak_finalize_path_active = 0;
static gpointer hook_address;

static int (*original_pmpi_finalize)(void);
extern void peak_fini(void);

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
        __atomic_store_n(&peak_finalize_result, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&peak_finalize_state,
                         PEAK_MPI_FINALIZE_DONE,
                         __ATOMIC_RELEASE);
        return 0;
    }

    if (original_pmpi_finalize == NULL &&
        (!direct_finalize || hook_address == NULL)) {
        __atomic_store_n(&peak_finalize_state,
                         PEAK_MPI_FINALIZE_DONE,
                         __ATOMIC_RELEASE);
        return 0;
    }

    for (;;) {
        int state = __atomic_load_n(&peak_finalize_state, __ATOMIC_ACQUIRE);

        if (state == PEAK_MPI_FINALIZE_DONE) {
            return __atomic_load_n(&peak_finalize_result, __ATOMIC_ACQUIRE);
        }

        if (state == PEAK_MPI_FINALIZE_IN_PROGRESS) {
            sched_yield();
            continue;
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
        __atomic_store_n(&peak_finalize_result, result, __ATOMIC_RELEASE);
        __atomic_store_n(&peak_finalize_state,
                         PEAK_MPI_FINALIZE_DONE,
                         __ATOMIC_RELEASE);
        return result;
    }
}

/**
 * @brief Custom implementation of `PMPI_Finalize` function
 *
 * This function is a custom implementation of the `PMPI_Finalize` function. It
 * records the application's finalization request. The default policy lets PEAK
 * emit final output while MPI is still alive on the application's own finalize
 * path, then returns to the real MPI finalizer after all-rank proof. Output
 * aggregation selects the report transport and does not change that ordering.
 * Only an explicit PEAK_MPI_FINALIZE_POLICY=defer calls the real finalizer
 * immediately and leaves PEAK output for normal process teardown. PEAK does
 * not replay `PMPI_Finalize()` later from process teardown.
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
