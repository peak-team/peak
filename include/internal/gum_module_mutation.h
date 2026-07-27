#ifndef PEAK_GUM_MODULE_MUTATION_H
#define PEAK_GUM_MODULE_MUTATION_H

#include "internal/gum_peak_compat.h"

/*
 * Patched Gum serializes PEAK mutations with its deferred module-registry
 * worker.  Stock/prebuilt Gum does not provide this extension, so keep the
 * call sites source- and ABI-compatible by compiling the guard to a no-op.
 */
static inline void
peak_gum_module_mutation_begin(void)
{
#if defined(GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION) && \
    GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION >= 3
    gum_interceptor_peak_begin_module_mutation();
#endif
}

static inline void
peak_gum_module_mutation_end(void)
{
#if defined(GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION) && \
    GUM_PEAK_DEFERRED_MODULE_SYNC_API_VERSION >= 3
    gum_interceptor_peak_end_module_mutation();
#endif
}

#endif /* PEAK_GUM_MODULE_MUTATION_H */
