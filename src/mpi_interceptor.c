#include "mpi_interceptor.h"

static GumInterceptor* mpi_interceptor;

static int peak_is_done = 0;
static gpointer hook_address;

static int (*original_pmpi_finalize)(void);

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
    if (peak_is_done)
        return original_pmpi_finalize();
    else
        return 0;
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

void mpi_interceptor_dettach()
{
    peak_is_done = 1;
    peak_pmpi_finalize();
    gum_interceptor_revert(mpi_interceptor, hook_address);
    g_object_unref(mpi_interceptor);
}