#ifndef __DLOPEN_INTERCEPTOR_H
#define __DLOPEN_INTERCEPTOR_H

/**
 * @file dlopen_interceptor.h
 * @brief Header file for dynamic library open interception functionality.
 *
 * Provides functions to attach and detach an interceptor for 
 * `dlopen`.
 */

#include "frida-gum.h"
#include <dlfcn.h> 
#include <unistd.h>

/**
 * @brief Attaches the dlopen interceptor.
 *
 * This function initializes and attaches an interceptor 
 * for the `dlopen` function. It obtains a GumInterceptor instance, 
 * locates the `dlopen` function's memory address, and replaces it with 
 * a custom implementation. 
 *
 * @return 0 on success, or an error code indicating failure.
 */
int dlopen_interceptor_attach(void);

/**
 * @brief Enables dynamic attach from the dlopen replacement.
 *
 * This should be called only after the general listener arrays have been
 * allocated and initial hooks have been published.
 */
void dlopen_interceptor_enable_dynamic_attach(void);

/**
 * @brief Drains queued dynamic attach work on the controller path.
 *
 * The dlopen replacement only enqueues retained handles. This function resolves
 * and attaches newly available symbols under the general listener controller
 * lock, outside the replacement body.
 */
void dlopen_interceptor_drain_dynamic_attach_queue(void);

/**
 * @brief Closes and releases dynamic attach work before listener teardown.
 *
 * After this call succeeds, `peak_dlopen` continues to forward to the real
 * dlopen but will not touch general-listener-owned state. Queued handles that
 * were not drained by the controller are released without attaching new hooks.
 *
 * @return TRUE when active dynamic attach and replacement bodies drained.
 */
gboolean dlopen_interceptor_shutdown_dynamic_attach(void);

/**
 * @brief Releases dynamic module handles retained for installed hooks.
 *
 * This must only be called after the general listener has physically detached
 * dynamic hooks and Gum teardown has flushed. Until then, PEAK keeps a retained
 * handle so application dlclose() cannot unload code that Gum still patches.
 */
void dlopen_interceptor_release_retained_dynamic_handles(void);

/**
 * @brief Detaches the dlopen interceptor.
 *
 * This function reverts the replacement of the intercepted `dloepn`
 * and releases the resources associated with the interceptor.
 */
gboolean dlopen_interceptor_dettach(void);

#endif /* __DLOPEN_INTERCEPTOR_H */
