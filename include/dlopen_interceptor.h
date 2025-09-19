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
 * @brief Detaches the dlopen interceptor.
 *
 * This function reverts the replacement of the intercepted `dloepn`
 * and releases the resources associated with the interceptor.
 */
void dlopen_interceptor_dettach(void);

#endif /* __DLOPEN_INTERCEPTOR_H */
