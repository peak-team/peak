#include "mpi_interceptor.h"

static GumInterceptor* mpi_interceptor;

static int peak_is_done = 0;
static int peak_delayed_finalize_allowed = 0;
typedef enum {
    PEAK_MPI_FINALIZE_NOT_REQUESTED = 0,
    PEAK_MPI_FINALIZE_REQUESTED = 1,
    PEAK_MPI_FINALIZE_IN_PROGRESS = 2,
    PEAK_MPI_FINALIZE_DONE = 3,
} PeakMpiFinalizeState;

static int peak_finalize_state = PEAK_MPI_FINALIZE_NOT_REQUESTED;
static gpointer hook_address;

static int (*original_pmpi_finalize)(void);

static void
mpi_interceptor_mark_finalize_requested(void)
{
    int expected = PEAK_MPI_FINALIZE_NOT_REQUESTED;

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
    int expected = PEAK_MPI_FINALIZE_REQUESTED;

    if (original_pmpi_finalize == NULL) {
        return 0;
    }

    if (!__atomic_compare_exchange_n(
            &peak_finalize_state,
            &expected,
            PEAK_MPI_FINALIZE_IN_PROGRESS,
            FALSE,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
        return 0;
    }

    int result = original_pmpi_finalize();
    __atomic_store_n(&peak_finalize_state,
                     PEAK_MPI_FINALIZE_DONE,
                     __ATOMIC_RELEASE);
    return result;
}

/**
 * @brief Custom implementation of `PMPI_Finalize` function
 *
 * This function is a custom implementation of the `PMPI_Finalize` function that can be used to perform additional
 * actions before calling the original function. It checks the value of `peak_is_done` to determine whether to call
 * the original function or return immediately.
 *
 * @return the return value of the original `PMPI_Finalize` function if `peak_is_done` is true, otherwise 0.
 */
static int
peak_pmpi_finalize(void)
{
    // g_printerr ("peak_pmpi_finalize called %p\n",  &peak_is_done);
    mpi_interceptor_mark_finalize_requested();
    if (__atomic_load_n(&peak_is_done, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&peak_delayed_finalize_allowed, __ATOMIC_ACQUIRE))
        return mpi_interceptor_call_original_finalize_once();
    return 0;
}

int mpi_interceptor_finalize_was_requested()
{
    return __atomic_load_n(&peak_finalize_state, __ATOMIC_ACQUIRE) !=
           PEAK_MPI_FINALIZE_NOT_REQUESTED;
}

int mpi_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    mpi_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(mpi_interceptor);
    hook_address = gum_find_function("PMPI_Finalize");
    // g_printerr ("PMPI_Finalize found at %p\n",  hook_address);
    if (hook_address) {
        replace_check = gum_interceptor_replace_fast(mpi_interceptor,
                                                     hook_address, &peak_pmpi_finalize,
                                                     (gpointer*)(&original_pmpi_finalize));
    }
    gum_interceptor_end_transaction(mpi_interceptor);
    return replace_check;
}

void mpi_interceptor_dettach(int allow_delayed_finalize)
{
    if (mpi_interceptor == NULL || hook_address == NULL) {
        return;
    }

    __atomic_store_n(&peak_delayed_finalize_allowed,
                     allow_delayed_finalize ? 1 : 0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&peak_is_done, 1, __ATOMIC_RELEASE);
    if (allow_delayed_finalize && mpi_interceptor_finalize_was_requested()) {
        mpi_interceptor_call_original_finalize_once();
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
