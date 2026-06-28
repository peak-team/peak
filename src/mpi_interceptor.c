#define _GNU_SOURCE
#include "mpi_interceptor.h"
#include "general_listener.h"
#include "peak_logging.h"

#include <sched.h>
#include <stdlib.h>
#include <string.h>

#define PEAK_MPI_FINALIZE_POLICY_ENV "PEAK_MPI_FINALIZE_POLICY"
#define PEAK_OUTPUT_AGGREGATION_ENV  "PEAK_OUTPUT_AGGREGATION"

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

typedef struct {
    const char* symbol;
    gpointer hook_address;
    int (*original_finalize)(void);
    int active;
} PeakMpiFinalizeHook;

static PeakMpiFinalizeHook finalize_hooks[] = {
    { "MPI_Finalize", NULL, NULL, 0 },
    { "PMPI_Finalize", NULL, NULL, 0 },
};
static __thread int active_finalize_hook = -1;
static __thread int calling_original_finalize_depth = 0;
extern void peak_fini(void);

static size_t
mpi_interceptor_finalize_hook_count(void)
{
    return sizeof(finalize_hooks) / sizeof(finalize_hooks[0]);
}

static PeakMpiFinalizeHook*
mpi_interceptor_current_finalize_hook(void)
{
    if (active_finalize_hook >= 0 &&
        active_finalize_hook < (int)mpi_interceptor_finalize_hook_count() &&
        finalize_hooks[active_finalize_hook].active) {
        return &finalize_hooks[active_finalize_hook];
    }

    for (size_t i = 0; i < mpi_interceptor_finalize_hook_count(); i++) {
        if (finalize_hooks[i].active) {
            return &finalize_hooks[i];
        }
    }
    return NULL;
}

static int
mpi_interceptor_call_reentered_original(int hook_index)
{
    if (hook_index < 0 ||
        hook_index >= (int)mpi_interceptor_finalize_hook_count()) {
        return 0;
    }

    PeakMpiFinalizeHook* hook = &finalize_hooks[hook_index];
    if (!hook->active || hook->original_finalize == NULL) {
        return 0;
    }

    return hook->original_finalize();
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
mpi_interceptor_socket_aggregation_selected(void)
{
    const char* value = getenv(PEAK_OUTPUT_AGGREGATION_ENV);

    return value != NULL &&
           (g_ascii_strcasecmp(value, "socket") == 0 ||
            g_ascii_strcasecmp(value, "tcp") == 0 ||
            g_ascii_strcasecmp(value, "interconnect") == 0);
}

static int
mpi_interceptor_finalize_policy_defer(void)
{
    const char* value = getenv(PEAK_MPI_FINALIZE_POLICY_ENV);

    if (value == NULL || value[0] == '\0') {
        return mpi_interceptor_socket_aggregation_selected();
    }

    return value != NULL &&
           (g_ascii_strcasecmp(value, "defer") == 0 ||
            g_ascii_strcasecmp(value, "deferred") == 0 ||
            g_ascii_strcasecmp(value, "continue") == 0 ||
            g_ascii_strcasecmp(value, "exit") == 0);
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
    PeakMpiFinalizeHook* current = mpi_interceptor_current_finalize_hook();

    if (mpi_interceptor == NULL || current == NULL) {
        return 0;
    }

    gum_interceptor_begin_transaction(mpi_interceptor);
    for (size_t i = 0; i < mpi_interceptor_finalize_hook_count(); i++) {
        if (finalize_hooks[i].active && finalize_hooks[i].hook_address != NULL) {
            gum_interceptor_revert(mpi_interceptor,
                                   finalize_hooks[i].hook_address);
        }
    }
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
    PeakMpiFinalizeHook* current = mpi_interceptor_current_finalize_hook();

    if (!mpi_interceptor_real_finalize_enabled() ||
        !__atomic_load_n(&peak_real_finalize_allowed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&peak_finalize_result, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&peak_finalize_state,
                         PEAK_MPI_FINALIZE_DONE,
                         __ATOMIC_RELEASE);
        return 0;
    }

    if (current == NULL ||
        (current->original_finalize == NULL &&
         (!direct_finalize || current->hook_address == NULL))) {
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
            current->hook_address != NULL &&
            mpi_interceptor_restore_finalize_for_direct_call()) {
            int (*direct_finalize_fn)(void) =
                (int (*)(void))current->hook_address;
            calling_original_finalize_depth++;
            result = direct_finalize_fn();
            calling_original_finalize_depth--;
        } else if (current->original_finalize != NULL) {
            calling_original_finalize_depth++;
            result = current->original_finalize();
            calling_original_finalize_depth--;
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
 * path, then returns to the real MPI finalizer after all-rank proof.
 * PEAK_MPI_FINALIZE_POLICY=defer, or explicitly selecting socket output
 * without an explicit finalize policy, calls the real finalizer immediately
 * and leaves PEAK output for normal process teardown. PEAK does not replay
 * `PMPI_Finalize()` later from process teardown.
 *
 * @return The original `PMPI_Finalize()` result.
 */
static int
peak_mpi_finalize_common(int hook_index)
{
    int previous_hook = active_finalize_hook;

    if (calling_original_finalize_depth > 0) {
        return mpi_interceptor_call_reentered_original(hook_index);
    }

    active_finalize_hook = hook_index;
    // g_printerr ("peak_pmpi_finalize called %p\n",  &peak_is_done);
    mpi_interceptor_mark_finalize_requested();
    if (mpi_interceptor_finalize_policy_defer()) {
        int result;

        mpi_interceptor_set_real_finalize_allowed(1);
        result = mpi_interceptor_call_original_finalize_once();
        active_finalize_hook = previous_hook;
        return result;
    }
    __atomic_store_n(&peak_finalize_path_active, 1, __ATOMIC_RELEASE);
    peak_fini();
    __atomic_store_n(&peak_finalize_path_active, 0, __ATOMIC_RELEASE);
    int result = mpi_interceptor_call_original_finalize_once();
    active_finalize_hook = previous_hook;
    return result;
}

static int
peak_mpi_finalize(void)
{
    return peak_mpi_finalize_common(0);
}

static int
peak_pmpi_finalize(void)
{
    return peak_mpi_finalize_common(1);
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
    int attached = 0;

    mpi_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(mpi_interceptor);
    for (size_t i = 0; i < mpi_interceptor_finalize_hook_count(); i++) {
        gpointer candidate = NULL;
        if (strcmp(finalize_hooks[i].symbol, "MPI_Finalize") == 0) {
            candidate = peak_general_listener_find_function("MPI_Finalize");
        } else if (strcmp(finalize_hooks[i].symbol, "PMPI_Finalize") == 0) {
            candidate = peak_general_listener_find_function("PMPI_Finalize");
        }
        if (candidate == NULL) {
            continue;
        }

        int duplicate = 0;
        for (size_t j = 0; j < i; j++) {
            if (finalize_hooks[j].active &&
                finalize_hooks[j].hook_address == candidate) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            finalize_hooks[i].hook_address = candidate;
            finalize_hooks[i].active = 1;
            attached++;
            continue;
        }

        gpointer replacement =
            (i == 0) ? (gpointer)&peak_mpi_finalize
                     : (gpointer)&peak_pmpi_finalize;
        GumReplaceReturn replace_check =
            gum_interceptor_replace_fast(
                mpi_interceptor,
                candidate,
                replacement,
                (gpointer*)(&finalize_hooks[i].original_finalize),
                NULL);
        if (replace_check == GUM_REPLACE_OK) {
            finalize_hooks[i].hook_address = candidate;
            finalize_hooks[i].active = 1;
            attached++;
        } else {
            finalize_hooks[i].hook_address = NULL;
            finalize_hooks[i].original_finalize = NULL;
            finalize_hooks[i].active = 0;
        }
    }
    gum_interceptor_end_transaction(mpi_interceptor);
    return attached > 0 ? 0 : -1;
}

void mpi_interceptor_dettach(int allow_delayed_finalize)
{
    (void)allow_delayed_finalize;

    if (mpi_interceptor == NULL) {
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
    for (size_t i = 0; i < mpi_interceptor_finalize_hook_count(); i++) {
        if (finalize_hooks[i].active &&
            finalize_hooks[i].hook_address != NULL &&
            finalize_hooks[i].original_finalize != NULL) {
            gum_interceptor_revert(mpi_interceptor,
                                   finalize_hooks[i].hook_address);
        }
    }
    gum_interceptor_end_transaction(mpi_interceptor);
    if (!gum_interceptor_flush(mpi_interceptor)) {
        g_printerr("[peak] MPI interceptor teardown did not flush; leaving MPI interceptor state alive\n");
        return;
    }
    for (size_t i = 0; i < mpi_interceptor_finalize_hook_count(); i++) {
        finalize_hooks[i].hook_address = NULL;
        finalize_hooks[i].original_finalize = NULL;
        finalize_hooks[i].active = 0;
    }
}
