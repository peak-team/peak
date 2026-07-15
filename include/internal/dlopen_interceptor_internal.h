#ifndef __PEAK_DLOPEN_INTERCEPTOR_INTERNAL_H
#define __PEAK_DLOPEN_INTERCEPTOR_INTERNAL_H

#include "detach_controller.h"
#include "frida-gum.h"

/*
 * Dynamic attach must not turn a terminal preparation error into an
 * unbounded caller-side dlopen wait.  Ordinary detach/reattach retries keep
 * their existing state-machine policy; this predicate is specific to an
 * exact dynamic-load request whose caller is blocked for completion.
 */
gboolean dlopen_interceptor_dynamic_attach_prepare_is_retryable(
    PeakDetachStatus status);

/*
 * Once the controller and dlopen admission are live, synchronously scan
 * matching providers that were already loaded during earlier constructors.
 * This uses the same exact per-request handshake as a runtime dlopen, so
 * provider-local IFUNC/STT_NOTYPE lookup remains on the original loader
 * caller.
 */
gboolean dlopen_interceptor_rescan_loaded_modules(void);

/*
 * Pin the loaded provider containing address before publishing a startup Gum
 * hook.  The opaque pin is NULL on success when the address belongs to the
 * main executable or PEAK already retains that provider.
 */
gboolean dlopen_interceptor_pin_loaded_provider(gpointer address,
                                                void** pin_out);
void dlopen_interceptor_commit_pinned_provider(void* pin);
void dlopen_interceptor_release_pinned_provider(void* pin);

/*
 * Serialize ordinary controller-owned Gum mutations against application
 * threads executing the dlopen replacement body. Dynamic-attach queue drains
 * deliberately do not use this gate: their waiting caller already owns the
 * load transaction and needs the controller to complete that exact request.
 */
gboolean dlopen_interceptor_try_begin_controller_mutation(void);
void dlopen_interceptor_end_controller_mutation(void);

#endif /* __PEAK_DLOPEN_INTERCEPTOR_INTERNAL_H */
