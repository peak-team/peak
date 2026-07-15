#ifndef __PEAK_DLOPEN_INTERCEPTOR_INTERNAL_H
#define __PEAK_DLOPEN_INTERCEPTOR_INTERNAL_H

#include "frida-gum.h"

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
